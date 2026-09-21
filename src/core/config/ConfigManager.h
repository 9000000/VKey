// VKey - Configuration Manager
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include "TypingConfig.h"
#include "core/UIConfig.h"
#include "core/SystemConfig.h"
#include "core/hotkey/HotkeyRegistry.h"

namespace NextKey {

/// Per-app override settings (encoding + input method)
struct AppOverrideEntry {
    int8_t inputMethod = -1;       // -1=inherit global, 0=Telex, 1=VNI, 2=SimpleTelex, 3=Combined, 4=UserDefined
    int8_t encodingOverride = -1;  // -1=inherit global, 0-4=CodeTable value
    int8_t sendMethod = -1;        // -1=inherit, 0=SendInput, 1=Clipboard
};

/// Manages loading and saving of configuration from TOML file
/// FR5: Load configuration from TOML at word boundary
/// FR8: Engine works with compiled defaults when config missing
class ConfigManager {
public:
    /// Load config from file, returns nullopt if file doesn't exist or is invalid
    [[nodiscard]] static std::optional<TypingConfig> LoadFromFile(const std::wstring& path);

    /// Save config to file, returns true on success
    [[nodiscard]] static bool SaveToFile(const std::wstring& path, const TypingConfig& config);

    /// Get the config file path (exe dir or %APPDATA% fallback).
    /// `moduleHint`: HMODULE of the calling module, or nullptr for the current
    /// process's main executable. Pass a DLL's own module handle (e.g. the TSF
    /// DLL's g_hInstance) when calling from code that may be loaded in-process
    /// into a foreign host (nullptr there resolves to the HOST exe's directory,
    /// not VKey's install dir — see EngineController::ReloadMacros).
    [[nodiscard]] static std::wstring GetConfigPath(void* moduleHint = nullptr);

    /// Load config with automatic path resolution
    /// Returns compiled defaults if no config file found
    [[nodiscard]] static TypingConfig LoadOrDefault();

    /// Load UI config from file, returns nullopt if not found
    [[nodiscard]] static std::optional<UIConfig> LoadUIConfig(const std::wstring& path);

    /// Save UI config to file (merges with existing config)
    [[nodiscard]] static bool SaveUIConfig(const std::wstring& path, const UIConfig& config);

    /// Load UI config with automatic path resolution
    [[nodiscard]] static UIConfig LoadUIConfigOrDefault();

    /// Load hotkey config from file
    [[nodiscard]] static std::optional<HotkeyConfig> LoadHotkeyConfig(const std::wstring& path);

    /// Save hotkey config to file (merges with existing config)
    [[nodiscard]] static bool SaveHotkeyConfig(const std::wstring& path, const HotkeyConfig& config);

    /// Load hotkey config with automatic path resolution
    [[nodiscard]] static HotkeyConfig LoadHotkeyConfigOrDefault();

    /// Load unified hotkey registry (`[[hotkeys]]` TOML array) — covers
    /// cancel-composition / skip-macro / toggle-enabled triggers. Returns
    /// nullopt if file unreadable, fresh-defaults if section missing.
    [[nodiscard]] static std::optional<HotkeyRegistry>
    LoadHotkeyRegistry(const std::wstring& path);

    /// Persist unified hotkey registry to `[[hotkeys]]` array (merges file).
    [[nodiscard]] static bool
    SaveHotkeyRegistry(const std::wstring& path, const HotkeyRegistry& registry);

    /// Load registry with automatic path resolution. Returns Defaults() on
    /// any read failure (file missing, parse error, IO error).
    [[nodiscard]] static HotkeyRegistry LoadHotkeyRegistryOrDefault();

    /// One-shot migration: if `[[hotkeys]]` is missing or empty, read pre-v3
    /// `[features]` toggles (esc_restore_raw / temp_off_macro_esc /
    /// temp_off_method) directly from TOML, build a registry from them, and
    /// persist it. Returns the registry that should now be used.
    ///
    /// Idempotent — if `[hotkey_state]` is already present (sentinel for "v3
    /// UI touched this file"), returns the stored registry without rewriting.
    /// Safe to call on every Start/ReloadFromToml.
    [[nodiscard]] static HotkeyRegistry MigrateLegacyHotkeysIfNeeded(const std::wstring& path);

    /// Load all excluded apps (merges [excluded_apps].list + .soft for backward compat)
    [[nodiscard]] static std::vector<std::wstring> LoadAllExcludedApps(const std::wstring& path);

    /// Save excluded apps list to config ([excluded_apps].list — hard-E).
    /// Preserves the sibling [excluded_apps].force_vn array.
    [[nodiscard]] static bool SaveExcludedApps(const std::wstring& path,
                                                const std::vector<std::wstring>& apps);

    /// Load apps locked to Vietnamese ([excluded_apps].force_vn — hard-V).
    [[nodiscard]] static std::vector<std::wstring> LoadForcedVnApps(const std::wstring& path);

    /// Save the hard-V app list to [excluded_apps].force_vn.
    /// Preserves the sibling [excluded_apps].list (hard-E) array.
    [[nodiscard]] static bool SaveForcedVnApps(const std::wstring& path,
                                                const std::vector<std::wstring>& apps);

    /// Load V2 schema `[smart_switch.apps]` table (lowercase exe → isVietnamese).
    /// Falls back to legacy `[smart_switch].english_mode_apps` array if V2
    /// is absent (one-time silent migration). Returns empty map on parse
    /// error or missing file. Unknown mode strings are skipped + logged.
    /// Cap at kMaxSmartSwitchEntries entries.
    [[nodiscard]] static std::unordered_map<std::wstring, bool>
        LoadSmartSwitchApps(const std::wstring& path);

    /// Save the full V2 schema. Keys sorted alphabetically (clean diffs).
    /// Existing other TOML sections preserved. Acquires ConfigFileLock.
    [[nodiscard]] static bool SaveSmartSwitchApps(
        const std::wstring& path,
        const std::unordered_map<std::wstring, bool>& apps);

    /// Load TSF apps list from config (apps that use TSF engine instead of hook)
    [[nodiscard]] static std::vector<std::wstring> LoadTsfApps(const std::wstring& path);

    /// Save TSF apps list to config (merges with existing)
    [[nodiscard]] static bool SaveTsfApps(const std::wstring& path, const std::vector<std::wstring>& apps);

    /// Load system config from file
    [[nodiscard]] static std::optional<SystemConfig> LoadSystemConfig(const std::wstring& path);

    /// Save system config to file (merges with existing config)
    [[nodiscard]] static bool SaveSystemConfig(const std::wstring& path, const SystemConfig& config);

    /// Load system config with automatic path resolution
    [[nodiscard]] static SystemConfig LoadSystemConfigOrDefault();

    /// Load convert config from file
    [[nodiscard]] static std::optional<ConvertConfig> LoadConvertConfig(const std::wstring& path);

    /// Save convert config to file (merges with existing config)
    [[nodiscard]] static bool SaveConvertConfig(const std::wstring& path, const ConvertConfig& config);

    /// Load convert config with automatic path resolution
    [[nodiscard]] static ConvertConfig LoadConvertConfigOrDefault();

    /// Load macro table (shorthand → expansion) from config
    [[nodiscard]] static std::unordered_map<std::wstring, std::wstring> LoadMacros(const std::wstring& path);

    /// Save macro table to config (merges with existing)
    [[nodiscard]] static bool SaveMacros(const std::wstring& path,
                                          const std::unordered_map<std::wstring, std::wstring>& macros);

    /// Load per-app override entries (encoding + behavior overrides)
    [[nodiscard]] static std::unordered_map<std::wstring, AppOverrideEntry> LoadAppOverrides(const std::wstring& path);

    /// Save per-app override entries
    static bool SaveAppOverrides(const std::wstring& path,
                                                const std::unordered_map<std::wstring, AppOverrideEntry>& entries);

    /// Import custom keymap from a standalone .keymap (TOML) file
    [[nodiscard]] static bool ImportCustomKeyMap(const std::wstring& path, TypingConfig& config);

    /// Export custom keymap to a standalone .keymap (TOML) file
    [[nodiscard]] static bool ExportCustomKeyMap(const std::wstring& path, const TypingConfig& config);

    /// Format a complete TOML configuration string with updated spellSuggest and spellExclusions.
    [[nodiscard]] static std::string FormatConfigTomlForLexicon(
        const std::wstring& configPath,
        bool spellSuggestEnabled,
        const std::vector<std::wstring>& spellExclusions);

    /// `%APPDATA%\VKey` (creates the directory if missing). Falls back to "."
    /// when SHGetFolderPathW fails. Public because runtime callers (HookEngine
    /// perf-histogram log path, logger fallback, etc.) need the same well-known
    /// per-user data root as the TOML config path.
    static std::wstring GetAppDataDirectory();

private:
    static std::wstring GetExeDirectory(void* moduleHint = nullptr);
    static bool DirectoryWritable(const std::wstring& path);

    // User-defined keymap helpers
    static void LoadCustomKeyMap(const void* table_ptr, TypingConfig& config);
    static void SaveCustomKeyMap(void* table_ptr, const TypingConfig& config);
};

}  // namespace NextKey
