// VKey - Heartbeat Publisher (Windows-only)
// SPDX-License-Identifier: GPL-3.0-only
//
// Publishes a 30s heartbeat to a named event so VKeyWatchdog.exe
// can detect process liveness. Also publishes a graceful-shutdown flag
// (named event in signaled state) so the watchdog distinguishes user-
// initiated quit from crash.
//
// Event names (Local\ session-scoped, kernel objects):
//   Local\VKeyHeartbeat         — auto-reset event, signaled every 30s
//                                     (queues until watchdog Wait observes)
//   Local\VKeyGracefulShutdown  — signaled by SignalGracefulShutdown
//                                     before VKey exits via tray quit

#pragma once

#ifdef _WIN32

#include <Windows.h>
#include "core/WatchdogEvents.h"
#include <atomic>
#include <thread>

namespace NextKey {

inline constexpr DWORD HEARTBEAT_INTERVAL_MS = 30'000;

class HeartbeatPublisher {
public:
    HeartbeatPublisher() = default;
    ~HeartbeatPublisher();

    HeartbeatPublisher(const HeartbeatPublisher&) = delete;
    HeartbeatPublisher& operator=(const HeartbeatPublisher&) = delete;

    /// Open the named events and spawn the pulse thread. Returns false
    /// if event creation fails (caller treats as best-effort — VKey
    /// still functions, just no auto-respawn).
    [[nodiscard]] bool Start();

    /// Stop the pulse thread and close handles. Idempotent.
    void Stop();

    /// Set the graceful-shutdown flag — call before tray-quit exit so
    /// the watchdog does NOT respawn VKey.
    /// Caller must not call this concurrently with Stop().
    void SignalGracefulShutdown();

private:
    void Run() noexcept;

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    HANDLE heartbeatEvent_ = nullptr;
    HANDLE gracefulShutdownEvent_ = nullptr;
    // Unnamed manual-reset event. Stop() signals it to unblock Run()'s 30s
    // wait immediately; the wait timeout (HEARTBEAT_INTERVAL_MS) drives the
    // heartbeat cadence. Replaces a Sleep(100) polling loop that woke the
    // worker 300×/heartbeat just to check the stop flag — blocked idle-trim.
    HANDLE stopEvent_ = nullptr;
};

}  // namespace NextKey

#endif  // _WIN32
