// FocusTargetDecisionTest.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/FocusTargetDecision.h"

namespace NextKey {
namespace {

constexpr std::uintptr_t kChrome = 0x50820;
constexpr std::uintptr_t kAltTabHelper = 0x6010E;
constexpr std::uintptr_t kJumpList = 0x10188;

// Alt+Tab switcher's event lands after chrome's: live foreground wins.
TEST(FocusTargetDecisionTest, LateHelperEventUsesLiveForeground) {
    EXPECT_EQ(ChooseFocusTarget(kAltTabHelper, kChrome, false), kChrome);
}

// Foreground is a transient JumpList/taskbar: trust the event's window.
TEST(FocusTargetDecisionTest, TransientForegroundKeepsTrigger) {
    EXPECT_EQ(ChooseFocusTarget(kChrome, kJumpList, true), kChrome);
}

TEST(FocusTargetDecisionTest, NoTriggerUsesForeground) {
    EXPECT_EQ(ChooseFocusTarget(0, kChrome, false), kChrome);
    EXPECT_EQ(ChooseFocusTarget(0, kJumpList, true), kJumpList);
}

TEST(FocusTargetDecisionTest, BothHelpersKeepTrigger) {
    EXPECT_EQ(ChooseFocusTarget(kAltTabHelper, kJumpList, true), kAltTabHelper);
}

TEST(FocusTargetDecisionTest, NothingToClassify) {
    EXPECT_EQ(ChooseFocusTarget(0, 0, false), 0u);
}

}  // namespace
}  // namespace NextKey
