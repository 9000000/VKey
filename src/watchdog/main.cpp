// VKey Watchdog - Process Supervisor
// SPDX-License-Identifier: GPL-3.0-only
//
// Monitors VKey.exe via Local\VKeyHeartbeat (30s pulse, 90s
// timeout = 3 missed pulses). On crash detect (heartbeat stale +
// graceful flag clear + process not in process list), respawns
// VKey.exe via CreateProcessW.
//
// Launched at-logon by Task Scheduler under user account (NOT SYSTEM)
// to keep AV happy and to be able to launch user-session UI.

#include <Windows.h>
#include <TlHelp32.h>
#include <string>

#include "../core/RestartThrottle.h"
#include "../core/WatchdogEvents.h"

using NextKey::HEARTBEAT_EVENT_NAME;
using NextKey::GRACEFUL_SHUTDOWN_EVENT_NAME;

namespace {

constexpr DWORD HEARTBEAT_TIMEOUT_MS = 90'000;  // 3× publisher interval
constexpr DWORD POST_RESPAWN_GRACE_MS = 5'000;  // Wait this long after respawn
constexpr DWORD POST_INIT_GRACE_MS = 30'000;    // Initial grace for VKey to start

// Restart cap: after MAX_RESTARTS respawns within RESTART_WINDOW_MS, stop
// supervising. A repeated kill (AV quarantine, corrupt install, boot-loop crash)
// is not a transient crash, and fighting it escalates AV behavior verdicts.
constexpr std::size_t MAX_RESTARTS = 5;
constexpr uint64_t RESTART_WINDOW_MS = 600'000;  // 10 minutes
NextKey::RestartThrottle<MAX_RESTARTS> g_restartThrottle{RESTART_WINDOW_MS};

// Logging helper — append to %LOCALAPPDATA%\VKey\watchdog.log.
// Best-effort: silent failure if file unavailable.
void LogLine(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, L"\\VKey");
    CreateDirectoryW(path, nullptr);  // best-effort; ERROR_ALREADY_EXISTS is fine
    wcscat_s(path, MAX_PATH, L"\\watchdog.log");

    HANDLE hFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hFile, 0, nullptr, FILE_END);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[1024];
    int n = swprintf_s(buf, 1024, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, fmt);
    n += vswprintf_s(buf + n, 1024 - n, fmt, args);
    va_end(args);

    if (n < 1023) { buf[n] = L'\n'; buf[n + 1] = L'\0'; n++; }

    DWORD written;
    WriteFile(hFile, buf, n * sizeof(wchar_t), &written, nullptr);
    CloseHandle(hFile);
}

bool IsProcessAlive(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::wstring GetVKeyExePath() {
    // Watchdog and VKey live in the same install dir.
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path(self);
    auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    std::wstring dir = path.substr(0, pos);

    // Classic and Sciter ship in separate ZIPs, so only one EXE is present per
    // install. Prefer VKeyClassic.exe when it exists; otherwise fall back to the
    // Sciter VKey.exe. (If both were extracted into one folder, Classic wins.)
    std::wstring classicPath = dir + L"\\VKeyClassic.exe";
    DWORD attrs = GetFileAttributesW(classicPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return classicPath;
    }
    return dir + L"\\VKey.exe";
}

bool RespawnVKey() {
    std::wstring exePath = GetVKeyExePath();
    if (exePath.empty()) {
        LogLine(L"RespawnVKey: cannot resolve VKey path");
        return false;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exePath + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        LogLine(L"RespawnVKey: CreateProcess FAILED err=%lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LogLine(L"RespawnVKey: spawned PID=%lu", pi.dwProcessId);
    return true;
}

// Respawn under the restart cap. Returns false when the cap is hit, signalling
// the supervisor to stop (do NOT fight a persistent kill source).
bool TryRespawn() {
    if (!g_restartThrottle.AllowRestart(GetTickCount64())) {
        LogLine(L"Restart cap hit (%zu in %llus) — stopping respawns. Likely AV "
                L"quarantine or corrupt install, not a transient crash. Watchdog "
                L"exiting; will relaunch when VKey is next started.",
                MAX_RESTARTS, RESTART_WINDOW_MS / 1000);
        return false;
    }
    return RespawnVKey();
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Hold both events for our entire lifetime. Reopening them each iteration
    // loses a quick user quit when the publisher closes its last handle before
    // the watchdog opens the event. Never respawn without the shutdown channel.
    struct EventHandles {
        HANDLE graceful = CreateEventW(nullptr, TRUE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
        HANDLE heartbeat = CreateEventW(nullptr, FALSE, FALSE, HEARTBEAT_EVENT_NAME);
        ~EventHandles() {
            if (heartbeat) CloseHandle(heartbeat);
            if (graceful) CloseHandle(graceful);
        }
    } events;
    if (!events.graceful || !events.heartbeat) {
        LogLine(L"Cannot create watchdog events — supervision disabled (err=%lu)", GetLastError());
        return 1;
    }

    // Single-instance guard — both VKey app launch and Task Scheduler
    // at-logon trigger may try to start watchdog. The second one bails
    // silently on ERROR_ALREADY_EXISTS so we never run two supervisors
    // racing each other to respawn on the same heartbeat stale event.
    HANDLE singletonMutex = CreateMutexW(nullptr, FALSE,
                                         L"Local\\VKeyWatchdogSingleton");
    if (!singletonMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singletonMutex) CloseHandle(singletonMutex);
        return 0;
    }

    LogLine(L"Watchdog starting (pid=%lu)", GetCurrentProcessId());

    // All delays remain interruptible by a user-requested shutdown.
    if (WaitForSingleObject(events.graceful, POST_INIT_GRACE_MS) != WAIT_TIMEOUT) return 0;

    while (true) {
        const DWORD waitResult = NextKey::WaitForWatchdogSignal(
            events.graceful, events.heartbeat, HEARTBEAT_TIMEOUT_MS);
        if (waitResult == WAIT_OBJECT_0) {
            LogLine(L"Graceful shutdown requested → watchdog exiting");
            return 0;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) continue;
        if (waitResult != WAIT_TIMEOUT) {
            LogLine(L"Watchdog wait failed — stopping supervision (err=%lu)", GetLastError());
            return 1;
        }

        // Check quit once more before a restart decision at the timeout edge.
        if (WaitForSingleObject(events.graceful, 0) != WAIT_TIMEOUT) return 0;
        if (IsProcessAlive(L"VKey.exe") || IsProcessAlive(L"VKeyClassic.exe")) {
            LogLine(L"Heartbeat stale but process alive — NOT respawning");
        } else {
            LogLine(L"Heartbeat stale + graceful clear + process dead → CRASH detected");
            if (!TryRespawn()) return 0;
        }
        if (WaitForSingleObject(events.graceful, POST_RESPAWN_GRACE_MS) != WAIT_TIMEOUT) return 0;
    }
}
