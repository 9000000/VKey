// VKey - ESC Restore Last Commit Edit Session
// SPDX-License-Identifier: GPL-3.0-only
//
// Edit session that restores raw keys for the most recent commit when user
// presses ESC after backspacing the commit trigger (design 2026-05-17).
//
// Steps inside DoEditSession:
//   1. Read current selection (caret position).
//   2. ShiftStart back by `textToReplace.size()` chars → range covers the
//      committed body still on screen after the user's BS deleted the trigger.
//   3. SetText(range, rawInput) → replaces composed Vietnamese with raw keys.
//   4. Collapse range to end and update selection so caret follows the
//      inserted raw text.
#pragma once

#include "EditSession.h"
#include "Define.h"

#include <string>
#include <msctf.h>

namespace NextKey {
namespace TSF {

class EscRestoreLastCommitSession : public EditSession {
public:
    EscRestoreLastCommitSession(ITfContext* pContext,
                                std::wstring textToReplace,
                                std::wstring rawInput) noexcept
        : EditSession(pContext),
          textToReplace_(std::move(textToReplace)),
          rawInput_(std::move(rawInput)) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pContext_ == nullptr || textToReplace_.empty()) return S_OK;

        TF_SELECTION sel{};
        ULONG fetched = 0;
        HRESULT hr = pContext_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (FAILED(hr) || fetched == 0 || sel.range == nullptr) {
            TSF_LOG(L"EscRestoreLastCommitSession: GetSelection failed hr=0x%08X", hr);
            if (sel.range) sel.range->Release();
            return FAILED(hr) ? hr : E_FAIL;
        }

        // Shift start back by text length so the range covers the committed body.
        const LONG shiftRequest = -static_cast<LONG>(textToReplace_.size());
        LONG shifted = 0;
        hr = sel.range->ShiftStart(ec, shiftRequest, &shifted, nullptr);
        if (FAILED(hr) || shifted != shiftRequest) {
            TSF_LOG(L"EscRestoreLastCommitSession: ShiftStart partial hr=0x%08X req=%ld shifted=%ld",
                    hr, shiftRequest, shifted);
            sel.range->Release();
            return FAILED(hr) ? hr : E_FAIL;
        }

        // Only overwrite the committed body if it is still what precedes the
        // caret. Hosts rewrite committed text (Teams ":smile:" → one emoji,
        // Word "(c)" → "©"), so a blind N-char replace would eat unrelated
        // text the user never asked to restore.
        std::wstring onScreen(textToReplace_.size(), L'\0');
        ULONG got = 0;
        hr = sel.range->GetText(ec, 0, onScreen.data(),
                                static_cast<ULONG>(onScreen.size()), &got);
        if (FAILED(hr) || got != onScreen.size() || onScreen != textToReplace_) {
            TSF_LOG(L"EscRestoreLastCommitSession: text before caret changed, skip (hr=0x%08X got=%lu)",
                    hr, got);
            sel.range->Release();
            return FAILED(hr) ? hr : E_FAIL;
        }

        // Replace with raw keys.
        hr = sel.range->SetText(ec, 0, rawInput_.data(), static_cast<LONG>(rawInput_.size()));
        if (FAILED(hr)) {
            TSF_LOG(L"EscRestoreLastCommitSession: SetText failed hr=0x%08X", hr);
            sel.range->Release();
            return hr;
        }

        // Collapse range to end and update selection so caret follows inserted text.
        hr = sel.range->Collapse(ec, TF_ANCHOR_END);
        if (SUCCEEDED(hr)) {
            sel.style.ase = TF_AE_END;
            sel.style.fInterimChar = FALSE;
            (void)pContext_->SetSelection(ec, 1, &sel);
        }

        sel.range->Release();
        replaced_ = true;
        TSF_LOG(L"EscRestoreLastCommitSession: replaced '%ls' with raw '%ls'",
                textToReplace_.c_str(), rawInput_.c_str());
        return S_OK;
    }

    /// True once the committed body was actually replaced with raw keys.
    [[nodiscard]] bool Replaced() const noexcept { return replaced_; }

private:
    std::wstring textToReplace_;
    std::wstring rawInput_;
    bool replaced_ = false;
};

}  // namespace TSF
}  // namespace NextKey
