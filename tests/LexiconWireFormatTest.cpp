// VKey - Lexicon Wire Format Unit Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/ipc/LexiconWireFormat.h"
#include "core/engine/RustEngineLoader.h"
#include "core/engine/RustInputEngine.h"
#include <vkey_engine.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace NextKey::Wire {
namespace {

TEST(LexiconWireFormatTest, WireHeaderOffsetAndAlignment) {
    EXPECT_EQ(sizeof(LexiconWireHeader), 128u);
    EXPECT_EQ(alignof(LexiconWireHeader), 8u);

    EXPECT_EQ(sizeof(DictWordEntryWire), 8u);
    EXPECT_EQ(alignof(DictWordEntryWire), 4u);

    // Verify all 19 header offsets
    EXPECT_EQ(offsetof(LexiconWireHeader, magic), 0x00u);
    EXPECT_EQ(offsetof(LexiconWireHeader, abiVersion), 0x04u);
    EXPECT_EQ(offsetof(LexiconWireHeader, headerSizeBytes), 0x06u);
    EXPECT_EQ(offsetof(LexiconWireHeader, totalSizeBytes), 0x08u);
    EXPECT_EQ(offsetof(LexiconWireHeader, seqlock), 0x0Cu);
    EXPECT_EQ(offsetof(LexiconWireHeader, generation), 0x10u);
    EXPECT_EQ(offsetof(LexiconWireHeader, crc32), 0x18u);
    EXPECT_EQ(offsetof(LexiconWireHeader, flags), 0x1Cu);
    EXPECT_EQ(offsetof(LexiconWireHeader, payloadOffsetBytes), 0x20u);
    EXPECT_EQ(offsetof(LexiconWireHeader, payloadSizeBytes), 0x24u);
    EXPECT_EQ(offsetof(LexiconWireHeader, exclusionsOffsetBytes), 0x28u);
    EXPECT_EQ(offsetof(LexiconWireHeader, exclusionsLengthBytes), 0x2Cu);
    EXPECT_EQ(offsetof(LexiconWireHeader, exclusionsRowCount), 0x30u);
    EXPECT_EQ(offsetof(LexiconWireHeader, userDictOffsetBytes), 0x34u);
    EXPECT_EQ(offsetof(LexiconWireHeader, userDictLengthBytes), 0x38u);
    EXPECT_EQ(offsetof(LexiconWireHeader, userDictWordCount), 0x3Cu);
    EXPECT_EQ(offsetof(LexiconWireHeader, userDictUtf16Units), 0x40u);
    EXPECT_EQ(offsetof(LexiconWireHeader, exclusionsUtf16Units), 0x44u);
    EXPECT_EQ(offsetof(LexiconWireHeader, reserved), 0x48u);

    // Verify index table entry offsets
    EXPECT_EQ(offsetof(DictWordEntryWire, offsetUnits), 0x00u);
    EXPECT_EQ(offsetof(DictWordEntryWire, lengthUtf16Units), 0x04u);
    EXPECT_EQ(offsetof(DictWordEntryWire, scalarCount), 0x06u);

    // Verify layout constants and headroom
    EXPECT_EQ(WIRE_TOTAL_SIZE, 147456u);
    EXPECT_EQ(WIRE_USED_MAX_BYTES, 141568u);
    EXPECT_EQ(WIRE_HEADROOM_BYTES, 5888u);
    EXPECT_EQ(WIRE_TOTAL_SIZE - WIRE_USED_MAX_BYTES, WIRE_HEADROOM_BYTES);
}

TEST(LexiconWireFormatTest, Crc32StandardTestVector) {
    const char testStr[] = "123456789";
    uint32_t crc = ComputeCrc32(testStr, 9);
    EXPECT_EQ(crc, 0xCBF43926u);

    EXPECT_EQ(ComputeCrc32(nullptr, 0), 0x00000000u);
    EXPECT_EQ(ComputeCrc32(testStr, 0), 0x00000000u);
}

TEST(LexiconWireFormatTest, UnicodeScalarComparatorParity) {
    // 1. Equal strings
    EXPECT_EQ(CompareUtf16ByUnicodeScalar(u"hanoi", u"hanoi"), 0);

    // 2. Simple ASCII prefix
    EXPECT_EQ(CompareUtf16ByUnicodeScalar(u"abc", u"abcd"), -1);
    EXPECT_EQ(CompareUtf16ByUnicodeScalar(u"abcd", u"abc"), 1);

    // 3. UTF-16 BMP vs Astral Plane (Surrogate pairs)
    // 'ợ' is U+1EE3 (BMP, 1 code unit in UTF-16: 0x1EE3).
    // Emoji 🚀 is U+1F680 (Astral, surrogate pair: 0xD83D 0xDE80).
    // In raw UTF-16 code units: 0xD83D < 0x1EE3 (emoji appears smaller than 'ợ').
    // In UTF-8 / Unicode Scalar value:
    //   'ợ' = U+1EE3 -> UTF-8 bytes: \xE1\xBB\xA3
    //   🚀 = U+1F680 -> UTF-8 bytes: \xF0\x9F\x9A\x80
    //   \xE1 < \xF0, so 'ợ' MUST be smaller than 🚀!
    std::u16string strO = u"a\u1EE3b";
    std::u16string strRocket = u"a\U0001F680b";
    std::string utf8O = "a\xE1\xBB\xA3" "b";
    std::string utf8Rocket = "a\xF0\x9F\x9A\x80" "b";

    int u8Cmp = utf8O.compare(utf8Rocket);
    EXPECT_LT(u8Cmp, 0);

    int scalarCmp = CompareUtf16ByUnicodeScalar(strO, strRocket);
    EXPECT_LT(scalarCmp, 0);
    EXPECT_EQ((scalarCmp < 0), (u8Cmp < 0));

    // Reverse comparison
    EXPECT_GT(CompareUtf16ByUnicodeScalar(strRocket, strO), 0);

    // 4. Scalar counting
    EXPECT_EQ(CountUnicodeScalars(u"abc"), 3u);
    EXPECT_EQ(CountUnicodeScalars(u"a\u1EE3b"), 3u);
    EXPECT_EQ(CountUnicodeScalars(u"a\U0001F680b"), 3u);
    EXPECT_EQ(CountUnicodeScalars(u"\U0001F680\U0001F600"), 2u);
}

TEST(LexiconWireFormatTest, EmptySerializationRoundTrip) {
    std::vector<uint8_t> buffer(WIRE_TOTAL_SIZE, 0);
    LexiconWireSerializeParams params;
    params.generation = 42;
    params.spellSuggestEnabled = false;

    std::string err;
    ASSERT_TRUE(LexiconWireSerializer::Serialize(params, buffer.data(), buffer.size(), &err)) << err;

    LexiconWireView view;
    ASSERT_TRUE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err)) << err;
    EXPECT_EQ(view.header->generation, 42u);
    EXPECT_EQ(view.header->exclusionsRowCount, 0u);
    EXPECT_EQ(view.header->userDictWordCount, 0u);
    EXPECT_EQ(view.exclusionsUnits, 0u);
    EXPECT_EQ(view.userDictUnits, 0u);
    EXPECT_FALSE(view.header->flags & LexiconWireFlags::SPELL_SUGGEST_ENABLED);

    std::vector<std::wstring> outExcl, outDict;
    uint64_t gen = 0;
    bool spellEnabled = true;
    ASSERT_TRUE(LexiconWireDeserializer::Deserialize(
        buffer.data(), buffer.size(), outExcl, outDict, &gen, &spellEnabled, &err)) << err;
    EXPECT_EQ(gen, 42u);
    EXPECT_FALSE(spellEnabled);
    EXPECT_TRUE(outExcl.empty());
    EXPECT_TRUE(outDict.empty());
}

TEST(LexiconWireFormatTest, StandardSerializationRoundTripAndSorting) {
    std::vector<uint8_t> buffer(WIRE_TOTAL_SIZE, 0);

    std::vector<std::wstring> exclusions = {
        L"github",
        L"microsoft",
        L"nexuskey"
    };

    // Intentionally pass unsorted and with duplicate to verify sorting & deduplication
    std::vector<std::wstring> dictWords = {
        L"vi\u1ec7t",
        L"nam",
        L"b\u1ea3o",
        L"nam",      // duplicate
        L"h\u00e0"
    };

    std::string err;
    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        exclusions, dictWords, 1001ULL, true, buffer.data(), buffer.size(), &err)) << err;

    LexiconWireView view;
    ASSERT_TRUE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err)) << err;
    EXPECT_EQ(view.header->generation, 1001ULL);
    EXPECT_EQ(view.header->exclusionsRowCount, 3u);
    EXPECT_EQ(view.header->userDictWordCount, 4u); // Deduplicated 5 -> 4
    EXPECT_TRUE(view.header->flags & LexiconWireFlags::SPELL_SUGGEST_ENABLED);

    std::vector<std::wstring> outExcl, outDict;
    uint64_t gen = 0;
    bool spellEnabled = false;
    ASSERT_TRUE(LexiconWireDeserializer::Deserialize(
        buffer.data(), buffer.size(), outExcl, outDict, &gen, &spellEnabled, &err)) << err;

    EXPECT_EQ(gen, 1001ULL);
    EXPECT_TRUE(spellEnabled);
    ASSERT_EQ(outExcl.size(), 3u);
    EXPECT_EQ(outExcl[0], L"github");
    EXPECT_EQ(outExcl[1], L"microsoft");
    EXPECT_EQ(outExcl[2], L"nexuskey");

    ASSERT_EQ(outDict.size(), 4u);
    // Monotonically sorted order
    EXPECT_EQ(outDict[0], L"b\u1ea3o");
    EXPECT_EQ(outDict[1], L"h\u00e0");
    EXPECT_EQ(outDict[2], L"nam");
    EXPECT_EQ(outDict[3], L"vi\u1ec7t");
}

TEST(LexiconWireFormatTest, CrcCorruptionDetection) {
    std::vector<uint8_t> buffer(WIRE_TOTAL_SIZE, 0);
    std::vector<std::wstring> exclusions = { L"sample" };
    std::vector<std::wstring> dictWords = { L"test" };

    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        exclusions, dictWords, 555ULL, true, buffer.data(), buffer.size()));

    LexiconWireView view;
    std::string err;
    EXPECT_TRUE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err));

    // Flip 1 bit in payload (e.g. at user dictionary text pool offset 0x2900)
    buffer[USER_DICT_POOL_OFFSET] ^= 0x01;

    EXPECT_FALSE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err));
    EXPECT_NE(err.find("CRC32"), std::string::npos);
}

TEST(LexiconWireFormatTest, CapacityLimitsAndValidationFailures) {
    std::vector<uint8_t> buffer(WIRE_TOTAL_SIZE, 0);
    std::string err;

    // 1. Buffer too small
    EXPECT_FALSE(LexiconWireSerializer::Serialize(
        {}, {}, 1, true, buffer.data(), WIRE_TOTAL_SIZE - 1, &err));

    // 2. Too many exclusions (> 8)
    std::vector<std::wstring> tooManyExcl(9, L"word");
    EXPECT_FALSE(LexiconWireSerializer::Serialize(
        tooManyExcl, {}, 1, true, buffer.data(), buffer.size(), &err));

    // 3. Exclusion row too short (< 2 scalars)
    std::vector<std::wstring> shortExcl = { L"a" };
    EXPECT_FALSE(LexiconWireSerializer::Serialize(
        shortExcl, {}, 1, true, buffer.data(), buffer.size(), &err));

    // 4. Too many dict words (> 1024)
    std::vector<std::wstring> tooManyWords;
    for (int i = 0; i < 1025; ++i) {
        tooManyWords.push_back(L"w" + std::to_wstring(i));
    }
    EXPECT_FALSE(LexiconWireSerializer::Serialize(
        {}, tooManyWords, 1, true, buffer.data(), buffer.size(), &err));

    // 5. Dict word too long (> 64 scalars)
    std::wstring longWord(65, L'a');
    EXPECT_FALSE(LexiconWireSerializer::Serialize(
        {}, { longWord }, 1, true, buffer.data(), buffer.size(), &err));

    // 6. Unpaired surrogate in input
    LexiconWireSerializeParams badSurrogateParams;
    badSurrogateParams.userDictionary = { u"valid", std::u16string{u'a', char16_t(0xD800), u'b'} };
    EXPECT_FALSE(LexiconWireSerializer::Serialize(
        badSurrogateParams, buffer.data(), buffer.size(), &err));
    EXPECT_NE(err.find("surrogate"), std::string::npos);

    // 7. Corrupt magic in deserializer
    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        { L"ok" }, { L"word" }, 1, true, buffer.data(), buffer.size()));
    auto* header = reinterpret_cast<LexiconWireHeader*>(buffer.data());
    header->magic = 0x12345678;
    LexiconWireView view;
    EXPECT_FALSE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err));
    EXPECT_NE(err.find("magic"), std::string::npos);

    // 8. Deserializer detects mismatched scalarCount in index table
    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        { L"ok" }, { L"word" }, 1, true, buffer.data(), buffer.size()));
    auto* indexTable = reinterpret_cast<DictWordEntryWire*>(buffer.data() + USER_DICT_INDEX_OFFSET);
    indexTable[0].scalarCount = 2; // Corrupt scalarCount (within 1..64 bounds but != actual 4 scalars)
    // Recalculate CRC so CRC passes and index validation fails
    header = reinterpret_cast<LexiconWireHeader*>(buffer.data());
    header->crc32 = ComputeCrc32(buffer.data() + header->payloadOffsetBytes, header->payloadSizeBytes);
    EXPECT_FALSE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err));
    EXPECT_NE(err.find("scalarCount"), std::string::npos);
}

TEST(LexiconWireFormatTest, SeqlockWriterReaderConcurrency) {
    // Emulate shared memory using aligned buffer
    alignas(8) uint8_t sharedMem[WIRE_TOTAL_SIZE]{};
    auto* wireHeader = reinterpret_cast<volatile LexiconWireHeader*>(sharedMem);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> successfulReads{0};
    std::atomic<uint64_t> retryCount{0};

    // Staging buffers for writer
    std::vector<uint8_t> stage1(WIRE_TOTAL_SIZE, 0);
    std::vector<uint8_t> stage2(WIRE_TOTAL_SIZE, 0);

    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        { L"alpha", L"beta" }, { L"one", L"two" }, 100, true, stage1.data(), stage1.size()));
    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        { L"gamma", L"delta" }, { L"three", L"four" }, 200, false, stage2.data(), stage2.size()));

    // Publish initial valid stage
    PublishStagedBufferToWire(wireHeader, stage1.data(), WIRE_TOTAL_SIZE);

    // Writer thread
    std::thread writer([&]() {
        uint64_t gen = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint8_t* chosenStage = (gen % 2 == 0) ? stage1.data() : stage2.data();
            PublishStagedBufferToWire(wireHeader, chosenStage, WIRE_TOTAL_SIZE);
            gen++;
            std::this_thread::yield();
        }
    });

    // Reader thread
    std::thread reader([&]() {
        alignas(8) uint8_t localBuf[WIRE_TOTAL_SIZE];
        while (!stop.load(std::memory_order_relaxed) && successfulReads.load() < 200) {
            if (ReadWireToLocalBuffer(wireHeader, localBuf, WIRE_TOTAL_SIZE, 10)) {
                LexiconWireView view;
                std::string err;
                if (LexiconWireDeserializer::ValidateAndInspect(localBuf, WIRE_TOTAL_SIZE, view, &err)) {
                    // Invariant: generation must be either 100 or 200, never corrupted
                    EXPECT_TRUE(view.header->generation == 100 || view.header->generation == 200);
                    successfulReads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    FAIL() << "Torn or corrupted read observed: " << err;
                }
            } else {
                retryCount.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });

    // Wait for reader to complete 200 valid reads
    while (successfulReads.load() < 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);

    writer.join();
    reader.join();

    EXPECT_GE(successfulReads.load(), 200u);
}

TEST(LexiconWireFormatTest, FfiDirectRoundTripTest) {
    if (!RustInputEngine::LibraryAvailable()) {
        GTEST_SKIP() << "vkey_engine library is not available in test environment";
    }

    auto loadRes = LoadRustEngineLibrary();
    ASSERT_NE(loadRes.handle, nullptr);

    auto setSpellExcl = reinterpret_cast<bool (*)(const uint16_t*, size_t)>(
        dlsym(loadRes.handle, "vkey_engine_set_spell_exclusions_utf16"));
    auto createDict = reinterpret_cast<VKeyUserDictionary* (*)(const uint16_t*, size_t, uint32_t*, size_t*)>(
        dlsym(loadRes.handle, "vkey_user_dictionary_create_utf16"));
    auto destroyDict = reinterpret_cast<void (*)(VKeyUserDictionary*)>(
        dlsym(loadRes.handle, "vkey_user_dictionary_destroy"));

    ASSERT_NE(setSpellExcl, nullptr);
    ASSERT_NE(createDict, nullptr);
    ASSERT_NE(destroyDict, nullptr);

    // Prepare wire buffer
    std::vector<uint8_t> buffer(WIRE_TOTAL_SIZE, 0);
    std::vector<std::wstring> exclusions = { L"msword", L"excel" };
    std::vector<std::wstring> dictWords = { L"nexuskey", L"vietnamese" };

    ASSERT_TRUE(LexiconWireSerializer::Serialize(
        exclusions, dictWords, 12345ULL, true, buffer.data(), buffer.size()));

    LexiconWireView view;
    std::string err;
    ASSERT_TRUE(LexiconWireDeserializer::ValidateAndInspect(buffer.data(), buffer.size(), view, &err)) << err;

    // 1. Pass exclusions buffer directly to FFI without copies or allocations
    bool exclOk = setSpellExcl(view.exclusionsBuf, view.exclusionsUnits);
    EXPECT_TRUE(exclOk);

    // 2. Pass dictionary buffer directly to FFI without copies or allocations
    uint32_t status = 999;
    size_t errorLine = 999;
    VKeyUserDictionary* dict = createDict(view.userDictBuf, view.userDictUnits, &status, &errorLine);

    EXPECT_NE(dict, nullptr);
    EXPECT_EQ(status, VKEY_USER_DICTIONARY_OK);
    EXPECT_EQ(errorLine, 0u);

    if (dict) {
        destroyDict(dict);
    }

    // Reset exclusions back to empty
    setSpellExcl(nullptr, 0);
    CloseRustEngineLibrary(loadRes.handle);
}

} // namespace
} // namespace NextKey::Wire
