// VKey - Spell Exclusion Canonicalizer
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace NextKey {

constexpr size_t kMaxSpellExclusionRows = 8;
constexpr size_t kMinSpellExclusionScalars = 2;
constexpr size_t kMaxSpellExclusionScalars = 128;

enum class SpellExclusionErrorKind : uint8_t {
    None = 0,
    TooManyEntries,
    TooShort,
    TooLong,
    InvalidEntry,
};

struct SpellExclusionError {
    SpellExclusionErrorKind kind = SpellExclusionErrorKind::None;
    size_t row = 0;       // 1-based source row index (for TooShort, TooLong, InvalidEntry)
    size_t count = 0;     // Distinct entry count after deduplication (for TooManyEntries)
    size_t scalars = 0;   // Scalar count of rejected entry (for TooLong)
    std::wstring message; // Detailed diagnostic message
};

struct SpellExclusionCanonicalResult {
    std::vector<std::wstring> entries;    // Canonical rows in deterministic UTF-8 persistence order
    std::vector<std::string> utf8Entries; // UTF-8 byte representations in the same order
    SpellExclusionError error;

    [[nodiscard]] bool Succeeded() const noexcept {
        return error.kind == SpellExclusionErrorKind::None;
    }
};

class SpellExclusionCanonicalizer {
public:
    /// Canonicalizes exclusion entries from wide strings (UTF-16 on Windows).
    [[nodiscard]] static SpellExclusionCanonicalResult Canonicalize(
        const std::vector<std::wstring>& entries);

    /// Canonicalizes exclusion entries from UTF-8 strings.
    [[nodiscard]] static SpellExclusionCanonicalResult Canonicalize(
        const std::vector<std::string>& entries);

    /// Canonicalizes newline-delimited exclusion text (UTF-16).
    [[nodiscard]] static SpellExclusionCanonicalResult CanonicalizeFromText(
        const std::wstring& text);

    /// Canonicalizes newline-delimited exclusion text (UTF-8).
    [[nodiscard]] static SpellExclusionCanonicalResult CanonicalizeFromText(
        std::string_view text);

    /// Checks if a 32-bit Unicode code point has the Unicode White_Space property.
    [[nodiscard]] static bool IsUnicodeWhitespace(uint32_t cp) noexcept;

    /// Checks if a 32-bit Unicode code point has the General_Category=Cc (Control) property.
    [[nodiscard]] static bool IsUnicodeControl(uint32_t cp) noexcept;

    /// Trims leading and trailing Unicode White_Space from a UTF-32 scalar string.
    [[nodiscard]] static std::u32string TrimWhitespace(std::u32string_view input);

    /// Normalizes a UTF-32 scalar string through the canonical pipeline:
    /// 1. Trim -> 2. NFC -> 3. Lowercase -> 4. NFC -> 5. Validate.
    static bool NormalizeAndValidate(
        std::u32string_view input,
        size_t row,
        std::string& outUtf8,
        std::wstring& outUtf16,
        SpellExclusionError& outError);

    /// Decodes a UTF-16 wide string to UTF-32 scalar values, validating surrogates.
    [[nodiscard]] static bool Utf16ToUtf32(const std::wstring& input, std::u32string& output);

    /// Encodes a UTF-32 scalar string to UTF-16 wide string.
    [[nodiscard]] static bool Utf32ToUtf16(const std::u32string& input, std::wstring& output);

    /// Encodes a UTF-32 scalar string to UTF-8.
    [[nodiscard]] static bool Utf32ToUtf8(const std::u32string& input, std::string& output);

    /// Decodes a UTF-8 string to UTF-32 scalar values, validating bytes and ranges.
    [[nodiscard]] static bool Utf8ToUtf32(std::string_view input, std::u32string& output);

private:
    /// Normalizes UTF-16 string to NFC form.
    static bool NormalizeNfcUtf16(const std::vector<uint16_t>& input, std::vector<uint16_t>& output);

    /// Converts UTF-16 string to lowercase (invariant locale).
    static bool ToLowercaseUtf16(const std::vector<uint16_t>& input, std::vector<uint16_t>& output);
};

} // namespace NextKey
