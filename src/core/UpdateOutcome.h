// VKey - Consume installer outcomes independently of housekeeping.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace NextKey {

enum class UpdateOutcome { None, Succeeded, Failed };
inline constexpr auto kUpdateFailedMarker = "_update_failed";
inline constexpr auto kUpdateSucceededMarker = "_update_succeeded";

// The installer records the installed EXE's digest, not merely the presence of
// backup files. A rollback always wins over a stale success receipt.
[[nodiscard]] inline UpdateOutcome ConsumeUpdateOutcome(
    const std::filesystem::path& directory, std::string_view runningExeHash) noexcept {
    try {
        const auto failed = directory / kUpdateFailedMarker;
        const auto succeeded = directory / kUpdateSucceededMarker;
        std::error_code ec;
        if (std::filesystem::exists(failed, ec)) {
            std::filesystem::remove(failed, ec);
            std::filesystem::remove(succeeded, ec);
            return UpdateOutcome::Failed;
        }
        if (ec) return UpdateOutcome::None;
        std::ifstream receipt(succeeded);
        if (!receipt) return UpdateOutcome::None;
        std::string expectedHash;
        std::getline(receipt, expectedHash);
        receipt.close();
        std::filesystem::remove(succeeded, ec);
        if (runningExeHash.size() == 64 && expectedHash == runningExeHash) {
            return UpdateOutcome::Succeeded;
        }
        return UpdateOutcome::Failed;
    } catch (...) {
        return UpdateOutcome::None;
    }
}

}  // namespace NextKey
