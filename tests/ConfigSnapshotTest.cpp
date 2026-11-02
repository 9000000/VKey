// ConfigSnapshotTest.cpp
// SPDX-License-Identifier: GPL-3.0-only
//
// Phase 3a — single-publisher RCU semantics for `ConfigSnapshot`. See
// docs/plans/2026-05-19-architecture-review-design.md §Phase 3.
//
// The snapshot collects all the "variable-size config data" the hook hot
// path reads — excluded apps set, TSF apps set, macro table, per-app
// encoding/method overrides — and publishes it as a single immutable
// `shared_ptr<const ConfigSnapshot>`. Writers (worker thread, off hook)
// build a fresh snapshot from TOML and `atomic_store` it; readers (hook
// thread, in ProcessKeyDown and ClassifyFocusedWindow) `atomic_load` once
// per call. Old snapshots stay alive while readers hold them — no
// use-after-free across publication boundaries.
//
// HookEngine itself is Win32-only and not linked into VKeyTests, so this
// suite exercises the type + RCU pattern in isolation, the same way
// TypingConfigRCUTests does for `config_`.

#include <gtest/gtest.h>

#include "core/config/ConfigSnapshot.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace NextKey {
namespace {

// Compile-time: snapshot must be usable through `std::atomic<std::shared_ptr<const T>>`
// (C++20 P0718). MSVC 16.11+ / GCC 12+ required — same toolchain floor as
// HookEngine::config_.
static_assert(std::is_same_v<
    decltype(std::declval<std::atomic<std::shared_ptr<const ConfigSnapshot>>&>().load()),
    std::shared_ptr<const ConfigSnapshot>>,
    "Phase 3a: ConfigSnapshot must round-trip through atomic<shared_ptr<const T>>.");

// ──────────────────────────────────────────────────────────────────────────
// Default state: every container empty, generation = 0. Locks the "always
// safe to dereference" invariant — HookEngine ctor default-constructs the
// snapshot before any TOML parse runs, so hook callbacks reaching the
// reader before Phase 3 producer publishes anything will see this default.
// ──────────────────────────────────────────────────────────────────────────
TEST(ConfigSnapshot, DefaultIsEmpty) {
    ConfigSnapshot snap;
    EXPECT_TRUE(snap.appEncodingOverrides.empty());
    EXPECT_TRUE(snap.appInputMethodOverrides.empty());
    EXPECT_TRUE(snap.appSendMethodOverrides.empty());
    EXPECT_TRUE(snap.excludedAppSet.empty());
    EXPECT_TRUE(snap.tsfAppSet.empty());
    EXPECT_TRUE(snap.macroTable.empty());
    EXPECT_TRUE(snap.spaceMacroKeys.empty());
    EXPECT_EQ(snap.generation, 0u);
}

// ──────────────────────────────────────────────────────────────────────────
// Round-trip: store + load returns the same data. Trivial correctness
// check — verifies the snapshot is shared_ptr-friendly (no copy ctor traps,
// no move-only members).
// ──────────────────────────────────────────────────────────────────────────
TEST(ConfigSnapshot, StoreLoadRoundTrip) {
    ConfigSnapshot src;
    src.excludedAppSet.insert(L"badapp.exe");
    src.tsfAppSet.insert(L"word.exe");
    src.macroTable[L"chol"] = L"chôl";
    src.macroTable[L"co the"] = L"có thể";
    src.spaceMacroKeys.insert(L"co the");
    src.appEncodingOverrides[L"legacy.exe"] = CodeTable::TCVN3;
    src.appInputMethodOverrides[L"legacy.exe"] = InputMethod::VNI;
    src.appSendMethodOverrides[L"clipboard-only.exe"] = 1;
    src.generation = 42;

    std::atomic<std::shared_ptr<const ConfigSnapshot>> field;
    field.store(std::make_shared<const ConfigSnapshot>(src),
                std::memory_order_release);

    auto loaded = field.load(std::memory_order_acquire);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->excludedAppSet.count(L"badapp.exe"), 1u);
    EXPECT_EQ(loaded->tsfAppSet.count(L"word.exe"), 1u);
    ASSERT_EQ(loaded->macroTable.count(L"chol"), 1u);
    EXPECT_EQ(loaded->macroTable.at(L"chol"), L"chôl");
    EXPECT_EQ(loaded->spaceMacroKeys.count(L"co the"), 1u);
    ASSERT_EQ(loaded->appEncodingOverrides.count(L"legacy.exe"), 1u);
    EXPECT_EQ(loaded->appEncodingOverrides.at(L"legacy.exe"), CodeTable::TCVN3);
    ASSERT_EQ(loaded->appInputMethodOverrides.count(L"legacy.exe"), 1u);
    EXPECT_EQ(loaded->appInputMethodOverrides.at(L"legacy.exe"), InputMethod::VNI);
    ASSERT_EQ(loaded->appSendMethodOverrides.count(L"clipboard-only.exe"), 1u);
    EXPECT_EQ(loaded->appSendMethodOverrides.at(L"clipboard-only.exe"), 1);
    EXPECT_EQ(loaded->generation, 42u);
}

// ──────────────────────────────────────────────────────────────────────────
// Old snapshot lifetime: a reader's shared_ptr keeps the object alive even
// after the writer publishes a newer one. This is the Phase 3 use-after-free
// safety guarantee — hook thread holds a load() through the duration of
// `ProcessKeyDown`; worker may republish mid-keystroke; the hook side must
// not see torn data.
// ──────────────────────────────────────────────────────────────────────────
TEST(ConfigSnapshot, OldSnapshotSurvivesWhileReaderHolds) {
    std::atomic<std::shared_ptr<const ConfigSnapshot>> field;

    auto snap1 = std::make_shared<ConfigSnapshot>();
    snap1->generation = 1;
    snap1->excludedAppSet.insert(L"first.exe");
    field.store(snap1, std::memory_order_release);

    // Reader takes its handle.
    auto readerView = field.load(std::memory_order_acquire);

    // Writer publishes a new snapshot.
    auto snap2 = std::make_shared<ConfigSnapshot>();
    snap2->generation = 2;
    snap2->excludedAppSet.insert(L"second.exe");
    field.store(snap2, std::memory_order_release);

    // Drop the writer's local handles. snap1 is now only held by readerView.
    snap1.reset();
    snap2.reset();

    // Reader can still read its (older) snapshot — no UAF.
    ASSERT_NE(readerView, nullptr);
    EXPECT_EQ(readerView->generation, 1u);
    EXPECT_EQ(readerView->excludedAppSet.count(L"first.exe"), 1u);
    EXPECT_EQ(readerView->excludedAppSet.count(L"second.exe"), 0u);

    // A fresh load now sees the newer snapshot.
    auto newerView = field.load(std::memory_order_acquire);
    ASSERT_NE(newerView, nullptr);
    EXPECT_EQ(newerView->generation, 2u);
    EXPECT_EQ(newerView->excludedAppSet.count(L"second.exe"), 1u);
}

// ──────────────────────────────────────────────────────────────────────────
// Last-store-wins: rapid writers don't lose data — the latest store
// becomes visible to subsequent loads. RCU doesn't merge or queue stores;
// it overwrites. Phase 3 producer is single-threaded (worker), so this is
// the relevant ordering.
// ──────────────────────────────────────────────────────────────────────────
TEST(ConfigSnapshot, RapidStoresLastWins) {
    std::atomic<std::shared_ptr<const ConfigSnapshot>> field;
    for (std::uint32_t gen = 1; gen <= 5; ++gen) {
        auto s = std::make_shared<ConfigSnapshot>();
        s->generation = gen;
        field.store(s, std::memory_order_release);
    }
    auto final = field.load(std::memory_order_acquire);
    ASSERT_NE(final, nullptr);
    EXPECT_EQ(final->generation, 5u);
}

// ──────────────────────────────────────────────────────────────────────────
// Concurrent readers + 1 writer stress. Producers cycle generations; readers
// must observe a non-null snapshot at every load AND the data must be
// internally consistent (generation matches the contents — locks the
// "no half-published snapshot" invariant). std::atomic_store of shared_ptr
// is the boundary; nothing inside ConfigSnapshot itself is atomic, so this
// test would catch a regression that tried to swap to e.g. atomic_store on
// individual fields instead of the whole pointer.
// ──────────────────────────────────────────────────────────────────────────
TEST(ConfigSnapshot, ConcurrentReadersSeeConsistentView) {
    std::atomic<std::shared_ptr<const ConfigSnapshot>> field;
    auto initial = std::make_shared<ConfigSnapshot>();
    initial->generation = 1;
    initial->excludedAppSet.insert(L"gen1.exe");
    field.store(initial, std::memory_order_release);

    std::atomic<bool> stop{false};
    std::atomic<int>  inconsistent{0};

    auto reader = [&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto snap = field.load(std::memory_order_acquire);
            if (!snap) { ++inconsistent; continue; }
            // Invariant: marker key for the current generation is present
            // exactly when the snapshot's generation matches. Any
            // mismatch implies torn publication.
            const auto gen = snap->generation;
            std::wstring expectedKey = L"gen" + std::to_wstring(gen) + L".exe";
            if (snap->excludedAppSet.count(expectedKey) != 1) {
                ++inconsistent;
            }
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back(reader);

    // Single writer cycles through 5 generations, then 5 again, then stop.
    for (int round = 0; round < 2; ++round) {
        for (std::uint32_t gen = 1; gen <= 5; ++gen) {
            auto s = std::make_shared<ConfigSnapshot>();
            s->generation = gen;
            s->excludedAppSet.insert(L"gen" + std::to_wstring(gen) + L".exe");
            field.store(s, std::memory_order_release);
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    EXPECT_EQ(inconsistent.load(), 0)
        << "Readers observed a snapshot whose marker key didn't match its "
           "generation — that's a torn-publication failure, the exact "
           "scenario RCU is supposed to prevent.";
}

// ──────────────────────────────────────────────────────────────────────────
// Build helper (P3b) — pure function that the worker-side producer uses to
// assemble a snapshot from raw config data. Two pieces of logic to verify:
//   (a) `spaceMacroKeys` is derived from `macroTable` (subset of keys that
//       contain a space). The hook reads this set to short-circuit the
//       multi-word-macro lookup on space commit; deriving inside Build
//       keeps the contract that snapshot fields are mutually consistent.
//   (b) All other fields pass through by move — round-trip preserves data.
// ──────────────────────────────────────────────────────────────────────────
TEST(ConfigSnapshotBuild, EmptyInputsProduceEmptySnapshotWithGeneration) {
    auto snap = ConfigSnapshot::Build(
        /*macroTable*/    {},
        /*excludedApps*/  {},
        /*forcedVnApps*/  {},
        /*tsfApps*/       {},
        /*encOverrides*/  {},
        /*imOverrides*/   {},
        /*sendOverrides*/ {},
        /*generation*/    7);
    EXPECT_TRUE(snap.macroTable.empty());
    EXPECT_TRUE(snap.excludedAppSet.empty());
    EXPECT_TRUE(snap.forcedVietnameseAppSet.empty());
    EXPECT_TRUE(snap.tsfAppSet.empty());
    EXPECT_TRUE(snap.appEncodingOverrides.empty());
    EXPECT_TRUE(snap.appInputMethodOverrides.empty());
    EXPECT_TRUE(snap.appSendMethodOverrides.empty());
    EXPECT_TRUE(snap.spaceMacroKeys.empty());
    EXPECT_EQ(snap.generation, 7u);
}

TEST(ConfigSnapshotBuild, SpaceMacroKeysDerivedFromMacroTable) {
    std::unordered_map<std::wstring, std::wstring> macros = {
        {L"chol",     L"chôl"},        // no space → excluded
        {L"co the",   L"có thể"},      // has space → included
        {L"vd",       L"ví dụ"},       // value has space, key doesn't → excluded
        {L"co le",    L"có lẽ"},       // has space → included
        {L"khong",    L"không"},       // no space → excluded
    };
    auto snap = ConfigSnapshot::Build(
        std::move(macros), {}, {}, {}, {}, {}, {}, /*generation*/ 1);
    EXPECT_EQ(snap.macroTable.size(), 5u)
        << "all macro entries should land in macroTable verbatim";
    EXPECT_EQ(snap.spaceMacroKeys.size(), 2u);
    EXPECT_EQ(snap.spaceMacroKeys.count(L"co the"), 1u);
    EXPECT_EQ(snap.spaceMacroKeys.count(L"co le"), 1u);
    EXPECT_EQ(snap.spaceMacroKeys.count(L"chol"), 0u);
    EXPECT_EQ(snap.spaceMacroKeys.count(L"khong"), 0u);
}

TEST(ConfigSnapshotBuild, RoundTripsAllFields) {
    std::unordered_map<std::wstring, std::wstring> macros = {{L"vn", L"Việt Nam"}};
    std::unordered_set<std::wstring> excluded = {L"banking.exe", L"vault.exe"};
    std::unordered_set<std::wstring> forcedVn = {L"zalo.exe", L"messenger.exe"};
    std::unordered_set<std::wstring> tsf      = {L"word.exe"};
    std::unordered_map<std::wstring, CodeTable>   enc  = {{L"legacy.exe", CodeTable::TCVN3}};
    std::unordered_map<std::wstring, InputMethod> im   = {{L"legacy.exe", InputMethod::VNI}};
    // P3d follow-up: appSendMethodOverrides was the last variable-size
    // config map left on HookEngine. Now flows through the snapshot so
    // the main-thread reader (ClassifyFocusedWindow) sees a stable view
    // even while the worker rebuilds.
    std::unordered_map<std::wstring, int8_t>      send = {{L"legacy.exe", 1}};

    auto snap = ConfigSnapshot::Build(
        std::move(macros), std::move(excluded), std::move(forcedVn), std::move(tsf),
        std::move(enc), std::move(im), std::move(send), /*generation*/ 99);

    EXPECT_EQ(snap.generation, 99u);
    EXPECT_EQ(snap.macroTable.at(L"vn"), L"Việt Nam");
    EXPECT_EQ(snap.excludedAppSet.count(L"banking.exe"), 1u);
    EXPECT_EQ(snap.excludedAppSet.count(L"vault.exe"), 1u);
    EXPECT_EQ(snap.forcedVietnameseAppSet.count(L"zalo.exe"), 1u);
    EXPECT_EQ(snap.forcedVietnameseAppSet.count(L"messenger.exe"), 1u);
    EXPECT_EQ(snap.tsfAppSet.count(L"word.exe"), 1u);
    EXPECT_EQ(snap.appEncodingOverrides.at(L"legacy.exe"), CodeTable::TCVN3);
    EXPECT_EQ(snap.appInputMethodOverrides.at(L"legacy.exe"), InputMethod::VNI);
    EXPECT_EQ(snap.appSendMethodOverrides.at(L"legacy.exe"), 1);
    // No spaces in this macro's key → empty spaceMacroKeys.
    EXPECT_TRUE(snap.spaceMacroKeys.empty());
}

TEST(ConfigSnapshotBuild, PrefixIndexStaysWithItsPublishedSnapshot) {
    auto first = ConfigSnapshot::Build(
        {{L"hi there", L"hello"}, {L"Strict Key", L"strict"}, {L"single", L"value"}},
        {}, {}, {}, {}, {}, {}, 1);
    auto retained = first;
    first = ConfigSnapshot::Build({{L"new phrase", L"new"}}, {}, {}, {}, {}, {}, {}, 2);
    EXPECT_TRUE(IsSpaceMacroPrefix(L"HI ", retained.macroPrefixIndex));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"Strict ", retained.macroPrefixIndex));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"strict ", retained.macroPrefixIndex));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"single", retained.macroPrefixIndex));
    EXPECT_FALSE(IsSpaceMacroPrefix(L"HI ", first.macroPrefixIndex));
    EXPECT_TRUE(IsSpaceMacroPrefix(L"NEW ", first.macroPrefixIndex));
}

}  // namespace
}  // namespace NextKey
