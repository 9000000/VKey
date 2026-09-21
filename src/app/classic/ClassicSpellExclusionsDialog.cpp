// VKey Classic — Spell Check Exclusions Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ClassicSpellExclusionsDialog.h"
#include "core/config/ConfigManager.h"
#include "core/CrashLog.h"
#include "app/helpers/AppHelpers.h"

#include <windowsx.h>
#include <algorithm>
#include <exception>

namespace NextKey::Classic {

// Control IDs
enum {
    IDC_SPELL_LIST = 3101,
    IDC_SPELL_EDIT,
    IDC_SPELL_BTN_ADD,
    IDC_SPELL_BTN_DELETE,
    IDC_SPELL_BTN_IMPORT,
    IDC_SPELL_BTN_EXPORT,
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
        L"Loại trừ kiểm tra chính tả",
        style, CW_USEDEFAULT, CW_USEDEFAULT, 300, 250,
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

    // ListView
    int listH = Dpi(180);
    listView_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        x, y, cw, listH, hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_LIST), hInstance_, nullptr);
    ListView_SetExtendedListViewStyle(listView_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Viết tắt (tối thiểu 2 ký tự)");
    col.cx = cw - Dpi(24);
    ListView_InsertColumn(listView_, 0, &col);
    y += listH + gap;

    // Row: edit + add button
    int editH = theme_.ModernHeight();
    int editW = cw - Dpi(60) - gap;
    editEntry_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, editW, editH, hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_EDIT), hInstance_, nullptr);
    SendMessageW(editEntry_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"hđ"));
    theme_.ApplyModernEntryStyle(editEntry_);

    btnAdd_ = CreateWindowExW(0, L"BUTTON", L"Thêm",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + editW + gap, y, Dpi(60), editH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_ADD), hInstance_, nullptr);
    y += editH + gap * 2;

    // Delete button
    btnDelete_ = CreateWindowExW(0, L"BUTTON", L"Xoá",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, Dpi(80), btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_DELETE), hInstance_, nullptr);

    int btnW2 = Dpi(75);
    btnImport_ = CreateWindowExW(0, L"BUTTON", L"Nhập",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + cw - btnW2 * 2 - gap, y, btnW2, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_IMPORT), hInstance_, nullptr);

    btnExport_ = CreateWindowExW(0, L"BUTTON", L"Xuất",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + cw - btnW2, y, btnW2, btnH,
        hwnd_, reinterpret_cast<HMENU>(IDC_SPELL_BTN_EXPORT), hInstance_, nullptr);
}

void ClassicSpellExclusionsDialog::PopulateList() {
    ListView_DeleteAllItems(listView_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(entries_[i].c_str());
        ListView_InsertItem(listView_, &item);
    }
}

// ════════════════════════════════════════════════════════════
// Actions
// ════════════════════════════════════════════════════════════

void ClassicSpellExclusionsDialog::AddEntry(const std::wstring& text) {
    // Trim whitespace
    size_t s = 0, e = text.size();
    while (s < e && text[s] == L' ') ++s;
    while (e > s && text[e - 1] == L' ') --e;
    if (e - s < 2) {
        MessageBoxW(hwnd_, L"Viết tắt phải có ít nhất 2 ký tự.",
            L"Lỗi", MB_ICONWARNING);
        return;
    }

    std::wstring entry = text.substr(s, e - s);
    for (auto& ch : entry) ch = towlower(ch);  // Store pre-lowercased

    // Dedup
    for (auto& existing : entries_) {
        if (existing == entry) return;
    }

    entries_.push_back(entry);
    std::sort(entries_.begin(), entries_.end());
    PopulateList();
    SaveData();
}

void ClassicSpellExclusionsDialog::DeleteSelected() {
    int sel = ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(entries_.size())) return;

    entries_.erase(entries_.begin() + sel);
    PopulateList();
    SaveData();
}

void ClassicSpellExclusionsDialog::ImportFromFile() {
    std::wstring path = OpenFileDialog(hwnd_, L"Text Files (*.txt)\0*.txt\0All Files\0*.*\0", L"Nhập danh sách loại trừ");
    if (path.empty()) return;

    // Ask replace or append
    int choice = MessageBoxW(hwnd_,
        L"Thay thế danh sách hiện tại hay thêm vào?",
        L"Nhập file",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDCANCEL) return;

    std::ifstream file(path);
    if (!file.is_open()) {
        MessageBoxW(hwnd_, L"Không thể mở file.", L"Lỗi", MB_ICONERROR);
        return;
    }

    if (choice == IDYES) entries_.clear();  // Replace

    int added = 0;
    ParseConfigLines(file, [&](const std::string& line) {
        std::wstring wline = Utf8ToWide(line);

        // Trim + min-length validate (entries < 2 chars are noise).
        size_t s = 0, e = wline.size();
        while (s < e && wline[s] == L' ') ++s;
        while (e > s && wline[e - 1] == L' ') --e;
        if (e - s < 2) return;

        std::wstring entry = wline.substr(s, e - s);
        for (auto& ch : entry) ch = towlower(ch);

        bool exists = false;
        for (const auto& existing : entries_) {
            if (existing == entry) { exists = true; break; }
        }
        if (!exists) {
            entries_.push_back(entry);
            added++;
        }
    });

    std::sort(entries_.begin(), entries_.end());
    PopulateList();
    if (added > 0 || choice == IDYES) SaveData();

    MessageBoxW(hwnd_,
        (L"Đã nhập " + std::to_wstring(added) + L" từ mới.").c_str(),
        L"Kết quả", MB_OK);
}

void ClassicSpellExclusionsDialog::ExportToFile() {
    if (entries_.empty()) {
        MessageBoxW(hwnd_, L"Danh sách trống.", L"Thông báo", MB_ICONINFORMATION);
        return;
    }

    std::wstring path = SaveFileDialog(hwnd_, L"Text Files (*.txt)\0*.txt\0", L"Xuất danh sách loại trừ", L"txt");
    if (path.empty()) return;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        MessageBoxW(hwnd_, L"Không thể tạo file.", L"Lỗi", MB_ICONERROR);
        return;
    }

    // Write UTF-8 BOM
    file.write("\xEF\xBB\xBF", 3);

    for (const auto& entry : entries_) {
        std::string utf8 = WideToUtf8(entry);
        file.write(utf8.c_str(), utf8.size());
        file.write("\r\n", 2);
    }

    MessageBoxW(hwnd_, L"Đã xuất danh sách thành công.", L"Thành công", MB_OK);
}

// ════════════════════════════════════════════════════════════
// Data I/O
// ════════════════════════════════════════════════════════════

void ClassicSpellExclusionsDialog::LoadData() {
    auto config = ConfigManager::LoadOrDefault();
    entries_ = std::move(config.spellExclusions);
}

void ClassicSpellExclusionsDialog::SaveData() {
    modified_ = true;

    // Load full config, update just spellExclusions, save back
    auto path = ConfigManager::GetConfigPath();
    auto config = ConfigManager::LoadFromFile(path).value_or(TypingConfig{});
    config.spellExclusions = entries_;
    (void)ConfigManager::SaveToFile(path, config);

    SignalConfigChange();
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
                case IDC_SPELL_BTN_ADD: {
                    wchar_t buf[256] = {};
                    GetWindowTextW(self->editEntry_, buf, 256);
                    self->AddEntry(buf);
                    SetWindowTextW(self->editEntry_, L"");
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
