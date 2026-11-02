// VKey - Macro Phrase-Prefix Matcher
// SPDX-License-Identifier: GPL-3.0-only
//
// Pure, platform-free helper used by HookEngine to decide whether a
// space-terminated word boundary should preserve the macro buffer (because it
// looks like the user is mid-way through typing a multi-word macro key) or
// clear it (normal word-commit behavior).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NextKey {

/// Returns true if `candidate` is a non-empty prefix of at least one key in
/// `spaceKeys`. Case sensitivity follows the macro case rule:
///   - key has any uppercase letter → strict prefix (typed case must match)
///   - key is all-lowercase          → case-insensitive prefix (ASCII-fold)
///
/// Empty `spaceKeys` short-circuits to false — this is the hot path for users
/// who haven't defined any multi-word macros.
[[nodiscard]] inline bool IsSpaceMacroPrefix(
    const std::wstring& candidate,
    const std::unordered_set<std::wstring>& spaceKeys) noexcept {
    if (spaceKeys.empty() || candidate.empty()) return false;

    auto hasUpper = [](const std::wstring& s) noexcept {
        for (auto c : s) if (iswupper(c)) return true;
        return false;
    };

    std::wstring candLower;  // built lazily, only if a lowercase key is found
    for (const auto& key : spaceKeys) {
        if (key.size() < candidate.size()) continue;
        if (hasUpper(key)) {
            if (key.compare(0, candidate.size(), candidate) == 0) return true;
        } else {
            if (candLower.empty()) {
                candLower = candidate;
                for (auto& c : candLower) c = static_cast<wchar_t>(towlower(c));
            }
            if (key.compare(0, candLower.size(), candLower) == 0) return true;
        }
    }
    return false;
}

// Own the indexed text so immutable snapshots remain safe to copy/move and
// publish independently of the source set's lifetime.
class MacroPrefixIndex {
    enum class KeyCase : unsigned char { Flexible, Strict, LocaleDependent };
    struct Key {
        std::size_t offset;
        std::size_t length;
        KeyCase caseRule;
    };
    struct Data {
        std::wstring text;
        std::vector<Key> keys;
        std::vector<std::size_t> sorted;
        std::vector<wchar_t> foldStarts;
        std::size_t maxLength{0};
        std::size_t commonLength{0};

        [[nodiscard]] std::wstring_view View(std::size_t id) const noexcept {
            const auto& key = keys[id];
            return std::wstring_view(text.data() + key.offset, key.length);
        }
    };

public:
    MacroPrefixIndex() = default;

    explicit MacroPrefixIndex(const std::unordered_set<std::wstring>& keys) {
        if (keys.empty()) return;
        auto data = std::make_shared<Data>();
        std::size_t totalLength = 0;
        for (const auto& key : keys) totalLength += key.size();
        data->text.reserve(totalLength);
        data->keys.reserve(keys.size());
        data->sorted.reserve(keys.size());
        for (const auto& key : keys) {
            KeyCase rule = KeyCase::Flexible;
            for (const auto ch : key) {
                if (ch >= L'A' && ch <= L'Z') {
                    rule = KeyCase::Strict;
                    break;
                }
                if (static_cast<std::uint32_t>(ch) > 127) rule = KeyCase::LocaleDependent;
            }
            if (data->keys.empty()) {
                firstKey_ = key;
                firstCase_ = rule;
            }
            data->sorted.push_back(data->keys.size());
            data->keys.push_back({data->text.size(), key.size(), rule});
            data->text.append(key);
            data->maxLength = (std::max)(data->maxLength, key.size());
        }
        std::sort(data->sorted.begin(), data->sorted.end(), [&](auto left, auto right) {
            return data->View(left) < data->View(right);
        });
        const auto low = data->View(data->sorted.front());
        const auto high = data->View(data->sorted.back());
        while (data->commonLength < low.size() && data->commonLength < high.size() &&
               low[data->commonLength] == high[data->commonLength]) ++data->commonLength;
        for (const auto id : data->sorted) {
            const auto key = data->View(id);
            if (data->keys[id].caseRule == KeyCase::Strict || key.empty()) continue;
            if (data->foldStarts.empty() || data->foldStarts.back() != key.front()) {
                data->foldStarts.push_back(key.front());
            }
        }
        data_ = std::move(data);
    }

    [[nodiscard]] bool ContainsPrefix(std::wstring_view candidate) const noexcept {
        if (!data_ || candidate.empty()) return false;
        // Keep the first-key check independent of index metadata and scratch.
        if (firstKey_.size() >= candidate.size() &&
            ComparePrefix(firstKey_, candidate, !HasUpper(firstKey_, firstCase_)) == 0) return true;
        if (data_->keys.size() == 1 || candidate.size() > data_->maxLength) return false;
        return ContainsOtherPrefixes(candidate);
    }

    /// Owned buffers and Data, excluding shared-control/allocator overhead.
    [[nodiscard]] std::size_t StorageBytes() const noexcept {
        if (!data_) return 0;
        return sizeof(Data) + (data_->text.capacity() + 1) * sizeof(wchar_t) +
            data_->keys.capacity() * sizeof(Key) + data_->sorted.capacity() * sizeof(std::size_t) +
            data_->foldStarts.capacity() * sizeof(wchar_t) +
            (firstKey_.capacity() + 1) * sizeof(wchar_t);
    }

private:
    [[nodiscard]] static bool HasUpper(std::wstring_view key, KeyCase rule) noexcept {
        if (rule != KeyCase::LocaleDependent) return rule == KeyCase::Strict;
        // ASCII uppercase already makes a key Strict at construction. Only
        // Unicode classification can change when a TSF host changes locale.
        for (const auto ch : key) {
            if (static_cast<std::uint32_t>(ch) > 127 && std::iswupper(ch)) return true;
        }
        return false;
    }

    [[nodiscard]] static wchar_t Fold(wchar_t ch) noexcept {
        // Lowercase ASCII, digits and punctuation are unchanged in every
        // locale. Keep uppercase/Unicode on towlower (notably Turkish I).
        if ((ch >= L'A' && ch <= L'Z') || static_cast<std::uint32_t>(ch) > 127) {
            return static_cast<wchar_t>(std::towlower(ch));
        }
        return ch;
    }

    // Keep bounded normalization scratch off the first-hit path.
    [[nodiscard]]
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__)
    __attribute__((noinline))
#endif
    bool ContainsOtherPrefixes(std::wstring_view candidate) const noexcept {
        const auto& data = *data_;
        const auto find = [&](std::wstring_view query, bool folded) {
            // Sorted endpoints bound the common prefix of every stored key.
            // Reject impossible ranges before any binary-search comparisons.
            const auto length = (std::min)(data.commonLength, query.size());
            if (length && ComparePrefix(data.View(data.sorted.front()).substr(0, length),
                                        query.substr(0, length), folded) != 0) return data.sorted.end();
            return std::lower_bound(data.sorted.begin(), data.sorted.end(), query,
                [&](std::size_t id, std::wstring_view value) {
                    return ComparePrefix(data.View(id), value, folded) < 0;
                });
        };
        bool rawFlexible = false;
        for (auto at = find(candidate, false); at != data.sorted.end(); ++at) {
            const auto key = data.View(*at);
            if (ComparePrefix(key, candidate, false) != 0) break;
            if (HasUpper(key, data.keys[*at].caseRule)) return true;
            rawFlexible = true;
        }
        if (data.foldStarts.empty()) return false;
        const auto first = Fold(candidate.front());
        if (!std::binary_search(data.foldStarts.begin(), data.foldStarts.end(), first)) return false;

        constexpr std::size_t kInlineFoldChars = 32;
        std::array<wchar_t, kInlineFoldChars> scratch;
        const bool normalized = candidate.size() <= scratch.size();
        std::wstring_view folded = candidate;
        if (normalized) {
            scratch[0] = first;
            for (std::size_t i = 1; i < candidate.size(); ++i) scratch[i] = Fold(candidate[i]);
            folded = std::wstring_view(scratch.data(), candidate.size());
            if (rawFlexible && folded == candidate) return true;
        }
        // Generic long keys stay supported without allocating a larger buffer.
        for (auto at = find(folded, !normalized); at != data.sorted.end(); ++at) {
            const auto key = data.View(*at);
            if (ComparePrefix(key, folded, !normalized) != 0) break;
            if (!HasUpper(key, data.keys[*at].caseRule)) return true;
        }
        return false;
    }

    // Prefix-equivalent keys compare equal. This still partitions the sorted
    // raw-key order, including when the query is folded in the current locale.
    [[nodiscard]] static int ComparePrefix(std::wstring_view key,
                                           std::wstring_view candidate,
                                           bool folded) noexcept {
        if (!folded) return key.substr(0, candidate.size()).compare(candidate);
        const auto length = (std::min)(key.size(), candidate.size());
        for (std::size_t i = 0; i < length; ++i) {
            const auto ch = Fold(candidate[i]);
            if (key[i] < ch) return -1;
            if (key[i] > ch) return 1;
        }
        return key.size() < candidate.size() ? -1 : 0;
    }

    std::shared_ptr<const Data> data_;
    std::wstring firstKey_;
    KeyCase firstCase_{KeyCase::Flexible};
};

[[nodiscard]] inline bool IsSpaceMacroPrefix(
    std::wstring_view candidate, const MacroPrefixIndex& index) noexcept {
    return index.ContainsPrefix(candidate);
}

}  // namespace NextKey
