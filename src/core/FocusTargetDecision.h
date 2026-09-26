// VKey - which window a focus classification describes
// SPDX-License-Identifier: GPL-3.0-only
//
// WINEVENT_OUTOFCONTEXT delivers EVENT_SYSTEM_FOREGROUND late and can reorder
// it: the Alt+Tab switcher's event may arrive after the target app's, so
// trusting the trigger HWND applies explorer's profile to chrome (bait lost,
// "gox" -> "goõ") or to a game (game send mode lost). The live foreground is
// the truth unless it is itself a transient helper (JumpList, taskbar, hidden
// message window) — the reason the trigger was preferred in the first place.
//
// Handles are opaque so the policy stays Linux-testable.

#pragma once

#include <cstdint>

namespace NextKey {

[[nodiscard]] constexpr std::uintptr_t ChooseFocusTarget(
    std::uintptr_t trigger, std::uintptr_t foreground,
    bool foregroundIsHelper) noexcept {
    if (foreground && !foregroundIsHelper) return foreground;
    return trigger ? trigger : foreground;
}

}  // namespace NextKey
