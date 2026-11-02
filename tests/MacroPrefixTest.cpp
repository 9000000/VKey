// VKey - Multi-word Macro Prefix Matcher Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>
#include <clocale>
#include <cstdio>
#include <vector>
#include "core/MacroPrefix.h"

namespace NextKey {
namespace {

TEST(MacroPrefixTest, EmptySetReturnsFalse) {
    std::unordered_set<std::wstring> keys;
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc ", keys));
}

TEST(MacroPrefixTest, EmptyCandidateReturnsFalse) {
    std::unordered_set<std::wstring> keys{L"oc om bok"};
    EXPECT_FALSE(IsSpaceMacroPrefix(L"", keys));
}

TEST(MacroPrefixTest, LowercaseKeyFlexiblePrefixHits) {
    std::unordered_set<std::wstring> keys{L"oc om bok"};
    EXPECT_TRUE(IsSpaceMacroPrefix(L"oc ", keys));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"oc om ", keys));
}

TEST(MacroPrefixTest, LowercaseKeyAcceptsTypedUppercase) {
    std::unordered_set<std::wstring> keys{L"oc om bok"};
    EXPECT_TRUE(IsSpaceMacroPrefix(L"OC ", keys));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"Oc Om ", keys));
}

TEST(MacroPrefixTest, UppercaseKeyRejectsTypedLowercase) {
    std::unordered_set<std::wstring> keys{L"Oc Om Bok"};
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc ", keys));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc om ", keys));
}

TEST(MacroPrefixTest, UppercaseKeyAcceptsExactCase) {
    std::unordered_set<std::wstring> keys{L"Oc Om Bok"};
    EXPECT_TRUE(IsSpaceMacroPrefix(L"Oc ", keys));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"Oc Om ", keys));
}

TEST(MacroPrefixTest, NonPrefixReturnsFalse) {
    std::unordered_set<std::wstring> keys{L"hi there"};
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc ", keys));
}

TEST(MacroPrefixTest, MidCharMismatchReturnsFalse) {
    std::unordered_set<std::wstring> keys{L"oc om bok"};
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc omm ", keys));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc op ", keys));
}

TEST(MacroPrefixTest, CandidateLongerThanKeyReturnsFalse) {
    std::unordered_set<std::wstring> keys{L"oc"};  // no space anyway, but tests length guard
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc om", keys));
}

TEST(MacroPrefixTest, SelfMatchesAsPrefix) {
    // The full key is trivially a prefix of itself. Caller (HookEngine) gates
    // this separately via exact-match in TryExpandMacro before the save path.
    std::unordered_set<std::wstring> keys{L"oc om bok"};
    EXPECT_TRUE(IsSpaceMacroPrefix(L"oc om bok", keys));
}

TEST(MacroPrefixTest, MultipleKeysFindsAnyMatch) {
    std::unordered_set<std::wstring> keys{L"hi there", L"oc om bok", L"good night"};
    EXPECT_TRUE(IsSpaceMacroPrefix(L"hi ", keys));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"oc ", keys));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"good ", keys));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"bye ", keys));
}

TEST(MacroPrefixTest, MixedCaseKeysRespectedIndependently) {
    std::unordered_set<std::wstring> keys{L"hi there", L"Oc Om Bok"};
    // Lowercase key: flexible
    EXPECT_TRUE(IsSpaceMacroPrefix(L"HI ", keys));
    // Uppercase key: strict — lowercase typing does not match it, but if no
    // other key matches, overall result is false.
    EXPECT_FALSE(IsSpaceMacroPrefix(L"oc ", keys));
    // Typed matches the strict key exactly
    EXPECT_TRUE(IsSpaceMacroPrefix(L"Oc ", keys));
}

}  // namespace
}  // namespace NextKey

namespace NextKey {
namespace {

TEST(MacroPrefixIndexTest, MatchesOriginalAcrossCaseUnicodeAndTableSizes) {
    std::vector<std::unordered_set<std::wstring>> tables{
        {}, {L""}, {L"a b"}, {L"Ab C"},
        {L"", L"a b", L"aB c", L"ab c", L"ab C", L"é a", L"É a", L"🦀 a"},
        {},
    };
    for (std::size_t i = 0; i < 512; ++i) {
        char number[8];
        std::snprintf(number, sizeof(number), "%04zu", i);
        const std::string ascii(number);
        tables.back().insert(L"macro" + std::wstring(ascii.begin(), ascii.end()) + L" more");
    }
    const std::vector<std::wstring> alphabet{L"a", L"A", L"b", L"B", L" ", L".", L"é", L"É", L"🦀"};
    std::size_t checked = 0;
    for (const auto& keys : tables) {
        const MacroPrefixIndex index(keys);
        const auto check = [&](const std::wstring& candidate) {
            ASSERT_EQ(IsSpaceMacroPrefix(candidate, index), IsSpaceMacroPrefix(candidate, keys))
                << "candidate code units=" << candidate.size() << ", table=" << keys.size();
            ++checked;
        };
        std::size_t combinations = 1;
        for (std::size_t length = 0; length <= 4; ++length) {
            for (std::size_t number = 0; number < combinations; ++number) {
                std::wstring candidate;
                std::size_t remaining = number;
                for (std::size_t at = 0; at < length; ++at) {
                    candidate += alphabet[remaining % alphabet.size()];
                    remaining /= alphabet.size();
                }
                check(candidate);
            }
            combinations *= alphabet.size();
        }
        for (const auto& key : keys) {
            for (std::size_t length = 0; length <= key.size(); ++length) check(key.substr(0, length));
            auto upper = key;
            for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
            check(upper);
        }
    }
    EXPECT_EQ(checked, 52'531u + (sizeof(wchar_t) == 2 ? 1u : 0u));
    RecordProperty("differential_queries", static_cast<int>(checked));
}

TEST(MacroPrefixIndexTest, UnicodeRulesFollowLocaleChangesAfterConstruction) {
    struct RestoreLocale {
        std::string saved{std::setlocale(LC_CTYPE, nullptr)};
        ~RestoreLocale() { std::setlocale(LC_CTYPE, saved.c_str()); }
    } restore;
    ASSERT_NE(std::setlocale(LC_CTYPE, "C"), nullptr);
    const std::unordered_set<std::wstring> keys{
        L"Été plage", L"école rue", L"Alpha Long", L"alpha long", L"k kelvin",
        L"i item", L"İçecek masa", L"hi There", L"🦀 place",
    };
    const MacroPrefixIndex index(keys);
    std::size_t checked = 0;
    for (const char* locale : {"C", "C.UTF-8", ".UTF8", "tr_TR.UTF-8", "Turkish_Turkey.1254"}) {
        if (!std::setlocale(LC_CTYPE, locale)) continue;
        for (const std::wstring candidate : {
            L"É", L"é", L"École ", L"école ", L"Été ", L"été ", L"K ", L"K ",
            L"I ", L"İ ", L"ı ", L"HI ", L"hi ", L"🦀 ",
        }) {
            EXPECT_EQ(IsSpaceMacroPrefix(candidate, index), IsSpaceMacroPrefix(candidate, keys))
                << "locale=" << locale;
            ++checked;
        }
    }
    EXPECT_GE(checked, 14u);
    RecordProperty("locale_queries", static_cast<int>(checked));
}

TEST(MacroPrefixIndexTest, OwnsKeysAcrossSourceDestructionCopyAndMove) {
    MacroPrefixIndex copy;
    {
        std::unordered_set<std::wstring> keys{L"hello world", L"Hello World"};
        MacroPrefixIndex original(keys);
        copy = original;
        keys.clear();
        original = MacroPrefixIndex{};
    }
    auto moved = std::move(copy);
    EXPECT_TRUE(IsSpaceMacroPrefix(L"HELLO ", moved));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"Hello ", moved));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"other ", moved));
    EXPECT_EQ(MacroPrefixIndex{}.StorageBytes(), 0u);
}

TEST(MacroPrefixIndexTest, RetainsGenericLongKeyAndFullPrefixSemantics) {
    const std::unordered_set<std::wstring> keys{
        std::wstring(128, L'a') + L" ending", std::wstring(129, L'A') + L" End",
    };
    const MacroPrefixIndex index(keys);
    for (const auto& key : keys) {
        for (std::size_t length = 0; length <= key.size() + 1; ++length) {
            const auto candidate = (key + L"!").substr(0, length);
            EXPECT_EQ(IsSpaceMacroPrefix(candidate, index), IsSpaceMacroPrefix(candidate, keys));
        }
    }
}

}  // namespace
}  // namespace NextKey
