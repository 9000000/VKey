// VKey - Self-Update Installer Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "UpdateInstaller.h"
#include "UpdateSecurity.h"
#include "core/Debug.h"

#include <TlHelp32.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <memory>
#include <set>

namespace NextKey {

namespace {

/// Get exe directory
std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0) return L".";
    std::wstring fullPath(path, len);
    auto pos = fullPath.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? fullPath.substr(0, pos) : L".";
}

/// Get full exe path
std::wstring GetExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

/// Get current process ID
DWORD GetCurrentPID() {
    return GetCurrentProcessId();
}

/// Wait for all other instances of this app to exit (up to timeoutMs)
bool WaitForOtherProcesses(DWORD timeoutMs) {
    DWORD myPid = GetCurrentPID();

    wchar_t myPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, myPath, MAX_PATH);
    std::wstring myName = myPath;
    auto pos = myName.find_last_of(L"\\/");
    if (pos != std::wstring::npos) myName = myName.substr(pos + 1);

    DWORD startTick = GetTickCount();
    while (GetTickCount() - startTick < timeoutMs) {
        bool othersRunning = false;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) break;

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);

        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID != myPid &&
                    (_wcsicmp(pe.szExeFile, myName.c_str()) == 0)) {
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    wchar_t otherPath[MAX_PATH] = {};
                    DWORD pathLength = MAX_PATH;
                    const bool resolved = process && QueryFullProcessImageNameW(process, 0, otherPath, &pathLength);
                    if (process) CloseHandle(process);
                    // A separate installation must not block this handover.
                    // If a same-name process cannot be resolved, fail closed.
                    if (!resolved || _wcsicmp(otherPath, myPath) == 0) {
                        othersRunning = true;
                        break;
                    }
                }
            } while (Process32NextW(snap, &pe));
        }

        CloseHandle(snap);

        if (!othersRunning) return true;
        Sleep(500);
    }

    return false;  // Timeout
}

/// Extract ZIP using PowerShell Expand-Archive (hidden window)
bool ExtractZip(const std::wstring& zipPath, const std::wstring& destDir) {
    std::wstring safeZipPath = EscapePowerShellSingleQuote(zipPath);
    std::wstring safeDestDir = EscapePowerShellSingleQuote(destDir);
    if (safeZipPath.empty() || safeDestDir.empty()) return false;

    // Build PowerShell command
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"";
    cmd += L"$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath '";
    cmd += safeZipPath;
    cmd += L"' -DestinationPath '";
    cmd += safeDestDir;
    cmd += L"' -Force\"";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
        (void)TerminateProcess(pi.hProcess, 1);
        (void)WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return exitCode == 0;
}

/// Replace a locked-prone DLL. Tries MoveFileW on the live copy first — NTFS
/// allows same-volume rename of image-mapped DLLs because the image section is
/// opened with FILE_SHARE_DELETE. If that succeeds, we copy the new file in
/// place and processes that haven't loaded the DLL yet pick up the new version
/// immediately.
///
/// If rename fails (rare: AV holding a non-share-delete handle), stash the new
/// DLL next to the old one with a `.pending` suffix and drop a marker file so
/// WinMain applies the swap on the next EXE launch.
bool HandleTsfDllReplace(const std::wstring& newDllSrc,
                         const std::wstring& exeDir,
                         const std::wstring& oldVersionDir, std::wstring& parkedBackup) {
    namespace fs = std::filesystem;

    std::wstring liveDll = exeDir + L"\\" + TSF_DLL_FILENAME;
    std::wstring parked  = oldVersionDir + L"\\" + TSF_DLL_FILENAME
                         + MakeParkedDllTimestamp();

    std::error_code ec;
    fs::create_directories(oldVersionDir, ec);

    if (GetFileAttributesW(liveDll.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        return CopyFileW(newDllSrc.c_str(), liveDll.c_str(), FALSE) != FALSE;
    }
    if (MoveFileW(liveDll.c_str(), parked.c_str())) {
        if (CopyFileW(newDllSrc.c_str(), liveDll.c_str(), FALSE)) {
            DeleteFileW((exeDir + L"\\" + TSF_DLL_FILENAME + TSF_DLL_PENDING_SUFFIX).c_str());
            DeleteFileW((exeDir + L"\\" + TSF_DLL_PENDING_MARKER).c_str());
            parkedBackup = parked;
            return true;
        }
        MoveFileW(parked.c_str(), liveDll.c_str());
    }

    // Fallback: defer to next EXE startup.
    std::wstring pendingPath = exeDir + L"\\" + TSF_DLL_FILENAME + TSF_DLL_PENDING_SUFFIX;
    if (!CopyFileW(newDllSrc.c_str(), pendingPath.c_str(), FALSE)) {
        // Disk full / permission denied / AV quarantine — leave NO marker so
        // the next boot's ApplyPendingDllUpdate doesn't see half-deferred
        // state. User retains the old DLL; EXE may be newer but ABI gate
        // will catch it on DLL-load.
        return false;
    }

    // Marker body = SHA256 of the pending DLL. ApplyPendingDllUpdate recomputes
    // and verifies before swapping — prevents applying a user-dropped or
    // corrupted `.pending` file.
    std::string sha = ComputeFileSha256(pendingPath);
    if (sha.empty()) {
        NEXTKEY_LOG(L"HandleTsfDllReplace: SHA-256 of pending DLL failed — discarding");
        DeleteFileW(pendingPath.c_str());
        return false;
    }
    std::wstring markerPath = exeDir + L"\\" + TSF_DLL_PENDING_MARKER;
    HANDLE hMarker = CreateFileW(markerPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hMarker == INVALID_HANDLE_VALUE) {
        NEXTKEY_LOG(L"HandleTsfDllReplace: CreateFileW marker failed (err=%lu) — discarding pending",
                    GetLastError());
        DeleteFileW(pendingPath.c_str());  // back out of the half-deferred state
        return false;
    }
    DWORD written = 0;
    BOOL wrote = WriteFile(hMarker, sha.data(), static_cast<DWORD>(sha.size()), &written, nullptr);
    CloseHandle(hMarker);
    if (!wrote || written != sha.size()) {
        NEXTKEY_LOG(L"HandleTsfDllReplace: marker WriteFile short/failed (wrote=%lu/%zu err=%lu) — discarding",
                    written, sha.size(), GetLastError());
        DeleteFileW(markerPath.c_str());
        DeleteFileW(pendingPath.c_str());
        return false;
    }
    return true;
}

/// Copy all files from srcDir to destDir (overwriting)
bool CopyDirectoryContents(const std::wstring& srcDir, const std::wstring& destDir) {
    try {
        namespace fs = std::filesystem;
        for (const auto& entry : fs::recursive_directory_iterator(srcDir)) {
            auto relativePath = fs::relative(entry.path(), srcDir);

            // Defense-in-depth: reject paths with ".." to prevent directory traversal
            if (relativePath.wstring().find(L"..") != std::wstring::npos) continue;

            auto destPath = fs::path(destDir) / relativePath;

            if (entry.is_directory()) {
                fs::create_directories(destPath);
            } else {
                fs::create_directories(destPath.parent_path());
                if (relativePath.wstring() == TSF_DLL_FILENAME) continue;
                // Preserving config.toml if it already exists at the root level:
                if (_wcsicmp(relativePath.wstring().c_str(), L"config.toml") == 0 && fs::exists(destPath)) {
                    continue; // Skip overwriting config.toml
                }
                fs::copy_file(entry.path(), destPath, fs::copy_options::overwrite_existing);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

// The authenticated ZIP covers third-party DLLs as well as our binaries.
// Require the exact edition layout so no companion or unrelated file can be
// mistaken for the application executable.
[[nodiscard]] bool VerifyPackageLayout(const std::wstring& directory) noexcept {
    try {
#ifdef VKEY_LITE_MODE
        const std::set<std::wstring> expected{L"THIRD_PARTY_NOTICES.txt", L"VKeyClassic.exe", L"VKeyTSF.dll"};
#else
        const std::set<std::wstring> expected{L"THIRD_PARTY_NOTICES.txt", L"VKey.exe", L"VKeyTSF.dll", L"sciter.dll"};
#endif
        std::set<std::wstring> found;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || entry.is_symlink()) return false;
            found.insert(entry.path().filename().wstring());
        }
        return found == expected;
    } catch (...) {
        return false;
    }
}

// Restore original binaries from _old_version/, mark the update failed, relaunch
// the restored app, and exit. Shared by the extraction-failure and signature-
// verification-failure paths so both recover identically.
[[noreturn]] void RollbackRestoreAndExit(const std::wstring& exeDir,
                                         const std::wstring& oldVersionDir, bool restoreBackup = true,
                                         const std::wstring& parkedTsf = L"") {
    namespace fs = std::filesystem;
    std::error_code rollbackEc;
    std::wstring restoredExePath = restoreBackup ? std::wstring{} : GetExePath();
    if (restoreBackup) {
        for (const auto& entry : fs::directory_iterator(oldVersionDir, rollbackEc)) {
            if (!entry.is_regular_file()) continue;
            std::wstring name = entry.path().filename().wstring();
            if (name.starts_with(std::wstring(TSF_DLL_FILENAME) + L"_")) continue;
            std::wstring destPath = exeDir + L"\\" + name;
            const bool restored = MoveFileExW(entry.path().c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
            if (restored && (_wcsicmp(name.c_str(), L"VKey.exe") == 0 ||
                             _wcsicmp(name.c_str(), L"VKeyClassic.exe") == 0)) {
                restoredExePath = destPath;
            }
        }
    }

    if (!parkedTsf.empty()) {
        const auto live = exeDir + L"\\" + TSF_DLL_FILENAME;
        const auto failed = oldVersionDir + L"\\" + TSF_DLL_FILENAME + MakeParkedDllTimestamp(L"_failed");
        (void)MoveFileW(live.c_str(), failed.c_str());
        (void)MoveFileW(parkedTsf.c_str(), live.c_str());
    }
    DeleteFileW((exeDir + L"\\" + TSF_DLL_FILENAME + TSF_DLL_PENDING_SUFFIX).c_str());
    DeleteFileW((exeDir + L"\\" + TSF_DLL_PENDING_MARKER).c_str());
    if (restoredExePath.empty() && fs::is_regular_file(GetExePath(), rollbackEc)) restoredExePath = GetExePath();

    std::wstring markerPath = exeDir + L"\\_update_failed";
    DeleteFileW((exeDir + L"\\_update_succeeded").c_str());
    HANDLE hMarker = CreateFileW(markerPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hMarker != INVALID_HANDLE_VALUE) CloseHandle(hMarker);

    if (!restoredExePath.empty()) {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::wstring cmdLine = L"\"" + restoredExePath + L"\"";
        if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                            CREATE_BREAKAWAY_FROM_JOB, nullptr, exeDir.c_str(), &si, &pi)) {
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                               0, nullptr, exeDir.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        } else {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    ExitProcess(1);
}

}  // namespace

std::wstring MakeParkedDllTimestamp(const wchar_t* extraSuffix) noexcept {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    // Include PID + tick-count low word to disambiguate same-second collisions
    // (e.g. user double-clicks the installer and two --install-update processes
    // race to park the live DLL).
    wchar_t ts[96];
    swprintf_s(ts, L"_%04u%02u%02u_%02u%02u%02u_%lu_%lx%s",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond,
               GetCurrentProcessId(),
               GetTickCount() & 0xFFFFu,
               extraSuffix ? extraSuffix : L"");
    return ts;
}

[[noreturn]] void RunUpdateInstaller(const std::wstring& zipPath) {
    // Loader failures (missing/quarantined imports, invalid images) must return
    // to our rollback path instead of waiting on a hidden Windows error dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::wstring exeDir = GetExeDirectory();
    std::wstring tempDir = exeDir + L"\\_update_temp";
    DeleteFileW((exeDir + L"\\_update_succeeded").c_str());

    // 1. Wait for all other VKey.exe processes to exit (120s timeout)
    if (!WaitForOtherProcesses(120000)) {
        // Do not move live binaries after a failed handover.
        std::ofstream(std::filesystem::path(exeDir) / kUpdateFailedMarker);
        ExitProcess(1);
    }

    const std::wstring oldVersionDir = exeDir + L"\\_old_version";
    const auto closePackage = [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); };
    std::unique_ptr<void, decltype(closePackage)> package(
        CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr), closePackage);
    if (package.get() == INVALID_HANDLE_VALUE || !VerifySignedUpdateZip(zipPath)) {
        RollbackRestoreAndExit(exeDir, oldVersionDir, false);
    }

    // Known-clean start: delete any stale .pending / marker from a prior
    // aborted update. Otherwise a rollback in this run + stale pending from
    // a prior run could pair an old .pending DLL with a freshly-rolled-back
    // EXE and apply mismatched bits at the next boot.
    DeleteFileW((exeDir + L"\\" + TSF_DLL_FILENAME + TSF_DLL_PENDING_SUFFIX).c_str());
    DeleteFileW((exeDir + L"\\" + TSF_DLL_PENDING_MARKER).c_str());

    std::wstring parkedTsf;

    // 3. Extract ZIP to _update_temp/
    {
        // Clean up any previous temp dir
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    bool extracted = ExtractZip(zipPath, tempDir);
    if (!extracted) {
        RollbackRestoreAndExit(exeDir, oldVersionDir, false);
    }
    package.reset();

    // 4. Detect ZIP structure: root files or single subdirectory
    std::wstring finalExePath;
    {
        namespace fs = std::filesystem;
        std::wstring sourceDir = tempDir;

        // Check if there's a single subdirectory (common ZIP structure)
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(tempDir)) {
            entries.push_back(e);
        }

        if (entries.size() == 1 && entries[0].is_directory()) {
            sourceDir = entries[0].path().wstring();
        }

        if (!VerifyPackageLayout(sourceDir)) {
            RollbackRestoreAndExit(exeDir, oldVersionDir, false);
        }

        // Back up application files only. An update must not remove unrelated
        // executables or optional companions from the installation directory.
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(oldVersionDir, ec);

            for (const auto& entry : fs::directory_iterator(exeDir)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().wstring();
                if (_wcsicmp(ext.c_str(), L".exe") == 0 || _wcsicmp(ext.c_str(), L".dll") == 0) {
                    std::wstring name = entry.path().filename().wstring();
                    if (name != L"VKey.exe" && name != L"VKeyClassic.exe" &&
                        name != L"sciter.dll" && name != TSF_DLL_FILENAME) continue;

                    // TSF DLL is handled separately after extraction — it may be
                    // mapped into foreign host processes and cannot be bulk-moved
                    // safely alongside the EXE kill path.
                    if (_wcsicmp(name.c_str(), TSF_DLL_FILENAME) == 0) continue;

                    std::wstring destPath = oldVersionDir + L"\\" + name;
                    DeleteFileW(destPath.c_str());
                    MoveFileW(entry.path().c_str(), destPath.c_str());
                }
            }
        }

        // 5b. Copy remaining new files to exe directory.
        if (!CopyDirectoryContents(sourceDir, exeDir)) {
            RollbackRestoreAndExit(exeDir, oldVersionDir, true, parkedTsf);
        }

        // TSF is swapped only after every ordinary file has copied. A failed
        // copy must not leave a new DLL paired with the restored application.
        if (!HandleTsfDllReplace(sourceDir + L"\\" + TSF_DLL_FILENAME, exeDir, oldVersionDir, parkedTsf)) {
            RollbackRestoreAndExit(exeDir, oldVersionDir, true, parkedTsf);
        }

        // Launch the same edition whose signed package was verified.
#ifdef VKEY_LITE_MODE
        const std::vector<std::wstring> preferredNames = { L"VKeyClassic.exe" };
#else
        const std::vector<std::wstring> preferredNames = { L"VKey.exe" };
#endif
        for (const auto& name : preferredNames) {
            std::wstring testPath = exeDir + L"\\" + name;
            if (fs::exists(testPath)) {
                finalExePath = testPath;
                break;
            }
        }

        // Never mistake a watchdog/browser companion for the main application.
        if (finalExePath.empty()) RollbackRestoreAndExit(exeDir, oldVersionDir, true, parkedTsf);
    }

    // 7. Clean up temp files
    DeleteFileW(zipPath.c_str());
    DeleteFileW((zipPath + L".sig").c_str());
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    // 8. Bind the success receipt to the executable actually installed. Startup
    // consumes this only if that exact EXE is running; debris cannot imply success.
    const std::string installedHash = ComputeFileSha256(finalExePath);
    if (installedHash.empty()) RollbackRestoreAndExit(exeDir, oldVersionDir, true, parkedTsf);
    {
        std::ofstream receipt(std::filesystem::path(exeDir) / kUpdateSucceededMarker);
        receipt << installedHash;
        receipt.close();
        if (!receipt) RollbackRestoreAndExit(exeDir, oldVersionDir, true, parkedTsf);
    }
    DeleteFileW((exeDir + L"\\_update_failed").c_str());

    // Launch new executable
    if (!finalExePath.empty()) {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};

        // Quote the path for CreateProcessW cmdline
        std::wstring cmdLine = L"\"" + finalExePath + L"\"";

        if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                            CREATE_BREAKAWAY_FROM_JOB, nullptr, exeDir.c_str(), &si, &pi)) {
            // Fallback: if breakaway fails due to restricted job object, retry without it
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                               0, nullptr, exeDir.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                RollbackRestoreAndExit(exeDir, oldVersionDir, true, parkedTsf);
            }
        } else {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    // 9. Exit updater
    ExitProcess(0);
}

UpdateOutcome ConsumeInstalledUpdateOutcome() noexcept {
    try {
        const auto directory = std::filesystem::path(GetExeDirectory());
        std::error_code ec;
        const auto hash = std::filesystem::exists(directory / kUpdateSucceededMarker, ec)
            ? ComputeFileSha256(GetExePath()) : std::string{};
        return ConsumeUpdateOutcome(directory, hash);
    } catch (...) {
        return UpdateOutcome::None;
    }
}

bool CleanupOldUpdateFiles() noexcept {
    bool cleaned = false;
    try {
        std::wstring exeDir = GetExeDirectory();
        namespace fs = std::filesystem;

        // 1. Delete *_old.* files (legacy cleanup)
        for (const auto& entry : fs::directory_iterator(exeDir)) {
            if (!entry.is_regular_file()) continue;
            std::wstring name = entry.path().filename().wstring();

            // Check for _old before extension
            auto stem = entry.path().stem().wstring();
            if (stem.size() >= 4 && stem.substr(stem.size() - 4) == L"_old") {
                std::error_code ec;
                if (fs::remove(entry.path(), ec)) {
                    cleaned = true;
                }
            }
        }

        // 2. Delete _old_version/ directory
        std::wstring oldVersionDir = exeDir + L"\\_old_version";
        if (fs::exists(oldVersionDir)) {
            std::error_code ec;
            std::uintmax_t count = fs::remove_all(oldVersionDir, ec);
            if (!ec && count > 0 && count != static_cast<std::uintmax_t>(-1)) {
                cleaned = true;
            }
        }

        // 3. Delete _update_temp/ directory if it exists
        std::wstring tempDir = exeDir + L"\\_update_temp";
        if (fs::exists(tempDir)) {
            std::error_code ec;
            fs::remove_all(tempDir, ec);
        }
    } catch (...) {
        // Cleanup is best-effort
    }
    return cleaned;
}

}  // namespace NextKey
