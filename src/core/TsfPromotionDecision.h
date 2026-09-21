// VKey - TSF Promotion Gate Decision
// SPDX-License-Identifier: GPL-3.0-only
//
// Pure decision: given the current typing/composition state of TSF,
// decide whether a pending configuration or dictionary snapshot can be safely
// promoted to the active engine without causing split-brain, tearing, or mid-word
// state corruption.
//
// Invariants:
// 1. engineBufferCount == 0 (engine buffer is completely empty)
// 2. !isComposing (TSF compositionMgr is not currently composing preedit)
// 3. rawMacroBufferEmpty (user is not mid-macro typing)
// 4. pendingReviveEmpty (no backspace revive word is armed waiting for key)

#pragma once

#include <cstddef>
#include <cstdint>

namespace NextKey {

struct TsfPromotionInputs {
    std::size_t engineBufferCount = 0;
    bool isComposing = false;
    bool rawMacroBufferEmpty = true;
    bool pendingReviveEmpty = true;
};

[[nodiscard]] constexpr bool CanPromoteTsfConfig(const TsfPromotionInputs& in) noexcept {
    return in.engineBufferCount == 0 &&
           !in.isComposing &&
           in.rawMacroBufferEmpty &&
           in.pendingReviveEmpty;
}

}  // namespace NextKey
