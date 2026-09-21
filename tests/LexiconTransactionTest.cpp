// VKey - Lexicon Transaction and Recovery Tests
// SPDX-License-Identifier: GPL-3.0-only

#include "core/config/LexiconTransaction.h"
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

} // namespace NextKey

