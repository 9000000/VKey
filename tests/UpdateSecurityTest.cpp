// VKey - UpdateSecurity Unit Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>
#include <algorithm>
#include "app/system/UpdateSecurity.h"

using namespace NextKey;

// ── SEC-002: PowerShell escaping ───────────────────────────────────────────

TEST(EscapePowerShellSingleQuoteTest, NoQuotes) {
    EXPECT_EQ(EscapePowerShellSingleQuote(L"C:\\Users\\Admin\\file.zip"),
              L"C:\\Users\\Admin\\file.zip");
}

TEST(EscapePowerShellSingleQuoteTest, SingleQuoteInUsername) {
    EXPECT_EQ(EscapePowerShellSingleQuote(L"C:\\Users\\O'Brien\\AppData\\file.zip"),
              L"C:\\Users\\O''Brien\\AppData\\file.zip");
}

TEST(EscapePowerShellSingleQuoteTest, MultipleQuotes) {
    EXPECT_EQ(EscapePowerShellSingleQuote(L"it's a 'test'"),
              L"it''s a ''test''");
}

TEST(EscapePowerShellSingleQuoteTest, EmptyString) {
    EXPECT_EQ(EscapePowerShellSingleQuote(L""), L"");
}

TEST(EscapePowerShellSingleQuoteTest, OnlyQuotes) {
    EXPECT_EQ(EscapePowerShellSingleQuote(L"'''"), L"''''''");
}

TEST(EscapePowerShellSingleQuoteTest, ConsecutiveQuotes) {
    EXPECT_EQ(EscapePowerShellSingleQuote(L"a''b"), L"a''''b");
}

// ── SEC-003: URL domain validation (wide) ──────────────────────────────────

TEST(IsAllowedDownloadUrlWideTest, GitHubReleasesUrl) {
    EXPECT_TRUE(IsAllowedDownloadUrl(
        L"https://github.com/phatMT97/VKey/releases/download/v2.0.0/VKey-x64.zip"));
}

TEST(IsAllowedDownloadUrlWideTest, GitHubObjectsUrl) {
    EXPECT_TRUE(IsAllowedDownloadUrl(
        L"https://objects.githubusercontent.com/github-production-release-asset/12345/abc.zip"));
}

TEST(IsAllowedDownloadUrlWideTest, GitHubCodeloadUrl) {
    EXPECT_TRUE(IsAllowedDownloadUrl(
        L"https://codeload.github.com/phatMT97/VKey/zip/refs/tags/v2.0.0"));
}

TEST(IsAllowedDownloadUrlWideTest, RejectsArbitraryDomain) {
    EXPECT_FALSE(IsAllowedDownloadUrl(L"https://evil.com/VKey-x64.zip"));
}

TEST(IsAllowedDownloadUrlWideTest, RejectsHttp) {
    EXPECT_FALSE(IsAllowedDownloadUrl(L"http://github.com/foo/bar.zip"));
}

TEST(IsAllowedDownloadUrlWideTest, RejectsSimilarDomain) {
    EXPECT_FALSE(IsAllowedDownloadUrl(L"https://github.com.evil.com/foo.zip"));
}

TEST(IsAllowedDownloadUrlWideTest, RejectsEmpty) {
    EXPECT_FALSE(IsAllowedDownloadUrl(std::wstring{}));
}

TEST(IsAllowedDownloadUrlWideTest, CaseInsensitive) {
    EXPECT_TRUE(IsAllowedDownloadUrl(
        L"HTTPS://GITHUB.COM/phatMT97/VKey/releases/download/v2.0.0/VKey-x64.zip"));
}

// ── SEC-003: URL domain validation (narrow) ────────────────────────────────

TEST(IsAllowedDownloadUrlNarrowTest, GitHubReleasesUrl) {
    EXPECT_TRUE(IsAllowedDownloadUrl(
        std::string("https://github.com/phatMT97/VKey/releases/download/v2.0.0/VKey-x64.zip")));
}

TEST(IsAllowedDownloadUrlNarrowTest, RejectsArbitraryDomain) {
    EXPECT_FALSE(IsAllowedDownloadUrl(std::string("https://evil.com/VKey-x64.zip")));
}

TEST(IsAllowedDownloadUrlNarrowTest, RejectsSubdomain) {
    EXPECT_FALSE(IsAllowedDownloadUrl(std::string("https://github.com.evil.com/foo.zip")));
}

TEST(IsAllowedDownloadUrlNarrowTest, CaseInsensitive) {
    EXPECT_TRUE(IsAllowedDownloadUrl(
        std::string("HTTPS://GITHUB.COM/phatMT97/VKey/releases/download/v2.0.0/VKey-x64.zip")));
}

// ── SEC-001: ParseSha256File (cross-platform string parsing) ─────────────

TEST(ParseSha256FileTest, StandardFormat) {
    // GNU coreutils sha256sum: "<hash>  <filename>"
    std::string content = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890  NextKey-x64.zip\n";
    EXPECT_EQ(ParseSha256File(content),
              "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
}

TEST(ParseSha256FileTest, HashOnly) {
    std::string content = "ABCDEF1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF1234567890\n";
    EXPECT_EQ(ParseSha256File(content),
              "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
}

TEST(ParseSha256FileTest, EmptyContent) {
    EXPECT_EQ(ParseSha256File(""), "");
}

TEST(ParseSha256FileTest, TooShortHash) {
    EXPECT_EQ(ParseSha256File("abcdef123"), "");
}

TEST(ParseSha256FileTest, TooLongHash) {
    EXPECT_EQ(ParseSha256File("abcdef1234567890abcdef1234567890abcdef1234567890abcdef12345678901"), "");
}

TEST(ParseSha256FileTest, InvalidHexChar) {
    EXPECT_EQ(ParseSha256File("abcdef1234567890abcdef1234567890abcdef1234567890abcdefXX34567890"), "");
}

TEST(ParseSha256FileTest, WindowsLineEnding) {
    std::string content = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890  NextKey-x64.zip\r\n";
    EXPECT_EQ(ParseSha256File(content),
              "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
}

TEST(ParseSha256FileTest, UppercaseNormalizedToLower) {
    std::string content = "ABCDEF0000000000ABCDEF0000000000ABCDEF0000000000ABCDEF0000000000";
    EXPECT_EQ(ParseSha256File(content),
              "abcdef0000000000abcdef0000000000abcdef0000000000abcdef0000000000");
}

#ifdef _WIN32
namespace {
std::vector<std::uint8_t> UpdateTestHex(std::string_view value) {
    std::vector<std::uint8_t> bytes;
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < value.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>((nibble(value[i]) << 4) | nibble(value[i + 1])));
    }
    return bytes;
}
}  // namespace
#endif

TEST(UpdatePackageSignatureTest, RejectsMissingTruncatedOrExtraSignatureBytes) {
    EXPECT_FALSE(UpdatePackageSignature::Parse(std::span<const std::uint8_t>{}));
    std::array<std::uint8_t, 73> bytes{};
    EXPECT_FALSE(UpdatePackageSignature::Parse(bytes));
    EXPECT_FALSE(UpdatePackageSignature::Parse(std::span(bytes).first(71)));
    // Unknown editions cannot be reinterpreted as either product.
    EXPECT_FALSE(UpdatePackageSignature::Parse(std::span(bytes).first(72)));
}

TEST(UpdatePackageSignatureTest, RejectsRollbackWrongVersionAndCrossEditionReplay) {
    UpdatePackageSignature::Parsed parsed;
    parsed.version = 0x040302;
    parsed.edition = UpdatePackageSignature::Edition::Sciter;
    EXPECT_TRUE(UpdatePackageSignature::Accepts(parsed, 0x040301, 0x040302, UpdatePackageSignature::Edition::Sciter));
    EXPECT_FALSE(UpdatePackageSignature::Accepts(parsed, 0x040302, 0x040302, UpdatePackageSignature::Edition::Sciter));
    EXPECT_FALSE(UpdatePackageSignature::Accepts(parsed, 0x040303, 0, UpdatePackageSignature::Edition::Sciter));
    EXPECT_FALSE(UpdatePackageSignature::Accepts(parsed, 0x040301, 0x040303, UpdatePackageSignature::Edition::Sciter));
    EXPECT_FALSE(UpdatePackageSignature::Accepts(parsed, 0x040301, 0x040302, UpdatePackageSignature::Edition::Classic));
}

#ifdef _WIN32
TEST(UpdatePackageSignatureTest, CngVerifiesIndependentSignatureAndRejectsEveryBoundFieldMutation) {
    const auto raw = UpdateTestHex("0203040001000000e5226a101cd0c07c1f306aef9e605a656ce83dd1c3efe598f4ecdbb2fb5acf011726c1c5cb89f934ab7b5d2a1e5f38f16709eb999b55824467bd2a036f2dd365");
    const auto hashBytes = UpdateTestHex("80ff14e46790b9c476fe9eaa1b026fd4d29b5b72dd3b7c557170565d9b97e78e");
    std::array<std::uint8_t, 32> digest{};
    std::copy(hashBytes.begin(), hashBytes.end(), digest.begin());
    const std::string key = "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
    const auto parsed = UpdatePackageSignature::Parse(raw);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(VerifyUpdateSignature(digest, *parsed, key));
    auto changedDigest = digest;
    changedDigest[0] ^= 1;
    EXPECT_FALSE(VerifyUpdateSignature(changedDigest, *parsed, key));
    auto changed = *parsed;
    ++changed.version;
    EXPECT_FALSE(VerifyUpdateSignature(digest, changed, key));
    changed = *parsed;
    changed.edition = UpdatePackageSignature::Edition::Classic;
    EXPECT_FALSE(VerifyUpdateSignature(digest, changed, key));
    changed = *parsed;
    changed.signature[0] ^= 1;
    EXPECT_FALSE(VerifyUpdateSignature(digest, changed, key));
    EXPECT_FALSE(VerifyUpdateSignature(digest, *parsed, std::string(128, '0')));
    EXPECT_FALSE(VerifyUpdateSignature(digest, *parsed, "not a public key"));
}
#endif
