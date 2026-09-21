// VKey - Lexicon Shared Memory Wire Manager and Lock-Free Reader
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LexiconWireFormat.h"

namespace NextKey::Wire {

/// Single-writer lifecycle and publishing manager for Local\VKeyLexiconWireMapping.
/// Only the Core process (VKeyApp.exe) creates and writes to this mapping.
class LexiconWireManager {
public:
    LexiconWireManager();
    ~LexiconWireManager();

    LexiconWireManager(const LexiconWireManager&) = delete;
    LexiconWireManager& operator=(const LexiconWireManager&) = delete;
    LexiconWireManager(LexiconWireManager&&) noexcept;
    LexiconWireManager& operator=(LexiconWireManager&&) noexcept;

    /// Creates Local\VKeyLexiconWireMapping (144 KiB) with AppContainer DACL.
    /// Returns true on success.
    bool Create();

    /// Unmaps and closes the shared memory mapping.
    void Close() noexcept;

    /// Returns true if the mapping is active and writable by this manager.
    [[nodiscard]] bool IsWritable() const noexcept;

    /// Gets the current 64-bit monotonic wire generation.
    [[nodiscard]] uint64_t GetWireGeneration() const noexcept;

    /// Sets the current 64-bit monotonic wire generation (e.g. from config.toml on startup).
    void SetWireGeneration(uint64_t generation) noexcept;

    /// Increments and returns the next 64-bit monotonic wire generation.
    uint64_t IncrementWireGeneration() noexcept;

    /// Publishes a pre-canonicalized lexicon snapshot using the current wire generation.
    /// Automatically increments wire generation before publishing.
    bool Publish(
        const std::vector<std::wstring>& spellExclusions,
        const std::vector<std::wstring>& userDictionary,
        bool spellSuggestEnabled,
        std::string* outError = nullptr);

    /// Publishes a pre-canonicalized lexicon snapshot with an explicit generation.
    bool Publish(
        const std::vector<std::wstring>& spellExclusions,
        const std::vector<std::wstring>& userDictionary,
        uint64_t generation,
        bool spellSuggestEnabled,
        std::string* outError = nullptr);

    /// Publishes using LexiconWireSerializeParams.
    bool Publish(
        const LexiconWireSerializeParams& params,
        std::string* outError = nullptr);

    /// Returns the mapped wire header pointer (or nullptr if not mapped).
    [[nodiscard]] volatile LexiconWireHeader* GetHeader() noexcept;
    [[nodiscard]] const volatile LexiconWireHeader* GetHeader() const noexcept;

    /// Test helper: inject in-memory storage for deterministic mock testing.
    void SetMockStorage(uint8_t* storage, size_t size) noexcept;

    /// Test helper: inject fault for security descriptor / ACL application failure.
    static void SetTestSecurityFailure(bool fail) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl_;
};

/// Lock-free zero-copy reader for Local\VKeyLexiconWireMapping.
/// Used by TSF DLL and Hook Engine to access dictionary snapshots without disk I/O.
class LexiconWireReader {
public:
    LexiconWireReader();
    ~LexiconWireReader();

    LexiconWireReader(const LexiconWireReader&) = delete;
    LexiconWireReader& operator=(const LexiconWireReader&) = delete;
    LexiconWireReader(LexiconWireReader&&) noexcept;
    LexiconWireReader& operator=(LexiconWireReader&&) noexcept;

    /// Opens Local\VKeyLexiconWireMapping in read-only mode.
    /// Returns true if mapping exists and is opened.
    bool Open();

    /// Unmaps and closes the shared memory mapping.
    void Close() noexcept;

    /// Returns true if currently mapped.
    [[nodiscard]] bool IsOpen() const noexcept;

    /// Checks if SharedState epoch has changed since the last cached snapshot.
    [[nodiscard]] bool HasEpochChanged(uint32_t currentSharedStateEpoch) const noexcept;

    /// Reads a consistent snapshot from the wire mapping into localBuffer.
    /// localBuffer is resized to WIRE_TOTAL_SIZE if needed.
    /// Populates outView with pointers pointing directly into localBuffer.
    /// Returns true on success, false if conflict persists or corrupted.
    bool ReadSnapshot(
        std::vector<uint8_t>& localBuffer,
        LexiconWireView& outView,
        std::string* outError = nullptr);

    /// Reads a consistent snapshot from the wire mapping into localBuffer.
    /// Updates cached identity with currentSharedStateEpoch, wireGeneration, and wireCrc32.
    bool ReadSnapshot(
        std::vector<uint8_t>& localBuffer,
        LexiconWireView& outView,
        uint32_t currentSharedStateEpoch,
        std::string* outError = nullptr);

    /// Fast-path snapshot reader:
    /// 1. If cached wireGeneration > 0 and !HasEpochChanged(currentSharedStateEpoch),
    ///    immediately returns true with outWasUpdated = false (< 1 ns).
    /// 2. If epoch changed but wire header generation & CRC32 match cachedIdentity,
    ///    updates cached sharedStateEpoch and returns true with outWasUpdated = false (skips 144 KiB copy).
    /// 3. If wire changed, reads 144 KiB via seqlock, validates CRC32, updates cachedIdentity,
    ///    and returns true with outWasUpdated = true.
    bool ReadSnapshotFast(
        std::vector<uint8_t>& localBuffer,
        LexiconWireView& outView,
        uint32_t currentSharedStateEpoch,
        bool* outWasUpdated = nullptr,
        std::string* outError = nullptr);

    /// Cached identity tuple for zero-latency hot-path checks.
    [[nodiscard]] const LexiconSnapshotIdentity& GetCachedIdentity() const noexcept;
    void SetCachedIdentity(const LexiconSnapshotIdentity& id) noexcept;
    void ResetCachedIdentity() noexcept;

    /// Test helper: inject in-memory storage for deterministic mock testing.
    void SetMockStorage(const uint8_t* storage, size_t size) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace NextKey::Wire
