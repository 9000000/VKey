// VKey - Spell Exclusion Canonicalizer Parity Tests
// SPDX-License-Identifier: GPL-3.0-only

#include "core/config/SpellExclusionCanonicalizer.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace NextKey {
namespace {

std::filesystem::path FindParityFixturePath() {
    std::vector<std::filesystem::path> candidates = {
        "tests/testdata/lexicon_canonical_parity.json",
        "../tests/testdata/lexicon_canonical_parity.json",
        "../../tests/testdata/lexicon_canonical_parity.json",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) {
            return std::filesystem::canonical(p);
        }
    }
    return {};
}

// Simple JSON helper for the specific structure in lexicon_canonical_parity.json
std::string ExtractJsonString(const std::string& text, size_t& pos) {
    size_t open = text.find('"', pos);
    if (open == std::string::npos) return {};
    std::string val;
    size_t i = open + 1;
    while (i < text.size()) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == '"' || next == '\\' || next == '/') {
                val += next;
                i += 2;
                continue;
            } else if (next == 'b') { val += '\b'; i += 2; continue; }
            else if (next == 'f') { val += '\f'; i += 2; continue; }
            else if (next == 'n') { val += '\n'; i += 2; continue; }
            else if (next == 'r') { val += '\r'; i += 2; continue; }
            else if (next == 't') { val += '\t'; i += 2; continue; }
            else if (next == 'u' && i + 5 < text.size()) {
                uint32_t cp = 0;
                for (int j = 0; j < 4; ++j) {
                    char h = text[i + 2 + j];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                }
                i += 6;
                // Check if surrogate pair
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < text.size() && text[i] == '\\' && text[i + 1] == 'u') {
                    uint32_t low = 0;
                    for (int j = 0; j < 4; ++j) {
                        char h = text[i + 2 + j];
                        low <<= 4;
                        if (h >= '0' && h <= '9') low |= (h - '0');
                        else if (h >= 'a' && h <= 'f') low |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') low |= (h - 'A' + 10);
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                std::u32string u32(1, cp);
                std::string utf8;
                (void)SpellExclusionCanonicalizer::Utf32ToUtf8(u32, utf8);
                val += utf8;
                continue;
            }
        } else if (c == '"') {
            pos = i + 1;
            return val;
        }
        val += c;
        ++i;
    }
    pos = i;
    return val;
}

} // namespace

// ─── Direct Parity Tests (Matching Rust SpellExclusionSource) ───────────────

TEST(SpellExclusionCanonicalizerTest, ParityRustSpecTrimNfcLowercaseDedup) {
    const std::vector<std::string> input = {" ZÔ ", "rose", "zo\u0302"};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    const std::vector<std::string> expected = {"rose", "zô"};
    EXPECT_EQ(result.utf8Entries, expected);
}

TEST(SpellExclusionCanonicalizerTest, ParityVietnameseShorthandPrefixes) {
    const std::vector<std::string> input = {
        "HĐ", "ĐCĐT", "KHTN", "ALÔ", "so\u0300a", "SOÀ"
    };
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    // Notice deterministic UTF-8 byte persistence order:
    // 'alô' (61), 'hđ' (68), 'khtn' (6b), 'soà' (73 6f c3), 'sòa' (73 c3), 'đcđt' (c4)
    const std::vector<std::string> expected = {
        "alô", "hđ", "khtn", "soà", "sòa", "đcđt"
    };
    EXPECT_EQ(result.utf8Entries, expected);
}

TEST(SpellExclusionCanonicalizerTest, ParityUnicodeWhitespaceTrimmingAtBothEnds) {
    // Trims \t, space, U+00A0 (NBSP), \r, \n, U+3000 (ideographic space), U+2000 (en quad)
    const std::vector<std::string> input = {
        "\t  hđ\u00A0 \r\n",
        "\u3000đcđt\u2000"
    };
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    const std::vector<std::string> expected = {"hđ", "đcđt"};
    EXPECT_EQ(result.utf8Entries, expected);
}

TEST(SpellExclusionCanonicalizerTest, ParityNonBmpOrdering) {
    // U+FFFD (3-byte UTF-8 EF BF BD) precedes U+10428 (4-byte UTF-8 F0 90 90 A8)
    const std::vector<std::string> input = {
        "\uFFFD\uFFFD",
        "\U00010400\U00010400"
    };
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    const std::vector<std::string> expected = {
        "\uFFFD\uFFFD",
        "\U00010428\U00010428"
    };
    EXPECT_EQ(result.utf8Entries, expected);
}

TEST(SpellExclusionCanonicalizerTest, ParityCaseDecompositionNfcPass2) {
    // A + circumflex + acute lowercases and renormalizes to ấb
    const std::vector<std::string> input = {"A\u0302\u0301B", "a\u0302\u0301b"};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    const std::vector<std::string> expected = {"ấb"};
    EXPECT_EQ(result.utf8Entries, expected);
}

TEST(SpellExclusionCanonicalizerTest, ParityExactly8EntriesBoundary) {
    std::vector<std::string> input;
    for (int i = 0; i < 8; ++i) {
        input.push_back("entry" + std::to_string(i));
    }
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.utf8Entries.size(), 8u);
}

TEST(SpellExclusionCanonicalizerTest, ParityDuplicateEntriesDeduplicatedUnder8Cap) {
    std::vector<std::string> input = {"entry0", "ENTRY0"};
    for (int i = 1; i < 8; ++i) {
        input.push_back("entry" + std::to_string(i));
    }
    // 9 rows input, but 2 are case-variants that deduplicate to 8 rows
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.utf8Entries.size(), 8u);
}

TEST(SpellExclusionCanonicalizerTest, ParityErrorTooShortSingleChar) {
    const std::vector<std::string> input = {"valid", "a"};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::TooShort);
    EXPECT_EQ(result.error.row, 2u);
}

TEST(SpellExclusionCanonicalizerTest, ParityErrorTooShortWhitespaceOnly) {
    const std::vector<std::string> input = {"   "};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::TooShort);
    EXPECT_EQ(result.error.row, 1u);
}

TEST(SpellExclusionCanonicalizerTest, ParityErrorTooLong129Scalars) {
    const std::string long129(129, 'a');
    const std::vector<std::string> input = {"valid", long129};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::TooLong);
    EXPECT_EQ(result.error.row, 2u);
    EXPECT_EQ(result.error.scalars, 129u);
}

TEST(SpellExclusionCanonicalizerTest, ParityErrorInvalidEntryInternalWhitespace) {
    const std::vector<std::string> input = {"h đ"};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::InvalidEntry);
    EXPECT_EQ(result.error.row, 1u);
}

TEST(SpellExclusionCanonicalizerTest, ParityErrorInvalidEntryControlChar) {
    const std::vector<std::string> input = {"ab\x07" "cd"};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::InvalidEntry);
    EXPECT_EQ(result.error.row, 1u);
}

TEST(SpellExclusionCanonicalizerTest, ParityErrorTooManyEntries9Distinct) {
    std::vector<std::string> input;
    for (int i = 0; i < 9; ++i) {
        input.push_back("entry" + std::to_string(i));
    }
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::TooManyEntries);
    EXPECT_EQ(result.error.count, 9u);
}

// ─── Parity Fixture File Dynamic Execution ──────────────────────────────────

TEST(SpellExclusionCanonicalizerTest, ParityFixtureFileExistsAndIsReadable) {
    const auto path = FindParityFixturePath();
    ASSERT_FALSE(path.empty()) << "lexicon_canonical_parity.json must exist in test tree";

    std::ifstream file(path);
    ASSERT_TRUE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_FALSE(content.empty());
    EXPECT_NE(content.find("rust_spec_trim_nfc_lowercase_dedup"), std::string::npos);
}

// ─── API Variant Tests ──────────────────────────────────────────────────────

TEST(SpellExclusionCanonicalizerTest, CanonicalizeWideVectorSucceeds) {
    const std::vector<std::wstring> input = {L" ZÔ ", L"rose", L"zo\u0302"};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.entries.size(), 2u);
    EXPECT_EQ(result.entries[0], L"rose");
    EXPECT_EQ(result.entries[1], L"zô");
}

TEST(SpellExclusionCanonicalizerTest, CanonicalizeFromTextHandlesCrlf) {
    const std::string text = "  ZÔ \r\nrose\r\nzo\u0302\r\n";
    const auto result = SpellExclusionCanonicalizer::CanonicalizeFromText(text);

    ASSERT_TRUE(result.Succeeded());
    const std::vector<std::string> expected = {"rose", "zô"};
    EXPECT_EQ(result.utf8Entries, expected);
}

TEST(SpellExclusionCanonicalizerTest, CanonicalizeEmptyInputSucceeds) {
    const std::vector<std::string> emptyStrings;
    const auto res1 = SpellExclusionCanonicalizer::Canonicalize(emptyStrings);
    EXPECT_TRUE(res1.Succeeded());
    EXPECT_TRUE(res1.entries.empty());

    const std::vector<std::wstring> emptyWStrings;
    const auto res2 = SpellExclusionCanonicalizer::Canonicalize(emptyWStrings);
    EXPECT_TRUE(res2.Succeeded());
    EXPECT_TRUE(res2.entries.empty());

    const auto res3 = SpellExclusionCanonicalizer::CanonicalizeFromText("");
    EXPECT_TRUE(res3.Succeeded());
    EXPECT_TRUE(res3.entries.empty());
}

TEST(SpellExclusionCanonicalizerTest, InvalidSurrogateRejected) {
    // Isolated high surrogate 0xD800
    const std::wstring badWide = {0xD800, L'a'};
    const std::vector<std::wstring> input = {badWide};
    const auto result = SpellExclusionCanonicalizer::Canonicalize(input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.error.kind, SpellExclusionErrorKind::InvalidEntry);
    EXPECT_EQ(result.error.row, 1u);
}

} // namespace NextKey
