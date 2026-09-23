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
#include <array>
#include <charconv>
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

bool TryReadFileBytes(const std::filesystem::path& path, std::string& out) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        if (file.bad()) return false;

        out = std::move(content);
        return true;
    } catch (...) {
        return false;
    }
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

    const auto alreadyRestored = [&](const std::filesystem::path& path,
                                     bool existedBefore,
                                     const std::string& oldHash) {
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            ec.clear();
            return false;
        }
        if (!existedBefore) return !exists;
        return exists && !oldHash.empty() &&
               ComputeFnv1aHex(ReadFileBytes(path)) == oldHash;
    };

    if (record.configExistedBefore) {
        if (std::filesystem::exists(configBak, ec)) {
            if (!DurableAtomicRename(configBak, configPath)) {
                ok = false;
            }
        } else if (!alreadyRestored(configPath, true, record.oldConfigHash)) {
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
        } else if (!alreadyRestored(dictPath, true, record.oldDictHash)) {
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
    ss << "notify_shared_state=" << (notifySharedState ? "1" : "0") << "\n";
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
    if (!std::getline(ss, header) || header != "VKEY_LEXICON_JOURNAL_V1") {
        return false;
    }

    constexpr std::array<std::string_view, 14> requiredKeys = {
        "state", "config_existed", "dict_existed", "notify_shared_state",
        "old_gen", "new_gen", "old_config_hash", "new_config_hash",
        "old_dict_hash", "new_dict_hash", "config_bak_path", "dict_bak_path",
        "config_tmp_path", "dict_tmp_path"
    };
    uint32_t seenKeys = 0;
    LexiconJournalRecord record;
    auto parseBoolean = [](std::string_view value, bool& target) {
        if (value == "1") { target = true; return true; }
        if (value == "0") { target = false; return true; }
        return false;
    };
    auto parseGeneration = [](std::string_view value, uint8_t& target) {
        unsigned parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || parsed > UINT8_MAX) {
            return false;
        }
        target = static_cast<uint8_t>(parsed);
        return true;
    };

    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        for (size_t i = 0; i < requiredKeys.size(); ++i) {
            if (key != requiredKeys[i]) continue;
            const uint32_t bit = uint32_t{1} << i;
            if ((seenKeys & bit) != 0) return false;
            seenKeys |= bit;
            break;
        }

        if (key == "state") {
            if (val == "PREPARED") record.state = LexiconJournalState::Prepared;
            else if (val == "FILES_REPLACED") record.state = LexiconJournalState::FilesReplaced;
            else if (val == "GENERATION_PUBLISHED") record.state = LexiconJournalState::GenerationPublished;
            else if (val == "COMMITTED") record.state = LexiconJournalState::Committed;
            else if (val == "ROLLBACK_PENDING") record.state = LexiconJournalState::RollbackPending;
            else return false;
        } else if (key == "config_existed") {
            if (!parseBoolean(val, record.configExistedBefore)) return false;
        } else if (key == "dict_existed") {
            if (!parseBoolean(val, record.dictExistedBefore)) return false;
        } else if (key == "notify_shared_state") {
            if (!parseBoolean(val, record.notifySharedState)) return false;
        } else if (key == "old_gen") {
            if (!parseGeneration(val, record.oldGeneration)) return false;
        } else if (key == "new_gen") {
            if (!parseGeneration(val, record.newGeneration)) return false;
        } else if (key == "old_config_hash") {
            record.oldConfigHash = val;
        } else if (key == "new_config_hash") {
            record.newConfigHash = val;
        } else if (key == "old_dict_hash") {
            record.oldDictHash = val;
        } else if (key == "new_dict_hash") {
            record.newDictHash = val;
        } else if (key == "config_bak_path") {
            record.configBakPath = val;
        } else if (key == "dict_bak_path") {
            record.dictBakPath = val;
        } else if (key == "config_tmp_path") {
            record.configTmpPath = val;
        } else if (key == "dict_tmp_path") {
            record.dictTmpPath = val;
        }
    }
    if (seenKeys != (uint32_t{1} << requiredKeys.size()) - 1 ||
        record.state == LexiconJournalState::Unknown) {
        return false;
    }
    outRecord = std::move(record);
    return true;
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
    std::string lockPath = "/tmp/vkey_config.lock";
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

                if (record.notifySharedState &&
                    !LexiconWriter::PublishGeneration(record.oldGeneration)) {
                    // Persist in RollbackPending state so future recovery knows files are already rolled back
                    if (!TransitionJournalState(configPath, record, LexiconJournalState::RollbackPending)) {
                        return false;
                    }
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
            if (record.notifySharedState &&
                !LexiconWriter::PublishGeneration(record.newGeneration)) {
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

            if (record.notifySharedState &&
                !LexiconWriter::PublishGeneration(record.oldGeneration)) {
                // Generation restore failed: persist in RollbackPending state, fail-stale
                return false;
            }

            // Re-publish wire mapping from the now-restored disk files so TSF sees the
            // rolled-back snapshot without needing a disk-read allowance.
            // The journal does not persist old wire data, so we re-parse the restored
            // config.toml to reconstruct the correct wire state.
            // CRITICAL: read the 64-bit internal.wire_generation from TOML — do NOT cast
            // from uint8_t oldGeneration, which is a different (smaller) counter.
            bool wireRestored = true;
            if (sWireManager || sTestWirePublisher) {
                try {
                    std::vector<std::wstring> recovExclusions;
                    std::vector<std::wstring> recovDictWords;
                    bool recovSpellSuggest = true;
                    uint64_t recovWireGen = static_cast<uint64_t>(record.oldGeneration);

                    auto recovTbl = toml::parse(ReadFileBytes(configPath));
                    if (auto* f = recovTbl["features"].as_table()) {
                        if (auto* ss = (*f)["spell_suggest"].as_boolean())
                            recovSpellSuggest = ss->get();
                        if (auto* arr = (*f)["spell_exclusions"].as_array()) {
                            std::vector<std::string> raw;
                            for (auto&& item : *arr)
                                if (auto* s = item.as_string()) raw.push_back(s->get());
                            auto c = SpellExclusionCanonicalizer::Canonicalize(raw);
                            if (c.Succeeded()) recovExclusions = std::move(c.entries);
                        }
                    }
                    // Read 64-bit wire_generation from internal table (may exceed uint8_t range).
                    if (auto* i = recovTbl["internal"].as_table()) {
                        if (auto* wg = (*i)["wire_generation"].as_integer()) {
                            if (wg->get() > 0) recovWireGen = static_cast<uint64_t>(wg->get());
                        }
                    }
                    if (std::filesystem::exists(dictPath, ec)) {
                        auto dictRes = LexiconValidator::ParseAndValidateUserDictText(
                            ReadFileBytes(dictPath));
                        if (dictRes.validation.Succeeded())
                            recovDictWords = std::move(dictRes.entries);
                    }

                    std::string wireErr;
                    wireRestored = LexiconWriter::PublishWireMapping(
                        recovExclusions, recovDictWords,
                        recovWireGen,
                        recovSpellSuggest, &wireErr);
                    // If wireRestored==false: do NOT clean journal below so next startup retries.
                } catch (...) {
                    // Malformed restored config — treat as wire restore failure (fail-stale).
                    wireRestored = false;
                }
            }

            // Only clean artifacts if wire was successfully restored.
            // If wire restore failed, leave journal in ROLLBACK_PENDING so RecoverIfNeeded()
            // retries on next startup rather than leaving a torn wire/disk state.
            if (wireRestored) {
                std::filesystem::remove(configTmp, ec);
                std::filesystem::remove(dictTmp, ec);
                std::filesystem::remove(configBak, ec);
                std::filesystem::remove(dictBak, ec);
                std::filesystem::remove(journalPath, ec);
            } else {
                // The disk files and SharedState generation are restored, but the
                // shared-memory snapshot is still unknown. Do not report recovery
                // success: callers must fail-stale and the journal must remain for
                // a later retry once the wire writer is available again.
                return false;
            }
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
#if !defined(VKEY_USE_RUST_ENGINE)
    // VKey Classic is intentionally C++-only and does not link the optional
    // Rust adapter. Its UI uses LoadUserDictionaryWordsLocked() below, which
    // parses the on-disk text without requiring a Rust snapshot.
    (void)configPath;
    outSnapshot.reset();
    return false;
#else
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
#endif
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
    const bool dictExists = std::filesystem::exists(dictPath, ec);
    if (ec) return false;
    if (!dictExists) {
        outWords.clear();
        return true;
    }

    std::string content;
    if (!TryReadFileBytes(dictPath, content)) return false;
    auto res = LexiconValidator::ParseAndValidateUserDictText(content);
    if (!res.validation.Succeeded()) {
        return false;
    }
    outWords = std::move(res.entries);
    return true;
}

// ─── LexiconWriter Implementation ───────────────────────────────────────────


static bool CommitTransactionImpl(
    const std::wstring& configPathStr,
    const std::string& newConfigToml,
    const std::string& newUserDictText,
    uint8_t oldGeneration,
    uint8_t newGeneration,
    bool notifySharedState,
    bool acquireLock) {
    std::unique_ptr<LexiconSyncLock> ownedLock;
    if (acquireLock) {
        ownedLock = std::make_unique<LexiconSyncLock>(kLexiconMutexTimeoutMs);
        if (!ownedLock->IsLocked()) return false;
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

    // Pre-flight validation: validate TOML syntax, spell exclusions, and user dictionary content.
    // Both files on disk MUST remain 100% untouched if any input payload is invalid.
    bool spellSuggest = true;
    std::vector<std::wstring> parsedExclusions;
    uint64_t wireGen = 0;
    try {
        auto tbl = toml::parse(newConfigToml);
        if (auto* features = tbl["features"].as_table()) {
            if (auto* ss = (*features)["spell_suggest"].as_boolean()) {
                spellSuggest = ss->get();
            }
            if (auto* arr = (*features)["spell_exclusions"].as_array()) {
                std::vector<std::string> rawExcl;
                rawExcl.reserve(arr->size());
                for (auto&& item : *arr) {
                    if (auto* s = item.as_string()) {
                        rawExcl.push_back(s->get());
                    } else {
                        return false; // Non-string entry in spell_exclusions array
                    }
                }
                if (!rawExcl.empty()) {
                    auto canon = SpellExclusionCanonicalizer::Canonicalize(rawExcl);
                    if (!canon.Succeeded()) {
                        return false; // Exclusions out of bounds (> 8 entries or invalid scalar count)
                    }
                    parsedExclusions = std::move(canon.entries);
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
        return false; // Malformed TOML! Fail early without creating temp files or touching disk!
    }

    auto dictRes = LexiconValidator::ParseAndValidateUserDictText(newUserDictText);
    if (!dictRes.validation.Succeeded()) {
        return false; // Malformed user dictionary! Fail early without touching disk!
    }

    if (wireGen == 0) {
        if (sWireManager && sWireManager->IsWritable()) {
            wireGen = sWireManager->GetWireGeneration() + 1;
        } else {
            wireGen = static_cast<uint64_t>(newGeneration);
        }
    }

    std::error_code ec;
    bool configExisted = std::filesystem::exists(configPath, ec);
    bool dictExisted = std::filesystem::exists(dictPath, ec);

    // Capture old config & dictionary metadata for wire rollback in case PublishGeneration fails
    std::vector<std::wstring> oldExclusions;
    std::vector<std::wstring> oldDictWords;
    uint64_t oldWireGen = static_cast<uint64_t>(oldGeneration);
    bool oldSpellSuggest = true;
    if (configExisted) {
        try {
            auto oldTbl = toml::parse(ReadFileBytes(configPath));
            if (auto* f = oldTbl["features"].as_table()) {
                if (auto* ss = (*f)["spell_suggest"].as_boolean()) oldSpellSuggest = ss->get();
                if (auto* arr = (*f)["spell_exclusions"].as_array()) {
                    std::vector<std::string> raw;
                    for (auto&& item : *arr) {
                        if (auto* s = item.as_string()) raw.push_back(s->get());
                    }
                    auto c = SpellExclusionCanonicalizer::Canonicalize(raw);
                    if (c.Succeeded()) oldExclusions = std::move(c.entries);
                }
            }
            if (auto* i = oldTbl["internal"].as_table()) {
                if (auto* wg = (*i)["wire_generation"].as_integer()) {
                    if (wg->get() > 0) oldWireGen = static_cast<uint64_t>(wg->get());
                }
            }
        } catch (...) {}
    }
    if (dictExisted) {
        oldDictWords = LexiconValidator::ParseAndValidateUserDictText(ReadFileBytes(dictPath)).entries;
    }

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
        if (!std::filesystem::copy_file(
                configPath, configBak, std::filesystem::copy_options::overwrite_existing, ec) || ec) {
            LexiconRecovery::CleanStaleArtifacts(configPath);
            return false;
        }
    }
    if (dictExisted) {
        if (!std::filesystem::copy_file(
                dictPath, dictBak, std::filesystem::copy_options::overwrite_existing, ec) || ec) {
            LexiconRecovery::CleanStaleArtifacts(configPath);
            return false;
        }
    }

    // 3. Prepare Journal Record (PREPARED state)
    LexiconJournalRecord record;
    record.state = LexiconJournalState::Prepared;
    record.configExistedBefore = configExisted;
    record.dictExistedBefore = dictExisted;
    record.notifySharedState = notifySharedState;
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

    // 6. Publish Wire Mapping FIRST (before publishing SharedState generation).
    // TSF will only observe the new SharedState epoch AFTER the wire mapping
    // already contains the new snapshot, preventing stale cache-hit races.
    bool wirePublished = false;
    if (notifySharedState && (sWireManager || sTestWirePublisher)) {
        std::string wireErr;
        if (!LexiconWriter::PublishWireMapping(
                parsedExclusions, dictRes.entries, wireGen, spellSuggest, &wireErr)) {
            // Publishing wire mapping failed! Transition journal to RollbackPending.
            // SharedState generation was NOT published, so SharedState remains clean at oldGeneration.
            if (!TransitionJournalState(configPath, record, LexiconJournalState::RollbackPending)) {
                return false;
            }
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
        wirePublished = true;
    }

    // 7. Publish Generation to SharedState (bumping SharedState epoch)
    if (notifySharedState) {
        if (!LexiconWriter::PublishGeneration(newGeneration)) {
            // Publishing generation failed! Transition journal to RollbackPending.
            if (!TransitionJournalState(configPath, record, LexiconJournalState::RollbackPending)) {
                return false;
            }
            // Restore old wire mapping if wire was published.
            // On failure, log and leave journal in ROLLBACK_PENDING (fail-stale).
            bool wireRestored = true;
            if (wirePublished) {
                std::string restoreErr;
                wireRestored = LexiconWriter::PublishWireMapping(
                    oldExclusions, oldDictWords, oldWireGen, oldSpellSuggest, &restoreErr);
                // If restore failed, journal stays ROLLBACK_PENDING; RecoverIfNeeded() will retry on next startup.
            }
            // Attempt to restore old generation in SharedState to ensure clean state
            bool oldGenRestored = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (LexiconWriter::PublishGeneration(oldGeneration)) {
                    oldGenRestored = true;
                    break;
                }
            }

            bool filesRestored = RollbackToBackup(record, configPath, dictPath, configBak, dictBak);
            // Only clean up artifacts if ALL three components were restored.
            // If wire restore failed, keep journal so RecoverIfNeeded() can signal TSF
            // to re-read from disk on next startup (fail-stale).
            if (oldGenRestored && filesRestored && wireRestored) {
                std::filesystem::remove(configTmp, ec);
                std::filesystem::remove(dictTmp, ec);
                std::filesystem::remove(configBak, ec);
                std::filesystem::remove(dictBak, ec);
                std::filesystem::remove(journalPath, ec);
                std::filesystem::remove(journalTmp, ec);
            }
            // If any restore failed, the journal remains in ROLLBACK_PENDING state so
            // RecoverIfNeeded() can retry on next startup.
            return false;
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
    uint8_t oldGeneration,
    uint8_t newGeneration,
    bool notifySharedState) {
    return CommitTransactionImpl(
        configPath,
        newConfigToml,
        newUserDictText,
        oldGeneration,
        newGeneration,
        notifySharedState,
        true);
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

bool LexiconWriter::CommitLexiconUpdate(
    const std::wstring& configPath,
    bool spellSuggestEnabled,
    const std::vector<std::wstring>& spellExclusions,
    const std::vector<std::wstring>& userDictionary) {
    LexiconSyncLock lock(kLexiconMutexTimeoutMs);
    if (!lock.IsLocked()) return false;

    const std::filesystem::path path(configPath);
    // The merge must be based on the last committed config, not on files left
    // behind by a crashed PREPARED transaction. CommitTransactionImpl also
    // recovers, but doing it there is too late because this function has
    // already parsed and modified the config by then.
    if (!LexiconRecovery::RecoverIfNeeded(path)) return false;

    std::string newConfigToml;
    try {
        toml::table table;
        std::error_code ec;
        const bool configExists = std::filesystem::exists(path, ec);
        if (ec) return false;
        if (configExists) {
            std::string configBytes;
            if (!TryReadFileBytes(path, configBytes)) return false;
            table = toml::parse(configBytes);
        }

        uint64_t currentWireGeneration = 1;
        if (auto* internal = table["internal"].as_table()) {
            if (auto* value = (*internal)["wire_generation"].as_integer();
                value && value->get() > 0) {
                currentWireGeneration = static_cast<uint64_t>(value->get());
            }
        }
        if (currentWireGeneration >= static_cast<uint64_t>(INT64_MAX)) return false;

        toml::array exclusions;
        for (const auto& entry : spellExclusions) {
            std::u32string scalars;
            std::string utf8;
            if (!SpellExclusionCanonicalizer::Utf16ToUtf32(entry, scalars) ||
                !SpellExclusionCanonicalizer::Utf32ToUtf8(scalars, utf8)) {
                return false;
            }
            exclusions.push_back(std::move(utf8));
        }

        auto* features = table["features"].as_table();
        if (!features) {
            table.insert_or_assign("features", toml::table{});
            features = table["features"].as_table();
        }
        features->insert_or_assign("spell_suggest", spellSuggestEnabled);
        features->insert_or_assign("spell_exclusions", std::move(exclusions));

        auto* internal = table["internal"].as_table();
        if (!internal) {
            table.insert_or_assign("internal", toml::table{});
            internal = table["internal"].as_table();
        }
        internal->insert_or_assign(
            "wire_generation", static_cast<int64_t>(currentWireGeneration + 1));

        std::ostringstream stream;
        stream << table;
        newConfigToml = stream.str();
    } catch (...) {
        return false;
    }

    const std::string newUserDictText = LexiconValidator::FormatUserDictText(userDictionary);
    uint8_t oldGeneration = 0;
#if defined(_WIN32)
    SharedStateManager sm;
    if (sm.Open()) {
        const SharedState state = sm.Read();
        if (state.IsValid()) oldGeneration = state.configGeneration;
    }
#endif
    const uint8_t newGeneration = static_cast<uint8_t>(oldGeneration + 1);

    // The main process owns the wire mapping. It publishes the committed disk
    // snapshot and only then announces this generation through SharedState.
    return CommitTransactionImpl(
        configPath,
        newConfigToml,
        newUserDictText,
        oldGeneration,
        newGeneration,
        false,
        false);
}

} // namespace NextKey
