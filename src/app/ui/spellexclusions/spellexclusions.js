// Lexicon & Spell Check Exclusions Dialog JavaScript

var activeTab = 0; // 0 = User Dictionary, 1 = Spell Exclusions
var selectedIndex = -1;
var userDictWords = [];
var spellExclusions = [];
var spellSuggestEnabled = true;

document.ready = function () {
    initSubDialog(".app-list");
    initLexiconDialog();
};

function initLexiconDialog() {
    var btnClose = document.getElementById("btn-close");
    var btnCancel = document.getElementById("btn-cancel");
    var btnAdd = document.getElementById("btn-add");
    var btnEdit = document.getElementById("btn-edit");
    var btnDelete = document.getElementById("btn-delete");
    var btnDeselect = document.getElementById("btn-deselect");
    var btnImport = document.getElementById("btn-import");
    var btnExport = document.getElementById("btn-export");
    var btnReload = document.getElementById("btn-reload");
    var btnSave = document.getElementById("btn-save");
    var chkSpellSuggest = document.getElementById("cfg-spell-suggest");

    var tabUserDict = document.getElementById("tab-user-dict");
    var tabSpellExcl = document.getElementById("tab-spell-excl");
    var entryInput = document.getElementById("entry-name");

    if (btnClose) {
        btnClose.addEventListener("click", function () {
            triggerAction("close");
        });
    }
    if (btnCancel) {
        btnCancel.addEventListener("click", function () {
            triggerAction("close");
        });
    }

    if (tabUserDict) {
        tabUserDict.addEventListener("click", function () {
            switchTab(0);
        });
    }
    if (tabSpellExcl) {
        tabSpellExcl.addEventListener("click", function () {
            switchTab(1);
        });
    }

    if (chkSpellSuggest) {
        chkSpellSuggest.addEventListener("change", function () {
            spellSuggestEnabled = chkSpellSuggest.checked;
            document.getElementById("val-spell-suggest").value = spellSuggestEnabled ? "1" : "0";
            updateDimmedState();
            triggerAction("toggle_suggest");
        });
    }

    if (btnAdd) {
        btnAdd.addEventListener("click", function () {
            onAdd();
        });
    }
    if (btnEdit) {
        btnEdit.addEventListener("click", function () {
            onEdit();
        });
    }
    if (btnDelete) {
        btnDelete.addEventListener("click", function () {
            onDelete();
        });
    }
    if (btnDeselect) {
        btnDeselect.addEventListener("click", function () {
            clearSelection();
        });
    }

    if (btnImport) {
        btnImport.addEventListener("click", function () {
            clearFieldError();
            triggerAction("import");
        });
    }
    if (btnExport) {
        btnExport.addEventListener("click", function () {
            clearFieldError();
            triggerAction("export");
        });
    }
    if (btnReload) {
        btnReload.addEventListener("click", function () {
            clearFieldError();
            triggerAction("reload");
        });
    }
    if (btnSave) {
        btnSave.addEventListener("click", function () {
            clearFieldError();
            triggerAction("save");
        });
    }

    // Input key handling
    if (entryInput) {
        entryInput.addEventListener("keydown", function (evt) {
            if (evt.keyCode === 13) { // Enter
                if (selectedIndex >= 0) {
                    onEdit();
                } else {
                    onAdd();
                }
                evt.preventDefault();
            } else if (evt.keyCode === 27) { // Esc
                triggerAction("close");
                evt.preventDefault();
            } else {
                clearFieldError();
            }
        });
    }

    // Global Esc dismiss immediately without saving
    document.addEventListener("keydown", function (evt) {
        if (evt.keyCode === 27) {
            triggerAction("close");
            evt.preventDefault();
        }
    });

    // Event delegation for clicking list items
    var listEl = document.getElementById("entry-list");
    if (listEl) {
        listEl.addEventListener("click", function (evt) {
            var item = evt.target.closest(".app-item");
            if (item) {
                var idxStr = item.getAttribute("data-index");
                var idx = parseInt(idxStr, 10);
                if (!isNaN(idx)) {
                    selectItem(idx);
                }
            }
        });
    }
}

function switchTab(tabIndex) {
    activeTab = tabIndex;
    clearSelection();
    clearFieldError();

    var tabUser = document.getElementById("tab-user-dict");
    var tabExcl = document.getElementById("tab-spell-excl");
    var infoEl = document.getElementById("tab-info-text");
    var titleEl = document.getElementById("list-header-title");
    var inputEl = document.getElementById("entry-name");

    if (tabIndex === 0) {
        if (tabUser) tabUser.classList.add("active");
        if (tabExcl) tabExcl.classList.remove("active");
        if (infoEl) infoEl.textContent = "Từ hoàn chỉnh (vd: soà, alo, in4, tên riêng). So khớp chính xác khi dứt từ (tối đa 1.024 từ).";
        if (titleEl) titleEl.textContent = "Danh sách từ cá nhân";
        if (inputEl) inputEl.setAttribute("placeholder", "vd: soà");
    } else {
        if (tabUser) tabUser.classList.remove("active");
        if (tabExcl) tabExcl.classList.add("active");
        if (infoEl) infoEl.textContent = "Bỏ qua kiểm tra chính tả khi đang gõ phím (vd: hđ, đcđt). So khớp tiền tố từ đầu (tối đa 8 từ).";
        if (titleEl) titleEl.textContent = "Danh sách ngoại lệ viết tắt";
        if (inputEl) inputEl.setAttribute("placeholder", "vd: hđ");
    }

    document.getElementById("val-target-list").value = String(activeTab);
    updateDimmedState();
    renderCurrentList();
}

function updateDimmedState() {
    var editorCard = document.getElementById("editor-card");
    var listCard = document.getElementById("list-card-container");
    var isDimmed = (activeTab === 0 && !spellSuggestEnabled);

    if (editorCard) {
        if (isDimmed) editorCard.classList.add("dimmed-user-dict");
        else editorCard.classList.remove("dimmed-user-dict");
    }
    if (listCard) {
        if (isDimmed) listCard.classList.add("dimmed-user-dict");
        else listCard.classList.remove("dimmed-user-dict");
    }
}

function selectItem(idx) {
    var items = (activeTab === 0) ? userDictWords : spellExclusions;
    if (idx < 0 || idx >= items.length) return;

    selectedIndex = idx;
    var inputEl = document.getElementById("entry-name");
    if (inputEl) {
        inputEl.value = items[idx];
        inputEl.focus();
    }

    var btnAdd = document.getElementById("btn-add");
    var btnEdit = document.getElementById("btn-edit");
    var btnDelete = document.getElementById("btn-delete");
    var btnDeselect = document.getElementById("btn-deselect");

    if (btnAdd) btnAdd.style.display = "none";
    if (btnEdit) btnEdit.style.display = "inline-block";
    if (btnDelete) btnDelete.style.display = "inline-block";
    if (btnDeselect) btnDeselect.style.display = "inline-block";

    renderCurrentList();
}

function clearSelection() {
    selectedIndex = -1;
    var inputEl = document.getElementById("entry-name");
    if (inputEl) inputEl.value = "";

    var btnAdd = document.getElementById("btn-add");
    var btnEdit = document.getElementById("btn-edit");
    var btnDelete = document.getElementById("btn-delete");
    var btnDeselect = document.getElementById("btn-deselect");

    if (btnAdd) btnAdd.style.display = "inline-block";
    if (btnEdit) btnEdit.style.display = "none";
    if (btnDelete) btnDelete.style.display = "none";
    if (btnDeselect) btnDeselect.style.display = "none";

    renderCurrentList();
}

function onAdd() {
    var inputEl = document.getElementById("entry-name");
    if (!inputEl) return;
    var text = inputEl.value.trim();
    if (!text) return;

    document.getElementById("val-target-list").value = String(activeTab);
    document.getElementById("val-entry-name").value = text;
    triggerAction("add");
}

function onEdit() {
    if (selectedIndex < 0) return;
    var inputEl = document.getElementById("entry-name");
    if (!inputEl) return;
    var text = inputEl.value.trim();
    if (!text) return;

    document.getElementById("val-target-list").value = String(activeTab);
    document.getElementById("val-entry-index").value = String(selectedIndex);
    document.getElementById("val-entry-name").value = text;
    triggerAction("edit");
}

function onDelete() {
    if (selectedIndex < 0) return;
    document.getElementById("val-target-list").value = String(activeTab);
    document.getElementById("val-entry-index").value = String(selectedIndex);
    triggerAction("delete");
}

function renderCurrentList() {
    var list = document.getElementById("entry-list");
    var counter = document.getElementById("list-counter");
    if (!list) return;

    list.innerHTML = "";
    var items = (activeTab === 0) ? userDictWords : spellExclusions;
    var maxCap = (activeTab === 0) ? 1024 : 8;

    if (counter) {
        counter.textContent = items.length + " / " + maxCap + " từ";
    }

    if (items.length === 0) {
        var emptyHint = document.createElement("div");
        emptyHint.className = "empty-list-hint";
        emptyHint.textContent = "(Chưa có từ nào)";
        list.appendChild(emptyHint);
        return;
    }

    for (var i = 0; i < items.length; i++) {
        var itemDiv = document.createElement("div");
        itemDiv.className = "app-item" + (i === selectedIndex ? " selected" : "");
        itemDiv.setAttribute("data-index", String(i));
        itemDiv.setAttribute("data-name", items[i]);

        var nameSpan = document.createElement("span");
        nameSpan.className = "app-item-name";
        nameSpan.textContent = items[i];
        itemDiv.appendChild(nameSpan);

        list.appendChild(itemDiv);
    }
}

function triggerAction(action) {
    var actionInput = document.getElementById("val-action");
    if (actionInput) {
        actionInput.value = action;
        var event = new Event("change", { bubbles: true });
        actionInput.dispatchEvent(event);
    }
}

// ─── Exposed functions called by C++ ────────────────────────────────────────

function setInitialData(spellSuggest, userWordsArr, exclArr) {
    spellSuggestEnabled = Boolean(spellSuggest);
    var chk = document.getElementById("cfg-spell-suggest");
    if (chk) chk.checked = spellSuggestEnabled;

    userDictWords = Array.isArray(userWordsArr) ? userWordsArr : [];
    spellExclusions = Array.isArray(exclArr) ? exclArr : [];

    updateDimmedState();
    renderCurrentList();
}

function renderList(listIndex, itemsArr) {
    if (listIndex === 0) {
        userDictWords = Array.isArray(itemsArr) ? itemsArr : [];
    } else {
        spellExclusions = Array.isArray(itemsArr) ? itemsArr : [];
    }
    clearSelection();
    clearFieldError();
    renderCurrentList();
}

function showFieldError(msg) {
    var errEl = document.getElementById("input-error-msg");
    if (errEl) {
        errEl.textContent = msg;
        errEl.style.display = "block";
    }
}

function clearFieldError() {
    var errEl = document.getElementById("input-error-msg");
    if (errEl) {
        errEl.textContent = "";
        errEl.style.display = "none";
    }
}

function showSuccessToast(msg) {
    if (typeof showToastI18n === "function") {
        showToastI18n(msg, msg);
    }
}
