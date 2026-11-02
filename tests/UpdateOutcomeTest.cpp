// VKey - Installer outcome regression tests.
// SPDX-License-Identifier: GPL-3.0-only
#include "core/UpdateOutcome.h"
#include <gtest/gtest.h>
#include <chrono>

namespace NextKey {
namespace {
class UpdateOutcomeTest : public ::testing::Test {
protected:
    std::filesystem::path directory;
    const std::string exeHash = std::string(64, 'a');
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("vkey-outcome-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
    void Write(const char* name, const std::string& content = {}) {
        std::ofstream file(directory / name);
        file << content;
        ASSERT_TRUE(file.good());
    }
};

TEST_F(UpdateOutcomeTest, LegacyRollbackAndBackupDebrisNeverReportSuccess) {
    std::filesystem::create_directory(directory / "_old_version");
    Write("VKey_old.exe", "old executable");
    Write(kUpdateFailedMarker);
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::Failed);
    EXPECT_FALSE(std::filesystem::exists(directory / kUpdateFailedMarker));
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::None);
}

TEST_F(UpdateOutcomeTest, CleanupDebrisAloneIsNotAnUpdateReceipt) {
    std::filesystem::create_directory(directory / "_old_version");
    Write("VKey_old.exe", "old executable");
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::None);
}

TEST_F(UpdateOutcomeTest, FailureOverridesStaleSuccessReceipt) {
    Write(kUpdateSucceededMarker, exeHash);
    Write(kUpdateFailedMarker);
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::Failed);
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::None);
}

TEST_F(UpdateOutcomeTest, SuccessRequiresTheRunningExecutableAndIsConsumedOnce) {
    Write(kUpdateSucceededMarker, exeHash);
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::Succeeded);
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::None);
    Write(kUpdateSucceededMarker, std::string(64, 'b'));
    EXPECT_EQ(ConsumeUpdateOutcome(directory, exeHash), UpdateOutcome::Failed);
    Write(kUpdateSucceededMarker);
    EXPECT_EQ(ConsumeUpdateOutcome(directory, ""), UpdateOutcome::Failed);
}
}  // namespace
}  // namespace NextKey
