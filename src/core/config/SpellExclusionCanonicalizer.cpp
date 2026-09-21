// VKey - Spell Exclusion Canonicalizer
// SPDX-License-Identifier: GPL-3.0-only

#include "SpellExclusionCanonicalizer.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(VKEY_HAS_ICU)
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#endif

namespace NextKey {

namespace {

bool ScalarsToUtf16Units(std::u32string_view scalars, std::vector<uint16_t>& units) {
    units.clear();
    units.reserve(scalars.size() * 2);
    for (uint32_t cp : scalars) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        if (cp <= 0xFFFF) {
            units.push_back(static_cast<uint16_t>(cp));
        } else {
            uint32_t v = cp - 0x10000;
            units.push_back(static_cast<uint16_t>(0xD800 + (v >> 10)));
            units.push_back(static_cast<uint16_t>(0xDC00 + (v & 0x3FF)));
        }
    }
    return true;
}

bool Utf16UnitsToScalars(const std::vector<uint16_t>& units, std::u32string& scalars) {
    scalars.clear();
    scalars.reserve(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= units.size()) return false;
            uint32_t low = units[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) return false;
            scalars.push_back(0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
            ++i;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;
        } else {
            scalars.push_back(u);
        }
    }
    return true;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = text.find_first_of("\r\n", pos);
        if (next == std::string_view::npos) {
            lines.emplace_back(text.substr(pos));
            break;
        }
        lines.emplace_back(text.substr(pos, next - pos));
        if (text[next] == '\r' && next + 1 < text.size() && text[next + 1] == '\n') {
            pos = next + 2;
        } else {
            pos = next + 1;
        }
    }
    return lines;
}

std::vector<std::wstring> SplitLinesWide(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = text.find_first_of(L"\r\n", pos);
        if (next == std::wstring::npos) {
            lines.emplace_back(text.substr(pos));
            break;
        }
        lines.emplace_back(text.substr(pos, next - pos));
        if (text[next] == L'\r' && next + 1 < text.size() && text[next + 1] == L'\n') {
            pos = next + 2;
        } else {
            pos = next + 1;
        }
    }
    return lines;
}

} // namespace

bool SpellExclusionCanonicalizer::IsUnicodeWhitespace(uint32_t cp) noexcept {
    switch (cp) {
        case 0x0009: // \t
        case 0x000A: // \n
        case 0x000B: // \v
        case 0x000C: // \f
        case 0x000D: // \r
        case 0x0020: // space
        case 0x0085: // next line
        case 0x00A0: // no-break space
        case 0x1680: // ogham space mark
        case 0x2000: // en quad
        case 0x2001: // em quad
        case 0x2002: // en space
        case 0x2003: // em space
        case 0x2004: // three-per-em space
        case 0x2005: // four-per-em space
        case 0x2006: // six-per-em space
        case 0x2007: // figure space
        case 0x2008: // punctuation space
        case 0x2009: // thin space
        case 0x200A: // hair space
        case 0x2028: // line separator
        case 0x2029: // paragraph separator
        case 0x202F: // narrow no-break space
        case 0x205F: // medium mathematical space
        case 0x3000: // ideographic space
            return true;
        default:
            return false;
    }
}

bool SpellExclusionCanonicalizer::IsUnicodeControl(uint32_t cp) noexcept {
    return (cp <= 0x1F) || (cp >= 0x7F && cp <= 0x9F);
}

std::u32string SpellExclusionCanonicalizer::TrimWhitespace(std::u32string_view input) {
    size_t start = 0;
    while (start < input.size() && IsUnicodeWhitespace(input[start])) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && IsUnicodeWhitespace(input[end - 1])) {
        --end;
    }
    return std::u32string(input.substr(start, end - start));
}

bool SpellExclusionCanonicalizer::Utf16ToUtf32(const std::wstring& input, std::u32string& output) {
    output.clear();
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        uint32_t unit = static_cast<uint16_t>(input[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= input.size()) return false;
            uint32_t low = static_cast<uint16_t>(input[i + 1]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            output.push_back(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
            ++i;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        } else {
            output.push_back(unit);
        }
    }
    return true;
}

bool SpellExclusionCanonicalizer::Utf32ToUtf16(const std::u32string& input, std::wstring& output) {
    output.clear();
    output.reserve(input.size() * 2);
    for (uint32_t cp : input) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp <= 0xFFFF) {
            output.push_back(static_cast<wchar_t>(cp));
        } else {
#if defined(_WIN32)
            uint32_t v = cp - 0x10000;
            output.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            output.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
#else
            output.push_back(static_cast<wchar_t>(cp));
#endif
        }
    }
    return true;
}

bool SpellExclusionCanonicalizer::Utf32ToUtf8(const std::u32string& input, std::string& output) {
    output.clear();
    output.reserve(input.size() * 3);
    for (uint32_t cp : input) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp <= 0x7F) {
            output.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return true;
}

bool SpellExclusionCanonicalizer::Utf8ToUtf32(std::string_view input, std::u32string& output) {
    output.clear();
    output.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        const auto first = static_cast<uint8_t>(input[i]);
        uint32_t scalar = 0;
        size_t width = 0;
        if (first <= 0x7F) {
            scalar = first;
            width = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            scalar = first & 0x1F;
            width = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            scalar = first & 0x0F;
            width = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            scalar = first & 0x07;
            width = 4;
        } else {
            return false;
        }
        if (i + width > input.size()) return false;
        for (size_t j = 1; j < width; ++j) {
            const auto continuation = static_cast<uint8_t>(input[i + j]);
            if ((continuation & 0xC0) != 0x80) return false;
            scalar = (scalar << 6) | (continuation & 0x3F);
        }
        if ((width == 3 && scalar < 0x800) || (width == 4 && scalar < 0x10000)
            || (scalar >= 0xD800 && scalar <= 0xDFFF) || scalar > 0x10FFFF) {
            return false;
        }
        output.push_back(scalar);
        i += width;
    }
    return true;
}

bool SpellExclusionCanonicalizer::NormalizeNfcUtf16(
    const std::vector<uint16_t>& input, std::vector<uint16_t>& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
#if defined(_WIN32)
    int required = ::NormalizeString(
        NormalizationC,
        reinterpret_cast<LPCWSTR>(input.data()),
        static_cast<int>(input.size()),
        nullptr, 0);
    if (required <= 0) return false;
    output.resize(static_cast<size_t>(required));
    int written = ::NormalizeString(
        NormalizationC,
        reinterpret_cast<LPCWSTR>(input.data()),
        static_cast<int>(input.size()),
        reinterpret_cast<LPWSTR>(output.data()),
        required);
    if (written <= 0) return false;
    output.resize(static_cast<size_t>(written));
    return true;
#elif defined(VKEY_HAS_ICU)
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* norm = unorm2_getNFCInstance(&status);
    if (U_FAILURE(status)) return false;

    output.resize(input.size() * 2 + 16);
    int32_t len = unorm2_normalize(
        norm,
        reinterpret_cast<const UChar*>(input.data()),
        static_cast<int32_t>(input.size()),
        reinterpret_cast<UChar*>(output.data()),
        static_cast<int32_t>(output.size()),
        &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        status = U_ZERO_ERROR;
        output.resize(static_cast<size_t>(len));
        len = unorm2_normalize(
            norm,
            reinterpret_cast<const UChar*>(input.data()),
            static_cast<int32_t>(input.size()),
            reinterpret_cast<UChar*>(output.data()),
            static_cast<int32_t>(output.size()),
            &status);
    }
    if (U_FAILURE(status)) return false;
    output.resize(static_cast<size_t>(len));
    return true;
#else
    // Fallback: copy as-is if no Unicode library present
    output = input;
    return true;
#endif
}

bool SpellExclusionCanonicalizer::ToLowercaseUtf16(
    const std::vector<uint16_t>& input, std::vector<uint16_t>& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
#if defined(_WIN32)
    int required = ::LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        reinterpret_cast<LPCWSTR>(input.data()),
        static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) return false;
    output.resize(static_cast<size_t>(required));
    int written = ::LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        reinterpret_cast<LPCWSTR>(input.data()),
        static_cast<int>(input.size()),
        reinterpret_cast<LPWSTR>(output.data()),
        required, nullptr, nullptr, 0);
    if (written <= 0) return false;
    output.resize(static_cast<size_t>(written));
    return true;
#elif defined(VKEY_HAS_ICU)
    UErrorCode status = U_ZERO_ERROR;
    output.resize(input.size() * 2 + 16);
    int32_t len = u_strToLower(
        reinterpret_cast<UChar*>(output.data()),
        static_cast<int32_t>(output.size()),
        reinterpret_cast<const UChar*>(input.data()),
        static_cast<int32_t>(input.size()),
        "", &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        status = U_ZERO_ERROR;
        output.resize(static_cast<size_t>(len));
        len = u_strToLower(
            reinterpret_cast<UChar*>(output.data()),
            static_cast<int32_t>(output.size()),
            reinterpret_cast<const UChar*>(input.data()),
            static_cast<int32_t>(input.size()),
            "", &status);
    }
    if (U_FAILURE(status)) return false;
    output.resize(static_cast<size_t>(len));
    return true;
#else
    output = input;
    for (auto& u : output) {
        if (u >= 'A' && u <= 'Z') u = u + ('a' - 'A');
    }
    return true;
#endif
}

bool SpellExclusionCanonicalizer::NormalizeAndValidate(
    std::u32string_view input,
    size_t row,
    std::string& outUtf8,
    std::wstring& outUtf16,
    SpellExclusionError& outError) {
    const std::u32string trimmed = TrimWhitespace(input);

    std::vector<uint16_t> u16in;
    if (!ScalarsToUtf16Units(trimmed, u16in)) {
        outError.kind = SpellExclusionErrorKind::InvalidEntry;
        outError.row = row;
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" contains invalid scalar";
        return false;
    }

    // Pipeline: NFC 1 -> Lowercase -> NFC 2
    std::vector<uint16_t> nfc1;
    if (!NormalizeNfcUtf16(u16in, nfc1)) {
        outError.kind = SpellExclusionErrorKind::InvalidEntry;
        outError.row = row;
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" could not be NFC-normalized";
        return false;
    }

    std::vector<uint16_t> lowered;
    if (!ToLowercaseUtf16(nfc1, lowered)) {
        outError.kind = SpellExclusionErrorKind::InvalidEntry;
        outError.row = row;
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" could not be lowercased";
        return false;
    }

    std::vector<uint16_t> nfc2;
    if (!NormalizeNfcUtf16(lowered, nfc2)) {
        outError.kind = SpellExclusionErrorKind::InvalidEntry;
        outError.row = row;
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" could not be NFC-normalized";
        return false;
    }

    std::u32string finalScalars;
    if (!Utf16UnitsToScalars(nfc2, finalScalars)) {
        outError.kind = SpellExclusionErrorKind::InvalidEntry;
        outError.row = row;
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" contains invalid UTF-16 surrogates";
        return false;
    }

    // Boundary checks
    if (finalScalars.size() < kMinSpellExclusionScalars) {
        outError.kind = SpellExclusionErrorKind::TooShort;
        outError.row = row;
        outError.scalars = finalScalars.size();
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" is shorter than 2 scalars";
        return false;
    }

    if (finalScalars.size() > kMaxSpellExclusionScalars) {
        outError.kind = SpellExclusionErrorKind::TooLong;
        outError.row = row;
        outError.scalars = finalScalars.size();
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" has " +
                           std::to_wstring(finalScalars.size()) + L" scalars, exceeds the 128-scalar cap";
        return false;
    }

    for (uint32_t scalar : finalScalars) {
        if (IsUnicodeWhitespace(scalar) || IsUnicodeControl(scalar)) {
            outError.kind = SpellExclusionErrorKind::InvalidEntry;
            outError.row = row;
            outError.message = L"spell exclusion row " + std::to_wstring(row) + L" contains whitespace or control text";
            return false;
        }
    }

    if (!Utf32ToUtf8(finalScalars, outUtf8) || !Utf32ToUtf16(finalScalars, outUtf16)) {
        outError.kind = SpellExclusionErrorKind::InvalidEntry;
        outError.row = row;
        outError.message = L"spell exclusion row " + std::to_wstring(row) + L" could not be encoded";
        return false;
    }

    return true;
}

SpellExclusionCanonicalResult SpellExclusionCanonicalizer::Canonicalize(
    const std::vector<std::wstring>& entries) {
    SpellExclusionCanonicalResult result;

    struct Item {
        std::string utf8;
        std::wstring utf16;
    };
    std::vector<Item> items;
    items.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
        size_t row = i + 1;
        std::u32string scalars;
        if (!Utf16ToUtf32(entries[i], scalars)) {
            result.error.kind = SpellExclusionErrorKind::InvalidEntry;
            result.error.row = row;
            result.error.message = L"spell exclusion row " + std::to_wstring(row) + L" contains invalid UTF-16 surrogates";
            return result;
        }
        std::string utf8;
        std::wstring utf16;
        if (!NormalizeAndValidate(scalars, row, utf8, utf16, result.error)) {
            return result;
        }
        items.push_back({std::move(utf8), std::move(utf16)});
    }

    // Deterministic UTF-8 byte sort (matching Rust sort_unstable on &str)
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.utf8 < b.utf8;
    });

    // Deduplication
    auto last = std::unique(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.utf8 == b.utf8;
    });
    items.erase(last, items.end());

    if (items.size() > kMaxSpellExclusionRows) {
        result.error.kind = SpellExclusionErrorKind::TooManyEntries;
        result.error.count = items.size();
        result.error.message = L"spell exclusions have " + std::to_wstring(items.size()) +
                               L" entries, exceeds the 8-entry cap";
        return result;
    }

    result.entries.reserve(items.size());
    result.utf8Entries.reserve(items.size());
    for (auto& item : items) {
        result.entries.push_back(std::move(item.utf16));
        result.utf8Entries.push_back(std::move(item.utf8));
    }
    return result;
}

SpellExclusionCanonicalResult SpellExclusionCanonicalizer::Canonicalize(
    const std::vector<std::string>& entries) {
    SpellExclusionCanonicalResult result;

    struct Item {
        std::string utf8;
        std::wstring utf16;
    };
    std::vector<Item> items;
    items.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
        size_t row = i + 1;
        std::u32string scalars;
        if (!Utf8ToUtf32(entries[i], scalars)) {
            result.error.kind = SpellExclusionErrorKind::InvalidEntry;
            result.error.row = row;
            result.error.message = L"spell exclusion row " + std::to_wstring(row) + L" contains invalid UTF-8 bytes";
            return result;
        }
        std::string utf8;
        std::wstring utf16;
        if (!NormalizeAndValidate(scalars, row, utf8, utf16, result.error)) {
            return result;
        }
        items.push_back({std::move(utf8), std::move(utf16)});
    }

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.utf8 < b.utf8;
    });

    auto last = std::unique(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.utf8 == b.utf8;
    });
    items.erase(last, items.end());

    if (items.size() > kMaxSpellExclusionRows) {
        result.error.kind = SpellExclusionErrorKind::TooManyEntries;
        result.error.count = items.size();
        result.error.message = L"spell exclusions have " + std::to_wstring(items.size()) +
                               L" entries, exceeds the 8-entry cap";
        return result;
    }

    result.entries.reserve(items.size());
    result.utf8Entries.reserve(items.size());
    for (auto& item : items) {
        result.entries.push_back(std::move(item.utf16));
        result.utf8Entries.push_back(std::move(item.utf8));
    }
    return result;
}

SpellExclusionCanonicalResult SpellExclusionCanonicalizer::CanonicalizeFromText(
    const std::wstring& text) {
    return Canonicalize(SplitLinesWide(text));
}

SpellExclusionCanonicalResult SpellExclusionCanonicalizer::CanonicalizeFromText(
    std::string_view text) {
    return Canonicalize(SplitLines(text));
}

} // namespace NextKey
