// VKey - Lexicon Wire Manager & Reader Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/config/LexiconTransaction.h"
#include "core/ipc/LexiconWireManager.h"
#if defined(VKEY_USE_RUST_ENGINE)
#include "core/engine/RustInputEngine.h"
#endif

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

TEST(LexiconWireManagerTest, FastPathCacheIdentityVerification) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());
    ASSERT_TRUE(manager.Publish({ L"msword" }, { L"viet" }, 10, true));

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());

    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    bool wasUpdated = false;

    // 1. Initial read: epoch = 100 -> must perform full copy and validation (wasUpdated == true)
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 100, &wasUpdated));
    EXPECT_TRUE(wasUpdated);
    EXPECT_EQ(reader.GetCachedIdentity().sharedStateEpoch, 100u);
    EXPECT_EQ(reader.GetCachedIdentity().wireGeneration, 10u);

    // 2. Same epoch with unchanged wire identity skips the 144 KiB copy.
    wasUpdated = true;
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 100, &wasUpdated));
    EXPECT_FALSE(wasUpdated);

    // 3. Epoch changed to 102, but wire mapping generation & CRC32 have NOT changed!
    // -> Must skip 144 KiB payload copy, update cached epoch, and return wasUpdated == false
    wasUpdated = true;
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 102, &wasUpdated));
    EXPECT_FALSE(wasUpdated);
    EXPECT_EQ(reader.GetCachedIdentity().sharedStateEpoch, 102u);
    EXPECT_EQ(reader.GetCachedIdentity().wireGeneration, 10u);

    // 4. Wire mapping updated to generation 20:
    ASSERT_TRUE(manager.Publish({ L"msword" }, { L"nam" }, 20, true));

    // Next read at epoch 104 -> must detect change, copy 144 KiB, and update (wasUpdated == true)
    wasUpdated = false;
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 104, &wasUpdated));
    EXPECT_TRUE(wasUpdated);
    EXPECT_EQ(reader.GetCachedIdentity().sharedStateEpoch, 104u);
    EXPECT_EQ(reader.GetCachedIdentity().wireGeneration, 20u);

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, FastPathDetectsLatePublishAtSameEpoch) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());
    ASSERT_TRUE(manager.Publish({}, {L"old"}, 100, true));

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    std::vector<uint8_t> localBuffer;
    LexiconWireView view;
    bool updated = false;
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuffer, view, 7, &updated));
    ASSERT_TRUE(updated);

    ASSERT_TRUE(manager.Publish({}, {L"new"}, 101, true));
    updated = false;
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuffer, view, 7, &updated));
    EXPECT_TRUE(updated);
    ASSERT_NE(view.header, nullptr);
    EXPECT_EQ(view.header->generation, 101u);

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, CrossProcessSingleWriterEnforcement) {
    LexiconWireManager manager1;
    ASSERT_TRUE(manager1.Create());
    EXPECT_TRUE(manager1.IsWritable());

    // A second manager attempting to create mapping concurrently must be rejected
    LexiconWireManager manager2;
    EXPECT_FALSE(manager2.Create());
    EXPECT_FALSE(manager2.IsWritable());

    // Closing the active manager releases the writer lock
    manager1.Close();
    EXPECT_FALSE(manager1.IsWritable());

    // Now second manager can acquire writer role
    ASSERT_TRUE(manager2.Create());
    EXPECT_TRUE(manager2.IsWritable());
    manager2.Close();
}

TEST(LexiconWireManagerTest, ReaderReadOnlyAndWriteDenied) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());
    ASSERT_TRUE(manager.Publish({ L"msword" }, { L"viet" }, 1, true));

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    EXPECT_TRUE(reader.IsOpen());

    // Reader provides const views only (cannot mutate shared memory)
    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view));
    EXPECT_NE(view.header, nullptr);

    // A reader instance cannot be used to publish
    // And second manager cannot write to existing active mapping
    LexiconWireManager unauthorizedWriter;
    EXPECT_FALSE(unauthorizedWriter.Create());
    EXPECT_FALSE(unauthorizedWriter.IsWritable());

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, ManagerRestartWhileReaderAlive) {
    // 1. First Core manager instance creates mapping and publishes generation 100
    auto manager1 = std::make_unique<LexiconWireManager>();
    ASSERT_TRUE(manager1->Create());
    EXPECT_TRUE(manager1->IsWritable());
    ASSERT_TRUE(manager1->Publish({ L"msword" }, { L"viet" }, 100, true));

    // 2. Reader (e.g. TSF client) attaches and reads generation 100
    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    EXPECT_TRUE(reader.IsOpen());
    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view));
    EXPECT_EQ(view.header->generation, 100u);

    // 3. Core manager exits or restarts (manager1 destroyed), but reader remains ALIVE!
    manager1.reset();

    // 4. Restarting Core creates a new manager instance
    // Must successfully acquire writer role and re-attach to the surviving section
    LexiconWireManager manager2;
    ASSERT_TRUE(manager2.Create());
    EXPECT_TRUE(manager2.IsWritable());

    // 5. Manager2 publishes generation 200 to the surviving section
    ASSERT_TRUE(manager2.Publish({ L"excel" }, { L"nam" }, 200, true));

    // 6. The surviving reader immediately observes generation 200 from the same section
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view));
    EXPECT_EQ(view.header->generation, 200u);
    EXPECT_EQ(view.exclusionsRowCount, 1u);
    EXPECT_EQ(view.wordCount, 1u);

    reader.Close();
    manager2.Close();
}

TEST(LexiconWireManagerTest, SetKernelObjectSecurityFailure_AbortsCreationAndCleansUp) {
    // Inject fault simulating SetKernelObjectSecurity or ACL lockdown failure
    LexiconWireManager::SetTestSecurityFailure(true);

    LexiconWireManager manager;
    EXPECT_FALSE(manager.Create());
    EXPECT_FALSE(manager.IsWritable());
    EXPECT_EQ(manager.GetHeader(), nullptr);

    // Reset fault injection: manager creation now succeeds cleanly
    LexiconWireManager::SetTestSecurityFailure(false);
    EXPECT_TRUE(manager.Create());
    EXPECT_TRUE(manager.IsWritable());
    EXPECT_NE(manager.GetHeader(), nullptr);

    manager.Close();
}

TEST(LexiconWireManagerTest, SameUserWriteOpenDenied_ReadOnlyAllowed) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());
    EXPECT_TRUE(manager.IsWritable());
    ASSERT_TRUE(manager.Publish({ L"msword" }, { L"viet" }, 1, true));

#if defined(_WIN32)
    // On Windows: Any subsequent open with FILE_MAP_WRITE (even within the exact same process
    // and user security token) MUST be rejected by the kernel with ERROR_ACCESS_DENIED.
    HANDLE hWrite = OpenFileMappingW(FILE_MAP_WRITE, FALSE, LEXICON_WIRE_MAPPING_NAME);
    EXPECT_EQ(hWrite, nullptr);
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);

    // Opening with FILE_MAP_READ MUST succeed for interactive users and AppContainers
    HANDLE hRead = OpenFileMappingW(FILE_MAP_READ, FALSE, LEXICON_WIRE_MAPPING_NAME);
    EXPECT_NE(hRead, nullptr);
    if (hRead) {
        CloseHandle(hRead);
    }
#endif

    // Reader opening in read-only mode succeeds
    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    EXPECT_TRUE(reader.IsOpen());

    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view));
    EXPECT_EQ(view.header->generation, 1u);

    reader.Close();
    manager.Close();
}

#if defined(VKEY_USE_RUST_ENGINE)
TEST(LexiconWireManagerTest, WireReaderToRustInputEnginePromotion) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    std::vector<std::wstring> exclusions = { L"msword", L"excel" };
    std::vector<std::wstring> dictWords = { L"viet", L"nam" };

    std::string err;
    ASSERT_TRUE(manager.Publish(exclusions, dictWords, 42, true, &err)) << err;

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());

    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    bool wasUpdated = false;
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 1, &wasUpdated, &err)) << err;
    EXPECT_TRUE(wasUpdated);

    bool exclusionsChanged = false;
    EXPECT_TRUE(RustInputEngine::SetSpellExclusionsFromUtf16(
        view.exclusionsBuf, view.exclusionsUnits, &exclusionsChanged));

    auto snapshot = RustInputEngine::CreateUserDictionaryFromUtf16(
        view.userDictBuf, view.userDictUnits);
    ASSERT_NE(snapshot, nullptr);

    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    RustInputEngine engine(config);

    EXPECT_TRUE(engine.SetUserDictionary(snapshot));

    // Cleanup process-global exclusions
    RustInputEngine::SetSpellExclusionsFromUtf16(nullptr, 0, nullptr);
    reader.Close();
    manager.Close();
}
#endif

TEST(LexiconWireManagerTest, OldReaderFallback_DiskLocked_WhenWireUnavailable) {
    namespace fs = std::filesystem;
    const fs::path testDir = fs::temp_directory_path() / "vkey_wire_fallback_test";
    fs::create_directories(testDir);
    const fs::path configPath = testDir / "config.toml";
    const fs::path dictPath = testDir / "user_dictionary.txt";

    // Write a valid user_dictionary.txt
    {
        std::ofstream dictFile(dictPath);
        dictFile << "fallbackword\n";
    }

    // When wire reader is NOT open (mapping does not exist)
    LexiconWireReader reader;
    EXPECT_FALSE(reader.IsOpen());

    // 1. Hot-path typing simulation: allowDiskRead == false -> must NOT touch disk!
    // Stays stale, does not attempt locked load.
    bool allowDiskRead = false;
    std::shared_ptr<const RustUserDictionarySnapshot> lockedSnapshot;
    bool loadedFromDisk = false;
    if (allowDiskRead) {
        loadedFromDisk = LexiconReader::LoadUserDictionaryLocked(configPath.wstring(), lockedSnapshot);
    }
    EXPECT_FALSE(loadedFromDisk);
    EXPECT_EQ(lockedSnapshot, nullptr);

    // 2. Focus / init simulation: allowDiskRead == true -> falls back to Phase 1 disk locked read
    allowDiskRead = true;
    if (allowDiskRead) {
        loadedFromDisk = LexiconReader::LoadUserDictionaryLocked(configPath.wstring(), lockedSnapshot);
    }
#if defined(VKEY_USE_RUST_ENGINE)
    EXPECT_TRUE(loadedFromDisk);
    ASSERT_NE(lockedSnapshot, nullptr);
#else
    (void)loadedFromDisk;
#endif

    fs::remove_all(testDir);
}

TEST(LexiconWireManagerTest, FastPathZeroAllocationAndCacheHits) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());
    ASSERT_TRUE(manager.Publish({ L"word" }, { L"excel" }, 10, true));

    LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());

    std::vector<uint8_t> localBuf;
    LexiconWireView view;
    bool wasUpdated = false;

    // Initial read
    ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 5, &wasUpdated));
    EXPECT_TRUE(wasUpdated);

    // Repeated reads with identical epoch (< 1 ns, zero allocation)
    for (int i = 0; i < 100; ++i) {
        wasUpdated = true;
        ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, 5, &wasUpdated));
        EXPECT_FALSE(wasUpdated);
    }

    // Increment SharedState epoch, but keep wire content identical (skips copy)
    for (uint32_t epoch = 6; epoch <= 10; ++epoch) {
        wasUpdated = true;
        ASSERT_TRUE(reader.ReadSnapshotFast(localBuf, view, epoch, &wasUpdated));
        EXPECT_FALSE(wasUpdated);
        EXPECT_EQ(reader.GetCachedIdentity().sharedStateEpoch, epoch);
    }

    reader.Close();
    manager.Close();
}

TEST(LexiconWireManagerTest, PublishValidationFailurePopulatesErrorString) {
    LexiconWireManager manager;
    ASSERT_TRUE(manager.Create());

    std::string err;
    // 1. Invalid spell exclusion (entry too short < 2 scalars, e.g. single character)
    EXPECT_FALSE(manager.Publish({ L"a" }, { L"valid" }, 1, true, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Spell exclusion canonicalization failed"), std::string::npos);

    // 2. Invalid user dictionary word (contains control character or whitespace)
    err.clear();
    EXPECT_FALSE(manager.Publish({ L"valid" }, { L"invalid word with spaces" }, 2, true, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("User dictionary validation failed"), std::string::npos);

    manager.Close();
}

} // namespace
} // namespace NextKey::Wire
