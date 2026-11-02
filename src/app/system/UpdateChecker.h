// VKey - Update Checker
// SPDX-License-Identifier: GPL-3.0-only
//
// Checks GitHub Releases API for new versions, downloads updates,
// and shows TaskDialog UI for update notifications.
// All network operations are synchronous — caller must use a background thread.

#pragma once

#include <Windows.h>
#include <string>
#include <cstdint>
#include <atomic>

namespace NextKey {

/// Information about an available update
struct UpdateInfo {
    bool available = false;       // True if a newer version exists
    bool checkSucceeded = false;  // True if the API call succeeded (even if no update)
    std::wstring version;         // e.g. "1.2.0"
    std::wstring downloadUrl;     // Direct URL to ZIP asset
    std::wstring changelogUrl;    // Release page URL
    uint32_t packedVersion = 0;
};

/// Checks for updates from GitHub Releases and provides download/UI helpers.
class UpdateChecker {
public:
    static constexpr const wchar_t* API_URL =
        L"https://api.github.com/repos/phatMT97/VKey/releases/latest";

    /// Check GitHub Releases for a newer version (synchronous, use from background thread)
    [[nodiscard]] static UpdateInfo CheckForUpdate() noexcept;

    /// Download a file from URL to local path (synchronous)
    [[nodiscard]] static bool DownloadFile(const std::wstring& url, const std::wstring& localPath, std::atomic<bool>& cancelFlag) noexcept;

    /// Download ZIP, verify hash, launch installer (synchronous — call from background thread).
    /// Returns true if installer was launched. On failure, cleans up the downloaded file.
    [[nodiscard]] static bool DownloadAndLaunchInstaller(const std::wstring& downloadUrl, std::atomic<bool>& cancelFlag, std::uint32_t expectedVersion) noexcept;

    /// Parse version string (e.g. "v1.2.3-beta") into packed format (major<<16 | minor<<8 | patch)
    [[nodiscard]] static uint32_t ParseVersion(const std::wstring& versionStr) noexcept;

    /// Show update confirmation and release notes.
    [[nodiscard]] static bool ShowUpdateDialog(HWND parent, const UpdateInfo& info);

    /// Show "You're on the latest version" message
    static void ShowUpToDateMessage(HWND parent);

    /// Show "Unable to check for updates" message
    static void ShowCheckFailedMessage(HWND parent);

    /// Show a modal progress dialog with marquee progress bar.
    /// Blocks until doneFlag becomes true (auto-closes) or user clicks Cancel.
    /// Returns true if the operation completed, false if user cancelled.
    [[nodiscard]] static bool ShowProgressDialog(HWND parent, const wchar_t* message,
                                                std::atomic<bool>& doneFlag);

    /// Download update with progress dialog, launch installer.
    /// Shows marquee progress, handles errors. Returns true if installer launched (caller should exit).
    [[nodiscard]] static bool DownloadWithProgress(HWND parent, const std::wstring& downloadUrl, std::uint32_t expectedVersion);

private:

    /// Download URL content to a string (via temp file)
    [[nodiscard]] static std::string DownloadToString(const std::wstring& url) noexcept;

    /// Simple JSON string value extraction (no dependency on JSON library)
    [[nodiscard]] static std::string ExtractJsonString(const std::string& json, const std::string& key);

    /// Find the download URL for the architecture-specific ZIP asset
    [[nodiscard]] static std::string FindAssetUrl(const std::string& json, const std::string& assetName);
};

}  // namespace NextKey
