// VKey — HeartbeatPublisher unit tests (Windows-only)
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>
#include "app/system/HeartbeatPublisher.h"

#ifdef _WIN32

namespace NextKey {

TEST(HeartbeatPublisherTest, StartCreatesNamedEvents) {
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());

    // Verify external observer can open the events.
    HANDLE hb = OpenEventW(SYNCHRONIZE, FALSE, HEARTBEAT_EVENT_NAME);
    HANDLE gs = OpenEventW(SYNCHRONIZE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
    EXPECT_NE(hb, nullptr);
    EXPECT_NE(gs, nullptr);
    if (hb) CloseHandle(hb);
    if (gs) CloseHandle(gs);

    pub.Stop();
}

TEST(HeartbeatPublisherTest, GracefulShutdownFlagSignalable) {
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());

    HANDLE gs = OpenEventW(SYNCHRONIZE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
    ASSERT_NE(gs, nullptr);

    // Initially not signaled.
    EXPECT_EQ(WaitForSingleObject(gs, 0), WAIT_TIMEOUT);

    pub.SignalGracefulShutdown();

    // Now signaled.
    EXPECT_EQ(WaitForSingleObject(gs, 100), WAIT_OBJECT_0);

    CloseHandle(gs);
    pub.Stop();
}

TEST(HeartbeatPublisherTest, StopIsIdempotent) {
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());
    pub.Stop();
    pub.Stop();  // No crash.
}

TEST(HeartbeatPublisherTest, IdempotentStartWhileRunning) {
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());
    EXPECT_TRUE(pub.Start());  // Second call must not spawn a second thread.
    pub.Stop();                // Single Stop must join cleanly (no orphan thread).
}

TEST(HeartbeatPublisherTest, RestartAfterStop) {
    // Toggle watchdog ON → OFF → ON exercises this restart cycle. Each Start
    // must create fresh named events; each Stop must release them so the next
    // Start sees a clean handle slot.
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());
    pub.Stop();

    // After Stop the named event should no longer be openable (publisher
    // closed its only handle and no consumer is holding the kernel object).
    HANDLE heartbeatHandle = OpenEventW(SYNCHRONIZE, FALSE, HEARTBEAT_EVENT_NAME);
    EXPECT_EQ(heartbeatHandle, nullptr);
    if (heartbeatHandle) CloseHandle(heartbeatHandle);

    // Re-Start: fresh events, thread runs again.
    ASSERT_TRUE(pub.Start());
    HANDLE heartbeatHandleAfterRestart = OpenEventW(SYNCHRONIZE, FALSE, HEARTBEAT_EVENT_NAME);
    EXPECT_NE(heartbeatHandleAfterRestart, nullptr);
    if (heartbeatHandleAfterRestart) CloseHandle(heartbeatHandleAfterRestart);

    pub.Stop();
}

TEST(HeartbeatPublisherTest, WatchdogRetainsQuickQuitAfterPublisherStops) {
    // The supervisor starts first and holds the events through its initial
    // grace period. A quick tray quit must remain visible after app teardown.
    HANDLE graceful = CreateEventW(nullptr, TRUE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
    HANDLE heartbeat = CreateEventW(nullptr, FALSE, FALSE, HEARTBEAT_EVENT_NAME);
    ASSERT_NE(graceful, nullptr);
    ASSERT_NE(heartbeat, nullptr);
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());
    pub.SignalGracefulShutdown();
    pub.Stop();
    // Also queue a heartbeat: explicit quit must win over a stale pulse.
    ASSERT_TRUE(SetEvent(heartbeat));
    EXPECT_EQ(WaitForWatchdogSignal(graceful, heartbeat, 100), WAIT_OBJECT_0);
    CloseHandle(heartbeat);
    CloseHandle(graceful);
}

TEST(HeartbeatPublisherTest, NewPublisherClearsPreviousGracefulShutdown) {
    HANDLE graceful = CreateEventW(nullptr, TRUE, TRUE, GRACEFUL_SHUTDOWN_EVENT_NAME);
    ASSERT_NE(graceful, nullptr);
    HeartbeatPublisher pub;
    ASSERT_TRUE(pub.Start());
    EXPECT_EQ(WaitForSingleObject(graceful, 0), WAIT_TIMEOUT);
    pub.Stop();
    CloseHandle(graceful);
}

TEST(HeartbeatPublisherTest, QuitSignalsExistingSupervisorWhenPublisherWasNotStarted) {
    HANDLE graceful = CreateEventW(nullptr, TRUE, FALSE, GRACEFUL_SHUTDOWN_EVENT_NAME);
    ASSERT_NE(graceful, nullptr);
    ResetEvent(graceful);
    HeartbeatPublisher pub;
    pub.SignalGracefulShutdown();
    EXPECT_EQ(WaitForSingleObject(graceful, 0), WAIT_OBJECT_0);
    CloseHandle(graceful);
}

}  // namespace NextKey

#endif  // _WIN32
