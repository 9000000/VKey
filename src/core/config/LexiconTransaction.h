// VKey - Lexicon Transaction and Paired Sync
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace NextKey {

class RustUserDictionarySnapshot;
struct TypingConfig;

constexpr const wchar_t* kLexiconSyncMutexName = L"Local\\VKeyLexiconSyncMutex";
constexpr uint32_t kLexiconMutexTimeoutMs = 5000; // 5,000 ms

enum class LexiconJournalState : uint8_t {
    Unknown = 0,
    Prepared = 1,
    FilesReplaced = 2,
    GenerationPublished = 3,
    Committed = 4,
};

struct LexiconJournalRecord {
    LexiconJournalState state = LexiconJournalState::Unknown;
    bool configExistedBefore = false;
    bool dictExistedBefore = false;
    uint8_t oldGeneration = 0;
    uint8_t newGeneration = 0;
    std::string oldConfigHash;
    std::string newConfigHash;
    std::string oldDictHash;
    std::string newDictHash;
    std::string configBakPath;
    std::string dictBakPath;
    std::string configTmpPath;
    std::string dictTmpPath;

    [[nodiscard]] static std::string ComputeHash(std::string_view data);
    [[nodiscard]] std::string Serialize() const;
    static bool Deserialize(std::string_view text, LexiconJournalRecord& outRecord);
};

/// Cross-process synchronization lock using named mutex Local\VKeyLexiconSyncMutex.
class LexiconSyncLock {
public:
    explicit LexiconSyncLock(uint32_t timeoutMs = kLexiconMutexTimeoutMs);
    ~LexiconSyncLock();

    LexiconSyncLock(const LexiconSyncLock&) = delete;
    LexiconSyncLock& operator=(const LexiconSyncLock&) = delete;
    LexiconSyncLock(LexiconSyncLock&& other) noexcept;
    LexiconSyncLock& operator=(LexiconSyncLock&& other) noexcept;

    [[nodiscard]] bool IsLocked() const noexcept { return locked_; }
    [[nodiscard]] bool WasAbandoned() const noexcept { return abandoned_; }
    void Unlock() noexcept;

private:
    void* handle_ = nullptr;
    bool locked_ = false;
    bool abandoned_ = false;
};

/// Crash recovery runner implementing the 4-state Journal FSM.
class LexiconRecovery {
public:
    /// Check for an unfinished transaction journal beside configPath and recover.
    /// Caller MUST hold LexiconSyncLock before calling.
    static bool RecoverIfNeeded(const std::filesystem::path& configPath);

    /// Force clean up all journal, backup, and temp files.
    static void CleanStaleArtifacts(const std::filesystem::path& configPath);
};

/// Paired snapshot loaded atomically under lock.
struct PairedLexiconSnapshot {
    std::shared_ptr<const RustUserDictionarySnapshot> userDictionary;
    uint8_t generation = 0;
    bool configLoaded = false;
    bool userDictLoaded = false;
};

/// Reader that holds LexiconSyncLock across both config.toml and user_dictionary.txt.
class LexiconReader {
public:
    /// Load user_dictionary.txt under lock, returning valid snapshot or nullptr on error.
    /// Fails stale (returns false) if lock acquisition fails or times out.
    static bool LoadUserDictionaryLocked(
        const std::wstring& configPath,
        std::shared_ptr<const RustUserDictionarySnapshot>& outSnapshot);

    /// Load user_dictionary.txt words under lock into outWords.
    /// Missing file succeeds with empty list (FR1/N7).
    static bool LoadUserDictionaryWordsLocked(
        const std::wstring& configPath,
        std::vector<std::wstring>& outWords);
};

/// Durable 4-state transaction writer for paired config.toml and user_dictionary.txt.
class LexiconWriter {
public:
    using GenerationPublisher = std::function<bool(uint8_t)>;

    /// Hook to override generation publishing in tests. Set to nullptr to restore default.
    static void SetTestGenerationPublisher(GenerationPublisher publisher);

    /// Execute a paired transaction writing both config.toml and user_dictionary.txt.
    /// Prepares temp files, creates backups, writes durable journal (PREPARED),
    /// replaces files (FILES_REPLACED), bumps generation (GENERATION_PUBLISHED),
    /// and cleans up (COMMITTED).
    static bool CommitTransaction(
        const std::wstring& configPath,
        const std::string& newConfigToml,
        const std::string& newUserDictText,
        uint8_t oldGeneration,
        uint8_t newGeneration,
        bool notifySharedState = true);

    /// Convenience overload that automatically discovers oldGeneration and increments it.
    static bool CommitTransaction(
        const std::wstring& configPath,
        const std::string& newConfigToml,
        const std::string& newUserDictText,
        bool notifySharedState = true);
};

} // namespace NextKey

