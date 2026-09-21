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

// Returns true and writes string representation of the current process's user SID (e.g. S-1-5-21-...).
inline bool GetCurrentProcessUserSidString(std::wstring& outSid) noexcept {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    DWORD len = 0;
    (void)GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (len == 0) {
        CloseHandle(hToken);
        return false;
    }
    std::vector<BYTE> buffer(len);
    if (!GetTokenInformation(hToken, TokenUser, buffer.data(), len, &len)) {
        CloseHandle(hToken);
        return false;
    }
    CloseHandle(hToken);
    auto* pTokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR stringSid = nullptr;
    if (!ConvertSidToStringSidW(pTokenUser->User.Sid, &stringSid)) {
        return false;
    }
    outSid = stringSid;
    LocalFree(stringSid);
    return true;
}

// Returns a SECURITY_ATTRIBUTES that grants full access (write) strictly to SYSTEM and Admins.
// Interactive User (IU), current user, and AppContainer sandbox packages (AC, RA) receive ONLY read access (GENERIC_READ).
// This enforces the single-writer invariant and prevents any other process of the same user from opening the mapping for write.
// Call LocalFree(sa.lpSecurityDescriptor) after the handle is created.
//
// SDDL breakdown:
//   D:PAI              — DACL, protected, auto-inherited
//   (A;;GA;;;SY)       — Allow GENERIC_ALL to SYSTEM
//   (A;;GA;;;BA)       — Allow GENERIC_ALL to Built-in Administrators
//   (A;;GR;;;<UserSID>)— Allow GENERIC_READ to Current User SID (read-only)
//   (A;;GR;;;IU)       — Allow GENERIC_READ to Interactively logged-on User (read-only)
//   (A;;GR;;;AC)       — Allow GENERIC_READ to ALL APPLICATION PACKAGES (S-1-15-2-1)
//   (A;;GR;;;RA)       — Allow GENERIC_READ to ALL RESTRICTED APPLICATION PACKAGES (S-1-15-2-2)
inline SECURITY_ATTRIBUTES MakeAppContainerReadableSecurityAttributes() noexcept {
    std::wstring userSid;
    std::wstring sddl;
    if (GetCurrentProcessUserSidString(userSid) && !userSid.empty()) {
        sddl = L"D:PAI(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;" + userSid + L")(A;;GR;;;IU)(A;;GR;;;AC)(A;;GR;;;RA)";
    } else {
        sddl = L"D:PAI(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)(A;;GR;;;AC)(A;;GR;;;RA)";
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(),
        SDDL_REVISION_1,
        &pSD,
        nullptr);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;
    return sa;
}

// Temporary security attributes granting read/write access to SYSTEM, Admins, and
// current User SID, used strictly by the creator process during section creation and re-attachment.
// Note on Windows NT security boundary:
// Named kernel objects in Windows authorize SIDs (user/group credentials), not process IDs (PIDs).
// Any write ACE for User SID technically allows processes in the same user logon session to request
// write access during the brief creation window. To minimize this, LexiconWireManager performs
// an immediate post-mapping DACL lockdown (MakeAppContainerReadableSecurityAttributes) via
// SetKernelObjectSecurity immediately after MapViewOfFile, restricting User SID and IU to GENERIC_READ.
// In addition, LexiconWireFormat enforces seqlock generation parity and CRC32 payload verification
// so readers immediately reject any unauthorized or torn data.
inline SECURITY_ATTRIBUTES MakeCreatorWriteSecurityAttributes() noexcept {
    std::wstring userSid;
    std::wstring sddl;
    if (GetCurrentProcessUserSidString(userSid) && !userSid.empty()) {
        sddl = L"D:PAI(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + userSid + L")(A;;GR;;;IU)(A;;GR;;;AC)(A;;GR;;;RA)";
    } else {
        sddl = L"D:PAI(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;CO)(A;;GR;;;IU)(A;;GR;;;AC)(A;;GR;;;RA)";
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(),
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
