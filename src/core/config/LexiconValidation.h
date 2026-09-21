// VKey - Lexicon Validation and Parsing for User Dictionary & Spell Exclusions
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace NextKey {

constexpr size_t kMaxUserDictEntries = 1024;
constexpr size_t kMaxUserDictWordScalars = 64;
constexpr size_t kMaxUserDictUtf16Units = 65536;

struct LexiconValidationResult {
    bool ok = true;
    size_t errorLine = 0;       // 1-based line number (0 if no error or global error)
    std::wstring errorMessage;  // Localized human-readable diagnostic message

    [[nodiscard]] bool Succeeded() const noexcept { return ok; }
};

struct LexiconImportResult {
    LexiconValidationResult validation;
    std::vector<std::wstring> entries;
    size_t addedCount = 0;
    size_t duplicateCount = 0;
};

class LexiconValidator {
public:
    /// Validate a single User Dictionary word.
    /// Normalizes (trim, lowercase, NFC) and validates scalar count (1..64) and characters.
    [[nodiscard]] static LexiconValidationResult ValidateUserDictWord(
        const std::wstring& word,
        size_t lineNum = 0,
        std::wstring* outNormalized = nullptr);

    /// Validate a single Spell Exclusion word.
    /// Normalizes and validates scalar count (2..128) and characters.
    [[nodiscard]] static LexiconValidationResult ValidateSpellExclusionWord(
        const std::wstring& word,
        size_t lineNum = 0,
        std::wstring* outNormalized = nullptr);

    /// Parse and validate User Dictionary text (UTF-8).
    /// Ignores comments (lines starting with ';') and blank lines.
    /// If any line is invalid, aborts and returns exact 1-based errorLine.
    [[nodiscard]] static LexiconImportResult ParseAndValidateUserDictText(
        std::string_view utf8Content,
        const std::vector<std::wstring>* existingEntries = nullptr,
        bool appendMode = false);

    /// Parse and validate Spell Exclusions text (UTF-8).
    /// Ignores comments and blank lines.
    /// If any line is invalid or count exceeds 8, aborts and returns exact 1-based errorLine.
    [[nodiscard]] static LexiconImportResult ParseAndValidateSpellExclusionsText(
        std::string_view utf8Content,
        const std::vector<std::wstring>* existingEntries = nullptr,
        bool appendMode = false);

    /// Format User Dictionary words to canonical UTF-8 text file content.
    [[nodiscard]] static std::string FormatUserDictText(
        const std::vector<std::wstring>& entries);

    /// Format Spell Exclusions to canonical UTF-8 text file content.
    [[nodiscard]] static std::string FormatSpellExclusionsText(
        const std::vector<std::wstring>& entries);
};

}  // namespace NextKey
