// VKey - Lexicon Shared Memory Wire Format (Phase 2 Wire Architecture)
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

#pragma pack(push, 8)

namespace NextKey::Wire {

// Dedicated Shared Memory Object Name for Lexicon Wire Mapping
#if defined(_WIN32)
constexpr const wchar_t* LEXICON_WIRE_MAPPING_NAME = L"Local\\VKeyLexiconWireMapping";
#else
constexpr const char* LEXICON_WIRE_MAPPING_NAME = "Local\\VKeyLexiconWireMapping";
#endif

// Wire ABI Constants
constexpr uint32_t WIRE_MAGIC = 0x58454C56; // "VLEX" in Little-Endian
constexpr uint16_t CURRENT_WIRE_ABI_VERSION = 1;
constexpr uint16_t WIRE_HEADER_SIZE = 128;
constexpr uint32_t WIRE_TOTAL_SIZE = 144 * 1024; // 147,456 bytes (0x24000)

// Memory Layout Offsets and Capacities
constexpr uint32_t EXCLUSIONS_REGION_OFFSET = 0x0080;   // 128 bytes
constexpr uint32_t EXCLUSIONS_REGION_MAX_BYTES = 2176;  // 0x0880 (1088 char16_t units)

constexpr uint32_t USER_DICT_INDEX_OFFSET = 0x0900;     // 2304 bytes
constexpr uint32_t USER_DICT_INDEX_MAX_BYTES = 8192;    // 0x2000 (1024 entries * 8 bytes)

constexpr uint32_t USER_DICT_POOL_OFFSET = 0x2900;      // 10496 bytes
constexpr uint32_t USER_DICT_POOL_MAX_BYTES = 131072;   // 0x20000 (65536 char16_t units * 2)

constexpr uint32_t WIRE_USED_MAX_BYTES = 141568;        // 0x22900
constexpr uint32_t WIRE_HEADROOM_BYTES = 5888;          // 0x1700 (147456 - 141568)

// Contract Limits
constexpr size_t MAX_EXCLUSIONS_ROWS = 8;
constexpr size_t MAX_USER_DICT_WORDS = 1024;
constexpr size_t MAX_USER_DICT_UTF16_UNITS = 65536;
constexpr size_t MAX_EXCLUSIONS_UTF16_UNITS = 1088;
constexpr size_t MIN_EXCLUSION_SCALARS = 2;
constexpr size_t MAX_EXCLUSION_SCALARS = 128;
constexpr size_t MIN_DICT_WORD_SCALARS = 1;
constexpr size_t MAX_DICT_WORD_SCALARS = 64;
constexpr size_t MAX_DICT_WORD_UTF16_UNITS = 128;

struct LexiconWireFlags {
    static constexpr uint32_t SPELL_SUGGEST_ENABLED = 0x00000001;
    static constexpr uint32_t DICTIONARY_VALID      = 0x00000002;
    static constexpr uint32_t EXCLUSIONS_VALID      = 0x00000004;
};

struct alignas(8) LexiconWireHeader {
    uint32_t magic;                 // 0x00: 0x58454C56 ("VLEX")
    uint16_t abiVersion;            // 0x04: 1
    uint16_t headerSizeBytes;       // 0x06: 128
    uint32_t totalSizeBytes;        // 0x08: 147456
    volatile uint32_t seqlock;      // 0x0C: Seqlock counter (even=clean, odd=writing)
    uint64_t generation;            // 0x10: 64-bit monotonic persistent generation identity
    uint32_t crc32;                 // 0x18: CRC32-IEEE over payload region
    uint32_t flags;                 // 0x1C: Bitmask of LexiconWireFlags
    uint32_t payloadOffsetBytes;    // 0x20: Offset to payload start (128)
    uint32_t payloadSizeBytes;     // 0x24: Total payload length in bytes
    uint32_t exclusionsOffsetBytes; // 0x28: Offset to exclusions UTF-16 text (0x0080)
    uint32_t exclusionsLengthBytes; // 0x2C: Length of exclusions text in bytes
    uint32_t exclusionsRowCount;    // 0x30: Number of exclusion rows (0..8)
    uint32_t userDictOffsetBytes;   // 0x34: Offset to dictionary UTF-16 text pool (0x2900)
    uint32_t userDictLengthBytes;   // 0x38: Total length of dictionary text pool in bytes
    uint32_t userDictWordCount;     // 0x3C: Number of dictionary words (0..1024)
    uint32_t userDictUtf16Units;    // 0x40: Total UTF-16 code units in text pool (<= 65536)
    uint32_t exclusionsUtf16Units;  // 0x44: Total UTF-16 code units in exclusions text
    uint64_t reserved[7];           // 0x48..0x7F: Padding to 128 bytes
};

static_assert(sizeof(LexiconWireHeader) == 128, "LexiconWireHeader must be exactly 128 bytes");
static_assert(alignof(LexiconWireHeader) == 8, "LexiconWireHeader must be 8-byte aligned");

static_assert(offsetof(LexiconWireHeader, magic) == 0x00, "magic offset must be 0x00");
static_assert(offsetof(LexiconWireHeader, abiVersion) == 0x04, "abiVersion offset must be 0x04");
static_assert(offsetof(LexiconWireHeader, headerSizeBytes) == 0x06, "headerSizeBytes offset must be 0x06");
static_assert(offsetof(LexiconWireHeader, totalSizeBytes) == 0x08, "totalSizeBytes offset must be 0x08");
static_assert(offsetof(LexiconWireHeader, seqlock) == 0x0C, "seqlock offset must be 0x0C");
static_assert(offsetof(LexiconWireHeader, generation) == 0x10, "generation offset must be 0x10");
static_assert(offsetof(LexiconWireHeader, crc32) == 0x18, "crc32 offset must be 0x18");
static_assert(offsetof(LexiconWireHeader, flags) == 0x1C, "flags offset must be 0x1C");
static_assert(offsetof(LexiconWireHeader, payloadOffsetBytes) == 0x20, "payloadOffsetBytes offset must be 0x20");
static_assert(offsetof(LexiconWireHeader, payloadSizeBytes) == 0x24, "payloadSizeBytes offset must be 0x24");
static_assert(offsetof(LexiconWireHeader, exclusionsOffsetBytes) == 0x28, "exclusionsOffsetBytes offset must be 0x28");
static_assert(offsetof(LexiconWireHeader, exclusionsLengthBytes) == 0x2C, "exclusionsLengthBytes offset must be 0x2C");
static_assert(offsetof(LexiconWireHeader, exclusionsRowCount) == 0x30, "exclusionsRowCount offset must be 0x30");
static_assert(offsetof(LexiconWireHeader, userDictOffsetBytes) == 0x34, "userDictOffsetBytes offset must be 0x34");
static_assert(offsetof(LexiconWireHeader, userDictLengthBytes) == 0x38, "userDictLengthBytes offset must be 0x38");
static_assert(offsetof(LexiconWireHeader, userDictWordCount) == 0x3C, "userDictWordCount offset must be 0x3C");
static_assert(offsetof(LexiconWireHeader, userDictUtf16Units) == 0x40, "userDictUtf16Units offset must be 0x40");
static_assert(offsetof(LexiconWireHeader, exclusionsUtf16Units) == 0x44, "exclusionsUtf16Units offset must be 0x44");
static_assert(offsetof(LexiconWireHeader, reserved) == 0x48, "reserved offset must be 0x48");

#pragma pack(push, 4)
struct alignas(4) DictWordEntryWire {
    uint32_t offsetUnits;      // Offset (tính theo số phần tử char16_t) từ đầu UTF-16 String Pool (0x2900)
    uint16_t lengthUtf16Units; // Số lượng UTF-16 code units (1..128 units, tương ứng tối đa 64 scalars)
    uint16_t scalarCount;      // Số lượng Unicode scalars của từ (1..64 scalars theo N7 contract)
};
#pragma pack(pop)

static_assert(sizeof(DictWordEntryWire) == 8, "DictWordEntryWire must be 8 bytes");
static_assert(alignof(DictWordEntryWire) == 4, "DictWordEntryWire must be 4-byte aligned");
static_assert(offsetof(DictWordEntryWire, offsetUnits) == 0x00, "offsetUnits offset must be 0x00");
static_assert(offsetof(DictWordEntryWire, lengthUtf16Units) == 0x04, "lengthUtf16Units offset must be 0x04");
static_assert(offsetof(DictWordEntryWire, scalarCount) == 0x06, "scalarCount offset must be 0x06");

// Seqlock Atomic Primitives
inline uint32_t SeqlockBeginWrite(volatile uint32_t& seqlock) noexcept {
#if defined(_WIN32)
    uint32_t seq = static_cast<uint32_t>(::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&seqlock)));
    MemoryBarrier();
    return seq;
#else
    std::atomic_ref<uint32_t> ref(const_cast<uint32_t&>(seqlock));
    uint32_t seq = ref.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return seq;
#endif
}

inline void SeqlockEndWrite(volatile uint32_t& seqlock) noexcept {
#if defined(_WIN32)
    MemoryBarrier();
    ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&seqlock));
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::atomic_ref<uint32_t> ref(const_cast<uint32_t&>(seqlock));
    ref.fetch_add(1, std::memory_order_acq_rel);
#endif
}

inline uint32_t SeqlockBeginRead(const volatile uint32_t& seqlock) noexcept {
#if defined(_WIN32)
    // Readers map the section with FILE_MAP_READ, so an interlocked RMW (even
    // one that writes the same value) faults on the read-only view. Aligned
    // 32-bit loads are atomic on supported Windows targets; the full barrier
    // supplies the acquire ordering required by the seqlock protocol.
    const uint32_t seq = seqlock;
    MemoryBarrier();
    return seq;
#else
    std::atomic_ref<uint32_t> ref(const_cast<uint32_t&>(seqlock));
    uint32_t seq = ref.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_acquire);
    return seq;
#endif
}

inline bool SeqlockValidateRead(const volatile uint32_t& seqlock, uint32_t startSeq) noexcept {
#if defined(_WIN32)
    MemoryBarrier();
    const uint32_t currentSeq = seqlock;
    return (startSeq == currentSeq) && !(startSeq & 1);
#else
    std::atomic_thread_fence(std::memory_order_acquire);
    std::atomic_ref<uint32_t> ref(const_cast<uint32_t&>(seqlock));
    uint32_t currentSeq = ref.load(std::memory_order_acquire);
    return (startSeq == currentSeq) && !(startSeq & 1);
#endif
}

// Write/Read Helpers for Full Staged Buffer
inline void PublishStagedBufferToWire(
    volatile LexiconWireHeader* wire,
    const uint8_t* stagedBuffer,
    size_t bufferSize = WIRE_TOTAL_SIZE) noexcept {
    if (!wire || !stagedBuffer || bufferSize < WIRE_TOTAL_SIZE) {
        return;
    }
    // 1. Begin write: seqlock goes odd
    SeqlockBeginWrite(wire->seqlock);

    // 2. Copy header prefix before seqlock (magic, abiVersion, headerSizeBytes, totalSizeBytes)
    constexpr size_t prefixSize = offsetof(LexiconWireHeader, seqlock);
    auto* dstBytes = const_cast<uint8_t*>(reinterpret_cast<volatile uint8_t*>(wire));
    std::memcpy(dstBytes, stagedBuffer, prefixSize);

    // 3. Copy remainder of wire (generation, crc32, flags, offsets, payload)
    constexpr size_t remainderOffset = offsetof(LexiconWireHeader, generation);
    const size_t copySize = bufferSize - remainderOffset;
    std::memcpy(dstBytes + remainderOffset, stagedBuffer + remainderOffset, copySize);

    // 4. End write: seqlock goes even
    SeqlockEndWrite(wire->seqlock);
}

inline bool ReadWireToLocalBuffer(
    const volatile LexiconWireHeader* wire,
    uint8_t* localBuffer,
    size_t bufferSize = WIRE_TOTAL_SIZE,
    int maxRetries = 3) noexcept {
    if (!wire || !localBuffer || bufferSize < WIRE_TOTAL_SIZE) {
        return false;
    }
    const auto* srcBytes = const_cast<const uint8_t*>(reinterpret_cast<const volatile uint8_t*>(wire));

    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        uint32_t startSeq = SeqlockBeginRead(wire->seqlock);
        if (startSeq & 1) { // Writing in progress
#if defined(_WIN32)
            SwitchToThread();
#else
            std::this_thread::yield();
#endif
            continue;
        }

        std::memcpy(localBuffer, srcBytes, WIRE_TOTAL_SIZE);

        if (SeqlockValidateRead(wire->seqlock, startSeq)) {
            auto* localHeader = reinterpret_cast<LexiconWireHeader*>(localBuffer);
            if (localHeader->magic != WIRE_MAGIC) {
                return false;
            }
            localHeader->seqlock = startSeq;
            return true;
        }
#if defined(_WIN32)
        SwitchToThread();
#else
        std::this_thread::yield();
#endif
    }
    return false;
}

// Cache Identity Tuple for TSF DLL
struct LexiconSnapshotIdentity {
    uint32_t sharedStateEpoch = 0;
    uint64_t wireGeneration = 0;
    uint32_t wireCrc32 = 0;

    bool operator==(const LexiconSnapshotIdentity& other) const noexcept {
        return sharedStateEpoch == other.sharedStateEpoch &&
               wireGeneration == other.wireGeneration &&
               wireCrc32 == other.wireCrc32;
    }
    bool operator!=(const LexiconSnapshotIdentity& other) const noexcept {
        return !(*this == other);
    }
};

// CRC32 IEEE 802.3 Helper
[[nodiscard]] uint32_t ComputeCrc32(const void* data, size_t size) noexcept;

// Unicode Comparison & Scalar Counting
[[nodiscard]] int CompareUtf16ByUnicodeScalar(std::u16string_view a, std::u16string_view b) noexcept;
[[nodiscard]] size_t CountUnicodeScalars(std::u16string_view str) noexcept;

// Wide / UTF-16 Conversion Helpers
[[nodiscard]] std::u16string WStringToU16(const std::wstring& wstr);
[[nodiscard]] std::wstring U16ToWString(std::u16string_view u16);

// View of a Validated Wire Mapping
struct LexiconWireView {
    const LexiconWireHeader* header = nullptr;
    const uint16_t* exclusionsBuf = nullptr;
    size_t exclusionsUnits = 0;
    size_t exclusionsRowCount = 0;
    const uint16_t* userDictBuf = nullptr;
    size_t userDictUnits = 0;
    const DictWordEntryWire* indexEntries = nullptr;
    size_t wordCount = 0;
};

// Serialization Parameters
struct LexiconWireSerializeParams {
    std::vector<std::u16string> spellExclusions;
    std::vector<std::u16string> userDictionary;
    uint64_t generation = 0;
    bool spellSuggestEnabled = true;
};

class LexiconWireSerializer {
public:
    [[nodiscard]] static bool Serialize(
        const LexiconWireSerializeParams& params,
        uint8_t* outBuffer,
        size_t bufferSize,
        std::string* outError = nullptr);

    [[nodiscard]] static bool Serialize(
        const std::vector<std::wstring>& spellExclusions,
        const std::vector<std::wstring>& userDictionary,
        uint64_t generation,
        bool spellSuggestEnabled,
        uint8_t* outBuffer,
        size_t bufferSize,
        std::string* outError = nullptr);
};

class LexiconWireDeserializer {
public:
    [[nodiscard]] static bool ValidateAndInspect(
        const uint8_t* wireBuffer,
        size_t bufferSize,
        LexiconWireView& outView,
        std::string* outError = nullptr);

    [[nodiscard]] static bool Deserialize(
        const uint8_t* wireBuffer,
        size_t bufferSize,
        std::vector<std::u16string>& outExclusions,
        std::vector<std::u16string>& outUserDict,
        uint64_t* outGeneration = nullptr,
        bool* outSpellSuggestEnabled = nullptr,
        std::string* outError = nullptr);

    [[nodiscard]] static bool Deserialize(
        const uint8_t* wireBuffer,
        size_t bufferSize,
        std::vector<std::wstring>& outExclusions,
        std::vector<std::wstring>& outUserDict,
        uint64_t* outGeneration = nullptr,
        bool* outSpellSuggestEnabled = nullptr,
        std::string* outError = nullptr);
};

} // namespace NextKey::Wire

#pragma pack(pop)
