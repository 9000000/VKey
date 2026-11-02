// VKey - Shared watchdog event contract (Windows-only).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef _WIN32
#include <Windows.h>

namespace NextKey {
inline constexpr const wchar_t* HEARTBEAT_EVENT_NAME = L"Local\\VKeyHeartbeat";
inline constexpr const wchar_t* GRACEFUL_SHUTDOWN_EVENT_NAME = L"Local\\VKeyGracefulShutdown";

// Quit takes precedence even when a queued heartbeat is also signalled.
[[nodiscard]] inline DWORD WaitForWatchdogSignal(HANDLE graceful, HANDLE heartbeat,
                                                DWORD timeoutMs) noexcept {
    const HANDLE events[] = {graceful, heartbeat};
    return WaitForMultipleObjects(2, events, FALSE, timeoutMs);
}
}  // namespace NextKey
#endif
