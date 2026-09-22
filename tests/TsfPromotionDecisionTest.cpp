// TsfPromotionDecisionTest.cpp
// Tests the pure decision logic for TSF pending config promotion gate
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/TsfPromotionDecision.h"

namespace NextKey {
namespace {

TEST(TsfPromotionDecisionTest, AllClearAllowsPromotion) {
    TsfPromotionInputs in{
        .engineBufferCount = 0,
        .isComposing = false,
        .rawMacroBufferEmpty = true,
        .pendingReviveEmpty = true,
    };
    EXPECT_TRUE(CanPromoteTsfConfig(in));
}

TEST(TsfPromotionDecisionTest, EngineBufferBlocksPromotion) {
    TsfPromotionInputs in{
        .engineBufferCount = 2,
        .isComposing = false,
        .rawMacroBufferEmpty = true,
        .pendingReviveEmpty = true,
    };
    EXPECT_FALSE(CanPromoteTsfConfig(in));
}

TEST(TsfPromotionDecisionTest, ActiveCompositionBlocksPromotion) {
    TsfPromotionInputs in{
        .engineBufferCount = 0,
        .isComposing = true,
        .rawMacroBufferEmpty = true,
        .pendingReviveEmpty = true,
    };
    EXPECT_FALSE(CanPromoteTsfConfig(in));
}

TEST(TsfPromotionDecisionTest, RawMacroBufferBlocksPromotion) {
    TsfPromotionInputs in{
        .engineBufferCount = 0,
        .isComposing = false,
        .rawMacroBufferEmpty = false,
        .pendingReviveEmpty = true,
    };
    EXPECT_FALSE(CanPromoteTsfConfig(in));
}

TEST(TsfPromotionDecisionTest, PendingReviveBlocksPromotion) {
    TsfPromotionInputs in{
        .engineBufferCount = 0,
        .isComposing = false,
        .rawMacroBufferEmpty = true,
        .pendingReviveEmpty = false,
    };
    EXPECT_FALSE(CanPromoteTsfConfig(in));
}

TEST(TsfPromotionDecisionTest, MultipleBlocksReturnFalse) {
    TsfPromotionInputs in{
        .engineBufferCount = 5,
        .isComposing = true,
        .rawMacroBufferEmpty = false,
        .pendingReviveEmpty = false,
    };
    EXPECT_FALSE(CanPromoteTsfConfig(in));
}

TEST(TsfPromotionDecisionTest, StagedLexiconSurvivesAnotherSharedStateUpdate) {
    const auto decision = DecidePendingTsfLexicon({
        .hasPendingConfig = true,
        .lexiconSnapshotStaged = true,
    });
    EXPECT_TRUE(decision.preserveConfigFields);
    EXPECT_TRUE(decision.keepPendingDictionary);
}

TEST(TsfPromotionDecisionTest, UnstagedLexiconDoesNotOverrideSharedState) {
    const auto decision = DecidePendingTsfLexicon({
        .hasPendingConfig = true,
        .lexiconSnapshotStaged = false,
    });
    EXPECT_FALSE(decision.preserveConfigFields);
    EXPECT_FALSE(decision.keepPendingDictionary);
}

}  // namespace
}  // namespace NextKey
