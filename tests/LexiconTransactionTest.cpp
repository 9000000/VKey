// VKey - Lexicon Transaction and Recovery Tests
// SPDX-License-Identifier: GPL-3.0-only

#include "core/config/LexiconTransaction.h"
#include "core/ipc/LexiconWireManager.h"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace NextKey {
namespace {

class LexiconTransactionTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = std::filesystem::temp_directory_path() /
                   ("vkey_lexicon_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testDir_);
        configPath_ = testDir_ / "config.toml";
        dictPath_ = testDir_ / "user_dictionary.txt";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir_, ec);
    }

    void WriteFile(const std::filesystem::path& path, const std::string& content) {
        std::ofstream file(path, std::ios::binary);
        file << content;
    }

    std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return {};
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    }

    std::filesystem::path testDir_;
    std::filesystem::path configPath_;
    std::filesystem::path dictPath_;
};

} // namespace

TEST_F(LexiconTransactionTest, JournalSerializationAndDeserialization) {
    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = true;
    record.dictExistedBefore = false;
    record.oldGeneration = 12;
    record.newGeneration = 13;
    record.oldConfigHash = "0123456789abcdef";
    record.newConfigHash = "fedcba9876543210";
    record.oldDictHash = "";
    record.newDictHash = "aabbccddeeff0011";
    record.configBakPath = "/tmp/test/config.toml.bak";
    record.dictBakPath = "/tmp/test/user_dictionary.txt.bak";
    record.configTmpPath = "/tmp/test/config.toml.tmp";
    record.dictTmpPath = "/tmp/test/user_dictionary.txt.tmp";

    const std::string serialized = record.Serialize();
    EXPECT_NE(serialized.find("VKEY_LEXICON_JOURNAL_V1"), std::string::npos);
    EXPECT_NE(serialized.find("state=PREPARED"), std::string::npos);
    EXPECT_NE(serialized.find("config_bak_path=/tmp/test/config.toml.bak"), std::string::npos);

    LexiconJournalRecord deserialized;
    ASSERT_TRUE(LexiconJournalRecord::Deserialize(serialized, deserialized));
    EXPECT_EQ(deserialized.state, LexiconJournalState::Prepared);
    EXPECT_TRUE(deserialized.configExistedBefore);
    EXPECT_FALSE(deserialized.dictExistedBefore);
    EXPECT_EQ(deserialized.oldGeneration, 12u);
    EXPECT_EQ(deserialized.newGeneration, 13u);
    EXPECT_EQ(deserialized.oldConfigHash, "0123456789abcdef");
    EXPECT_EQ(deserialized.newConfigHash, "fedcba9876543210");
    EXPECT_EQ(deserialized.oldDictHash, "");
    EXPECT_EQ(deserialized.newDictHash, "aabbccddeeff0011");
    EXPECT_EQ(deserialized.configBakPath, "/tmp/test/config.toml.bak");
    EXPECT_EQ(deserialized.dictBakPath, "/tmp/test/user_dictionary.txt.bak");
    EXPECT_EQ(deserialized.configTmpPath, "/tmp/test/config.toml.tmp");
    EXPECT_EQ(deserialized.dictTmpPath, "/tmp/test/user_dictionary.txt.tmp");
}

TEST_F(LexiconTransactionTest, LexiconSyncLockBasicAcquisition) {
    LexiconSyncLock lock(1000);
    EXPECT_TRUE(lock.IsLocked());

    LexiconSyncLock moved(std::move(lock));
    EXPECT_TRUE(moved.IsLocked());
    EXPECT_FALSE(lock.IsLocked());

    moved.Unlock();
    EXPECT_FALSE(moved.IsLocked());
}

TEST_F(LexiconTransactionTest, CommitTransactionCreatesBothFilesAndCleansArtifacts) {
    const std::string newToml = "[general]\nmethod = 0\n";
    const std::string newDict = "# User Dictionary\nsoà\n";

    ASSERT_TRUE(LexiconWriter::CommitTransaction(
        configPath_.wstring(),
        newToml,
        newDict,
        1,
        2,
        false));

    EXPECT_TRUE(std::filesystem::exists(configPath_));
    EXPECT_TRUE(std::filesystem::exists(dictPath_));
    EXPECT_EQ(ReadFile(configPath_), newToml);
    EXPECT_EQ(ReadFile(dictPath_), newDict);

    // Verify all journals, backups and temps are cleaned
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal.tmp"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "config.toml.bak"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "user_dictionary.txt.bak"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "config.toml.tmp"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "user_dictionary.txt.tmp"));
}

TEST_F(LexiconTransactionTest, CommitTransactionReplacesExistingFilesAtomically) {
    WriteFile(configPath_, "initial config");
    WriteFile(dictPath_, "initial dict");

    const std::string updatedToml = "updated config toml";
    const std::string updatedDict = "updated dict text";

    ASSERT_TRUE(LexiconWriter::CommitTransaction(
        configPath_.wstring(),
        updatedToml,
        updatedDict,
        5,
        6,
        false));

    EXPECT_EQ(ReadFile(configPath_), updatedToml);
    EXPECT_EQ(ReadFile(dictPath_), updatedDict);
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal"));
}

TEST_F(LexiconTransactionTest, CrashRecoveryAtPreparedRollsBackToOriginalFiles) {
    // Setup initial state: original files existed and backups were taken
    WriteFile(configPath_, "partially written config");
    WriteFile(dictPath_, "partially written dict");

    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "pristine old config");
    WriteFile(dictBak, "pristine old dict");

    // Write journal with state PREPARED
    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    record.oldGeneration = 1;
    record.newGeneration = 2;

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    // Run recovery
    ASSERT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // Must be rolled back to pristine old content
    EXPECT_EQ(ReadFile(configPath_), "pristine old config");
    EXPECT_EQ(ReadFile(dictPath_), "pristine old dict");

    // Journal and backups must be gone
    EXPECT_FALSE(std::filesystem::exists(journalPath));
    EXPECT_FALSE(std::filesystem::exists(configBak));
    EXPECT_FALSE(std::filesystem::exists(dictBak));
}

TEST_F(LexiconTransactionTest, CrashRecoveryAtFilesReplacedRollsForward) {
    // Setup state: both files were replaced with new content, crash happened before commit
    WriteFile(configPath_, "new replaced config");
    WriteFile(dictPath_, "new replaced dict");

    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "old config");
    WriteFile(dictBak, "old dict");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::FilesReplaced;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    record.oldGeneration = 1;
    record.newGeneration = 2;
    record.newConfigHash = LexiconJournalRecord::ComputeHash("new replaced config");
    record.newDictHash = LexiconJournalRecord::ComputeHash("new replaced dict");

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    ASSERT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // Roll-forward: new files are KEPT
    EXPECT_EQ(ReadFile(configPath_), "new replaced config");
    EXPECT_EQ(ReadFile(dictPath_), "new replaced dict");

    // Journal and backups cleaned
    EXPECT_FALSE(std::filesystem::exists(journalPath));
    EXPECT_FALSE(std::filesystem::exists(configBak));
    EXPECT_FALSE(std::filesystem::exists(dictBak));
}

TEST_F(LexiconTransactionTest, LockedReaderRecoversUnfinishedTransactionAndLoads) {
    // Setup state: journal PREPARED with backups
    WriteFile(configPath_, "bad config");
    WriteFile(dictPath_, "bad dict");
    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "restored config");
    WriteFile(dictBak, "# Valid dictionary\nsoà\n");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    std::shared_ptr<const RustUserDictionarySnapshot> snapshot;
    EXPECT_TRUE(LexiconReader::LoadUserDictionaryLocked(configPath_.wstring(), snapshot));
    ASSERT_NE(snapshot, nullptr);

    // Verify recovery rolled back to dictBak
    EXPECT_EQ(ReadFile(configPath_), "restored config");
    EXPECT_EQ(ReadFile(dictPath_), "# Valid dictionary\nsoà\n");
    EXPECT_FALSE(std::filesystem::exists(journalPath));
}

TEST_F(LexiconTransactionTest, LoadUserDictionaryWordsLocked_MissingFileReturnsEmpty) {
    std::vector<std::wstring> words;
    EXPECT_TRUE(LexiconReader::LoadUserDictionaryWordsLocked(configPath_.wstring(), words));
    EXPECT_TRUE(words.empty());
}

TEST_F(LexiconTransactionTest, LoadUserDictionaryWordsLocked_ExistingFileReturnsParsedWords) {
    WriteFile(dictPath_, "; Header comment\nso\u00e0\n\nalo\n");
    std::vector<std::wstring> words;
    EXPECT_TRUE(LexiconReader::LoadUserDictionaryWordsLocked(configPath_.wstring(), words));
    EXPECT_EQ(words.size(), 2u);
    EXPECT_EQ(words[0], L"alo");
    EXPECT_EQ(words[1], L"so\u00e0");
}

TEST_F(LexiconTransactionTest, CommitTransaction_ConvenienceOverload) {
    const std::string newToml = "[features]\nspell_suggest = true\n";
    const std::string newDict = "; Dict\nalo\n";

    EXPECT_TRUE(LexiconWriter::CommitTransaction(configPath_.wstring(), newToml, newDict, false));
    EXPECT_EQ(ReadFile(configPath_), newToml);
    EXPECT_EQ(ReadFile(dictPath_), newDict);
}

TEST_F(LexiconTransactionTest, PublishGenerationFailure_RollsBackFilesAndFailsTransaction) {
    WriteFile(configPath_, "pristine config");
    WriteFile(dictPath_, "pristine dict");

    LexiconWriter::SetTestGenerationPublisher([](uint8_t) {
        return false; // Simulate failure to open/write shared state
    });

    const std::string newToml = "[features]\nspell_suggest = true\n";
    const std::string newDict = "new dict\n";

    bool success = LexiconWriter::CommitTransaction(
        configPath_.wstring(),
        newToml,
        newDict,
        1,
        2,
        true);

    EXPECT_FALSE(success);

    // Verify rollback: disk still has pristine old content
    EXPECT_EQ(ReadFile(configPath_), "pristine config");
    EXPECT_EQ(ReadFile(dictPath_), "pristine dict");

    // All artifacts cleaned
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal.tmp"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "config.toml.bak"));
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "user_dictionary.txt.bak"));

    LexiconWriter::SetTestGenerationPublisher(nullptr);
}

TEST_F(LexiconTransactionTest, CorruptedJournal_FailsRecoveryAndReaderFailsStale) {
    WriteFile(configPath_, "good config");
    WriteFile(dictPath_, "; comment\nvalidword\n");

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, "CORRUPT_HEADER_NOT_VKEY\ngarbage=123\n");

    // Recovery must fail
    EXPECT_FALSE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // Must be quarantined to .corrupt
    EXPECT_TRUE(std::filesystem::exists(testDir_ / "lexicon_txn.journal.corrupt"));

    // Reader must fail-stale
    std::shared_ptr<const RustUserDictionarySnapshot> snapshot;
    EXPECT_FALSE(LexiconReader::LoadUserDictionaryLocked(configPath_.wstring(), snapshot));

    std::vector<std::wstring> words;
    EXPECT_FALSE(LexiconReader::LoadUserDictionaryWordsLocked(configPath_.wstring(), words));

    // CleanStaleArtifacts cleans the .corrupt file
    LexiconRecovery::CleanStaleArtifacts(configPath_);
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal.corrupt"));
}

TEST_F(LexiconTransactionTest, FilesReplaced_HashMismatchRollsBackToBackup) {
    // Original files backed up
    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "original config");
    WriteFile(dictBak, "original dict");

    // Replaced files on disk are torn/corrupted
    WriteFile(configPath_, "corrupted half-written config");
    WriteFile(dictPath_, "corrupted half-written dict");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::FilesReplaced;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    record.oldGeneration = 1;
    record.newGeneration = 2;
    record.newConfigHash = LexiconJournalRecord::ComputeHash("intact new config");
    record.newDictHash = LexiconJournalRecord::ComputeHash("intact new dict");

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    // RecoverIfNeeded detects hash mismatch and rolls back
    ASSERT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // Rolled back to original files
    EXPECT_EQ(ReadFile(configPath_), "original config");
    EXPECT_EQ(ReadFile(dictPath_), "original dict");

    EXPECT_FALSE(std::filesystem::exists(journalPath));
    EXPECT_FALSE(std::filesystem::exists(configBak));
    EXPECT_FALSE(std::filesystem::exists(dictBak));
}

TEST_F(LexiconTransactionTest, RecoveryAtGenerationPublished_CleansBackupsAndCommits) {
    WriteFile(configPath_, "published new config");
    WriteFile(dictPath_, "published new dict");

    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "old config");
    WriteFile(dictBak, "old dict");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::GenerationPublished;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    record.oldGeneration = 1;
    record.newGeneration = 2;

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    ASSERT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));

    EXPECT_EQ(ReadFile(configPath_), "published new config");
    EXPECT_EQ(ReadFile(dictPath_), "published new dict");
    EXPECT_FALSE(std::filesystem::exists(journalPath));
    EXPECT_FALSE(std::filesystem::exists(configBak));
    EXPECT_FALSE(std::filesystem::exists(dictBak));
}

TEST_F(LexiconTransactionTest, RecoveryAtCommitted_CleansRemainingJournal) {
    WriteFile(configPath_, "committed config");
    WriteFile(dictPath_, "committed dict");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::Committed;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    ASSERT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));

    EXPECT_EQ(ReadFile(configPath_), "committed config");
    EXPECT_EQ(ReadFile(dictPath_), "committed dict");
    EXPECT_FALSE(std::filesystem::exists(journalPath));
}

TEST_F(LexiconTransactionTest, CrashRecoveryAtPrepared_DestinationAlreadyExists_OverwritesAtomically) {
    // Both destination files already exist on disk (partially written/modified during crash)
    WriteFile(configPath_, "corrupted half-written config");
    WriteFile(dictPath_, "corrupted half-written dict");

    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "pristine old config");
    WriteFile(dictBak, "pristine old dict");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    record.oldGeneration = 1;
    record.newGeneration = 2;
    record.configBakPath = configBak.string();
    record.dictBakPath = dictBak.string();

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    ASSERT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // Must successfully overwrite existing files with pristine backups
    EXPECT_EQ(ReadFile(configPath_), "pristine old config");
    EXPECT_EQ(ReadFile(dictPath_), "pristine old dict");
    EXPECT_FALSE(std::filesystem::exists(journalPath));
    EXPECT_FALSE(std::filesystem::exists(configBak));
    EXPECT_FALSE(std::filesystem::exists(dictBak));
}

TEST_F(LexiconTransactionTest, PublishGeneration_DefaultAndCustomPublisher) {
    // 1. Custom publisher hook
    uint8_t publishedGen = 0;
    LexiconWriter::SetTestGenerationPublisher([&](uint8_t gen) {
        publishedGen = gen;
        return true;
    });
    EXPECT_TRUE(LexiconWriter::PublishGeneration(99));
    EXPECT_EQ(publishedGen, 99);

    // 2. Custom publisher failure
    LexiconWriter::SetTestGenerationPublisher([](uint8_t) {
        return false;
    });
    EXPECT_FALSE(LexiconWriter::PublishGeneration(100));

    // 3. Reset to default publisher
    LexiconWriter::SetTestGenerationPublisher(nullptr);
#if !defined(_WIN32)
    // On Linux/macOS, default returns true
    EXPECT_TRUE(LexiconWriter::PublishGeneration(101));
#endif
}

TEST_F(LexiconTransactionTest, RollbackToBackupFailure_PreservesBackupFilesAndJournal) {
    // configBak exists, but dictBak is missing even though dictExistedBefore = true
    const auto configBak = testDir_ / "config.toml.bak";
    WriteFile(configBak, "backup config");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = true;
    record.dictExistedBefore = true; // but dictBak does not exist!
    record.configBakPath = configBak.string();
    record.dictBakPath = (testDir_ / "user_dictionary.txt.bak").string();

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    // Recovery must fail because rollback could not restore dict
    EXPECT_FALSE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // Journal must be preserved for forensics/subsequent recovery
    EXPECT_TRUE(std::filesystem::exists(journalPath));
    // And config was either restored to configPath or remains as backup
    EXPECT_TRUE(std::filesystem::exists(configPath_) || std::filesystem::exists(configBak));
}

TEST_F(LexiconTransactionTest, RecoveryAtFilesReplaced_PublishGenerationFailure_PreservesBackupAndJournalAndFails) {
    const std::string newConfig = "[features]\nspell_suggest = true\n";
    const std::string newDict = "# Valid dictionary\nso\u00e0\n";
    WriteFile(configPath_, newConfig);
    WriteFile(dictPath_, newDict);

    const auto configBak = testDir_ / "config.toml.bak";
    const auto dictBak = testDir_ / "user_dictionary.txt.bak";
    WriteFile(configBak, "old config");
    WriteFile(dictBak, "old dict");

    LexiconJournalRecord record;
    record.state = LexiconJournalState::FilesReplaced;
    record.configExistedBefore = true;
    record.dictExistedBefore = true;
    record.oldGeneration = 1;
    record.newGeneration = 2;
    record.newConfigHash = LexiconJournalRecord::ComputeHash(newConfig);
    record.newDictHash = LexiconJournalRecord::ComputeHash(newDict);
    record.configBakPath = configBak.string();
    record.dictBakPath = dictBak.string();

    const auto journalPath = testDir_ / "lexicon_txn.journal";
    WriteFile(journalPath, record.Serialize());

    // Fault injection: publisher fails
    LexiconWriter::SetTestGenerationPublisher([](uint8_t) {
        return false;
    });

    // Recovery must fail
    EXPECT_FALSE(LexiconRecovery::RecoverIfNeeded(configPath_));

    // CRITICAL: .bak and journal must be PRESERVED on disk!
    EXPECT_TRUE(std::filesystem::exists(journalPath));
    EXPECT_TRUE(std::filesystem::exists(configBak));
    EXPECT_TRUE(std::filesystem::exists(dictBak));

    // Reader must fail-stale because journal recovery failed
    std::shared_ptr<const RustUserDictionarySnapshot> snapshot;
    EXPECT_FALSE(LexiconReader::LoadUserDictionaryLocked(configPath_.wstring(), snapshot));

    // Retry recovery with working publisher -> must roll-forward and clean up
    LexiconWriter::SetTestGenerationPublisher([](uint8_t) {
        return true;
    });

    EXPECT_TRUE(LexiconRecovery::RecoverIfNeeded(configPath_));
    EXPECT_FALSE(std::filesystem::exists(journalPath));
    EXPECT_FALSE(std::filesystem::exists(configBak));
    EXPECT_FALSE(std::filesystem::exists(dictBak));

    // Now reader succeeds
    EXPECT_TRUE(LexiconReader::LoadUserDictionaryLocked(configPath_.wstring(), snapshot));

    LexiconWriter::SetTestGenerationPublisher(nullptr);
}

TEST_F(LexiconTransactionTest, CommitTransaction_PublishesWireMappingWhenConfigured) {
    // 1. Setup wire manager
    Wire::LexiconWireManager wireManager;
    ASSERT_TRUE(wireManager.Create());
    LexiconWriter::SetWireManager(&wireManager);

    std::string newConfigToml = R"(
[input]
method = "telex"

[features]
spell_suggest = true
spell_exclusions = [ "msword", "excel" ]

[internal]
wire_generation = 4000
)";
    std::string newUserDict = "t\u1eeb\n\u0111i\u1ec3n\n";

    bool ok = LexiconWriter::CommitTransaction(configPath_.wstring(), newConfigToml, newUserDict);
    EXPECT_TRUE(ok);

    // Verify wire mapping was updated with generation 4000
    Wire::LexiconWireReader reader;
    ASSERT_TRUE(reader.Open());
    std::vector<uint8_t> localBuf;
    Wire::LexiconWireView view;
    std::string err;
    ASSERT_TRUE(reader.ReadSnapshot(localBuf, view, &err)) << err;

    EXPECT_EQ(view.header->generation, 4000u);
    EXPECT_EQ(view.exclusionsRowCount, 2u);
    EXPECT_EQ(view.wordCount, 2u);

    reader.Close();
    LexiconWriter::SetWireManager(nullptr);
    wireManager.Close();
}

TEST_F(LexiconTransactionTest, CommitTransaction_RollsBackWhenWirePublishingFails) {
    // 1. Initial valid commit
    std::string validToml = "[input]\nmethod = \"telex\"\n";
    std::string validDict = "ban\n";
    ASSERT_TRUE(LexiconWriter::CommitTransaction(configPath_.wstring(), validToml, validDict));

    // 2. Setup mock wire publisher hook that fails
    LexiconWriter::SetTestWirePublisher([](const auto&, const auto&, uint64_t, bool, std::string* outErr) {
        if (outErr) *outErr = "Simulated wire publish failure";
        return false;
    });

    std::string newConfigToml = "[input]\nmethod = \"vni\"\n[internal]\nwire_generation = 5000\n";
    std::string newUserDict = "moi\n";

    // Transaction must fail
    bool ok = LexiconWriter::CommitTransaction(configPath_.wstring(), newConfigToml, newUserDict);
    EXPECT_FALSE(ok);

    // Files must have rolled back to previous content
    std::string currentToml = ReadFile(configPath_);
    EXPECT_NE(currentToml.find("telex"), std::string::npos);
    EXPECT_EQ(currentToml.find("vni"), std::string::npos);

    std::string currentDict = ReadFile(dictPath_);
    EXPECT_NE(currentDict.find("ban"), std::string::npos);
    EXPECT_EQ(currentDict.find("moi"), std::string::npos);

    // Journal and backups must be cleaned up
    EXPECT_FALSE(std::filesystem::exists(testDir_ / "lexicon_txn.journal"));
    EXPECT_FALSE(std::filesystem::exists(configPath_.string() + ".bak"));
    EXPECT_FALSE(std::filesystem::exists(dictPath_.string() + ".bak"));

    LexiconWriter::SetTestWirePublisher(nullptr);
}

} // namespace NextKey

