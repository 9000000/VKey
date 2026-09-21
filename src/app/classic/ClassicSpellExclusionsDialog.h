// VKey Classic — Lexicon & Spell Check Exclusions Dialog
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

/// Win32 native dialog for managing User Dictionary and Spell Check Exclusions.
/// Implements N7 Lexicon contract with all 8 groups.
class ClassicSpellExclusionsDialog {
public:
    /// Show modal dialog. Returns true if list was modified and saved.
    static bool Show(HINSTANCE hInstance, HWND parent, bool forceLightTheme = false);

private:
    ClassicSpellExclusionsDialog() = default;

    bool Init(HINSTANCE hInstance, HWND parent, bool forceLightTheme);
    void CreateControls();
    void PopulateList();
    void SwitchTab(int tabIndex);
    void UpdateDimmedState();

    void AddEntry(const std::wstring& text);
    void EditSelected(const std::wstring& text);
    void DeleteSelected();
    void ImportFromFile();
    void ExportToFile();
    void ReloadData();
    bool SaveAndApply();

    void LoadData();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    int Dpi(int value) const noexcept;

    // Layout
    static constexpr int kWidth = 420;
    static constexpr int kHeight = 480;
    static constexpr int kPadding = 12;
    static constexpr int kBtnHeight = 28;
    static constexpr int kBtnGap = 6;

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ClassicTheme theme_;
    UINT dpi_ = 96;
    bool modified_ = false;

    // Controls
    HWND chkSuggest_ = nullptr;
    HWND tabControl_ = nullptr;
    HWND listView_ = nullptr;
    HWND editEntry_ = nullptr;
    HWND btnAdd_ = nullptr;
    HWND btnEdit_ = nullptr;
    HWND btnDelete_ = nullptr;
    HWND btnImport_ = nullptr;
    HWND btnExport_ = nullptr;
    HWND btnReload_ = nullptr;
    HWND btnSave_ = nullptr;
    HWND btnClose_ = nullptr;

    // State
    int activeTab_ = 0; // 0 = User Dictionary, 1 = Spell Exclusions
    bool spellSuggestEnabled_ = true;
    std::vector<std::wstring> userDictWords_;
    std::vector<std::wstring> spellExclusions_;

    static constexpr const wchar_t* kClassName = L"VKeySpellExclusions";
};

}  // namespace NextKey::Classic

#endif  // _WIN32

