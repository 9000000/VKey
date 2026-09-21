// LexiconValidationTest.cpp
// Tests for LexiconValidator (User Dictionary & Spell Exclusions validation)
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>
#include "core/config/LexiconValidation.h"

namespace NextKey {
namespace {

TEST(LexiconValidationTest, ValidUserDictWord) {
    std::wstring normalized;
    auto res = LexiconValidator::ValidateUserDictWord(L"  So\u00e0  ", 1, &normalized);
    EXPECT_TRUE(res.Succeeded());
    EXPECT_EQ(normalized, L"so\u00e0");
}

TEST(LexiconValidationTest, UserDictWordRejectsWhitespaceInside) {
    auto res = LexiconValidator::ValidateUserDictWord(L"so \u00e0", 5);
    EXPECT_FALSE(res.Succeeded());
    EXPECT_EQ(res.errorLine, 5u);
    EXPECT_NE(res.errorMessage.find(L"kho\u1ea3ng tr\u1eafng"), std::wstring::npos);
}

TEST(LexiconValidationTest, UserDictWordRejectsEmpty) {
    auto res = LexiconValidator::ValidateUserDictWord(L"   ", 3);
    EXPECT_FALSE(res.Succeeded());
    EXPECT_EQ(res.errorLine, 3u);
}

TEST(LexiconValidationTest, UserDictWordRejectsTooLong) {
    std::wstring longWord(65, L'a');
    auto res = LexiconValidator::ValidateUserDictWord(longWord, 10);
    EXPECT_FALSE(res.Succeeded());
    EXPECT_EQ(res.errorLine, 10u);
}

TEST(LexiconValidationTest, ValidSpellExclusionWord) {
    std::wstring normalized;
    auto res = LexiconValidator::ValidateSpellExclusionWord(L"  H\u0110  ", 1, &normalized);
    EXPECT_TRUE(res.Succeeded());
    EXPECT_EQ(normalized, L"h\u0111");
}

TEST(LexiconValidationTest, SpellExclusionRejectsSingleChar) {
    auto res = LexiconValidator::ValidateSpellExclusionWord(L"a", 7);
    EXPECT_FALSE(res.Succeeded());
    EXPECT_EQ(res.errorLine, 7u);
    EXPECT_NE(res.errorMessage.find(L"\u00edt nh\u1ea5t 2"), std::wstring::npos);
}

TEST(LexiconValidationTest, ParseUserDictTextWithCommentsAndLineNumbers) {
    // Use '#' (the only valid comment prefix) for all comment lines.
    std::string text =
        "# Header comment\n"
        "so\u00e0\n"
        "\n"
        "# Another comment\n"
        "alo\n"
        "in4\n";

    auto res = LexiconValidator::ParseAndValidateUserDictText(text);
    EXPECT_TRUE(res.validation.Succeeded());
    EXPECT_EQ(res.entries.size(), 3u);
    EXPECT_EQ(res.addedCount, 3u);
}

TEST(LexiconValidationTest, ParseUserDictTextReportsExactErrorLine) {
    std::string text =
        "so\u00e0\n"
        "alo\n"
        "invalid word with space\n"
        "in4\n";

    auto res = LexiconValidator::ParseAndValidateUserDictText(text);
    EXPECT_FALSE(res.validation.Succeeded());
    EXPECT_EQ(res.validation.errorLine, 3u);
}

TEST(LexiconValidationTest, ParseUserDictTextImportAppendDedups) {
    std::vector<std::wstring> existing = {L"alo", L"so\u00e0"};
    std::string text = "so\u00e0\nin4\n";

    auto res = LexiconValidator::ParseAndValidateUserDictText(text, &existing, true);
    EXPECT_TRUE(res.validation.Succeeded());
    EXPECT_EQ(res.entries.size(), 3u); // alo, in4, soà
    EXPECT_EQ(res.addedCount, 1u);     // in4 added
    EXPECT_EQ(res.duplicateCount, 1u); // soà duplicate
}

TEST(LexiconValidationTest, ParseSpellExclusionsMaxCapEnforced) {
    std::string text =
        "aa\nbb\ncc\ndd\nee\nff\ngg\nhh\nii\n"; // 9 entries

    auto res = LexiconValidator::ParseAndValidateSpellExclusionsText(text);
    EXPECT_FALSE(res.validation.Succeeded());
    EXPECT_NE(res.validation.errorMessage.find(L"8 t\u1eeb"), std::wstring::npos);
}

TEST(LexiconValidationTest, FormattingRoundTrip) {
    std::vector<std::wstring> words = {L"alo", L"in4", L"so\u00e0"};
    std::string formatted = LexiconValidator::FormatUserDictText(words);
    EXPECT_NE(formatted.find("# VKey User Dictionary"), std::string::npos);
    EXPECT_NE(formatted.find("so\xc3\xa0\n"), std::string::npos);

    auto parsed = LexiconValidator::ParseAndValidateUserDictText(formatted);
    EXPECT_TRUE(parsed.validation.Succeeded());
    EXPECT_EQ(parsed.entries, words);
}

// Parity test: ';' is NOT a valid comment prefix in user_dictionary.txt.
// C++ validation must reject it so the file never reaches the Rust FFI,
// which would silently treat ';' lines as data entries and fail the dictionary.
TEST(LexiconValidationTest, SemicolonLineIsRejectedByValidation) {
    std::string text =
        "# Hash comment — valid\n"
        "alo\n"
        "; This is NOT a comment — must be rejected\n"
        "kh\xc6\xb0m\n";

    auto parsed = LexiconValidator::ParseAndValidateUserDictText(text);
    EXPECT_FALSE(parsed.validation.Succeeded());
    // The ';' line (line 3) must be the reported error line.
    EXPECT_EQ(parsed.validation.errorLine, 3u);
}

TEST(LexiconValidationTest, ParseUserDictHashCommentIgnored) {
    std::string text =
        "# Hash comment\n"
        "alo\n"
        "# Another comment\n"
        "kh\xc6\xb0m\n"
        "so\xc3\xa0\n";

    auto parsed = LexiconValidator::ParseAndValidateUserDictText(text);
    EXPECT_TRUE(parsed.validation.Succeeded());
    std::vector<std::wstring> expected = {L"alo", L"kh\u01b0m", L"so\u00e0"};
    EXPECT_EQ(parsed.entries, expected);
}

}  // namespace
}  // namespace NextKey
