// VKey - RCU config snapshot (Phase 3a — config reload out of keydown)
// SPDX-License-Identifier: GPL-3.0-only
//
// Phase 3 of the 2026-05-19 architecture review design
// (docs/plans/2026-05-19-architecture-review-design.md §Phase 3).
//
// Bundles every "variable-size config field" that today lives as a
// separately-owned `unordered_map` / `unordered_set` member of HookEngine
// (excludedAppSet_, tsfAppSet_, macroTable_, spaceMacroKeys_,
// appEncodingOverrides_, appInputMethodOverrides_) into a single
// immutable struct, published via `atomic<shared_ptr<const T>>` — the
// same RCU pattern HookEngine already uses for `config_` and `hotkeys_`.
//
// Why this exists (Rule 11.3 + Rule 11.2):
//   - Pre-Phase-3, `ReloadFromToml` ran on hook thread when triggered from
//     `QuickSyncFromSharedState` slow path inside `ProcessKeyDown` — that's
//     1-10ms of TOML parsing on the LL callback thread, exactly the
//     pattern Rule 11.2 forbids.
//   - Phase 3 moves the parse onto the worker thread and lets it publish
//     a fresh `shared_ptr<const ConfigSnapshot>`. The hook thread just
//     does an atomic load once per call — no syscalls, no allocations,
//     no contention with main.
//
// Phase 3a ships THIS header + the atomic field on HookEngine dormant —
// no producer yet, no readers migrated. Phase 3b adds the worker-side
// builder; Phase 3c migrates the readers; Phase 3d removes the legacy
// fields.
//
// Linux-portable: no Win32 dependencies. The tests in
// `tests/ConfigSnapshotTest.cpp` exercise the RCU semantics on Linux.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/config/TypingConfig.h"
#include "core/MacroPrefix.h"

namespace NextKey {

/// Immutable bundle of all "variable-size config data" the hook hot path
/// needs to read. Published via `std::atomic<std::shared_ptr<const
/// ConfigSnapshot>>`; readers load once per call, writers swap the whole
/// pointer atomically. The struct itself is plain — only the publishing
/// pointer is atomic.
///
/// `generation` mirrors `SharedState.configGeneration` at the moment the
/// snapshot was built. Readers needing to detect "config changed since I
/// last looked" compare the snapshot's generation against a cached value;
/// equality means no further work needed.
struct ConfigSnapshot {
    /// Per-app code-table override. -1 in source data ("inherit global")
    /// is filtered out before insertion — readers can treat membership
    /// as "yes, this app has a real override".
    std::unordered_map<std::wstring, CodeTable> appEncodingOverrides;

    /// Per-app input-method override. Same filtering as above.
    std::unordered_map<std::wstring, InputMethod> appInputMethodOverrides;

    /// Per-app send-method override (0 = SendInput, 1 = clipboard paste,
    /// -1 = inherit global → filtered out at build time). Read on main
    /// thread by `ClassifyFocusedWindow` to populate `FocusClassification
    /// ::localUseClipboardInjector`. Pre-promotion lived as a plain
    /// HookEngine member written from the worker rebuild path; Rule
    /// 11.3 torn-read window of ~5 ms on TOML reparse. Promoting into
    /// the snapshot puts every variable-size config map under the same
    /// RCU contract.
    std::unordered_map<std::wstring, int8_t> appSendMethodOverrides;

    /// Apps that bypass Vietnamese processing entirely (force English).
    /// Lookup is exe-name lowercase.
    std::unordered_set<std::wstring> excludedAppSet;

    /// Apps locked to Vietnamese (per-app hard-V: force V on focus, block the
    /// V/E toggle while focused). Lookup is exe-name lowercase. Kept DISJOINT
    /// from excludedAppSet at build time (excluded wins — see
    /// core/PerAppModeDecision.h) so the runtime never sees an app in both.
    std::unordered_set<std::wstring> forcedVietnameseAppSet;

    /// Apps that should use the TSF TIP instead of the LL hook engine.
    /// Same lookup as excludedAppSet.
    std::unordered_set<std::wstring> tsfAppSet;

    /// Macro expansions — key is the typed sequence (lowercased), value
    /// is the expanded text. Hook reads on commit-trigger via
    /// `TryExpandMacro`.
    std::unordered_map<std::wstring, std::wstring> macroTable;

    /// Subset of macroTable KEYS (whole strings, not characters) that
    /// contain a space — the hook's space-trigger path uses these to
    /// decide whether `<rawBuffer> + ' '` could still grow into a
    /// multi-word macro before committing. Mirrors the legacy
    /// `HookEngine::spaceMacroKeys_` field exactly so P3c reader
    /// migration is a 1:1 swap, not a re-interpretation.
    std::unordered_set<std::wstring> spaceMacroKeys;

    /// Built with the snapshot, so typing only reads the derived prefix index.
    MacroPrefixIndex macroPrefixIndex;

    /// SharedState.configGeneration at build time. Producers must set
    /// this; default = 0 means "no config has been published yet"
    /// (HookEngine ctor's default snapshot).
    std::uint32_t generation{0};

    /// Pure builder used by the worker-side producer (P3b). Takes ownership
    /// of caller-built maps (move them in to avoid an extra copy) and
    /// derives `spaceMacroKeys` from `macroTable` in one pass — that
    /// derivation is the only piece of real logic; everything else is a
    /// move-in. Linux-portable, gtest-covered. Wired into HookEngine's
    /// slow-path producer at the bottom of `ReloadFromToml`.
    [[nodiscard]] static ConfigSnapshot Build(
        std::unordered_map<std::wstring, std::wstring> macroTable,
        std::unordered_set<std::wstring>               excludedAppSet,
        std::unordered_set<std::wstring>               forcedVietnameseAppSet,
        std::unordered_set<std::wstring>               tsfAppSet,
        std::unordered_map<std::wstring, CodeTable>    appEncodingOverrides,
        std::unordered_map<std::wstring, InputMethod>  appInputMethodOverrides,
        std::unordered_map<std::wstring, std::int8_t>  appSendMethodOverrides,
        std::uint32_t                                  generation) {
        ConfigSnapshot snap;
        snap.macroTable              = std::move(macroTable);
        snap.excludedAppSet          = std::move(excludedAppSet);
        snap.forcedVietnameseAppSet  = std::move(forcedVietnameseAppSet);
        snap.tsfAppSet               = std::move(tsfAppSet);
        snap.appEncodingOverrides    = std::move(appEncodingOverrides);
        snap.appInputMethodOverrides = std::move(appInputMethodOverrides);
        snap.appSendMethodOverrides  = std::move(appSendMethodOverrides);
        snap.generation              = generation;
        for (const auto& [key, _] : snap.macroTable) {
            if (key.find(L' ') != std::wstring::npos) {
                snap.spaceMacroKeys.insert(key);
            }
        }
        snap.macroPrefixIndex = MacroPrefixIndex(snap.spaceMacroKeys);
        return snap;
    }
};

}  // namespace NextKey
