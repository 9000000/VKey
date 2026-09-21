// VKey - Lexicon Shared Memory Wire Manager and Lock-Free Reader Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "LexiconWireManager.h"

#include "core/config/LexiconValidation.h"
#include "core/config/SpellExclusionCanonicalizer.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <Windows.h>
#include "SecurityHelpers.h"
#endif

namespace NextKey::Wire {

namespace {

#if !defined(_WIN32)
// Thread-safe in-process named shared memory registry for POSIX / Linux CI testing
struct PosixNamedSharedMemoryRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> mappings;
    std::unordered_set<std::string> activeWriters;

    std::shared_ptr<std::vector<uint8_t>> CreateForWriter(const std::string& name, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeWriters.find(name) != activeWriters.end()) {
            return nullptr; // Another manager is already the active writer!
        }
        activeWriters.insert(name);
        auto it = mappings.find(name);
        if (it != mappings.end()) {
            return it->second;
        }
        auto block = std::make_shared<std::vector<uint8_t>>(size, 0);
        mappings[name] = block;
        return block;
    }

    std::shared_ptr<std::vector<uint8_t>> Find(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = mappings.find(name);
        if (it != mappings.end()) {
            return it->second;
        }
        return nullptr;
    }

    void ReleaseWriter(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        activeWriters.erase(name);
    }

    void Remove(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        activeWriters.erase(name);
        mappings.erase(name);
    }
};

PosixNamedSharedMemoryRegistry& GetPosixRegistry() {
    static PosixNamedSharedMemoryRegistry sRegistry;
    return sRegistry;
}
#endif

} // namespace

// ─── LexiconWireManager::Impl ────────────────────────────────────────────────

struct LexiconWireManager::Impl {
#if defined(_WIN32)
    HANDLE hMapping = nullptr;
    HANDLE hWriterMutex = nullptr;
    volatile LexiconWireHeader* pMapping = nullptr;
#else
    std::shared_ptr<std::vector<uint8_t>> posixBlock;
    volatile LexiconWireHeader* pMapping = nullptr;
#endif
    uint8_t* mockStorage = nullptr;
    size_t mockSize = 0;
    bool isWritable = false;
    uint64_t wireGeneration = 1;
};

LexiconWireManager::LexiconWireManager() : pImpl_(std::make_unique<Impl>()) {}

LexiconWireManager::~LexiconWireManager() {
    Close();
}

LexiconWireManager::LexiconWireManager(LexiconWireManager&&) noexcept = default;
LexiconWireManager& LexiconWireManager::operator=(LexiconWireManager&&) noexcept = default;

bool LexiconWireManager::Create() {
    Close();

    if (pImpl_->mockStorage && pImpl_->mockSize >= WIRE_TOTAL_SIZE) {
        pImpl_->pMapping = reinterpret_cast<volatile LexiconWireHeader*>(pImpl_->mockStorage);
        pImpl_->isWritable = true;
        return true;
    }

#if defined(_WIN32)
    pImpl_->hWriterMutex = CreateMutexW(nullptr, TRUE, L"Local\\VKeyLexiconWireWriterMutex");
    if (!pImpl_->hWriterMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (pImpl_->hWriterMutex) {
            CloseHandle(pImpl_->hWriterMutex);
            pImpl_->hWriterMutex = nullptr;
        }
        return false;
    }

    SECURITY_ATTRIBUTES sa = MakeAppContainerReadableSecurityAttributes();

    pImpl_->hMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        sa.lpSecurityDescriptor ? &sa : nullptr,
        PAGE_READWRITE,
        0,
        WIRE_TOTAL_SIZE,
        LEXICON_WIRE_MAPPING_NAME);

    if (sa.lpSecurityDescriptor) {
        LocalFree(sa.lpSecurityDescriptor);
    }

    if (!pImpl_->hMapping) {
        CloseHandle(pImpl_->hWriterMutex);
        pImpl_->hWriterMutex = nullptr;
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(pImpl_->hMapping);
        pImpl_->hMapping = nullptr;
        CloseHandle(pImpl_->hWriterMutex);
        pImpl_->hWriterMutex = nullptr;
        return false;
    }

    void* pView = MapViewOfFile(pImpl_->hMapping, FILE_MAP_ALL_ACCESS, 0, 0, WIRE_TOTAL_SIZE);
    if (!pView) {
        CloseHandle(pImpl_->hMapping);
        pImpl_->hMapping = nullptr;
        CloseHandle(pImpl_->hWriterMutex);
        pImpl_->hWriterMutex = nullptr;
        return false;
    }

    pImpl_->pMapping = static_cast<volatile LexiconWireHeader*>(pView);
    pImpl_->isWritable = true;
    return true;
#else
    pImpl_->posixBlock = GetPosixRegistry().CreateForWriter(LEXICON_WIRE_MAPPING_NAME, WIRE_TOTAL_SIZE);
    if (!pImpl_->posixBlock) {
        return false;
    }
    pImpl_->pMapping = reinterpret_cast<volatile LexiconWireHeader*>(pImpl_->posixBlock->data());
    pImpl_->isWritable = true;
    return true;
#endif
}

void LexiconWireManager::Close() noexcept {
    if (pImpl_->mockStorage) {
        pImpl_->mockStorage = nullptr;
        pImpl_->mockSize = 0;
        pImpl_->pMapping = nullptr;
        pImpl_->isWritable = false;
        return;
    }

#if defined(_WIN32)
    if (pImpl_->pMapping) {
        UnmapViewOfFile(const_cast<void*>(reinterpret_cast<volatile void*>(pImpl_->pMapping)));
        pImpl_->pMapping = nullptr;
    }
    if (pImpl_->hMapping) {
        CloseHandle(pImpl_->hMapping);
        pImpl_->hMapping = nullptr;
    }
    if (pImpl_->hWriterMutex) {
        ReleaseMutex(pImpl_->hWriterMutex);
        CloseHandle(pImpl_->hWriterMutex);
        pImpl_->hWriterMutex = nullptr;
    }
#else
    if (pImpl_->posixBlock) {
        GetPosixRegistry().Remove(LEXICON_WIRE_MAPPING_NAME);
        pImpl_->posixBlock.reset();
        pImpl_->pMapping = nullptr;
    }
#endif
    pImpl_->isWritable = false;
}

bool LexiconWireManager::IsWritable() const noexcept {
    return pImpl_->isWritable && (pImpl_->pMapping != nullptr);
}

uint64_t LexiconWireManager::GetWireGeneration() const noexcept {
    return pImpl_->wireGeneration;
}

void LexiconWireManager::SetWireGeneration(uint64_t generation) noexcept {
    pImpl_->wireGeneration = generation;
}

uint64_t LexiconWireManager::IncrementWireGeneration() noexcept {
    pImpl_->wireGeneration++;
    return pImpl_->wireGeneration;
}

bool LexiconWireManager::Publish(
    const std::vector<std::wstring>& spellExclusions,
    const std::vector<std::wstring>& userDictionary,
    bool spellSuggestEnabled,
    std::string* outError) {
    uint64_t nextGen = IncrementWireGeneration();
    return Publish(spellExclusions, userDictionary, nextGen, spellSuggestEnabled, outError);
}

bool LexiconWireManager::Publish(
    const std::vector<std::wstring>& spellExclusions,
    const std::vector<std::wstring>& userDictionary,
    uint64_t generation,
    bool spellSuggestEnabled,
    std::string* outError) {
    std::vector<std::wstring> canonExcl;
    if (!spellExclusions.empty()) {
        auto res = SpellExclusionCanonicalizer::Canonicalize(spellExclusions);
        if (!res.Succeeded()) {
            if (outError) {
                std::string msg(res.error.message.begin(), res.error.message.end());
                *outError = "Spell exclusion canonicalization failed: " + msg;
            }
            return false;
        }
        canonExcl = std::move(res.entries);
    }

    std::vector<std::wstring> canonDict;
    canonDict.reserve(userDictionary.size());
    for (size_t i = 0; i < userDictionary.size(); ++i) {
        std::wstring norm;
        auto res = LexiconValidator::ValidateUserDictWord(userDictionary[i], i + 1, &norm);
        if (!res.Succeeded()) {
            if (outError) {
                std::string msg(res.errorLine > 0 ? "entry " + std::to_string(res.errorLine) + ": " : "");
                msg.append(res.errorMessage.begin(), res.errorMessage.end());
                *outError = "User dictionary validation failed at " + msg;
            }
            return false;
        }
        canonDict.push_back(std::move(norm));
    }

    LexiconWireSerializeParams params;
    params.generation = generation;
    params.spellSuggestEnabled = spellSuggestEnabled;
    params.spellExclusions.reserve(canonExcl.size());
    for (const auto& w : canonExcl) {
        params.spellExclusions.push_back(WStringToU16(w));
    }
    params.userDictionary.reserve(canonDict.size());
    for (const auto& w : canonDict) {
        params.userDictionary.push_back(WStringToU16(w));
    }
    return Publish(params, outError);
}

bool LexiconWireManager::Publish(
    const LexiconWireSerializeParams& params,
    std::string* outError) {
    if (!IsWritable()) {
        if (outError) *outError = "Wire mapping is not created or writable";
        return false;
    }

    alignas(8) uint8_t stagedBuffer[WIRE_TOTAL_SIZE];
    if (!LexiconWireSerializer::Serialize(params, stagedBuffer, WIRE_TOTAL_SIZE, outError)) {
        return false;
    }

    PublishStagedBufferToWire(pImpl_->pMapping, stagedBuffer, WIRE_TOTAL_SIZE);
    pImpl_->wireGeneration = params.generation;
    return true;
}

volatile LexiconWireHeader* LexiconWireManager::GetHeader() noexcept {
    return pImpl_->pMapping;
}

const volatile LexiconWireHeader* LexiconWireManager::GetHeader() const noexcept {
    return pImpl_->pMapping;
}

void LexiconWireManager::SetMockStorage(uint8_t* storage, size_t size) noexcept {
    Close();
    pImpl_->mockStorage = storage;
    pImpl_->mockSize = size;
    if (storage && size >= WIRE_TOTAL_SIZE) {
        pImpl_->pMapping = reinterpret_cast<volatile LexiconWireHeader*>(storage);
        pImpl_->isWritable = true;
    }
}

// ─── LexiconWireReader::Impl ────────────────────────────────────────────────

struct LexiconWireReader::Impl {
#if defined(_WIN32)
    HANDLE hMapping = nullptr;
    const volatile LexiconWireHeader* pMapping = nullptr;
#else
    std::shared_ptr<std::vector<uint8_t>> posixBlock;
    const volatile LexiconWireHeader* pMapping = nullptr;
#endif
    const uint8_t* mockStorage = nullptr;
    size_t mockSize = 0;
    bool isMapped = false;
    LexiconSnapshotIdentity cachedIdentity{};
};

LexiconWireReader::LexiconWireReader() : pImpl_(std::make_unique<Impl>()) {}

LexiconWireReader::~LexiconWireReader() {
    Close();
}

LexiconWireReader::LexiconWireReader(LexiconWireReader&&) noexcept = default;
LexiconWireReader& LexiconWireReader::operator=(LexiconWireReader&&) noexcept = default;

bool LexiconWireReader::Open() {
    Close();

    if (pImpl_->mockStorage && pImpl_->mockSize >= WIRE_TOTAL_SIZE) {
        pImpl_->pMapping = reinterpret_cast<const volatile LexiconWireHeader*>(pImpl_->mockStorage);
        pImpl_->isMapped = true;
        return true;
    }

#if defined(_WIN32)
    pImpl_->hMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, LEXICON_WIRE_MAPPING_NAME);
    if (!pImpl_->hMapping) {
        return false;
    }

    void* pView = MapViewOfFile(pImpl_->hMapping, FILE_MAP_READ, 0, 0, WIRE_TOTAL_SIZE);
    if (!pView) {
        CloseHandle(pImpl_->hMapping);
        pImpl_->hMapping = nullptr;
        return false;
    }

    pImpl_->pMapping = static_cast<const volatile LexiconWireHeader*>(pView);
    pImpl_->isMapped = true;
    return true;
#else
    pImpl_->posixBlock = GetPosixRegistry().Find(LEXICON_WIRE_MAPPING_NAME);
    if (!pImpl_->posixBlock) {
        return false;
    }
    pImpl_->pMapping = reinterpret_cast<const volatile LexiconWireHeader*>(pImpl_->posixBlock->data());
    pImpl_->isMapped = true;
    return true;
#endif
}

void LexiconWireReader::Close() noexcept {
    if (pImpl_->mockStorage) {
        pImpl_->mockStorage = nullptr;
        pImpl_->mockSize = 0;
        pImpl_->pMapping = nullptr;
        pImpl_->isMapped = false;
        return;
    }

#if defined(_WIN32)
    if (pImpl_->pMapping) {
        UnmapViewOfFile(const_cast<void*>(reinterpret_cast<const volatile void*>(pImpl_->pMapping)));
        pImpl_->pMapping = nullptr;
    }
    if (pImpl_->hMapping) {
        CloseHandle(pImpl_->hMapping);
        pImpl_->hMapping = nullptr;
    }
#else
    pImpl_->posixBlock.reset();
    pImpl_->pMapping = nullptr;
#endif
    pImpl_->isMapped = false;
}

bool LexiconWireReader::IsOpen() const noexcept {
    return pImpl_->isMapped && (pImpl_->pMapping != nullptr);
}

bool LexiconWireReader::HasEpochChanged(uint32_t currentSharedStateEpoch) const noexcept {
    return currentSharedStateEpoch != pImpl_->cachedIdentity.sharedStateEpoch;
}

bool LexiconWireReader::ReadSnapshot(
    std::vector<uint8_t>& localBuffer,
    LexiconWireView& outView,
    std::string* outError) {
    return ReadSnapshot(localBuffer, outView, pImpl_->cachedIdentity.sharedStateEpoch, outError);
}

bool LexiconWireReader::ReadSnapshot(
    std::vector<uint8_t>& localBuffer,
    LexiconWireView& outView,
    uint32_t currentSharedStateEpoch,
    std::string* outError) {
    if (!IsOpen()) {
        if (outError) *outError = "LexiconWireReader is not open";
        return false;
    }

    if (localBuffer.size() < WIRE_TOTAL_SIZE) {
        localBuffer.resize(WIRE_TOTAL_SIZE);
    }

    if (!ReadWireToLocalBuffer(pImpl_->pMapping, localBuffer.data(), WIRE_TOTAL_SIZE, 5)) {
        if (outError) *outError = "Failed to obtain consistent seqlock read from wire mapping";
        return false;
    }

    if (!LexiconWireDeserializer::ValidateAndInspect(localBuffer.data(), WIRE_TOTAL_SIZE, outView, outError)) {
        return false;
    }

    pImpl_->cachedIdentity.sharedStateEpoch = currentSharedStateEpoch;
    pImpl_->cachedIdentity.wireGeneration = outView.header->generation;
    pImpl_->cachedIdentity.wireCrc32 = outView.header->crc32;
    return true;
}

bool LexiconWireReader::ReadSnapshotFast(
    std::vector<uint8_t>& localBuffer,
    LexiconWireView& outView,
    uint32_t currentSharedStateEpoch,
    bool* outWasUpdated,
    std::string* outError) {
    if (!IsOpen()) {
        if (!Open()) {
            if (outError) *outError = "LexiconWireReader could not open wire mapping";
            return false;
        }
    }

    // Path 1: Zero-cost fast path (< 1 ns): SharedState epoch has not changed since last read
    if (pImpl_->cachedIdentity.wireGeneration > 0 &&
        !HasEpochChanged(currentSharedStateEpoch)) {
        if (outWasUpdated) *outWasUpdated = false;
        return true;
    }

    // Path 2: SharedState epoch changed, but wire mapping generation & CRC32 might be identical
    if (pImpl_->cachedIdentity.wireGeneration > 0 && pImpl_->pMapping != nullptr) {
        uint32_t seq1 = SeqlockBeginRead(pImpl_->pMapping->seqlock);
        if ((seq1 & 1) == 0 && pImpl_->pMapping->magic == WIRE_MAGIC) {
            uint64_t gen = pImpl_->pMapping->generation;
            uint32_t crc = pImpl_->pMapping->crc32;
            if (SeqlockValidateRead(pImpl_->pMapping->seqlock, seq1) &&
                gen == pImpl_->cachedIdentity.wireGeneration &&
                crc == pImpl_->cachedIdentity.wireCrc32) {
                // Wire mapping content did not change; update cached epoch and return
                pImpl_->cachedIdentity.sharedStateEpoch = currentSharedStateEpoch;
                if (outWasUpdated) *outWasUpdated = false;
                return true;
            }
        }
    }

    // Path 3: Wire content changed or initial read: perform full seqlock copy and validation
    if (!ReadSnapshot(localBuffer, outView, currentSharedStateEpoch, outError)) {
        return false;
    }

    if (outWasUpdated) *outWasUpdated = true;
    return true;
}

const LexiconSnapshotIdentity& LexiconWireReader::GetCachedIdentity() const noexcept {
    return pImpl_->cachedIdentity;
}

void LexiconWireReader::SetCachedIdentity(const LexiconSnapshotIdentity& id) noexcept {
    pImpl_->cachedIdentity = id;
}

void LexiconWireReader::ResetCachedIdentity() noexcept {
    pImpl_->cachedIdentity = {};
}

void LexiconWireReader::SetMockStorage(const uint8_t* storage, size_t size) noexcept {
    Close();
    pImpl_->mockStorage = storage;
    pImpl_->mockSize = size;
    if (storage && size >= WIRE_TOTAL_SIZE) {
        pImpl_->pMapping = reinterpret_cast<const volatile LexiconWireHeader*>(storage);
        pImpl_->isMapped = true;
    }
}

} // namespace NextKey::Wire
