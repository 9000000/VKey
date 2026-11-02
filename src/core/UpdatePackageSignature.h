// VKey - Detached application-update signature protocol.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace NextKey::UpdatePackageSignature {
enum class Edition : std::uint32_t { Sciter = 1, Classic = 2 };
inline constexpr std::size_t kFileBytes = 72;
struct Parsed {
    std::uint32_t version = 0;
    Edition edition = Edition::Sciter;
    std::array<std::uint8_t, 64> signature{};
};

[[nodiscard]] inline std::optional<Parsed> Parse(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != kFileBytes) return std::nullopt;
    Parsed result;
    std::uint32_t edition = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        result.version |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
        edition |= static_cast<std::uint32_t>(bytes[4 + i]) << (8 * i);
    }
    if (edition != 1 && edition != 2) return std::nullopt;
    result.edition = static_cast<Edition>(edition);
    std::memcpy(result.signature.data(), bytes.data() + 8, result.signature.size());
    return result;
}

[[nodiscard]] inline bool Accepts(const Parsed& parsed, std::uint32_t currentVersion,
                                 std::uint32_t expectedVersion, Edition edition) noexcept {
    return parsed.edition == edition && parsed.version > currentVersion &&
           (expectedVersion == 0 || parsed.version == expectedVersion);
}

[[nodiscard]] inline std::array<std::uint8_t, 48> BuildPayload(
    const std::array<std::uint8_t, 32>& digest, const Parsed& parsed) noexcept {
    std::array<std::uint8_t, 48> result{};
    constexpr char domain[] = "VKEYUPD1";
    std::memcpy(result.data(), domain, 8);
    std::memcpy(result.data() + 8, digest.data(), digest.size());
    for (std::size_t i = 0; i < 4; ++i) {
        result[40 + i] = static_cast<std::uint8_t>((parsed.version >> (8 * i)) & 0xff);
        result[44 + i] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(parsed.edition) >> (8 * i)) & 0xff);
    }
    return result;
}
}  // namespace NextKey::UpdatePackageSignature
