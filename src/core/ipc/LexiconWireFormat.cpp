// VKey - Lexicon Shared Memory Wire Format Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "LexiconWireFormat.h"

#include <algorithm>
#include <sstream>

namespace NextKey::Wire {

namespace {

// CRC32 IEEE 802.3 lookup table (polynomial 0xEDB88320)
constexpr uint32_t MakeCrc32TableEntry(uint32_t byte) noexcept {
    uint32_t crc = byte;
    for (int j = 0; j < 8; ++j) {
        crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1));
    }
    return crc;
}

struct Crc32LookupTable {
    uint32_t entries[256];
    constexpr Crc32LookupTable() : entries{} {
        for (uint32_t i = 0; i < 256; ++i) {
            entries[i] = MakeCrc32TableEntry(i);
        }
    }
};

constexpr Crc32LookupTable kCrc32Table{};

char32_t DecodeNextScalar(std::u16string_view str, size_t& index) noexcept {
    if (index >= str.size()) {
        return 0;
    }
    char16_t c1 = str[index++];
    if (c1 >= 0xD800 && c1 <= 0xDBFF && index < str.size()) {
        char16_t c2 = str[index];
        if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
            index++;
            return 0x10000 + ((static_cast<char32_t>(c1 - 0xD800) << 10) |
                              static_cast<char32_t>(c2 - 0xDC00));
        }
    }
    return static_cast<char32_t>(c1);
}

bool HasInvalidSurrogates(std::u16string_view str) noexcept {
    size_t index = 0;
    while (index < str.size()) {
        char32_t cp = DecodeNextScalar(str, index);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return true;
        }
    }
    return false;
}

} // namespace

uint32_t ComputeCrc32(const void* data, size_t size) noexcept {
    if (!data || size == 0) {
        return 0;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        uint8_t tableIndex = static_cast<uint8_t>(crc ^ bytes[i]);
        crc = (crc >> 8) ^ kCrc32Table.entries[tableIndex];
    }
    return crc ^ 0xFFFFFFFFu;
}

size_t CountUnicodeScalars(std::u16string_view str) noexcept {
    size_t count = 0;
    size_t index = 0;
    while (index < str.size()) {
        DecodeNextScalar(str, index);
        count++;
    }
    return count;
}

int CompareUtf16ByUnicodeScalar(std::u16string_view a, std::u16string_view b) noexcept {
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        char32_t scalarA = DecodeNextScalar(a, i);
        char32_t scalarB = DecodeNextScalar(b, j);

        if (scalarA != scalarB) {
            return (scalarA < scalarB) ? -1 : 1;
        }
    }

    if (i < a.size()) {
        return 1;
    }
    if (j < b.size()) {
        return -1;
    }
    return 0;
}

std::u16string WStringToU16(const std::wstring& wstr) {
#if defined(_WIN32)
    return std::u16string(reinterpret_cast<const char16_t*>(wstr.data()), wstr.size());
#else
    std::u16string u16;
    for (wchar_t wc : wstr) {
        if (wc <= 0xD7FF || (wc >= 0xE000 && wc <= 0xFFFF)) {
            u16.push_back(static_cast<char16_t>(wc));
        } else if (wc >= 0x10000 && wc <= 0x10FFFF) {
            uint32_t cp = static_cast<uint32_t>(wc) - 0x10000;
            u16.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            u16.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return u16;
#endif
}

std::wstring U16ToWString(std::u16string_view u16) {
#if defined(_WIN32)
    return std::wstring(reinterpret_cast<const wchar_t*>(u16.data()), u16.size());
#else
    std::wstring ws;
    size_t i = 0;
    while (i < u16.size()) {
        char32_t cp = DecodeNextScalar(u16, i);
        ws.push_back(static_cast<wchar_t>(cp));
    }
    return ws;
#endif
}

bool LexiconWireSerializer::Serialize(
    const LexiconWireSerializeParams& params,
    uint8_t* outBuffer,
    size_t bufferSize,
    std::string* outError) {
    if (!outBuffer || bufferSize < WIRE_TOTAL_SIZE) {
        if (outError) *outError = "Output buffer is null or smaller than WIRE_TOTAL_SIZE (144 KiB)";
        return false;
    }

    if (params.spellExclusions.size() > MAX_EXCLUSIONS_ROWS) {
        if (outError) *outError = "Spell exclusions row count exceeds maximum limit (8)";
        return false;
    }

    // Zero entire buffer up to total wire size
    std::memset(outBuffer, 0, WIRE_TOTAL_SIZE);

    // Section 1: Spell Exclusions Region (0x0080..0x08FF)
    uint32_t exclusionsUtf16Units = 0;
    if (!params.spellExclusions.empty()) {
        auto* exclDest = reinterpret_cast<char16_t*>(outBuffer + EXCLUSIONS_REGION_OFFSET);
        for (size_t rowIdx = 0; rowIdx < params.spellExclusions.size(); ++rowIdx) {
            const auto& row = params.spellExclusions[rowIdx];
            if (HasInvalidSurrogates(row)) {
                if (outError) *outError = "Spell exclusion row contains invalid or unpaired UTF-16 surrogate code point";
                return false;
            }
            size_t scalars = CountUnicodeScalars(row);
            if (scalars < MIN_EXCLUSION_SCALARS || scalars > MAX_EXCLUSION_SCALARS) {
                if (outError) {
                    *outError = "Spell exclusion row scalar count must be between 2 and 128";
                }
                return false;
            }
            if (exclusionsUtf16Units + row.size() + 1 > MAX_EXCLUSIONS_UTF16_UNITS) {
                if (outError) {
                    *outError = "Spell exclusions total UTF-16 units exceed region capacity (1088 units)";
                }
                return false;
            }
            std::memcpy(exclDest + exclusionsUtf16Units, row.data(), row.size() * sizeof(char16_t));
            exclusionsUtf16Units += static_cast<uint32_t>(row.size());
            exclDest[exclusionsUtf16Units] = u'\n';
            exclusionsUtf16Units += 1;
        }
    }
    uint32_t exclusionsLengthBytes = exclusionsUtf16Units * sizeof(char16_t);

    // Section 2 & 3: User Dictionary Index Table (0x0900) & Text Pool (0x2900)
    std::vector<std::u16string> sortedWords = params.userDictionary;
    std::sort(sortedWords.begin(), sortedWords.end(), [](const std::u16string& a, const std::u16string& b) {
        return CompareUtf16ByUnicodeScalar(a, b) < 0;
    });
    sortedWords.erase(
        std::unique(sortedWords.begin(), sortedWords.end(), [](const std::u16string& a, const std::u16string& b) {
            return CompareUtf16ByUnicodeScalar(a, b) == 0;
        }),
        sortedWords.end());

    if (sortedWords.size() > MAX_USER_DICT_WORDS) {
        if (outError) *outError = "User dictionary unique words exceed maximum limit (1024)";
        return false;
    }

    uint32_t userDictWordCount = static_cast<uint32_t>(sortedWords.size());
    uint32_t userDictUtf16Units = 0;
    auto* indexTable = reinterpret_cast<DictWordEntryWire*>(outBuffer + USER_DICT_INDEX_OFFSET);
    auto* textPool = reinterpret_cast<char16_t*>(outBuffer + USER_DICT_POOL_OFFSET);

    for (size_t i = 0; i < sortedWords.size(); ++i) {
        const auto& word = sortedWords[i];
        if (HasInvalidSurrogates(word)) {
            if (outError) *outError = "User dictionary word contains invalid or unpaired UTF-16 surrogate code point";
            return false;
        }
        size_t scalars = CountUnicodeScalars(word);
        if (scalars < MIN_DICT_WORD_SCALARS || scalars > MAX_DICT_WORD_SCALARS) {
            if (outError) *outError = "User dictionary word scalar count must be between 1 and 64";
            return false;
        }
        if (word.size() > MAX_DICT_WORD_UTF16_UNITS) {
            if (outError) *outError = "User dictionary word UTF-16 units exceed limit (128 units)";
            return false;
        }
        if (userDictUtf16Units + word.size() + 1 > MAX_USER_DICT_UTF16_UNITS) {
            if (outError) *outError = "User dictionary total UTF-16 units exceed pool capacity (65536 units)";
            return false;
        }

        indexTable[i].offsetUnits = userDictUtf16Units;
        indexTable[i].lengthUtf16Units = static_cast<uint16_t>(word.size());
        indexTable[i].scalarCount = static_cast<uint16_t>(scalars);

        std::memcpy(textPool + userDictUtf16Units, word.data(), word.size() * sizeof(char16_t));
        userDictUtf16Units += static_cast<uint32_t>(word.size());
        textPool[userDictUtf16Units] = u'\n';
        userDictUtf16Units += 1;
    }
    uint32_t userDictLengthBytes = userDictUtf16Units * sizeof(char16_t);

    // Populate Header (0x0000..0x007F)
    auto* header = reinterpret_cast<LexiconWireHeader*>(outBuffer);
    header->magic = WIRE_MAGIC;
    header->abiVersion = CURRENT_WIRE_ABI_VERSION;
    header->headerSizeBytes = WIRE_HEADER_SIZE;
    header->totalSizeBytes = WIRE_TOTAL_SIZE;
    header->seqlock = 0;
    header->generation = params.generation;
    header->flags = (params.spellSuggestEnabled ? LexiconWireFlags::SPELL_SUGGEST_ENABLED : 0) |
                    LexiconWireFlags::DICTIONARY_VALID |
                    LexiconWireFlags::EXCLUSIONS_VALID;
    header->payloadOffsetBytes = WIRE_HEADER_SIZE;
    header->payloadSizeBytes = (USER_DICT_POOL_OFFSET - WIRE_HEADER_SIZE) + userDictLengthBytes;
    header->exclusionsOffsetBytes = EXCLUSIONS_REGION_OFFSET;
    header->exclusionsLengthBytes = exclusionsLengthBytes;
    header->exclusionsRowCount = static_cast<uint32_t>(params.spellExclusions.size());
    header->userDictOffsetBytes = USER_DICT_POOL_OFFSET;
    header->userDictLengthBytes = userDictLengthBytes;
    header->userDictWordCount = userDictWordCount;
    header->userDictUtf16Units = userDictUtf16Units;
    header->exclusionsUtf16Units = exclusionsUtf16Units;

    header->crc32 = ComputeCrc32(outBuffer + header->payloadOffsetBytes, header->payloadSizeBytes);

    return true;
}

bool LexiconWireSerializer::Serialize(
    const std::vector<std::wstring>& spellExclusions,
    const std::vector<std::wstring>& userDictionary,
    uint64_t generation,
    bool spellSuggestEnabled,
    uint8_t* outBuffer,
    size_t bufferSize,
    std::string* outError) {
    LexiconWireSerializeParams params;
    params.generation = generation;
    params.spellSuggestEnabled = spellSuggestEnabled;
    params.spellExclusions.reserve(spellExclusions.size());
    for (const auto& w : spellExclusions) {
        params.spellExclusions.push_back(WStringToU16(w));
    }
    params.userDictionary.reserve(userDictionary.size());
    for (const auto& w : userDictionary) {
        params.userDictionary.push_back(WStringToU16(w));
    }
    return Serialize(params, outBuffer, bufferSize, outError);
}

bool LexiconWireDeserializer::ValidateAndInspect(
    const uint8_t* wireBuffer,
    size_t bufferSize,
    LexiconWireView& outView,
    std::string* outError) {
    outView = {};

    if (!wireBuffer || bufferSize < WIRE_TOTAL_SIZE) {
        if (outError) *outError = "Buffer is null or smaller than WIRE_TOTAL_SIZE (144 KiB)";
        return false;
    }

    const auto* header = reinterpret_cast<const LexiconWireHeader*>(wireBuffer);
    if (header->magic != WIRE_MAGIC) {
        if (outError) *outError = "Wire magic mismatch";
        return false;
    }
    if (header->abiVersion != CURRENT_WIRE_ABI_VERSION) {
        if (outError) *outError = "Unsupported wire ABI version";
        return false;
    }
    if (header->headerSizeBytes != WIRE_HEADER_SIZE) {
        if (outError) *outError = "Invalid wire header size";
        return false;
    }
    if (header->totalSizeBytes != WIRE_TOTAL_SIZE) {
        if (outError) *outError = "Invalid total size in header";
        return false;
    }
    if (header->payloadOffsetBytes != WIRE_HEADER_SIZE) {
        if (outError) *outError = "Invalid payload offset";
        return false;
    }
    if (header->payloadSizeBytes > (WIRE_TOTAL_SIZE - WIRE_HEADER_SIZE)) {
        if (outError) *outError = "Payload size exceeds total buffer capacity";
        return false;
    }
    if (header->exclusionsOffsetBytes != EXCLUSIONS_REGION_OFFSET) {
        if (outError) *outError = "Invalid exclusions region offset";
        return false;
    }
    if (header->exclusionsLengthBytes > EXCLUSIONS_REGION_MAX_BYTES) {
        if (outError) *outError = "Exclusions length bytes exceed region maximum";
        return false;
    }
    if (header->exclusionsUtf16Units * sizeof(char16_t) != header->exclusionsLengthBytes) {
        if (outError) *outError = "Exclusions UTF-16 units inconsistent with byte length";
        return false;
    }
    if (header->exclusionsRowCount > MAX_EXCLUSIONS_ROWS) {
        if (outError) *outError = "Exclusions row count exceeds maximum limit";
        return false;
    }
    if (header->userDictOffsetBytes != USER_DICT_POOL_OFFSET) {
        if (outError) *outError = "Invalid user dictionary pool offset";
        return false;
    }
    if (header->userDictLengthBytes > USER_DICT_POOL_MAX_BYTES) {
        if (outError) *outError = "User dictionary length bytes exceed pool maximum";
        return false;
    }
    if (header->userDictUtf16Units * sizeof(char16_t) != header->userDictLengthBytes) {
        if (outError) *outError = "User dictionary UTF-16 units inconsistent with byte length";
        return false;
    }
    if (header->userDictWordCount > MAX_USER_DICT_WORDS) {
        if (outError) *outError = "User dictionary word count exceeds maximum limit";
        return false;
    }
    if (header->userDictUtf16Units > MAX_USER_DICT_UTF16_UNITS) {
        if (outError) *outError = "User dictionary UTF-16 units exceed maximum limit";
        return false;
    }

    uint32_t expectedPayloadSize = (USER_DICT_POOL_OFFSET - WIRE_HEADER_SIZE) + header->userDictLengthBytes;
    if (header->payloadSizeBytes != expectedPayloadSize) {
        if (outError) *outError = "Payload size does not match exact active sections size";
        return false;
    }

    // Verify CRC32
    uint32_t calculatedCrc = ComputeCrc32(wireBuffer + header->payloadOffsetBytes, header->payloadSizeBytes);
    if (calculatedCrc != header->crc32) {
        if (outError) *outError = "CRC32 checksum mismatch in wire payload";
        return false;
    }

    // Validate spell exclusion rows
    if (header->exclusionsRowCount > 0) {
        const auto* exclChars = reinterpret_cast<const char16_t*>(wireBuffer + header->exclusionsOffsetBytes);
        size_t rowStart = 0;
        size_t parsedRows = 0;
        for (size_t i = 0; i < header->exclusionsUtf16Units; ++i) {
            if (exclChars[i] == u'\n') {
                if (i <= rowStart) {
                    if (outError) *outError = "Spell exclusions contain empty row";
                    return false;
                }
                std::u16string_view rowView(exclChars + rowStart, i - rowStart);
                if (HasInvalidSurrogates(rowView)) {
                    if (outError) *outError = "Spell exclusion row contains invalid or unpaired UTF-16 surrogate code point";
                    return false;
                }
                size_t scalars = CountUnicodeScalars(rowView);
                if (scalars < MIN_EXCLUSION_SCALARS || scalars > MAX_EXCLUSION_SCALARS) {
                    if (outError) *outError = "Spell exclusion row scalar count out of allowed bounds (2..128)";
                    return false;
                }
                parsedRows++;
                rowStart = i + 1;
            }
        }
        if (parsedRows != header->exclusionsRowCount) {
            if (outError) *outError = "Spell exclusions row count mismatch with actual newline delimiters";
            return false;
        }
    }

    // Validate index entries
    const auto* indexTable = reinterpret_cast<const DictWordEntryWire*>(wireBuffer + USER_DICT_INDEX_OFFSET);
    const auto* textPool = reinterpret_cast<const char16_t*>(wireBuffer + USER_DICT_POOL_OFFSET);

    std::u16string_view prevWord;
    for (size_t i = 0; i < header->userDictWordCount; ++i) {
        const auto& entry = indexTable[i];
        if (entry.offsetUnits + entry.lengthUtf16Units >= header->userDictUtf16Units) {
            if (outError) *outError = "Index entry bounds exceed text pool units";
            return false;
        }
        if (textPool[entry.offsetUnits + entry.lengthUtf16Units] != u'\n') {
            if (outError) *outError = "Index entry missing newline delimiter in text pool";
            return false;
        }
        if (entry.lengthUtf16Units < 1 || entry.lengthUtf16Units > MAX_DICT_WORD_UTF16_UNITS) {
            if (outError) *outError = "Index entry UTF-16 length out of allowed bounds";
            return false;
        }
        if (entry.scalarCount < MIN_DICT_WORD_SCALARS || entry.scalarCount > MAX_DICT_WORD_SCALARS) {
            if (outError) *outError = "Index entry scalar count out of allowed bounds";
            return false;
        }

        std::u16string_view currentWord(textPool + entry.offsetUnits, entry.lengthUtf16Units);
        if (HasInvalidSurrogates(currentWord)) {
            if (outError) *outError = "User dictionary word contains invalid or unpaired UTF-16 surrogate code point";
            return false;
        }
        size_t actualScalars = CountUnicodeScalars(currentWord);
        if (actualScalars != entry.scalarCount) {
            if (outError) *outError = "Index entry scalarCount does not match actual decoded Unicode scalars in word";
            return false;
        }

        if (i > 0) {
            if (CompareUtf16ByUnicodeScalar(prevWord, currentWord) >= 0) {
                if (outError) *outError = "User dictionary index entries not strictly monotonically sorted";
                return false;
            }
        }
        prevWord = currentWord;
    }

    // Populate validated view
    outView.header = header;
    outView.exclusionsBuf = reinterpret_cast<const uint16_t*>(wireBuffer + header->exclusionsOffsetBytes);
    outView.exclusionsUnits = header->exclusionsUtf16Units;
    outView.userDictBuf = reinterpret_cast<const uint16_t*>(wireBuffer + header->userDictOffsetBytes);
    outView.userDictUnits = header->userDictUtf16Units;
    outView.indexEntries = indexTable;
    outView.wordCount = header->userDictWordCount;

    return true;
}

bool LexiconWireDeserializer::Deserialize(
    const uint8_t* wireBuffer,
    size_t bufferSize,
    std::vector<std::u16string>& outExclusions,
    std::vector<std::u16string>& outUserDict,
    uint64_t* outGeneration,
    bool* outSpellSuggestEnabled,
    std::string* outError) {
    outExclusions.clear();
    outUserDict.clear();

    LexiconWireView view;
    if (!ValidateAndInspect(wireBuffer, bufferSize, view, outError)) {
        return false;
    }

    if (outGeneration) {
        *outGeneration = view.header->generation;
    }
    if (outSpellSuggestEnabled) {
        *outSpellSuggestEnabled = (view.header->flags & LexiconWireFlags::SPELL_SUGGEST_ENABLED) != 0;
    }

    // Extract spell exclusions
    if (view.exclusionsUnits > 0) {
        const auto* exclChars = reinterpret_cast<const char16_t*>(view.exclusionsBuf);
        size_t start = 0;
        for (size_t i = 0; i < view.exclusionsUnits; ++i) {
            if (exclChars[i] == u'\n') {
                if (i > start) {
                    outExclusions.emplace_back(exclChars + start, i - start);
                }
                start = i + 1;
            }
        }
    }

    // Extract user dictionary words via index table
    if (view.wordCount > 0) {
        outUserDict.reserve(view.wordCount);
        const auto* textPool = reinterpret_cast<const char16_t*>(view.userDictBuf);
        for (size_t i = 0; i < view.wordCount; ++i) {
            const auto& entry = view.indexEntries[i];
            outUserDict.emplace_back(textPool + entry.offsetUnits, entry.lengthUtf16Units);
        }
    }

    return true;
}

bool LexiconWireDeserializer::Deserialize(
    const uint8_t* wireBuffer,
    size_t bufferSize,
    std::vector<std::wstring>& outExclusions,
    std::vector<std::wstring>& outUserDict,
    uint64_t* outGeneration,
    bool* outSpellSuggestEnabled,
    std::string* outError) {
    std::vector<std::u16string> u16Exclusions;
    std::vector<std::u16string> u16UserDict;
    if (!Deserialize(wireBuffer, bufferSize, u16Exclusions, u16UserDict, outGeneration, outSpellSuggestEnabled, outError)) {
        return false;
    }
    outExclusions.clear();
    outExclusions.reserve(u16Exclusions.size());
    for (const auto& w : u16Exclusions) {
        outExclusions.push_back(U16ToWString(w));
    }
    outUserDict.clear();
    outUserDict.reserve(u16UserDict.size());
    for (const auto& w : u16UserDict) {
        outUserDict.push_back(U16ToWString(w));
    }
    return true;
}

} // namespace NextKey::Wire
