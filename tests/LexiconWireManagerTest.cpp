// VKey - Lexicon Wire Manager & Reader Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/config/LexiconTransaction.h"
#include "core/ipc/LexiconWireManager.h"

namespace NextKey::Wire {
namespace {

TEST(LexiconWireManagerTest, ManagerLifecycleAndCreation) {
    LexiconWireManager manager;
    EXPECT_FALSE(manager.IsWritable());
    EXPECT_EQ(manager.GetHeader(), nullptr);

    ASSERT_TRUE(manager.Create());
    EXPECT_TRUE(manager.IsWritable());
    EXPECT_NE(manager.GetHeader(), nullptr);

    manager.Close();
    EXPECT_FALSE(manager.IsWritable());
    EXPECT_EQ(manager.GetHeader(), nullptr);
}

TEST(LexiconWireManagerTest, ReaderLifecycleBeforeAndAfterManager) {
    LexiconWireReader reader;
    // Reader cannot open when manager has not created mapping
    EXPECT_FALSE(reader.Open());
    EXPECT_FALSE(reader.IsOpen());

    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    // Reader successfully opens existing mapping
    ASSERT_TRUE(reader.Open());
    EXPECT_TRUE(reader.IsOpen());

    reader.Close();
    EXPECT_FALSE(reader.IsOpen());

    manager.Close();
}

TEST(LexiconWireManagerTest, PublishAndReadSnapshotRoundTrip) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());
    manager.SetWireGeneration(5000ULL);

    std::vector<std::wstring> exclusions = { L"msword", L"excel" };
    std::vector<std::wstring> dictWords = { L"vi\u1ec7t", L"nam", L"h\u00e0", L"b\u1ea3o" };

    std::string err;
    ASSERT_TRUE(manager.Publish(exclusions, dictWords, true, &err)) << err;
    EXPECT_EQ(manager.GetWireGeneration(), 5001ULL);

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());

    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view, &err)) << err;

    EXPECT_EQ(view.header->generation, 5001ULL);
    EXPECT_EQ(view.wordCount, 4u);
    EXPECT_EQ(view.exclusionsRowCount, 2u);
    EXPECT_TRUE(view.header->flags & LexiconWireFlags::SPELL_SUGGEST_ENABLED);

    // Verify reader cached identity
    const auto& id = reader.GetCachedIdentity();
    EXPECT_EQ(id.wireGeneration, 5001ULL);
    EXPECT_EQ(id.wireCrc32, view.header->crc32);

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, CachedIdentityEpochTracking) {
    LexiconWireReader reader;
    LexiconSnapshotIdentity id;
    id.sharedStateEpoch = 42;
    id.wireGeneration = 100;
    id.wireCrc32 = 0x12345678;

    reader.SetCachedIdentity(id);
    EXPECT_FALSE(reader.HasEpochChanged(42));
    EXPECT_TRUE(reader.HasEpochChanged(44));

    reader.ResetCachedIdentity();
    EXPECT_TRUE(reader.HasEpochChanged(42));
}

TEST(LexiconWireManagerTest, PersistentGenerationAntiAba) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    // Simulate restoring wireGeneration = 123456789ULL from config.toml on Core startup
    manager.SetWireGeneration(123456789ULL);
    EXPECT_EQ(manager.GetWireGeneration(), 123456789ULL);

    // Incrementing monotonically
    uint64_t nextGen = manager.IncrementWireGeneration();
    EXPECT_EQ(nextGen, 123456790ULL);
    EXPECT_EQ(manager.GetWireGeneration(), 123456790ULL);

    ASSERT_TRUE(manager.Publish({ L"test" }, { L"word" }, nextGen, true));

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view));
    EXPECT_EQ(view.header->generation, 123456790ULL);

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, SingleWriterEnforcement) {
    LexiconWireManager manager;
    std::string err;
    // Cannot publish without Create()
    EXPECT_FALSE(manager.Publish({ L"test" }, { L"word" }, true, &err));
    EXPECT_NE(err.find("not created or writable"), std::string::npos);
}

TEST(LexiconWireManagerTest, MockStorageFaultInjection) {
    alignas(8) uint8_t mockMem[WIRE_TOTAL_SIZE]{};

    LexiconWireManager manager;
    manager.SetMockStorage(mockMem, sizeof(mockMem));
    ASSERT_TRUE(manager.Publish({ L"sample" }, { L"hello" }, 1, true));

    LexiconWireReader reader;
    reader.SetMockStorage(mockMem, sizeof(mockMem));

    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    std::string err;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view, &err)) << err;

    // Corrupt memory in mock storage
    mockMem[USER_DICT_POOL_OFFSET] ^= 0xFF;

    // Read must fail with CRC mismatch
    EXPECT_FALSE(reader.ReadSnapshot(localBuf, view, &err));
    EXPECT_NE(err.find("CRC32"), std::string::npos);
}

TEST(LexiconWireManagerTest, ConcurrentManagerWriterAndReader) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    // Initial publish
    ASSERT_TRUE(manager.Publish({ L"init" }, { L"word" }, 10, true));

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> successfulReads{0};

    // Writer thread
    std::thread writer([&]() {
        uint64_t iter = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            uint64_t gen = (iter % 2 == 0) ? 100ULL : 200ULL;
            bool enabled = (iter % 2 == 0);
            manager.Publish({ L"excl" }, { L"dict" }, gen, enabled);
            iter++;
            std::this_thread::yield();
        }
    });

    // Reader thread
    std::thread readerThread([&]() {
        std::vector<uint8_t> localBuf;
        while (!stop.load(std::memory_order_relaxed) && successfulReads.load() < 200) {
            LexiconWireView view;
            if (reader.ReadSnapshot(localBuf, view)) {
                // Invariant: generation must be 10, 100, or 200 (never torn)
                uint64_t g = view.header->generation;
                EXPECT_TRUE(g == 10 || g == 100 || g == 200);
                successfulReads.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    while (successfulReads.load() < 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);

    writer.join();
    readerThread.join();

    EXPECT_GE(successfulReads.load(), 200u);

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, PublishWithNormalizationAndValidation) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    // 1. Un-normalized inputs should be canonicalized (trimmed, lowercased, NFC)
    std::vector<std::wstring> rawExclusions = { L"  MSWORD  " };
    std::vector<std::wstring> rawDict = { L"  Vi\u1ec6T  " };

    std::string err;
    ASSERT_TRUE(manager.Publish(rawExclusions, rawDict, 1, true, &err)) << err;

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view, &err)) << err;

    EXPECT_EQ(view.exclusionsRowCount, 1u);
    EXPECT_EQ(view.wordCount, 1u);

    // Verify exclusions buffer contains normalized lowercase "msword\n"
    std::u16string exclStr(reinterpret_cast<const char16_t*>(view.exclusionsBuf), view.exclusionsUnits);
    EXPECT_EQ(exclStr, u"msword\n");

    // Verify dict buffer contains normalized lowercase NFC "việt\n"
    std::u16string dictStr(reinterpret_cast<const char16_t*>(view.userDictBuf), view.userDictUnits);
    EXPECT_EQ(dictStr, u"vi\u1ec7t\n");

    // 2. Invalid exclusion (single character < 2 scalars)
    EXPECT_FALSE(manager.Publish({ L"a" }, { L"valid" }, 2, true, &err));
    EXPECT_NE(err.find("canonicalization failed"), std::string::npos);

    // 3. Invalid user dictionary word (contains internal space)
    EXPECT_FALSE(manager.Publish({ L"valid" }, { L"two words" }, 3, true, &err));
    EXPECT_NE(err.find("validation failed"), std::string::npos);

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, LexiconWriterPublishWireMappingIntegration) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    // Register wire manager with LexiconWriter
    LexiconWriter::SetWireManager(&manager);
    EXPECT_EQ(LexiconWriter::GetWireManager(), &manager);

    std::string err;
    ASSERT_TRUE(LexiconWriter::PublishWireMapping({ L"word" }, { L"excel" }, 50, true, &err)) << err;

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view, &err)) << err;
    EXPECT_EQ(view.header->generation, 50u);

    // Test custom test publisher hook
    bool hookCalled = false;
    LexiconWriter::SetTestWirePublisher([&](const auto&, const auto&, uint64_t gen, bool, std::string*) {
        hookCalled = true;
        return gen == 999;
    });

    EXPECT_TRUE(LexiconWriter::PublishWireMapping({ L"word" }, { L"excel" }, 999, true, &err));
    EXPECT_TRUE(hookCalled);

    // Reset hooks
    LexiconWriter::SetTestWirePublisher(nullptr);
    LexiconWriter::SetWireManager(nullptr);
    EXPECT_EQ(LexiconWriter::GetWireManager(), nullptr);

    // Publishing without manager fails
    EXPECT_FALSE(LexiconWriter::PublishWireMapping({ L"word" }, { L"excel" }, 1000, true, &err));
    EXPECT_NE(err.find("not configured"), std::string::npos);

    reader.Close();
    manager.Close();
}

} // namespace
} // namespace NextKey::Wire
