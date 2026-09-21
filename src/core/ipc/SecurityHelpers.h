// VKey - Security Helper Utilities
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>

namespace NextKey {

// Returns a SECURITY_ATTRIBUTES that grants full access only to SYSTEM and the
// creator/owner. Call LocalFree(sa.lpSecurityDescriptor) after the handle is created.
//
// SDDL breakdown:
//   D:PAI          — DACL, protected, auto-inherited
//   (A;;GA;;;SY)   — Allow GENERIC_ALL to SYSTEM
//   (A;;GA;;;CO)   — Allow GENERIC_ALL to Creator/Owner
inline SECURITY_ATTRIBUTES MakeCreatorOnlySecurityAttributes() noexcept {
    PSECURITY_DESCRIPTOR pSD = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:PAI(A;;GA;;;SY)(A;;GA;;;CO)",
        SDDL_REVISION_1,
        &pSD,
        nullptr);
    // If ConvertString fails (shouldn't on any supported Windows version),
    // pSD stays nullptr → CreateFileMapping uses default DACL → safer than crashing.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;
    return sa;
}

// Returns a SECURITY_ATTRIBUTES that grants full access to SYSTEM, Admins, and Interactive User,
// plus read-only access to ALL APPLICATION PACKAGES (AC) and ALL RESTRICTED APPLICATION PACKAGES (RA)
// so AppContainer sandbox processes (Edge, Chromium, UWP) can read the shared memory mapping.
// Call LocalFree(sa.lpSecurityDescriptor) after the handle is created.
//
// SDDL breakdown:
//   D:PAI          — DACL, protected, auto-inherited
//   (A;;GA;;;SY)   — Allow GENERIC_ALL to SYSTEM
//   (A;;GA;;;BA)   — Allow GENERIC_ALL to Built-in Administrators
//   (A;;GA;;;IU)   — Allow GENERIC_ALL to Interactively logged-on User
//   (A;;GR;;;AC)   — Allow GENERIC_READ to ALL APPLICATION PACKAGES (S-1-15-2-1)
//   (A;;GR;;;RA)   — Allow GENERIC_READ to ALL RESTRICTED APPLICATION PACKAGES (S-1-15-2-2)
inline SECURITY_ATTRIBUTES MakeAppContainerReadableSecurityAttributes() noexcept {
    PSECURITY_DESCRIPTOR pSD = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:PAI(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GR;;;AC)(A;;GR;;;RA)",
        SDDL_REVISION_1,
        &pSD,
        nullptr);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;
    return sa;
}

} // namespace NextKey
#endif // _WIN32
