// VKey - Lexicon & Spell Check Exclusions Dialog Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "SciterSubDialog.h"
#include <string>
#include <vector>

namespace NextKey {

/// Dialog for managing User Dictionary and Spell Check Exclusions (Sciter subdialog).
/// Implements N7 Lexicon contract with 8 functional groups.
class SpellExclusionsDialog : public SciterSubDialog {
public:
    explicit SpellExclusionsDialog(HWND parent);

    bool handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) override;

protected:
    LRESULT onCustomMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

private:
    void loadData();
    void populateUI();
    void populateList(int listIndex);

    void addEntry(int targetList, const std::wstring& text);
    void editEntry(int targetList, int index, const std::wstring& text);
    void deleteEntry(int targetList, int index);
    void importEntries(int targetList);
    void exportEntries(int targetList);
    void reloadData();
    bool saveAndApply();

    bool spellSuggestEnabled_ = true;
    std::vector<std::wstring> userDictWords_;
    std::vector<std::wstring> spellExclusions_;
    bool modified_ = false;
};

}  // namespace NextKey

