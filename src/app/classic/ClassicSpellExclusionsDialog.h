// VKey Classic — Spell Check Exclusions Dialog
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifdef _WIN32
#include "ClassicTheme.h"
#include "ClassicDialogUtils.h"
#include <Windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

namespace NextKey::Classic {

/// Win32 native dialog for managing spell check exclusion prefixes.
/// Modal dialog with ListView + add/delete buttons.
class ClassicSpellExclusionsDialog {
public:
    /// Show modal dialog. Returns true if list was modified.
    static bool Show(HINSTANCE hInstance, HWND parent, bool forceLightTheme = false);

private:
    ClassicSpellExclusionsDialog() = default;

    bool Init(HINSTANCE hInstance, HWND parent, bool forceLightTheme);
    void CreateControls();
    void PopulateList();
    void AddEntry(const std::wstring& text);
    void DeleteSelected();
    void ImportFromFile();
    void ExportToFile();

    void LoadData();
    void SaveData();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    int Dpi(int value) const noexcept;

    // Layout
    static constexpr int kWidth = 340;
    static constexpr int kHeight = 278;
    static constexpr int kPadding = 12;
    static constexpr int kBtnHeight = 28;
    static constexpr int kBtnGap = 6;

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ClassicTheme theme_;
    UINT dpi_ = 96;
    bool modified_ = false;

    // Controls
    HWND listView_ = nullptr;
    HWND editEntry_ = nullptr;
    HWND btnAdd_ = nullptr;
    HWND btnDelete_ = nullptr;
    HWND btnImport_ = nullptr;
    HWND btnExport_ = nullptr;

    // State
    std::vector<std::wstring> entries_;

    static constexpr const wchar_t* kClassName = L"VKeySpellExclusions";
};

}  // namespace NextKey::Classic

#endif  // _WIN32
