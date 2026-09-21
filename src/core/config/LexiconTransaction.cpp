// VKey - Lexicon Transaction and Paired Sync Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "LexiconTransaction.h"
#include "LexiconValidation.h"
#include "SpellExclusionCanonicalizer.h"
#include "core/engine/RustInputEngine.h"
#include "core/ipc/LexiconWireManager.h"

#include <toml.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "core/ipc/SharedStateManager.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace NextKey {

std::string LexiconJournalRecord::ComputeHash(std::string_view data) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : data) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

namespace {

std::string ComputeFnv1aHex(std::string_view data) {
    return LexiconJournalRecord::ComputeHash(data);
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

bool DurableWrite(const std::filesystem::path& path, std::string_view data) {
#if defined(_WIN32)
    HANDLE hFile = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    DWORD total = static_cast<DWORD>(data.size());
    DWORD offset = 0;
    while (offset < total) {
        if (!::WriteFile(hFile, data.data() + offset, total - offset, &written, nullptr) || written == 0) {
            ::CloseHandle(hFile);
            return false;
        }
        offset += written;
    }
    BOOL flushed = ::FlushFileBuffers(hFile);
    ::CloseHandle(hFile);
    return flushed != 0;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            ::close(fd);
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    int synced = ::fsync(fd);
    ::close(fd);
    return synced == 0;
#endif
}

bool DurableAtomicRename(const std::filesystem::path& src, const std::filesystem::path& dst) {
#if defined(_WIN32)
    return ::MoveFileExW(
        src.c_str(),
        dst.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(src.c_str(), dst.c_str()) == 0;
#endif
}

bool ReplaceOrMove(
    const std::filesystem::path& tmpPath,
    const std::filesystem::path& dstPath,
    const std::filesystem::path& bakPath,
    bool dstExisted) {
#if defined(_WIN32)
    if (dstExisted) {
        if (::ReplaceFileW(
                dstPath.c_str(),
                tmpPath.c_str(),
                bakPath.c_str(),
                REPLACEFILE_WRITE_THROUGH,
                nullptr, nullptr) != 0) {
            return true;
        }
        // If ReplaceFile fails (e.g. file lock or permissions), fallback to MoveFileEx
    }
    return ::MoveFileExW(
        tmpPath.c_str(),
        dstPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED) != 0;
#else
    if (dstExisted) {
        std::error_code ec;
        std::filesystem::copy_file(dstPath, bakPath, std::filesystem::copy_options::overwrite_existing, ec);
    }
    return ::rename(tmpPath.c_str(), dstPath.c_str()) == 0;
#endif
}

std::filesystem::path GetJournalPath(const std::filesystem::path& configPath) {
    return configPath.parent_path() / "lexicon_txn.journal";
}

std::filesystem::path GetJournalTmpPath(const std::filesystem::path& configPath) {
    return configPath.parent_path() / "lexicon_txn.journal.tmp";
}

std::string PathToUtf8String(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path Utf8StringToPath(std::string_view utf8Str) {
    std::u8string u8(reinterpret_cast<const char8_t*>(utf8Str.data()), utf8Str.size());
    return std::filesystem::path(u8);
}

bool RollbackToBackup(
    const LexiconJournalRecord& record,
    const std::filesystem::path& configPath,
    const std::filesystem::path& dictPath,
    const std::filesystem::path& configBak,
    const std::filesystem::path& dictBak) {
    std::error_code ec;
    bool ok = true;

    if (record.configExistedBefore) {
        if (std::filesystem::exists(configBak, ec)) {
            if (!DurableAtomicRename(configBak, configPath)) {
                ok = false;
            }
        } else {
            ok = false;
        }
    } else {
        if (!std::filesystem::remove(configPath, ec) && std::filesystem::exists(configPath, ec)) {
            ok = false;
        }
    }

    if (record.dictExistedBefore) {
        if (std::filesystem::exists(dictBak, ec)) {
            if (!DurableAtomicRename(dictBak, dictPath)) {
                ok = false;
            }
        } else {
            ok = false;
        }
    } else {
        if (!std::filesystem::remove(dictPath, ec) && std::filesystem::exists(dictPath, ec)) {
            ok = false;
        }
    }

    return ok;
}

bool TransitionJournalState(
    const std::filesystem::path& configPath,
    LexiconJournalRecord& record,
    LexiconJournalState newState) {
    record.state = newState;
    const auto journalTmp = GetJournalTmpPath(configPath);
    const auto journalPath = GetJournalPath(configPath);
    if (!DurableWrite(journalTmp, record.Serialize())) {
        return false;
    }
    return DurableAtomicRename(journalTmp, journalPath);
}

LexiconWriter::GenerationPublisher sTestGenerationPublisher = nullptr;
Wire::LexiconWireManager* sWireManager = nullptr;
LexiconWriter::WirePublisher sTestWirePublisher = nullptr;

} // namespace

void LexiconWriter::SetTestGenerationPublisher(GenerationPublisher publisher) {
    sTestGenerationPublisher = std::move(publisher);
}

void LexiconWriter::SetTestWirePublisher(WirePublisher publisher) {
    sTestWirePublisher = std::move(publisher);
}

void LexiconWriter::SetWireManager(Wire::LexiconWireManager* manager) noexcept {
    sWireManager = manager;
}

Wire::LexiconWireManager* LexiconWriter::GetWireManager() noexcept {
    return sWireManager;
}

bool LexiconWriter::PublishWireMapping(
    const std::vector<std::wstring>& spellExclusions,
    const std::vector<std::wstring>& userDictionary,
    uint64_t generation,
    bool spellSuggestEnabled,
    std::string* outError) {
    if (sTestWirePublisher) {
        return sTestWirePublisher(spellExclusions, userDictionary, generation, spellSuggestEnabled, outError);
    }
    if (sWireManager && sWireManager->IsWritable()) {
        return sWireManager->Publish(spellExclusions, userDictionary, generation, spellSuggestEnabled, outError);
    }
    if (outError) {
        *outError = "Wire mapping manager is not configured or not writable";
    }
    return false;
}

bool LexiconWriter::PublishGeneration(uint8_t newGeneration) {
    if (sTestGenerationPublisher) {
        return sTestGenerationPublisher(newGeneration);
    }
#if defined(_WIN32)
    SharedStateManager sm;
    if (!sm.OpenReadWrite() || !sm.IsConnected()) {
        return false;
    }
    SharedState state = sm.Read();
    if (!state.IsValid()) {
        return false;
    }
    state.configGeneration = newGeneration;
    sm.Write(state);

    SharedState verified = sm.Read();
    return verified.IsValid() && verified.configGeneration == newGeneration;
#else
    (void)newGeneration;
    return true;
#endif
}

// ─── LexiconJournalRecord Serialization ─────────────────────────────────────

std::string LexiconJournalRecord::Serialize() const {
    std::ostringstream ss;
    ss << "VKEY_LEXICON_JOURNAL_V1\n";
    ss << "state=";
    switch (state) {
        case LexiconJournalState::Prepared: ss << "PREPARED\n"; break;
        case LexiconJournalState::FilesReplaced: ss << "FILES_REPLACED\n"; break;
        case LexiconJournalState::GenerationPublished: ss << "GENERATION_PUBLISHED\n"; break;
        case LexiconJournalState::Committed: ss << "COMMITTED\n"; break;
        case LexiconJournalState::RollbackPending: ss << "ROLLBACK_PENDING\n"; break;
        default: ss << "UNKNOWN\n"; break;
    }
    ss << "config_existed=" << (configExistedBefore ? "1" : "0") << "\n";
    ss << "dict_existed=" << (dictExistedBefore ? "1" : "0") << "\n";
    ss << "old_gen=" << static_cast<unsigned>(oldGeneration) << "\n";
    ss << "new_gen=" << static_cast<unsigned>(newGeneration) << "\n";
    ss << "old_config_hash=" << oldConfigHash << "\n";
    ss << "new_config_hash=" << newConfigHash << "\n";
    ss << "old_dict_hash=" << oldDictHash << "\n";
    ss << "new_dict_hash=" << newDictHash << "\n";
    ss << "config_bak_path=" << configBakPath << "\n";
    ss << "dict_bak_path=" << dictBakPath << "\n";
    ss << "config_tmp_path=" << configTmpPath << "\n";
    ss << "dict_tmp_path=" << dictTmpPath << "\n";
    return ss.str();
}

bool LexiconJournalRecord::Deserialize(std::string_view text, LexiconJournalRecord& outRecord) {
    const std::string textStr(text);
    std::istringstream ss(textStr);
    std::string header;
    if (!std::getline(ss, header) || header.find("VKEY_LEXICON_JOURNAL_V1") == std::string::npos) {
        return false;
    }

    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line.back() == '\r') {
            if (!line.empty()) line.pop_back();
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "state") {
            if (val == "PREPARED") outRecord.state = LexiconJournalState::Prepared;
            else if (val == "FILES_REPLACED") outRecord.state = LexiconJournalState::FilesReplaced;
            else if (val == "GENERATION_PUBLISHED") outRecord.state = LexiconJournalState::GenerationPublished;
            else if (val == "COMMITTED") outRecord.state = LexiconJournalState::Committed;
            else if (val == "ROLLBACK_PENDING") outRecord.state = LexiconJournalState::RollbackPending;
            else outRecord.state = LexiconJournalState::Unknown;
        } else if (key == "config_existed") {
            outRecord.configExistedBefore = (val == "1" || val == "true");
        } else if (key == "dict_existed") {
            outRecord.dictExistedBefore = (val == "1" || val == "true");
        } else if (key == "old_gen") {
            outRecord.oldGeneration = static_cast<uint8_t>(std::stoul(val));
        } else if (key == "new_gen") {
            outRecord.newGeneration = static_cast<uint8_t>(std::stoul(val));
        } else if (key == "old_config_hash") {
            outRecord.oldConfigHash = val;
        } else if (key == "new_config_hash") {
            outRecord.newConfigHash = val;
        } else if (key == "old_dict_hash") {
            outRecord.oldDictHash = val;
        } else if (key == "new_dict_hash") {
            outRecord.newDictHash = val;
        } else if (key == "config_bak_path") {
            outRecord.configBakPath = val;
        } else if (key == "dict_bak_path") {
            outRecord.dictBakPath = val;
        } else if (key == "config_tmp_path") {
            outRecord.configTmpPath = val;
        } else if (key == "dict_tmp_path") {
            outRecord.dictTmpPath = val;
        }
    }
    return outRecord.state != LexiconJournalState::Unknown;
}

// ─── LexiconSyncLock Implementation ─────────────────────────────────────────

LexiconSyncLock::LexiconSyncLock(uint32_t timeoutMs) {
#if defined(_WIN32)
    handle_ = ::CreateMutexW(nullptr, FALSE, kLexiconSyncMutexName);
    if (!handle_) return;

    DWORD res = ::WaitForSingleObject(static_cast<HANDLE>(handle_), timeoutMs);
    if (res == WAIT_OBJECT_0) {
        locked_ = true;
    } else if (res == WAIT_ABANDONED) {
        locked_ = true;
        abandoned_ = true;
    }
#else
    // POSIX lock file for tests / Linux builds
    std::string lockPath = "/tmp/vkey_lexicon_sync.lock";
    int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) return;
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));

    // Try acquiring flock with timeout
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            locked_ = true;
            break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutMs) {
            ::close(fd);
            handle_ = nullptr;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
}

LexiconSyncLock::~LexiconSyncLock() {
    Unlock();
}

LexiconSyncLock::LexiconSyncLock(LexiconSyncLock&& other) noexcept
    : handle_(other.handle_), locked_(other.locked_), abandoned_(other.abandoned_) {
    other.handle_ = nullptr;
    other.locked_ = false;
    other.abandoned_ = false;
}

LexiconSyncLock& LexiconSyncLock::operator=(LexiconSyncLock&& other) noexcept {
    if (this != &other) {
        Unlock();
        handle_ = other.handle_;
        locked_ = other.locked_;
        abandoned_ = other.abandoned_;
        other.handle_ = nullptr;
        other.locked_ = false;
        other.abandoned_ = false;
    }
    return *this;
}

void LexiconSyncLock::Unlock() noexcept {
    if (!handle_) return;
#if defined(_WIN32)
    if (locked_) {
        ::ReleaseMutex(static_cast<HANDLE>(handle_));
        locked_ = false;
    }
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
#else
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    if (locked_) {
        ::flock(fd, LOCK_UN);
        locked_ = false;
    }
    ::close(fd);
    handle_ = nullptr;
#endif
}

// ─── LexiconRecovery Implementation ─────────────────────────────────────────

bool LexiconRecovery::RecoverIfNeeded(const std::filesystem::path& configPath) {
    const auto journalPath = GetJournalPath(configPath);
    const auto corruptPath = configPath.parent_path() / "lexicon_txn.journal.corrupt";
    std::error_code ec;

    if (std::filesystem::exists(corruptPath, ec)) {
        // A previously quarantined corrupt journal exists: fail recovery (fail-stale policy)
        return false;
    }

    if (!std::filesystem::exists(journalPath, ec)) {
        return true;
    }

    const std::string journalText = ReadFileBytes(journalPath);
    LexiconJournalRecord record;
    if (!LexiconJournalRecord::Deserialize(journalText, record)) {
        // Corrupt journal: quarantine to .corrupt so forensics can inspect, and fail recovery (fail-stale policy)
        std::filesystem::rename(journalPath, corruptPath, ec);
        if (ec) {
            std::filesystem::remove(journalPath, ec);
        }
        return false;
    }

    const auto configBak = record.configBakPath.empty()
        ? configPath.parent_path() / (configPath.filename().string() + ".bak")
        : Utf8StringToPath(record.configBakPath);
    const auto dictPath = configPath.parent_path() / "user_dictionary.txt";
    const auto dictBak = record.dictBakPath.empty()
        ? configPath.parent_path() / "user_dictionary.txt.bak"
        : Utf8StringToPath(record.dictBakPath);
    const auto configTmp = record.configTmpPath.empty()
        ? configPath.parent_path() / (configPath.filename().string() + ".tmp")
        : Utf8StringToPath(record.configTmpPath);
    const auto dictTmp = record.dictTmpPath.empty()
        ? configPath.parent_path() / "user_dictionary.txt.tmp"
        : Utf8StringToPath(record.dictTmpPath);

    switch (record.state) {
        case LexiconJournalState::Prepared: {
            // Crash occurred before or during file replacement.
            // Rollback to original files.
            if (!RollbackToBackup(record, configPath, dictPath, configBak, dictBak)) {
                return false;
            }

            std::filesystem::remove(configTmp, ec);
            std::filesystem::remove(dictTmp, ec);
            std::filesystem::remove(configBak, ec);
            std::filesystem::remove(dictBak, ec);
            std::filesystem::remove(journalPath, ec);
            break;
        }

        case LexiconJournalState::FilesReplaced: {
            // Both files were replaced on disk before crash.
            // MUST verify checksums of the replaced files.
            const std::string curConfigHash = ComputeFnv1aHex(ReadFileBytes(configPath));
            const std::string curDictHash = ComputeFnv1aHex(ReadFileBytes(dictPath));

            if (curConfigHash != record.newConfigHash || curDictHash != record.newDictHash) {
                // Hash mismatch! Files on disk are corrupted, incomplete, or were previously rolled back.
                bool filesRestored = false;
                if (std::filesystem::exists(configBak, ec) || std::filesystem::exists(dictBak, ec)) {
                    filesRestored = RollbackToBackup(record, configPath, dictPath, configBak, dictBak);
                } else {
                    const std::string oldCfg = ComputeFnv1aHex(ReadFileBytes(configPath));
                    const std::string oldDic = ComputeFnv1aHex(ReadFileBytes(dictPath));
                    filesRestored = (!record.configExistedBefore || oldCfg == record.oldConfigHash) &&
                                    (!record.dictExistedBefore || oldDic == record.oldDictHash);
                }

                if (!filesRestored) {
                    return false;
                }

                if (!LexiconWriter::PublishGeneration(record.oldGeneration)) {
                    // Persist in RollbackPending state so future recovery knows files are already rolled back
                    TransitionJournalState(configPath, record, LexiconJournalState::RollbackPending);
                    return false;
                }
                std::filesystem::remove(configTmp, ec);
                std::filesystem::remove(dictTmp, ec);
                std::filesystem::remove(configBak, ec);
                std::filesystem::remove(dictBak, ec);
                std::filesystem::remove(journalPath, ec);
                break;
            }

            // Hashes verified: roll-forward
            if (!LexiconWriter::PublishGeneration(record.newGeneration)) {
                // Generation publishing failed! Keep backups and journal, fail-stale.
                return false;
            }

            // Transition to GENERATION_PUBLISHED
            if (!TransitionJournalState(configPath, record, LexiconJournalState::GenerationPublished)) {
                return false;
            }

            std::filesystem::remove(configTmp, ec);
            std::filesystem::remove(dictTmp, ec);
            std::filesystem::remove(configBak, ec);
            std::filesystem::remove(dictBak, ec);

            // Transition to COMMITTED before final journal deletion
            if (!TransitionJournalState(configPath, record, LexiconJournalState::Committed)) {
                return false;
            }
            std::filesystem::remove(journalPath, ec);
            break;
        }

        case LexiconJournalState::GenerationPublished: {
            std::filesystem::remove(configTmp, ec);
            std::filesystem::remove(dictTmp, ec);
            std::filesystem::remove(configBak, ec);
            std::filesystem::remove(dictBak, ec);

            // Transition to COMMITTED before final journal deletion
            if (!TransitionJournalState(configPath, record, LexiconJournalState::Committed)) {
                return false;
            }
            std::filesystem::remove(journalPath, ec);
            break;
        }

        case LexiconJournalState::Committed: {
            // Clean up left-over artifacts
            std::filesystem::remove(configTmp, ec);
            std::filesystem::remove(dictTmp, ec);
            std::filesystem::remove(configBak, ec);
            std::filesystem::remove(dictBak, ec);
            std::filesystem::remove(journalPath, ec);
            break;
        }

        case LexiconJournalState::RollbackPending: {
            // Transaction was aborted and is in the process of rolling back.
            bool filesRestored = false;
            if (std::filesystem::exists(configBak, ec) || std::filesystem::exists(dictBak, ec)) {
                filesRestored = RollbackToBackup(record, configPath, dictPath, configBak, dictBak);
            } else {
                const std::string curConfigHash = ComputeFnv1aHex(ReadFileBytes(configPath));
                const std::string curDictHash = ComputeFnv1aHex(ReadFileBytes(dictPath));
                filesRestored = (!record.configExistedBefore || curConfigHash == record.oldConfigHash) &&
                                (!record.dictExistedBefore || curDictHash == record.oldDictHash);
            }

            if (!filesRestored) {
                return false;
            }

            if (!LexiconWriter::PublishGeneration(record.oldGeneration)) {
                // Generation restore failed: persist in RollbackPending state, fail-stale
                return false;
            }

            std::filesystem::remove(configTmp, ec);
            std::filesystem::remove(dictTmp, ec);
            std::filesystem::remove(configBak, ec);
            std::filesystem::remove(dictBak, ec);
            std::filesystem::remove(journalPath, ec);
            break;
        }

        default:
            std::filesystem::remove(journalPath, ec);
            break;
    }
    return true;
}

void LexiconRecovery::CleanStaleArtifacts(const std::filesystem::path& configPath) {
    std::error_code ec;
    std::filesystem::remove(GetJournalPath(configPath), ec);
    std::filesystem::remove(GetJournalTmpPath(configPath), ec);
    std::filesystem::remove(configPath.parent_path() / "lexicon_txn.journal.corrupt", ec);
    std::filesystem::remove(configPath.parent_path() / (configPath.filename().string() + ".tmp"), ec);
    std::filesystem::remove(configPath.parent_path() / (configPath.filename().string() + ".bak"), ec);
    std::filesystem::remove(configPath.parent_path() / "user_dictionary.txt.tmp", ec);
    std::filesystem::remove(configPath.parent_path() / "user_dictionary.txt.bak", ec);
}

// ─── LexiconReader Implementation ───────────────────────────────────────────

bool LexiconReader::LoadUserDictionaryLocked(
    const std::wstring& configPath,
    std::shared_ptr<const RustUserDictionarySnapshot>& outSnapshot) {
    LexiconSyncLock lock(kLexiconMutexTimeoutMs);
    if (!lock.IsLocked()) {
        // Timeout or error acquiring mutex: FAIL-STALE (do not proceed without lock)
        return false;
    }

    // Recover any unfinished transaction before reading.
    // If recovery fails (e.g. corrupt journal or unrecoverable state), FAIL-STALE!
    if (!LexiconRecovery::RecoverIfNeeded(configPath)) {
        return false;
    }

    auto result = RustInputEngine::LoadUserDictionary(configPath);
    if (result.Succeeded()) {
        outSnapshot = std::move(result.snapshot);
        return true;
    }
    return false;
}

bool LexiconReader::LoadUserDictionaryWordsLocked(
    const std::wstring& configPathStr,
    std::vector<std::wstring>& outWords) {
    LexiconSyncLock lock(kLexiconMutexTimeoutMs);
    if (!lock.IsLocked()) {
        return false;
    }

    const std::filesystem::path configPath(configPathStr);
    if (!LexiconRecovery::RecoverIfNeeded(configPath)) {
        return false;
    }

    const std::filesystem::path dictPath = configPath.parent_path() / "user_dictionary.txt";
    std::error_code ec;
    if (!std::filesystem::exists(dictPath, ec)) {
        outWords.clear();
        return true;
    }

    std::string content = ReadFileBytes(dictPath);
    auto res = LexiconValidator::ParseAndValidateUserDictText(content);
    if (!res.validation.Succeeded()) {
        return false;
    }
    outWords = std::move(res.entries);
    return true;
}

// ─── LexiconWriter Implementation ───────────────────────────────────────────


bool LexiconWriter::CommitTransaction(
    const std::wstring& configPathStr,
    const std::string& newConfigToml,
    const std::string& newUserDictText,
    uint8_t oldGeneration,
    uint8_t newGeneration,
    bool notifySharedState) {
    LexiconSyncLock lock(kLexiconMutexTimeoutMs);
    if (!lock.IsLocked()) {
        return false;
    }

    const std::filesystem::path configPath(configPathStr);
    const std::filesystem::path configDir = configPath.parent_path();
    const std::filesystem::path dictPath = configDir / "user_dictionary.txt";
    const std::filesystem::path configTmp = configDir / (configPath.filename().string() + ".tmp");
    const std::filesystem::path dictTmp = configDir / "user_dictionary.txt.tmp";
    const std::filesystem::path configBak = configDir / (configPath.filename().string() + ".bak");
    const std::filesystem::path dictBak = configDir / "user_dictionary.txt.bak";
    const std::filesystem::path journalPath = GetJournalPath(configPath);
    const std::filesystem::path journalTmp = GetJournalTmpPath(configPath);

    // Run recovery on any previous dead transaction first.
    // If previous transaction failed recovery (e.g. corrupt journal), abort safely.
    if (!LexiconRecovery::RecoverIfNeeded(configPath)) {
        return false;
    }

#if defined(_WIN32)
    // Pre-flight check: ensure SharedState can be opened with read-write access before touching files
    if (notifySharedState && !sTestGenerationPublisher) {
        SharedStateManager sm;
        if (!sm.OpenReadWrite() || !sm.IsConnected()) {
            return false;
        }
    }
#endif

    std::error_code ec;
    bool configExisted = std::filesystem::exists(configPath, ec);
    bool dictExisted = std::filesystem::exists(dictPath, ec);

    // 1. Write temp files durably
    if (!DurableWrite(configTmp, newConfigToml)) {
        LexiconRecovery::CleanStaleArtifacts(configPath);
        return false;
    }
    if (!DurableWrite(dictTmp, newUserDictText)) {
        LexiconRecovery::CleanStaleArtifacts(configPath);
        return false;
    }

    // 2. Create backups of existing files
    if (configExisted) {
        std::filesystem::copy_file(configPath, configBak, std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (dictExisted) {
        std::filesystem::copy_file(dictPath, dictBak, std::filesystem::copy_options::overwrite_existing, ec);
    }

    // 3. Prepare Journal Record (PREPARED state)
    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = configExisted;
    record.dictExistedBefore = dictExisted;
    record.oldGeneration = oldGeneration;
    record.newGeneration = newGeneration;
    record.newConfigHash = ComputeFnv1aHex(newConfigToml);
    record.newDictHash = ComputeFnv1aHex(newUserDictText);
    if (configExisted) record.oldConfigHash = ComputeFnv1aHex(ReadFileBytes(configPath));
    if (dictExisted) record.oldDictHash = ComputeFnv1aHex(ReadFileBytes(dictPath));
    record.configBakPath = PathToUtf8String(configBak);
    record.dictBakPath = PathToUtf8String(dictBak);
    record.configTmpPath = PathToUtf8String(configTmp);
    record.dictTmpPath = PathToUtf8String(dictTmp);

    // Durable write journal in PREPARED state
    if (!TransitionJournalState(configPath, record, LexiconJournalState::Prepared)) {
        LexiconRecovery::CleanStaleArtifacts(configPath);
        return false;
    }

    // 4. Replace files
    if (!ReplaceOrMove(configTmp, configPath, configBak, configExisted)) {
        if (RollbackToBackup(record, configPath, dictPath, configBak, dictBak)) {
            LexiconRecovery::CleanStaleArtifacts(configPath);
        }
        return false;
    }
    if (!ReplaceOrMove(dictTmp, dictPath, dictBak, dictExisted)) {
        if (RollbackToBackup(record, configPath, dictPath, configBak, dictBak)) {
            LexiconRecovery::CleanStaleArtifacts(configPath);
        }
        return false;
    }

    // 5. Transition journal to FILES_REPLACED
    if (!TransitionJournalState(configPath, record, LexiconJournalState::FilesReplaced)) {
        LexiconRecovery::RecoverIfNeeded(configPath);
        return false;
    }

    // 6. Publish Generation & Wire Mapping
    if (notifySharedState) {
        if (!PublishGeneration(newGeneration)) {
            // Publishing failed! Rollback to backup files and fail transaction.
            TransitionJournalState(configPath, record, LexiconJournalState::RollbackPending);
            if (RollbackToBackup(record, configPath, dictPath, configBak, dictBak)) {
                std::filesystem::remove(configTmp, ec);
                std::filesystem::remove(dictTmp, ec);
                std::filesystem::remove(configBak, ec);
                std::filesystem::remove(dictBak, ec);
                std::filesystem::remove(journalPath, ec);
                std::filesystem::remove(journalTmp, ec);
            }
            return false;
        }

        // Publish wire mapping if wire manager or test publisher is configured
        if (sWireManager || sTestWirePublisher) {
            bool spellSuggest = true;
            std::vector<std::wstring> exclusions;
            uint64_t wireGen = 0;

            try {
                auto tbl = toml::parse(newConfigToml);
                if (auto* features = tbl["features"].as_table()) {
                    if (auto* ss = (*features)["spell_suggest"].as_boolean()) {
                        spellSuggest = ss->get();
                    }
                    if (auto* arr = (*features)["spell_exclusions"].as_array()) {
                        for (auto&& item : *arr) {
                            if (auto* s = item.as_string()) {
                                std::string u8 = s->get();
                                std::wstring u16;
                                std::u32string u32;
                                if (SpellExclusionCanonicalizer::Utf8ToUtf32(u8, u32) &&
                                    SpellExclusionCanonicalizer::Utf32ToUtf16(u32, u16)) {
                                    exclusions.push_back(u16);
                                }
                            }
                        }
                    }
                }
                if (auto* internalTbl = tbl["internal"].as_table()) {
                    if (auto* wg = (*internalTbl)["wire_generation"].as_integer()) {
                        if (wg->get() > 0) {
                            wireGen = static_cast<uint64_t>(wg->get());
                        }
                    }
                }
            } catch (...) {
            }

            if (wireGen == 0) {
                if (sWireManager && sWireManager->IsWritable()) {
                    wireGen = sWireManager->GetWireGeneration() + 1;
                } else {
                    wireGen = static_cast<uint64_t>(newGeneration);
                }
            }

            auto dictRes = LexiconValidator::ParseAndValidateUserDictText(newUserDictText);
            std::string wireErr;
            if (!PublishWireMapping(exclusions, dictRes.entries, wireGen, spellSuggest, &wireErr)) {
                // SharedState generation was published at step 6; transition journal to RollbackPending
                // BEFORE attempting rollback so crash recovery knows a rollback is underway.
                TransitionJournalState(configPath, record, LexiconJournalState::RollbackPending);

                // Attempt to restore old generation in SharedState
                bool oldGenRestored = false;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    if (PublishGeneration(oldGeneration)) {
                        oldGenRestored = true;
                        break;
                    }
                }

                const bool rollbackOk = RollbackToBackup(record, configPath, dictPath, configBak, dictBak);
                if (oldGenRestored && rollbackOk) {
                    // Both generation and files cleanly rolled back
                    std::filesystem::remove(configTmp, ec);
                    std::filesystem::remove(dictTmp, ec);
                    std::filesystem::remove(configBak, ec);
                    std::filesystem::remove(dictBak, ec);
                    std::filesystem::remove(journalPath, ec);
                    std::filesystem::remove(journalTmp, ec);
                }
                // If oldGenRestored is false (or rollback failed), the journal remains in ROLLBACK_PENDING state
                // on disk. Files are safely at old hashes and RecoverIfNeeded() can retry generation restoration.
                return false;
            }
        }
    }

    // 7. Transition journal to GENERATION_PUBLISHED
    if (!TransitionJournalState(configPath, record, LexiconJournalState::GenerationPublished)) {
        return false;
    }

    // 8. Clean up backups
    std::filesystem::remove(configBak, ec);
    std::filesystem::remove(dictBak, ec);

    // 9. Transition journal to COMMITTED
    if (!TransitionJournalState(configPath, record, LexiconJournalState::Committed)) {
        return false;
    }

    // 10. Clean up journal
    std::filesystem::remove(journalPath, ec);
    std::filesystem::remove(journalTmp, ec);

    return true;
}

bool LexiconWriter::CommitTransaction(
    const std::wstring& configPath,
    const std::string& newConfigToml,
    const std::string& newUserDictText,
    bool notifySharedState) {
    uint8_t oldGen = 0;
#if defined(_WIN32)
    SharedStateManager sm;
    if (sm.Open()) {
        SharedState state = sm.Read();
        if (state.IsValid()) {
            oldGen = state.configGeneration;
        }
    }
#endif
    uint8_t newGen = static_cast<uint8_t>(oldGen + 1);
    return CommitTransaction(configPath, newConfigToml, newUserDictText, oldGen, newGen, notifySharedState);
}

} // namespace NextKey

