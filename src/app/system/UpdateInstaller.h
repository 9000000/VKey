// VKey - Self-Update Installer
// SPDX-License-Identifier: GPL-3.0-only
//
// Handles the --install-update CLI mode: waits for processes to exit,
// replaces files from ZIP, and relaunches.
// No Sciter dependency — runs before any Sciter initialization.

#pragma once

#include <Windows.h>
#include <string>
#include "core/UpdateOutcome.h"

namespace NextKey {

// Single source of truth for the TSF DLL filename. The update flow treats this
// file specially (see HandleTsfDllReplace) because it is routinely mapped into
// foreign host processes (Chrome, Word, Outlook, …) via Windows TSF.
inline constexpr const wchar_t* TSF_DLL_FILENAME = L"VKeyTSF.dll";

// New DLL stashed with this suffix when the live copy can't be displaced.
inline constexpr const wchar_t* TSF_DLL_PENDING_SUFFIX = L".pending";

// Marker file written next to the pending DLL so EXE startup can verify
// authenticity (not user-dropped debris).
inline constexpr const wchar_t* TSF_DLL_PENDING_MARKER = L"_pending_dll_update";

// Directory where displaced DLLs are parked until reboot releases the section.
inline constexpr const wchar_t* OLD_VERSION_DIRNAME = L"_old_version";

/// Build a timestamp suffix of the form "_YYYYMMDD_HHMMSS" (plus optional extra
/// suffix). Used for parked-DLL filenames so repeated updates don't collide.
std::wstring MakeParkedDllTimestamp(const wchar_t* extraSuffix = L"") noexcept;

/// Run the self-update installer mode.
/// Waits for other VKey processes to exit, extracts ZIP, replaces files, relaunches.
/// Called from --install-update CLI route. Never returns.
[[noreturn]] void RunUpdateInstaller(const std::wstring& zipPath);

/// Consume an explicit installer receipt for the running EXE, before cleanup.
[[nodiscard]] UpdateOutcome ConsumeInstalledUpdateOutcome() noexcept;

/// Clean up leftover files from a previous update (*_old.*, _update_temp/).
/// Returns true if any files were actually cleaned up.
bool CleanupOldUpdateFiles() noexcept;

}  // namespace NextKey
