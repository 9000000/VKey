// VKey - Lexicon Validation and Parsing Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "LexiconValidation.h"
#include "SpellExclusionCanonicalizer.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace NextKey {

namespace {

std::wstring FormatLinePrefix(size_t lineNum) {
    if (lineNum > 0) {
        return L"D\u00f2ng " + std::to_wstring(lineNum) + L": ";
    }
    return L"";
}

bool CanonicalizeUserWord(
    const std::wstring& input,
    size_t lineNum,
    std::wstring& outUtf16,
    LexiconValidationResult& outResult) {
    outResult = {};

    std::u32string u32;
    if (!SpellExclusionCanonicalizer::Utf16ToUtf32(input, u32)) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"T\u1eeb ch\u1ee9a m\u00e3 UTF-16 surrogate kh\u00f4ng h\u1ee3p l\u1ec7.";
        return false;
    }

    u32 = SpellExclusionCanonicalizer::TrimWhitespace(u32);
    if (u32.empty()) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"T\u1eeb kh\u00f4ng \u0111\u01b0\u1ee3c \u0111\u1ec3 tr\u1ed1ng.";
        return false;
    }

    for (uint32_t scalar : u32) {
        if (SpellExclusionCanonicalizer::IsUnicodeWhitespace(scalar)) {
            outResult.ok = false;
            outResult.errorLine = lineNum;
            outResult.errorMessage = FormatLinePrefix(lineNum) +
                L"T\u1eeb kh\u00f4ng \u0111\u01b0\u1ee3c ch\u1ee9a kho\u1ea3ng tr\u1eafng.";
            return false;
        }
        if (SpellExclusionCanonicalizer::IsUnicodeControl(scalar)) {
            outResult.ok = false;
            outResult.errorLine = lineNum;
            outResult.errorMessage = FormatLinePrefix(lineNum) +
                L"T\u1eeb ch\u1ee9a k\u00fd t\u1ef1 \u0111i\u1ec1u khi\u1ec3n kh\u00f4ng h\u1ee3p l\u1ec7.";
            return false;
        }
        if (scalar == 0xFFFD) {
            outResult.ok = false;
            outResult.errorLine = lineNum;
            outResult.errorMessage = FormatLinePrefix(lineNum) +
                L"T\u1eeb ch\u1ee9a k\u00fd t\u1ef1 Unicode kh\u00f4ng h\u1ee3p l\u1ec7 (U+FFFD).";
            return false;
        }
    }

    if (u32.size() > kMaxUserDictWordScalars) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"\u0110\u1ed9 d\u00e0i t\u1eeb v\u01b0\u1ee3t qu\u00e1 " +
            std::to_wstring(kMaxUserDictWordScalars) +
            L" k\u00fd t\u1ef1 (hi\u1ec7n c\u00f3 " + std::to_wstring(u32.size()) + L" k\u00fd t\u1ef1).";
        return false;
    }

    std::wstring utf16;
    if (!SpellExclusionCanonicalizer::Utf32ToUtf16(u32, utf16)) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"Kh\u00f4ng th\u1ec3 chuy\u1ec3n \u0111\u1ed5i sang UTF-16.";
        return false;
    }

    std::vector<uint16_t> u16Units(utf16.begin(), utf16.end());
    std::vector<uint16_t> nfc1;
    if (!SpellExclusionCanonicalizer::NormalizeNfcUtf16(u16Units, nfc1)) {
        nfc1 = u16Units;
    }

    std::vector<uint16_t> lower;
    if (!SpellExclusionCanonicalizer::ToLowercaseUtf16(nfc1, lower)) {
        lower = nfc1;
    }

    std::vector<uint16_t> nfc2;
    if (!SpellExclusionCanonicalizer::NormalizeNfcUtf16(lower, nfc2)) {
        nfc2 = lower;
    }

    std::wstring finalUtf16(nfc2.begin(), nfc2.end());
    std::u32string finalU32;
    if (!SpellExclusionCanonicalizer::Utf16ToUtf32(finalUtf16, finalU32)) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"L\u1ed7i chu\u1ea9n ho\u00e1 Unicode.";
        return false;
    }

    if (finalU32.empty()) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"T\u1eeb kh\u00f4ng \u0111\u01b0\u1ee3c \u0111\u1ec3 tr\u1ed1ng.";
        return false;
    }

    if (finalU32.size() > kMaxUserDictWordScalars) {
        outResult.ok = false;
        outResult.errorLine = lineNum;
        outResult.errorMessage = FormatLinePrefix(lineNum) +
            L"\u0110\u1ed9 d\u00e0i t\u1eeb v\u01b0\u1ee3t qu\u00e1 " +
            std::to_wstring(kMaxUserDictWordScalars) +
            L" k\u00fd t\u1ef1 sau chu\u1ea9n ho\u00e1.";
        return false;
    }

    outUtf16 = std::move(finalUtf16);
    return true;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() || (!text.empty() && text.back() == '\n')) {
        lines.push_back(std::move(current));
    }
    return lines;
}

} // namespace

LexiconValidationResult LexiconValidator::ValidateUserDictWord(
    const std::wstring& word,
    size_t lineNum,
    std::wstring* outNormalized) {
    LexiconValidationResult res;
    std::wstring normalized;
    if (CanonicalizeUserWord(word, lineNum, normalized, res)) {
        if (outNormalized) *outNormalized = std::move(normalized);
    }
    return res;
}

LexiconValidationResult LexiconValidator::ValidateSpellExclusionWord(
    const std::wstring& word,
    size_t lineNum,
    std::wstring* outNormalized) {
    LexiconValidationResult res;
    std::u32string u32;
    if (!SpellExclusionCanonicalizer::Utf16ToUtf32(word, u32)) {
        res.ok = false;
        res.errorLine = lineNum;
        res.errorMessage = FormatLinePrefix(lineNum) +
            L"T\u1eeb ch\u1ee9a m\u00e3 UTF-16 surrogate kh\u00f4ng h\u1ee3p l\u1ec7.";
        return res;
    }

    u32 = SpellExclusionCanonicalizer::TrimWhitespace(u32);
    std::string utf8;
    std::wstring utf16;
    SpellExclusionError err;
    if (!SpellExclusionCanonicalizer::NormalizeAndValidate(u32, lineNum, utf8, utf16, err)) {
        res.ok = false;
        res.errorLine = lineNum;
        if (err.kind == SpellExclusionErrorKind::TooShort) {
            res.errorMessage = FormatLinePrefix(lineNum) +
                L"Ngo\u1ea1i l\u1ec7 ch\u00ednh t\u1ea3 ph\u1ea3i c\u00f3 \u00edt nh\u1ea5t 2 k\u00fd t\u1ef1.";
        } else if (err.kind == SpellExclusionErrorKind::TooLong) {
            res.errorMessage = FormatLinePrefix(lineNum) +
                L"Ngo\u1ea1i l\u1ec7 ch\u00ednh t\u1ea3 v\u01b0\u1ee3t qu\u00e1 128 k\u00fd t\u1ef1.";
        } else {
            res.errorMessage = FormatLinePrefix(lineNum) +
                (err.message.empty() ? L"Ngo\u1ea1i l\u1ec7 ch\u00ednh t\u1ea3 ch\u1ee9a k\u00fd t\u1ef1 kh\u00f4ng h\u1ee3p l\u1ec7." : err.message);
        }
        return res;
    }

    if (outNormalized) *outNormalized = std::move(utf16);
    return res;
}

LexiconImportResult LexiconValidator::ParseAndValidateUserDictText(
    std::string_view utf8Content,
    const std::vector<std::wstring>* existingEntries,
    bool appendMode) {
    LexiconImportResult result;
    std::vector<std::string> rawLines = SplitLines(utf8Content);

    std::vector<std::wstring> parsedWords;
    for (size_t i = 0; i < rawLines.size(); ++i) {
        const size_t lineNum = i + 1;
        std::u32string u32;
        if (!SpellExclusionCanonicalizer::Utf8ToUtf32(rawLines[i], u32)) {
            result.validation.ok = false;
            result.validation.errorLine = lineNum;
            result.validation.errorMessage = FormatLinePrefix(lineNum) +
                L"D\u00f2ng kh\u00f4ng ph\u1ea3i UTF-8 h\u1ee3p l\u1ec7.";
            return result;
        }

        u32 = SpellExclusionCanonicalizer::TrimWhitespace(u32);
        if (u32.empty() || u32[0] == U';') {
            continue; // Ignore blank lines and comments
        }

        std::wstring lineUtf16;
        if (!SpellExclusionCanonicalizer::Utf32ToUtf16(u32, lineUtf16)) {
            result.validation.ok = false;
            result.validation.errorLine = lineNum;
            result.validation.errorMessage = FormatLinePrefix(lineNum) +
                L"L\u1ed7i gi\u1ea3i m\u00e3 UTF-16.";
            return result;
        }

        std::wstring normalized;
        auto val = ValidateUserDictWord(lineUtf16, lineNum, &normalized);
        if (!val.Succeeded()) {
            result.validation = val;
            return result;
        }
        parsedWords.push_back(std::move(normalized));
    }

    std::vector<std::wstring> combined;
    std::unordered_set<std::wstring> seen;

    if (appendMode && existingEntries) {
        for (const auto& w : *existingEntries) {
            if (seen.insert(w).second) {
                combined.push_back(w);
            }
        }
    }

    size_t added = 0;
    size_t dupes = 0;
    for (auto& w : parsedWords) {
        if (seen.insert(w).second) {
            combined.push_back(std::move(w));
            ++added;
        } else {
            ++dupes;
        }
    }

    if (combined.size() > kMaxUserDictEntries) {
        result.validation.ok = false;
        result.validation.errorLine = 0;
        result.validation.errorMessage =
            L"Danh s\u00e1ch t\u1eeb c\u00e1 nh\u00e2n v\u01b0\u1ee3t qu\u00e1 gi\u1edbi h\u1ea1n t\u1ed1i \u0111a (" +
            std::to_wstring(kMaxUserDictEntries) + L" t\u1eeb). Hi\u1ec7n c\u00f3 " +
            std::to_wstring(combined.size()) + L" t\u1eeb.";
        return result;
    }

    // Check total UTF-16 units
    size_t totalUtf16 = 0;
    for (const auto& w : combined) {
        totalUtf16 += w.size();
    }
    if (totalUtf16 > kMaxUserDictUtf16Units) {
        result.validation.ok = false;
        result.validation.errorLine = 0;
        result.validation.errorMessage =
            L"T\u1ed5ng k\u00edch th\u01b0\u1edbc danh s\u00e1ch t\u1eeb c\u00e1 nh\u00e2n v\u01b0\u1ee3t qu\u00e1 gi\u1edbi h\u1ea1n (" +
            std::to_wstring(kMaxUserDictUtf16Units) + L" \u0111\u01a1n v\u1ecb UTF-16).";
        return result;
    }

    // Persistence order: sort alphabetically by UTF-8 bytes to match Rust
    std::vector<std::pair<std::string, std::wstring>> pairs;
    pairs.reserve(combined.size());
    for (auto& w : combined) {
        std::u32string u32;
        (void)SpellExclusionCanonicalizer::Utf16ToUtf32(w, u32);
        std::string u8;
        (void)SpellExclusionCanonicalizer::Utf32ToUtf8(u32, u8);
        pairs.emplace_back(std::move(u8), std::move(w));
    }
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    result.entries.clear();
    result.entries.reserve(pairs.size());
    for (auto& p : pairs) {
        result.entries.push_back(std::move(p.second));
    }
    result.addedCount = added;
    result.duplicateCount = dupes;
    return result;
}

LexiconImportResult LexiconValidator::ParseAndValidateSpellExclusionsText(
    std::string_view utf8Content,
    const std::vector<std::wstring>* existingEntries,
    bool appendMode) {
    LexiconImportResult result;
    std::vector<std::string> rawLines = SplitLines(utf8Content);

    std::vector<std::wstring> parsedExclusions;
    for (size_t i = 0; i < rawLines.size(); ++i) {
        const size_t lineNum = i + 1;
        std::u32string u32;
        if (!SpellExclusionCanonicalizer::Utf8ToUtf32(rawLines[i], u32)) {
            result.validation.ok = false;
            result.validation.errorLine = lineNum;
            result.validation.errorMessage = FormatLinePrefix(lineNum) +
                L"D\u00f2ng kh\u00f4ng ph\u1ea3i UTF-8 h\u1ee3p l\u1ec7.";
            return result;
        }

        u32 = SpellExclusionCanonicalizer::TrimWhitespace(u32);
        if (u32.empty() || u32[0] == U';') {
            continue;
        }

        std::wstring lineUtf16;
        if (!SpellExclusionCanonicalizer::Utf32ToUtf16(u32, lineUtf16)) {
            result.validation.ok = false;
            result.validation.errorLine = lineNum;
            result.validation.errorMessage = FormatLinePrefix(lineNum) +
                L"L\u1ed7i gi\u1ea3i m\u00e3 UTF-16.";
            return result;
        }

        std::wstring normalized;
        auto val = ValidateSpellExclusionWord(lineUtf16, lineNum, &normalized);
        if (!val.Succeeded()) {
            result.validation = val;
            return result;
        }
        parsedExclusions.push_back(std::move(normalized));
    }

    std::vector<std::wstring> candidateList;
    if (appendMode && existingEntries) {
        candidateList = *existingEntries;
    }
    size_t added = 0;
    size_t dupes = 0;
    for (const auto& w : parsedExclusions) {
        if (std::find(candidateList.begin(), candidateList.end(), w) == candidateList.end()) {
            candidateList.push_back(w);
            ++added;
        } else {
            ++dupes;
        }
    }

    auto canonical = SpellExclusionCanonicalizer::Canonicalize(candidateList);
    if (!canonical.Succeeded()) {
        result.validation.ok = false;
        result.validation.errorLine = 0;
        if (canonical.error.kind == SpellExclusionErrorKind::TooManyEntries) {
            result.validation.errorMessage =
                L"Danh s\u00e1ch ngo\u1ea1i l\u1ec7 ch\u00ednh t\u1ea3 v\u01b0\u1ee3t qu\u00e1 gi\u1edbi h\u1ea1n t\u1ed1i \u0111a (" +
                std::to_wstring(kMaxSpellExclusionRows) + L" t\u1eeb). Hi\u1ec7n c\u00f3 " +
                std::to_wstring(canonical.error.count) + L" t\u1eeb.";
        } else {
            result.validation.errorMessage = canonical.error.message;
        }
        return result;
    }

    result.entries = std::move(canonical.entries);
    result.addedCount = added;
    result.duplicateCount = dupes;
    return result;
}

std::string LexiconValidator::FormatUserDictText(
    const std::vector<std::wstring>& entries) {
    std::ostringstream ss;
    ss << "; VKey User Dictionary\n; One word per line\n";
    for (const auto& w : entries) {
        std::u32string u32;
        (void)SpellExclusionCanonicalizer::Utf16ToUtf32(w, u32);
        std::string u8;
        (void)SpellExclusionCanonicalizer::Utf32ToUtf8(u32, u8);
        ss << u8 << "\n";
    }
    return ss.str();
}

std::string LexiconValidator::FormatSpellExclusionsText(
    const std::vector<std::wstring>& entries) {
    std::ostringstream ss;
    ss << "; VKey Spell Exclusions\n; One exclusion prefix per line (max 8)\n";
    for (const auto& w : entries) {
        std::u32string u32;
        (void)SpellExclusionCanonicalizer::Utf16ToUtf32(w, u32);
        std::string u8;
        (void)SpellExclusionCanonicalizer::Utf32ToUtf8(u32, u8);
        ss << u8 << "\n";
    }
    return ss.str();
}

}  // namespace NextKey
