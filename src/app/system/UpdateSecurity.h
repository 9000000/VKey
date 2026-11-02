// VKey - Update Security Helpers
// SPDX-License-Identifier: GPL-3.0-only
//
// Security hardening for the self-update system:
// - SHA-256 hash verification of downloaded files (SEC-001)
// - PowerShell argument escaping (SEC-002)
// - Download URL domain validation (SEC-003)

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <array>
#include <string_view>
#include "core/UpdatePackageSignature.h"

namespace NextKey {

// ── SEC-002: PowerShell single-quote escaping ──────────────────────────────

/// Escape a string for safe use inside PowerShell single-quoted strings.
/// Replaces every `'` with `''` (PowerShell's single-quote escape sequence).
/// Example: `C:\Users\O'Brien\file` -> `C:\Users\O''Brien\file`
[[nodiscard]] std::wstring EscapePowerShellSingleQuote(const std::wstring& input) noexcept;

// ── SEC-003: URL domain validation ─────────────────────────────────────────

/// Validate that a download URL points to an allowed GitHub domain.
/// Allowed prefixes:
///   https://github.com/
///   https://objects.githubusercontent.com/
///   https://codeload.github.com/
/// Returns true if the URL starts with one of the allowed prefixes.
[[nodiscard]] bool IsAllowedDownloadUrl(const std::wstring& url) noexcept;

/// Overload for narrow strings (used by FindAssetUrl which returns std::string).
[[nodiscard]] bool IsAllowedDownloadUrl(const std::string& url) noexcept;

// ── SEC-001: SHA-256 hash verification ─────────────────────────────────────

/// Parse a .sha256 checksum file (format: "<hex>  <filename>" or just "<hex>").
/// Returns the extracted hex hash in lowercase, or empty string if unparseable.
[[nodiscard]] std::string ParseSha256File(const std::string& content) noexcept;

#ifdef _WIN32

/// Verify the package protocol with Windows CNG. Public key input is explicit
/// for protocol tests; application entry points always use the compiled-in key.
[[nodiscard]] bool VerifyUpdateSignature(
    const std::array<std::uint8_t, 32>& digest,
    const UpdatePackageSignature::Parsed& signature, std::string_view publicKeyHex) noexcept;

/// Verify ZIP + adjacent .sig against this application's key, edition and
/// version floor. expectedVersion=0 is used only by the installer recheck.
[[nodiscard]] bool VerifySignedUpdateZip(const std::wstring& zipPath,
                                       std::uint32_t expectedVersion = 0) noexcept;

/// Compute SHA-256 hash of a file using Windows CNG (bcrypt.h).
/// Returns lowercase hex string (64 chars), or empty string on failure.
/// Thread-safe — creates/destroys its own CNG handles.
[[nodiscard]] std::string ComputeFileSha256(const std::wstring& filePath) noexcept;

/// Download the .sha256 sidecar for a ZIP URL and verify the ZIP's integrity.
/// Precondition: zipUrl must have already been validated by IsAllowedDownloadUrl.
/// 1. Appends ".sha256" to zipUrl → downloads checksum file
/// 2. Parses expected hash from checksum content
/// 3. Computes actual SHA-256 of localZipPath
/// 4. Returns true if hashes match
///
/// On any failure (download, parse, hash mismatch) returns false.
///
/// NOTE: SHA-256 here proves the ZIP matches its sidecar, but the sidecar is
/// served from the same origin as the ZIP — an attacker who controls the origin
/// (or a compromised release) controls both halves. It defends against
/// corruption/CDN bit-rot, NOT tampering. Tamper resistance comes from
/// VerifySignedUpdateZip and its compiled-in project public key.
[[nodiscard]] bool VerifyDownloadedZip(
    const std::wstring& zipUrl,
    const std::wstring& localZipPath,
    std::atomic<bool>& cancelFlag) noexcept;

/// Verify a file carries a valid Authenticode signature via WinVerifyTrust: the
/// signature must be intact AND chain to a trusted root CA (revocation checked
/// on the whole chain). If `expectedSubjectSubstring` is non-empty, the signing
/// certificate's subject must ALSO contain it (case-insensitive) — a publisher
/// pin that stops a validly-signed-but-different-publisher binary.
///
/// This is the tamper-resistant check the SHA-256 path can't provide: it proves
/// *who* signed the actual code that will run. Call it on every extracted
/// .exe/.dll before installing/running them.
///
/// Returns true only if all requested checks pass; false on unsigned, untrusted,
/// revoked, publisher-mismatch, or any API failure.
[[nodiscard]] bool VerifyAuthenticodeSignature(
    const std::wstring& filePath,
    const std::wstring& expectedSubjectSubstring = L"") noexcept;

#endif  // _WIN32

}  // namespace NextKey
