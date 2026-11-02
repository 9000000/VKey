// VKey - Heartbeat Publisher Implementation (Windows-only)
// SPDX-License-Identifier: GPL-3.0-only

#include "HeartbeatPublisher.h"

#ifdef _WIN32

#include "core/Debug.h"

namespace NextKey {

HeartbeatPublisher::~HeartbeatPublisher() {
    Stop();
}

bool HeartbeatPublisher::Start() {
    if (heartbeatEvent_) return true;  // Idempotent

    // Auto-reset event — SetEvent queues a signal that auto-clears on first
    // waiter wakeup. Avoids the PulseEvent (manual-reset + Set+Reset) anti-
    // pattern, which a kernel-mode APC can race past, leaving the watchdog
    // waiter unreleased.
    heartbeatEvent_ = CreateEventW(nullptr, FALSE, FALSE, HEARTBEAT_EVENT_NAME);
    if (!heartbeatEvent_) {
        NEXTKEY_LOG(L"HeartbeatPublisher: CreateEvent heartbeat FAILED err=%lu",
                    GetLastError());
        return false;
    }

    gracefulShutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
    if (!gracefulShutdownEvent_) {
        NEXTKEY_LOG(L"HeartbeatPublisher: CreateEvent graceful FAILED err=%lu",
                    GetLastError());
        CloseHandle(heartbeatEvent_);
        heartbeatEvent_ = nullptr;
        return false;
    }

    // The watchdog keeps this named event alive across publisher restarts.
    // CreateEvent's initial state is ignored when the object already exists.
    ResetEvent(gracefulShutdownEvent_);

    // Unnamed (kernel-internal) manual-reset event. Stays signaled once Stop()
    // fires it so any in-flight wait — or a wait that begins after Stop() —
    // returns immediately. Manual-reset rather than auto-reset because we
    // never need to consume the signal: a single Set ends the publisher's
    // lifetime.
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        NEXTKEY_LOG(L"HeartbeatPublisher: CreateEvent stop FAILED err=%lu",
                    GetLastError());
        CloseHandle(gracefulShutdownEvent_);
        gracefulShutdownEvent_ = nullptr;
        CloseHandle(heartbeatEvent_);
        heartbeatEvent_ = nullptr;
        return false;
    }

    stopRequested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this]() { Run(); });
    return true;
}

void HeartbeatPublisher::Stop() {
    if (!heartbeatEvent_ && !gracefulShutdownEvent_ && !stopEvent_) return;

    stopRequested_.store(true, std::memory_order_release);
    // Unblock Run()'s 30s wait so join completes promptly. SetEvent before
    // join — the worker may be mid-wait or about to enter one.
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();

    if (heartbeatEvent_) {
        CloseHandle(heartbeatEvent_);
        heartbeatEvent_ = nullptr;
    }
    if (gracefulShutdownEvent_) {
        CloseHandle(gracefulShutdownEvent_);
        gracefulShutdownEvent_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void HeartbeatPublisher::SignalGracefulShutdown() {
    if (gracefulShutdownEvent_) {
        SetEvent(gracefulShutdownEvent_);
    } else {
        // A supervisor from a previous installation may still be running even
        // when this installation has watchdog disabled. Honour tray quit there.
        HANDLE graceful = OpenEventW(EVENT_MODIFY_STATE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
        if (graceful) {
            SetEvent(graceful);
            CloseHandle(graceful);
        }
    }
}

void HeartbeatPublisher::Run() noexcept {
    // Heartbeat loop: SetEvent + wait. Auto-reset `heartbeatEvent_` clears
    // on first waiter wakeup; if no waiter is parked yet, the signal queues
    // until the next WaitForSingleObject call observes it.
    //
    // The wait blocks for HEARTBEAT_INTERVAL_MS on `stopEvent_`:
    //   - WAIT_OBJECT_0 → Stop() fired SetEvent → exit immediately.
    //   - WAIT_TIMEOUT  → 30 s elapsed without stop → loop and publish next
    //                     heartbeat.
    // Replaces a 100 ms Sleep polling loop (300 wakes per heartbeat cycle)
    // that prevented Windows from idle-trimming this thread's working set.
    // Stop() responsiveness preserved: SetEvent unblocks the wait at once.
    while (!stopRequested_.load(std::memory_order_acquire)) {
        if (heartbeatEvent_) {
            SetEvent(heartbeatEvent_);  // Auto-reset event: clears on first waiter wakeup
        }
        if (WaitForSingleObject(stopEvent_, HEARTBEAT_INTERVAL_MS) == WAIT_OBJECT_0) {
            return;
        }
    }
}

}  // namespace NextKey

#endif  // _WIN32
