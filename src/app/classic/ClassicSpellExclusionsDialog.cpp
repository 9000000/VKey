// VKey Classic — Lexicon & Spell Check Exclusions Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ClassicSpellExclusionsDialog.h"
#include "core/config/ConfigManager.h"
#include "core/config/LexiconTransaction.h"
#include "core/config/LexiconValidation.h"
#include "core/config/SpellExclusionCanonicalizer.h"
#include "core/CrashLog.h"
#include "app/helpers/AppHelpers.h"

#include <windowsx.h>
#include <algorithm>
#include <exception>
#include <fstream>

namespace NextKey::Classic {

// Control IDs
enum {
    IDC_SPELL_CHK_SUGGEST = 3100,
    IDC_SPELL_TAB = 3101,
    IDC_SPELL_LIST = 3102,
    IDC_SPELL_EDIT = 3103,
    IDC_SPELL_BTN_ADD = 3104,
    IDC_SPELL_BTN_EDIT = 3105,
    IDC_SPELL_BTN_DELETE = 3106,
    IDC_SPELL_BTN_IMPORT = 3107,
    IDC_SPELL_BTN_EXPORT = 3108,
    IDC_SPELL_BTN_RELOAD = 3109,
    IDC_SPELL_BTN_SAVE = 3110,
    IDC_SPELL_BTN_CLOSE = 3111,
};

// ════════════════════════════════════════════════════════════
// Public entry point
// ════════════════════════════════════════════════════════════

bool ClassicSpellExclusionsDialog::Show(HINSTANCE hInstance, HWND parent, bool forceLightTheme) {
    ClassicSpellExclusionsDialog dlg;
    if (!dlg.Init(hInstance, parent, forceLightTheme)) return false;

    // Modal message loop
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg.hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return dlg.modified_;
}

// ════════════════════════════════════════════════════════════
// Initialization
// ════════════════════════════════════════════════════════════

bool ClassicSpellExclusionsDialog::Init(HINSTANCE hInstance, HWND parent, bool forceLightTheme) {
    hInstance_ = hInstance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbWndExtra = sizeof(void*);
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, kClassName,
        L"VKey - Từ điển & Loại trừ chính tả",
        style, CW_USEDEFAULT, CW_USEDEFAULT, 350, 300,
        parent, nullptr, hInstance, this);
    if (!hwnd_) return false;

    dpi_ = Classic::GetWindowDpi(hwnd_);

    // Resize + center
    int w = Dpi(kWidth), h = Dpi(kHeight);
    RECT rc = {0, 0, w, h};
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOPMOST);
    int aw = rc.right - rc.left, ah = rc.bottom - rc.top;
    POINT pt = NextKey::GetCenteredPos(hwnd_, aw, ah);
    SetWindowPos(hwnd_, nullptr, pt.x, pt.y, aw, ah, SWP_NOZORDER);

    theme_.Init(hwnd_, forceLightTheme);
    theme_.ApplyWindowAttributes(hwnd_);

    LoadData();
    CreateControls();
    PopulateList();
    UpdateDimmedState();

    // Apply theme + font
    EnumChildWindows(hwnd_, [](HWND h, LPARAM lp) -> BOOL {
        auto* self = reinterpret_cast<ClassicSpellExclusionsDialog*>(lp);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(self->theme_.Fonts().body), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    theme_.ThemeAllChildren(hwnd_);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

// ════════════════════════════════════════════════════════════
// Controls
// ════════════════════════════════════════════════════════════

void ClassicSpellExclusionsDialog::CreateControls() {
    int x = Dpi(kPadding), y = Dpi(kPadding);
    int cw = Dpi(kWidth - kPadding * 2);
    int btnH = Dpi(kBtnHeight);
    int gap = Dpi(kBtnGap);

    // 1. Checkbox: Kiểm tra chính tả nâng cao
    int chkH = Dpi(22);
    chkSuggest_ = CreateWindowExW(0, L"BUTTON",
        L"Kiểm tra chính tả nâng cao (chống sửa sai từ hoàn chỉnh)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, cw, chkH, hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_CHK_SUGGEST), hInstance_, nullptr);
    SendMessageW(chkSuggest_, BM_SETCHECK, spellSuggestEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    y += chkH + gap;

    // 2. Tab Control
    int tabH = Dpi(28);
    tabControl_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        x, y, cw, tabH, hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_TAB), hInstance_, nullptr);

    TCITEMW tie{};
    tie.mask = TCIF_TEXT;
    tie.pszText = const_cast<wchar_t*>(L"Từ điển cá nhân");
    TabCtrl_InsertItem(tabControl_, 0, &tie);
    tie.pszText = const_cast<wchar_t*>(L"Ngoại lệ viết tắt");
    TabCtrl_InsertItem(tabControl_, 1, &tie);
    TabCtrl_SetCurSel(tabControl_, activeTab_);
    y += tabH + gap;

    // 3. ListView
    int listH = Dpi(200);
    listView_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        x, y, cw, listH, hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_LIST), hInstance_, nullptr);
    ListView_SetExtendedListViewStyle(listView_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Từ khóa");
    col.cx = cw - Dpi(24);
    ListView_InsertColumn(listView_, 0, &col);
    y += listH + gap;

    // 4. Row: Edit + Thêm + Sửa
    int editH = theme_.ModernHeight();
    int btnW = Dpi(55);
    int editW = cw - (btnW * 2) - (gap * 2);

    editEntry_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, editW, editH, hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_EDIT), hInstance_, nullptr);
    SendMessageW(editEntry_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Nhập từ..."));
    theme_.ApplyModernEntryStyle(editEntry_);

    btnAdd_ = CreateWindowExW(0, L"BUTTON", L"Thêm",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + editW + gap, y, btnW, editH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_ADD), hInstance_, nullptr);

    btnEdit_ = CreateWindowExW(0, L"BUTTON", L"Sửa",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + editW + gap + btnW + gap, y, btnW, editH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_EDIT), hInstance_, nullptr);
    y += editH + gap;

    // 5. Row: Xóa, Nhập, Xuất
    int subBtnW = Dpi(75);
    btnDelete_ = CreateWindowExW(0, L"BUTTON", L"Xoá",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, subBtnW, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_DELETE), hInstance_, nullptr);

    btnImport_ = CreateWindowExW(0, L"BUTTON", L"Nhập...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + cw - subBtnW * 2 - gap, y, subBtnW, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_IMPORT), hInstance_, nullptr);

    btnExport_ = CreateWindowExW(0, L"BUTTON", L"Xuất...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + cw - subBtnW, y, subBtnW, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_EXPORT), hInstance_, nullptr);
    y += btnH + gap * 2;

    // 6. Footer: Nạp lại (trái), Lưu & Áp dụng, Đóng (phải)
    int reloadW = Dpi(80);
    btnReload_ = CreateWindowExW(0, L"BUTTON", L"Nạp lại",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, reloadW, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_RELOAD), hInstance_, nullptr);

    int saveW = Dpi(115);
    int closeW = Dpi(70);
    btnSave_ = CreateWindowExW(0, L"BUTTON", L"Lưu & Áp dụng",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        x + cw - saveW - closeW - gap, y, saveW, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_SAVE), hInstance_, nullptr);

    btnClose_ = CreateWindowExW(0, L"BUTTON", L"Đóng",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + cw - closeW, y, closeW, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDCANCEL), hInstance_, nullptr);
}

void ClassicSpellExclusionsDialog::SwitchTab(int tabIndex) {
    activeTab_ = tabIndex;
    SetWindowTextW(editEntry_, L"");
    UpdateDimmedState();
    PopulateList();
}

void ClassicSpellExclusionsDialog::UpdateDimmedState() {
    bool enableList = (activeTab_ != 0 || spellSuggestEnabled_);
    EnableWindow(listView_, enableList);
    EnableWindow(editEntry_, enableList);
    EnableWindow(btnAdd_, enableList);
    EnableWindow(btnEdit_, enableList);
    EnableWindow(btnDelete_, enableList);
    EnableWindow(btnImport_, enableList);
    EnableWindow(btnExport_, enableList);
}

void ClassicSpellExclusionsDialog::PopulateList() {
    ListView_DeleteAllItems(listView_);

    int cw = Dpi(kWidth - kPadding * 2);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = cw - Dpi(24);

    if (activeTab_ == 0) {
        std::wstring title = L"Từ cá nhân (" + std::to_wstring(userDictWords_.size()) + L"/1024)";
        col.pszText = const_cast<wchar_t*>(title.c_str());
        ListView_SetColumn(listView_, 0, &col);

        for (size_t i = 0; i < userDictWords_.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(userDictWords_[i].c_str());
            ListView_InsertItem(listView_, &item);
        }
    } else {
        std::wstring title = L"Ngoại lệ viết tắt (" + std::to_wstring(spellExclusions_.size()) + L"/8)";
        col.pszText = const_cast<wchar_t*>(title.c_str());
        ListView_SetColumn(listView_, 0, &col);

        for (size_t i = 0; i < spellExclusions_.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(spellExclusions_[i].c_str());
            ListView_InsertItem(listView_, &item);
        }
    }
}

// ════════════════════════════════════════════════════════════
// Actions
// ════════════════════════════════════════════════════════════

void ClassicSpellExclusionsDialog::AddEntry(const std::wstring& text) {
    if (activeTab_ == 0) {
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateUserDictWord(text, userDictWords_.size() + 1, &normalized);
        if (!valRes.Succeeded()) {
            MessageBoxW(hwnd_, valRes.errorMessage.c_str(), L"Lỗi", MB_OK | MB_ICONWARNING);
            return;
        }
        if (userDictWords_.size() >= kMaxUserDictEntries) {
            MessageBoxW(hwnd_, L"Từ điển cá nhân đã đạt giới hạn 1.024 từ.", L"Giới hạn", MB_OK | MB_ICONWARNING);
            return;
        }
        if (std::find(userDictWords_.begin(), userDictWords_.end(), normalized) != userDictWords_.end()) {
            MessageBoxW(hwnd_, L"Từ này đã có trong từ điển cá nhân.", L"Trùng lặp", MB_OK | MB_ICONINFORMATION);
            return;
        }
        userDictWords_.push_back(normalized);
        std::sort(userDictWords_.begin(), userDictWords_.end());
        modified_ = true;
        PopulateList();
    } else {
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateSpellExclusionWord(text, spellExclusions_.size() + 1, &normalized);
        if (!valRes.Succeeded()) {
            MessageBoxW(hwnd_, valRes.errorMessage.c_str(), L"Lỗi", MB_OK | MB_ICONWARNING);
            return;
        }
        if (spellExclusions_.size() >= 8) {
            MessageBoxW(hwnd_, L"Ngoại lệ đã đạt giới hạn tối đa 8 từ.", L"Giới hạn", MB_OK | MB_ICONWARNING);
            return;
        }
        if (std::find(spellExclusions_.begin(), spellExclusions_.end(), normalized) != spellExclusions_.end()) {
            MessageBoxW(hwnd_, L"Từ viết tắt này đã có trong danh sách ngoại lệ.", L"Trùng lặp", MB_OK | MB_ICONINFORMATION);
            return;
        }
        spellExclusions_.push_back(normalized);
        auto canon = SpellExclusionCanonicalizer::Canonicalize(spellExclusions_);
        if (canon.Succeeded()) {
            spellExclusions_ = std::move(canon.entries);
        }
        modified_ = true;
        PopulateList();
    }
}

void ClassicSpellExclusionsDialog::EditSelected(const std::wstring& text) {
    int sel = ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
    if (sel < 0) return;

    if (activeTab_ == 0) {
        if (sel >= static_cast<int>(userDictWords_.size())) return;
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateUserDictWord(text, sel + 1, &normalized);
        if (!valRes.Succeeded()) {
            MessageBoxW(hwnd_, valRes.errorMessage.c_str(), L"Lỗi", MB_OK | MB_ICONWARNING);
            return;
        }
        userDictWords_[sel] = normalized;
        std::sort(userDictWords_.begin(), userDictWords_.end());
        userDictWords_.erase(std::unique(userDictWords_.begin(), userDictWords_.end()), userDictWords_.end());
        modified_ = true;
        PopulateList();
    } else {
        if (sel >= static_cast<int>(spellExclusions_.size())) return;
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateSpellExclusionWord(text, sel + 1, &normalized);
        if (!valRes.Succeeded()) {
            MessageBoxW(hwnd_, valRes.errorMessage.c_str(), L"Lỗi", MB_OK | MB_ICONWARNING);
            return;
        }
        spellExclusions_[sel] = normalized;
        auto canon = SpellExclusionCanonicalizer::Canonicalize(spellExclusions_);
        if (canon.Succeeded()) {
            spellExclusions_ = std::move(canon.entries);
        }
        modified_ = true;
        PopulateList();
    }
}


void ClassicSpellExclusionsDialog::DeleteSelected() {
    int sel = ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
    if (sel < 0) return;

    if (activeTab_ == 0) {
        if (sel < static_cast<int>(userDictWords_.size())) {
            userDictWords_.erase(userDictWords_.begin() + sel);
            modified_ = true;
            PopulateList();
            SetWindowTextW(editEntry_, L"");
        }
    } else {
        if (sel < static_cast<int>(spellExclusions_.size())) {
            spellExclusions_.erase(spellExclusions_.begin() + sel);
            modified_ = true;
            PopulateList();
            SetWindowTextW(editEntry_, L"");
        }
    }
}

void ClassicSpellExclusionsDialog::ImportFromFile() {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        MessageBoxW(hwnd_, L"Không thể mở file được chọn.", L"Lỗi", MB_OK | MB_ICONERROR);
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    int choice = MessageBoxW(
        hwnd_,
        L"Bạn có muốn thêm tiếp vào danh sách hiện tại (Chọn YES) hay thay thế toàn bộ (Chọn NO)?",
        L"Chế độ nhập dữ liệu",
        MB_YESNOCANCEL | MB_ICONQUESTION
    );
    if (choice == IDCANCEL) return;
    bool append = (choice == IDYES);

    if (activeTab_ == 0) {
        auto res = LexiconValidator::ParseAndValidateUserDictText(content, append ? &userDictWords_ : nullptr, append);
        if (!res.validation.Succeeded()) {
            MessageBoxW(hwnd_, res.validation.errorMessage.c_str(), L"Lỗi dữ liệu", MB_OK | MB_ICONERROR);
            return;
        }
        userDictWords_ = std::move(res.entries);
        modified_ = true;
        PopulateList();
        MessageBoxW(hwnd_, L"Nhập từ điển thành công!", L"Thành công", MB_OK | MB_ICONINFORMATION);
    } else {
        auto res = LexiconValidator::ParseAndValidateSpellExclusionsText(content, append ? &spellExclusions_ : nullptr, append);
        if (!res.validation.Succeeded()) {
            MessageBoxW(hwnd_, res.validation.errorMessage.c_str(), L"Lỗi dữ liệu", MB_OK | MB_ICONERROR);
            return;
        }
        spellExclusions_ = std::move(res.entries);
        modified_ = true;
        PopulateList();
        MessageBoxW(hwnd_, L"Nhập ngoại lệ thành công!", L"Thành công", MB_OK | MB_ICONINFORMATION);
    }
}

void ClassicSpellExclusionsDialog::ExportToFile() {
    std::wstring defaultName = (activeTab_ == 0) ? L"user_dictionary.txt" : L"spell_exclusions.txt";
    wchar_t filename[MAX_PATH] = {};
    wcsncpy_s(filename, defaultName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    std::string content;
    if (activeTab_ == 0) {
        content = LexiconValidator::FormatUserDictText(userDictWords_);
    } else {
        content = LexiconValidator::FormatSpellExclusionsText(spellExclusions_);
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        MessageBoxW(hwnd_, L"Không thể tạo file xuất dữ liệu.", L"Lỗi", MB_OK | MB_ICONERROR);
        return;
    }
    file.write(content.data(), content.size());
    file.close();

    MessageBoxW(hwnd_, L"Đã xuất danh sách thành công.", L"Thành công", MB_OK | MB_ICONINFORMATION);
}

void ClassicSpellExclusionsDialog::ReloadData() {
    if (modified_) {
        int choice = MessageBoxW(
            hwnd_,
            L"Các thay đổi chưa lưu sẽ bị hủy bỏ. Bạn có chắc chắn muốn nạp lại từ đĩa?",
            L"Nạp lại",
            MB_YESNO | MB_ICONQUESTION
        );
        if (choice != IDYES) return;
    }
    LoadData();
    PopulateList();
    UpdateDimmedState();
    SendMessageW(chkSuggest_, BM_SETCHECK, spellSuggestEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(editEntry_, L"");
}

bool ClassicSpellExclusionsDialog::SaveAndApply() {
    auto configPath = ConfigManager::GetConfigPath();

    auto canon = SpellExclusionCanonicalizer::Canonicalize(spellExclusions_);
    if (!canon.Succeeded()) {
        std::wstring err = L"Ngoại lệ viết tắt không hợp lệ: " + canon.error.message;
        MessageBoxW(hwnd_, err.c_str(), L"Lỗi", MB_OK | MB_ICONERROR);
        return false;
    }
    spellExclusions_ = std::move(canon.entries);

    std::string newConfigToml = ConfigManager::FormatConfigTomlForLexicon(
        configPath, spellSuggestEnabled_, spellExclusions_);
    std::string newUserDictText = LexiconValidator::FormatUserDictText(userDictWords_);

    bool ok = LexiconWriter::CommitTransaction(configPath, newConfigToml, newUserDictText);
    if (!ok) {
        MessageBoxW(hwnd_, L"Lỗi khi thực hiện giao dịch lưu từ điển và cấu hình.", L"Lỗi giao dịch", MB_OK | MB_ICONERROR);
        return false;
    }

    modified_ = false;
    SignalConfigChange();

    PopulateList();
    MessageBoxW(hwnd_, L"Đã lưu & áp dụng thành công!", L"Thành công", MB_OK | MB_ICONINFORMATION);
    return true;
}

// ════════════════════════════════════════════════════════════
// Data I/O
// ════════════════════════════════════════════════════════════

void ClassicSpellExclusionsDialog::LoadData() {
    auto configPath = ConfigManager::GetConfigPath();
    auto config = ConfigManager::LoadFromFile(configPath).value_or(TypingConfig{});
    spellSuggestEnabled_ = config.spellSuggestEnabled;
    spellExclusions_ = std::move(config.spellExclusions);

    userDictWords_.clear();
    (void)LexiconReader::LoadUserDictionaryWordsLocked(configPath, userDictWords_);
    modified_ = false;
}

// ════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════

int ClassicSpellExclusionsDialog::Dpi(int value) const noexcept {
    return Classic::DpiScale(value, dpi_);
}

// ════════════════════════════════════════════════════════════
// Window procedure
// ════════════════════════════════════════════════════════════

LRESULT CALLBACK ClassicSpellExclusionsDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) try {
    ClassicSpellExclusionsDialog* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<ClassicSpellExclusionsDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<ClassicSpellExclusionsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_COMMAND: {
            UINT id = LOWORD(wParam);

            switch (id) {
                case IDC_SPELL_CHK_SUGGEST: {
                    self->spellSuggestEnabled_ = (SendMessageW(self->chkSuggest_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    self->modified_ = true;
                    self->UpdateDimmedState();
                    return 0;
                }
                case IDC_SPELL_BTN_ADD: {
                    wchar_t buf[256] = {};
                    GetWindowTextW(self->editEntry_, buf, 256);
                    self->AddEntry(buf);
                    SetWindowTextW(self->editEntry_, L"");
                    SetFocus(self->editEntry_);
                    return 0;
                }
                case IDC_SPELL_BTN_EDIT: {
                    wchar_t buf[256] = {};
                    GetWindowTextW(self->editEntry_, buf, 256);
                    self->EditSelected(buf);
                    SetFocus(self->editEntry_);
                    return 0;
                }
                case IDC_SPELL_BTN_DELETE:
                    self->DeleteSelected();
                    return 0;
                case IDC_SPELL_BTN_IMPORT:
                    self->ImportFromFile();
                    return 0;
                case IDC_SPELL_BTN_EXPORT:
                    self->ExportToFile();
                    return 0;
                case IDC_SPELL_BTN_RELOAD:
                    self->ReloadData();
                    return 0;
                case IDC_SPELL_BTN_SAVE:
                    (void)self->SaveAndApply();
                    return 0;
                case IDCANCEL:
                    // ESC key or Cancel button - immediate dismiss without saving
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        }

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->idFrom == IDC_SPELL_TAB && hdr->code == TCN_SELCHANGE) {
                self->SwitchTab(TabCtrl_GetCurSel(self->tabControl_));
                return 0;
            }
            if (hdr->idFrom == IDC_SPELL_LIST && (hdr->code == NM_CLICK || hdr->code == LVN_ITEMCHANGED)) {
                int sel = ListView_GetNextItem(self->listView_, -1, LVNI_SELECTED);
                if (sel >= 0) {
                    wchar_t buf[256] = {};
                    ListView_GetItemText(self->listView_, sel, 0, buf, 256);
                    SetWindowTextW(self->editEntry_, buf);
                }
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, self->theme_.BrushBackground());
            return 1;
        }

        case WM_CTLCOLORSTATIC:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorStatic(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

        case WM_CTLCOLOREDIT:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorEdit(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

        case WM_CTLCOLORLISTBOX:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorListBox(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorBtn(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            self->theme_.Destroy();
            self->hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
} catch (const std::exception& e) {
    NextKey::CrashLog(L"ClassicSpellExclusionsDialog::WndProc", e.what());
    return DefWindowProcW(hwnd, msg, wParam, lParam);
} catch (...) {
    NextKey::CrashLog(L"ClassicSpellExclusionsDialog::WndProc", "(non-std exception)");
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace NextKey::Classic
