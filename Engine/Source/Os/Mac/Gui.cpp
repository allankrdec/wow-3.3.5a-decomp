// macOS counterpart of Engine/Source/Os/Win64/Gui.cpp. The Cocoa parts (title, active window, modifier keys, beep,
// the NSAlert of OsGuiMessageBox) live in MacWindow.mm.
// The client creates no native dialogs on macOS (the Win32 dialog templates and common controls have no Mac
// counterpart and the game never opens one), so the COsDialog code that asks the native controls answers "none"
// and OsGuiProcessMessage handles nothing; the logic over the control lists is the Win64 one.

#include <cstdio>
#include <cstdint>
#include <malloc.h>
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Os/Mac/MacWindow.h"
#include "Engine/Source/Storm/H/storm.h"

static void* s_GxDevWindow;

int32_t COsDialog::OnEvent(int32_t itemId, int32_t notifyCode, int32_t code) {
    if (this->m_callback) {
        OsGuiCallbackParams params;
        params.type = itemId;
        params.subType = notifyCode;
        params.code = code;
        params.user = this->m_callbackParam;
        this->m_callback(params);
    }

    return 1;
}

// Win64: 0x0086C520
int32_t COsControl::SendEvent(int32_t event, int32_t code) {
    if (this->OnEvent(this->m_id, event, code)) {
        return 1;
    }

    if (this->m_dialog) {
        return this->m_dialog->OnEvent(this->m_id, event, code);
    }

    return 0;
}

// Win64: 0x0086C590
static int32_t sIsCharacterAllowed(char c, uint32_t filters) {
    uint8_t ch = static_cast<uint8_t>(c);

    if (ch <= 0x1F) {
        return 1;
    }

    if (filters & 0x1) {
        if (ch >= 'a' && ch <= 'z') {
            return 1;
        }

        if (ch >= 'A' && ch <= 'Z') {
            return 1;
        }
    }

    if ((filters & 0x2) && ch >= '0' && ch <= '9') {
        return 1;
    }

    if ((filters & 0x4) && (ch == '+' || ch == '-')) {
        return 1;
    }

    if ((filters & 0x8) && ch == '.') {
        return 1;
    }

    if ((filters & 0x10) && ch == ' ') {
        return 1;
    }

    if ((filters & 0x20) && ch == '_') {
        return 1;
    }

    return (filters & 0x40) && ch <= 0x7E;
}

// No native tree view, so never an edit control in it (TVM_GETEDITCONTROL).
void* COsTreeView::GetEditControl() {
    return nullptr;
}

// Win64: 0x0086C600
int32_t COsTreeView::IsCharacterAllowed(char c) {
    if (!this->GetEditControl()) {
        return 1;
    }

    if (this->m_filtersEnabled) {
        return sIsCharacterAllowed(c, this->m_filters);
    }

    return 1;
}

// Win64: 0x0086C650
void OsGuiSetWindowTitle(void* window, const char* title) {
    MacGameWindowSetTitle(window, title);
}

// Win64: 0x0086C6A0
void* OsGuiGetWindow(int32_t type) {
    switch (type) {
        case 0:
            return s_GxDevWindow;

        case 1:
        case 2:
            return MacGuiGetWindow(type);

        default:
            return nullptr;
    }
}

// Win64: 0x0086C6D0
void OsGuiSetGxWindow(void* window) {
    s_GxDevWindow = window;
}

// Win64: 0x0086C6E0
// An app-modal NSAlert whatever parentWindow is. Also printed to stderr, so failures stay in the logs of runs
// without a screen; WOWDECOMP_MAC_BACKGROUND=1 skips the alert and answers the first button.
int32_t OsGuiMessageBox(void* parentWindow, int32_t style, const char* message, const char* title) {
    if (!message) {
        message = "";
    }

    if (!title) {
        title = "";
    }

    std::fprintf(stderr, "OsGuiMessageBox: %s: %s\n", title, message);

    int32_t button = MacMessageBox(style, message, title);

    // MessageBoxW: IDOK and IDYES are 0, IDNO 1, IDCANCEL (and the rest) 2.
    switch (style) {
        case 1:
            // OK, Cancel
            return button == 1 ? 2 : 0;

        case 2:
            // Yes, No
            return button == 1 ? 1 : 0;

        case 3:
            // Yes, No, Cancel
            return button == 1 ? 1 : (button == 2 ? 2 : 0);

        default:
            return 0;
    }
}

// Win64: 0x0086C7D0
int32_t OsGuiIsModifierKeyDown(int32_t key) {
    return MacIsModifierKeyDown(key);
}

void OsGuiBeep() {
    MacBeep();
}

int32_t COsEditBox::IsCharacterAllowed(char c) {
    if (!this->m_filtersEnabled) {
        return 1;
    }

    return sIsCharacterAllowed(c, this->m_filters);
}

// TCM_GETITEMCOUNT of a native tab control: none.
int32_t COsTabControl::GetNumItems() {
    return 0;
}

// GetParent of the native dialog window: none.
void* COsDialog::GetParentWindow() {
    return nullptr;
}

int32_t COsDialog::HasFlag(uint32_t flag) {
    return (this->m_flags & flag) != 0;
}

int32_t COsDialog::IsInFront() {
    return this->m_handle == OsGuiGetWindow(2);
}

// Win64: 0x0086C840
COsControl* COsDialog::FindControl(void* handle) {
    for (uint32_t i = 0; i < this->m_controls.Count(); i++) {
        if (this->m_controls[i]->IsHandleFromControl(handle)) {
            return this->m_controls[i];
        }
    }

    return nullptr;
}

// Win64: 0x0086C890
int32_t COsDialog::OnCancel() {
    if (!this->m_cancelButton || this->m_cancelButton->OnEvent(this->m_cancelButton->m_id, 0, 0)) {
        return 1;
    }

    return this->OnEvent(this->m_cancelButton->m_id, 0, 0);
}

// Win64: 0x0086C900
int32_t COsTabControl::OnControlTab() {
    int32_t shift = OsGuiIsModifierKeyDown(1);
    int32_t value = this->GetValue();
    int32_t count = this->GetNumItems();

    int32_t next = shift ? value - 1 : value + 1;

    if (next < 0) {
        next = count - 1;
    }

    if (next >= count) {
        next = 0;
    }

    this->SetValue(next);
    this->SendEvent(2, 0);

    return 1;
}

// Win64: 0x0086C980
int32_t COsDialog::OnControlTab() {
    int32_t count = this->m_controls.Count();

    for (int32_t i = 0; i < count; i++) {
        if (this->m_controls[i]->m_type == 15) {
            COsTabControl* tabControl = static_cast<COsTabControl*>(this->m_controls[i]);
            return tabControl ? tabControl->OnControlTab() : 0;
        }
    }

    return 0;
}

// Win64: 0x0086C9C0
// The Win64 version filters the MSG of a native dialog (Escape, WM_CHAR filters, Return, Ctrl+Tab) before
// IsDialogMessage; there are no native dialogs or MSGs here.
int32_t COsDialog::ProcessMessage(void* message) {
    return 0;
}

// Win64: 0x0086CB00
// Menu accelerators, dialog messages and tooltips of the native GUI: none on macOS (OsInputGet never calls it).
int32_t OsGuiProcessMessage(void* message) {
    return 0;
}
