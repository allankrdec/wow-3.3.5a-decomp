// macOS input layer; counterpart of Engine/Source/Os/Win64/Input.cpp.
//
// The Cocoa side lives in MacWindow.mm: OsInputGet pumps NSApp events (MacPumpEvent) the way Win64 pumps
// PeekMessage/DispatchMessage, and the game view reports keys, characters, mouse and window events through the
// MacInput* functions at the end of this file, which do what the WM_* cases of the Win64 OsWindowProc do.
// Differences with Win64:
// - Keys are macOS virtual key codes (kVK_*); ConvertKeyCode maps them to the same KEY_* values. Command is not
//   mapped (no Cmd as Ctrl yet). Modifier keys come from flagsChanged with their exact side.
// - Characters arrive as Unicode code points (insertText:), so the ANSI code page tables are not needed.
// - The window frame (title bar drag, resize, minimize) is Cocoa's, so the non-client drag code is gone.
// - Relative mouse mode freezes the cursor (CGAssociateMouseAndMouseCursorPosition) and reads event deltas instead
//   of re-centering the cursor; the system mouse speed is never changed, OsInputSetMouseSpeed scales those deltas.

#include <Carbon/Carbon.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/Base/Platform/WinTypes.h"
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Joystick.h"
#include "Engine/Source/Os/Mac/MacInput.h"
#include "Engine/Source/Os/Mac/MacWindow.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C2iVector.h"

static int32_t s_appActive = 1;

static C2iVector s_mousePos;

static int32_t s_queueHead;

static int32_t s_queueTail;

static OSEVENT s_queue[16];

static int32_t s_mouseSpeed;

static int32_t s_savedMouseSpeed;

static uint32_t s_osButtonState;

static tagRECT s_defaultWindowRect;

static intptr_t s_windowSize;

static OS_MOUSE_MODE s_osMouseMode;

static int32_t s_WindowResizeLock;

static void (*s_eventPollProc)();

// Mac only: Windows asks GetAsyncKeyState, here the key state comes from the view's events (indexed by kVK_*).
static uint8_t s_keyDown[128];

static int32_t s_quitRequested;

static double s_wheelRemainder;

static double s_relativeRemainderX;

static double s_relativeRemainderY;

static const uint16_t s_functionKeyCodes[12] = {
    kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12,
};

static const uint16_t s_numberKeyCodes[10] = {
    kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
};

// Win64: 0x00868D40
void OsQueuePut(OSINPUT id, int32_t param0, int32_t param1, int32_t param2, int32_t param3) {
    int32_t next = s_queueTail == 15 ? 0 : s_queueTail + 1;

    if (next == s_queueHead) {
        s_queueHead = s_queueHead == 15 ? 0 : s_queueHead + 1;
    }

    OSEVENT& event = s_queue[s_queueTail];
    event.id = id;
    event.param[0] = param0;
    event.param[1] = param1;
    event.param[2] = param2;
    event.param[3] = OsGetAsyncTimeMs();

    s_queueTail = next;
}

// Win64: 0x00868DB0
int32_t OsQueueGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    if (s_queueHead == s_queueTail) {
        return 0;
    }

    OSEVENT& event = s_queue[s_queueHead];
    *id = event.id;
    *param0 = event.param[0];
    *param1 = event.param[1];
    *param2 = event.param[2];
    *param3 = event.param[3];

    s_queueHead = s_queueHead == 15 ? 0 : s_queueHead + 1;

    return 1;
}

// Win64: 0x00868E30
static int32_t ConvertKeyCode(uint32_t vkey, KEY* key) {
    for (uint32_t i = 0; i < 12; i++) {
        if (vkey == s_functionKeyCodes[i]) {
            *key = static_cast<KEY>(KEY_F1 + i);
            return 1;
        }
    }

    // The number row is positional like VK_0..VK_9, whatever the layout.
    for (uint32_t i = 0; i < 10; i++) {
        if (vkey == s_numberKeyCodes[i]) {
            *key = static_cast<KEY>(KEY_0 + i);
            return 1;
        }
    }

    switch (vkey) {
        // The Mac-only KEY_ values: F13 is the key where Print Screen is.
        case kVK_F13:
            *key = KEY_F13;
            return 1;

        case kVK_F14:
            *key = KEY_F14;
            return 1;

        case kVK_F15:
            *key = KEY_F15;
            return 1;

        case kVK_F16:
            *key = KEY_F16;
            return 1;

        case kVK_F17:
            *key = KEY_F17;
            return 1;

        case kVK_F18:
            *key = KEY_F18;
            return 1;

        case kVK_F19:
            *key = KEY_F19;
            return 1;

        case kVK_Shift:
            *key = KEY_LSHIFT;
            return 1;

        case kVK_RightShift:
            *key = KEY_RSHIFT;
            return 1;

        case kVK_Control:
            *key = KEY_LCONTROL;
            return 1;

        case kVK_RightControl:
            *key = KEY_RCONTROL;
            return 1;

        case kVK_Option:
            *key = KEY_LALT;
            return 1;

        case kVK_RightOption:
            *key = KEY_RALT;
            return 1;

        case kVK_Escape:
            *key = KEY_ESCAPE;
            return 1;

        case kVK_Return:
        case kVK_ANSI_KeypadEnter:
            *key = KEY_ENTER;
            return 1;

        case kVK_Delete:
            *key = KEY_BACKSPACE;
            return 1;

        case kVK_Tab:
            *key = KEY_TAB;
            return 1;

        case kVK_LeftArrow:
            *key = KEY_LEFT;
            return 1;

        case kVK_UpArrow:
            *key = KEY_UP;
            return 1;

        case kVK_RightArrow:
            *key = KEY_RIGHT;
            return 1;

        case kVK_DownArrow:
            *key = KEY_DOWN;
            return 1;

        // Help is the key where Insert is.
        case kVK_Help:
            *key = KEY_INSERT;
            return 1;

        case kVK_ForwardDelete:
            *key = KEY_DELETE;
            return 1;

        case kVK_Home:
            *key = KEY_HOME;
            return 1;

        case kVK_End:
            *key = KEY_END;
            return 1;

        case kVK_PageUp:
            *key = KEY_PAGEUP;
            return 1;

        case kVK_PageDown:
            *key = KEY_PAGEDOWN;
            return 1;

        case kVK_CapsLock:
            *key = KEY_CAPSLOCK;
            return 1;

        // Clear sits where Num Lock is.
        case kVK_ANSI_KeypadClear:
            *key = KEY_NUMLOCK;
            return 1;

        case kVK_Space:
            *key = KEY_SPACE;
            return 1;

        case kVK_ANSI_Keypad0:
            *key = KEY_NUMPAD0;
            return 1;

        case kVK_ANSI_Keypad1:
            *key = KEY_NUMPAD1;
            return 1;

        case kVK_ANSI_Keypad2:
            *key = KEY_NUMPAD2;
            return 1;

        case kVK_ANSI_Keypad3:
            *key = KEY_NUMPAD3;
            return 1;

        case kVK_ANSI_Keypad4:
            *key = KEY_NUMPAD4;
            return 1;

        case kVK_ANSI_Keypad5:
            *key = KEY_NUMPAD5;
            return 1;

        case kVK_ANSI_Keypad6:
            *key = KEY_NUMPAD6;
            return 1;

        case kVK_ANSI_Keypad7:
            *key = KEY_NUMPAD7;
            return 1;

        case kVK_ANSI_Keypad8:
            *key = KEY_NUMPAD8;
            return 1;

        case kVK_ANSI_Keypad9:
            *key = KEY_NUMPAD9;
            return 1;

        case kVK_ANSI_KeypadPlus:
            *key = KEY_NUMPAD_PLUS;
            return 1;

        case kVK_ANSI_KeypadMinus:
            *key = KEY_NUMPAD_MINUS;
            return 1;

        case kVK_ANSI_KeypadMultiply:
            *key = KEY_NUMPAD_MULTIPLY;
            return 1;

        case kVK_ANSI_KeypadDivide:
            *key = KEY_NUMPAD_DIVIDE;
            return 1;

        case kVK_ANSI_KeypadDecimal:
            *key = KEY_NUMPAD_DECIMAL;
            return 1;

        case kVK_ANSI_KeypadEquals:
            *key = KEY_NUMPAD_EQUALS;
            return 1;

        // Command, Function and the media keys have no KEY_ value.
        case kVK_Command:
        case kVK_RightCommand:
        case kVK_Function:
            return 0;

        default: {
            // MapVirtualKeyA(MAPVK_VK_TO_CHAR): the unshifted character in the current layout, letters uppercase.
            uint32_t ch = MacKeyCodeToCharacter(static_cast<uint16_t>(vkey));
            if (ch >= 'a' && ch <= 'z') {
                ch -= 'a' - 'A';
            }

            *key = static_cast<KEY>(ch);
            if (ch == 0 || ch > 0xFF) {
                return 0;
            }

            return 1;
        }
    }
}

// Win64: 0x008691C0
static int32_t ConvertKeyToVirtualKey(KEY key, uint32_t* vkey) {
    for (uint32_t i = 0; i < sizeof(s_keyDown); i++) {
        KEY converted;
        if (ConvertKeyCode(i, &converted) && converted == key) {
            *vkey = i;
            return 1;
        }
    }

    return 0;
}

// Win64: 0x008694E0
static int32_t ConvertButton(int32_t buttonNumber, MOUSEBUTTON* button) {
    switch (buttonNumber) {
        case 0:
            *button = MOUSE_BUTTON_LEFT;
            return 1;

        case 1:
            *button = MOUSE_BUTTON_RIGHT;
            return 1;

        case 2:
            *button = MOUSE_BUTTON_MIDDLE;
            return 1;

        case 3:
            *button = MOUSE_BUTTON_XBUTTON1;
            return 1;

        case 4:
            *button = MOUSE_BUTTON_XBUTTON2;
            return 1;

        default:
            break;
    }

    *button = MOUSE_BUTTON_NONE;
    return 0;
}

// Win64: 0x008695B0
static void RestoreMouse() {
    MacGameWindowSetMousePosition(s_mousePos.x, s_mousePos.y);
}

// Win64: 0x00869600
static void SaveMouse(int32_t x, int32_t y) {
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        return;
    }

    s_mousePos.x = x;
    s_mousePos.y = y;
}

// Win64: 0x00869620
void OsInputSetWindowResizeLock(int32_t resizeLock) {
    // Only the Win64 non-client drag code reads it; Cocoa resizes the window itself.
    s_WindowResizeLock = resizeLock;
}

// Win64: 0x00869630
float OsInputGetMouseSpeed() {
    return s_mouseSpeed * 0.1f;
}

// Win64: 0x00869640
int32_t OsInputIsKeyDown(KEY key) {
    uint32_t vkey;
    if (!ConvertKeyToVirtualKey(key, &vkey)) {
        return 0;
    }

    return s_keyDown[vkey] != 0;
}

// Win64: 0x00869670
int32_t OsInputIsMouseButtonDown(MOUSEBUTTON button) {
    uint32_t pressed = MacPressedMouseButtons();

    switch (button) {
        case MOUSE_BUTTON_LEFT:
            return (pressed >> 0) & 1;

        case MOUSE_BUTTON_MIDDLE:
            return (pressed >> 2) & 1;

        case MOUSE_BUTTON_RIGHT:
            return (pressed >> 1) & 1;

        case MOUSE_BUTTON_XBUTTON1:
            return (pressed >> 3) & 1;

        case MOUSE_BUTTON_XBUTTON2:
            return (pressed >> 4) & 1;

        default:
            return 0;
    }
}

// Win64: 0x00869720
void OsInputInitialize() {
    // No system mouse speed to save: 10 is the Windows default (OsInputGetMouseSpeed 1.0).
    s_savedMouseSpeed = 10;
    s_mouseSpeed = s_savedMouseSpeed;
}

// Win64: 0x00869760
void OsInputDestroy() {
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        MacSetMouseAssociated(true);
    }
}

// Win64: 0x008697E0
static void ProcessMouseEvent(int32_t x, int32_t y, MOUSEBUTTON button, OSINPUT id) {
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        x = s_mousePos.x;
        y = s_mousePos.y;
    }

    OsQueuePut(id, button, x, y, 0);
}

// Win64: 0x00869870
static int32_t HandleMouseDown(int32_t buttonNumber, int32_t x, int32_t y) {
    MOUSEBUTTON button;
    if (!ConvertButton(buttonNumber, &button)) {
        return 0;
    }

    // Cocoa keeps sending dragged events to the view while a button is down, like SetCapture.
    s_osButtonState |= button;

    ProcessMouseEvent(x, y, button, OS_INPUT_MOUSE_DOWN);

    return 1;
}

// Win64: 0x008698C0
static int32_t HandleMouseUp(int32_t buttonNumber, int32_t x, int32_t y) {
    MOUSEBUTTON button;
    if (!ConvertButton(buttonNumber, &button)) {
        return 0;
    }

    s_osButtonState &= ~button;

    ProcessMouseEvent(x, y, button, OS_INPUT_MOUSE_UP);

    return 1;
}

// Win64: 0x00869D90
void OsInputSetEventPollProc(void (*pollProc)()) {
    s_eventPollProc = pollProc;
}

// Win64: 0x00869DA0
static void OsInputCallEventPollProc() {
    s_eventPollProc();
}

// Win64: 0x00869DB0
static void CenterMouse() {
    // Win64 parks the cursor on the saved point and reads how far it moved; here the cursor is frozen there and
    // the deltas come with the mouse events.
    if (!MacGameWindowView()) {
        return;
    }

    s_relativeRemainderX = 0.0;
    s_relativeRemainderY = 0.0;
    MacSetMouseAssociated(false);
}

// Win64: 0x00869E00
// Debugging aid for headless runs: WOWDECOMP_MAC_SCREENSHOT=<seconds> presses Print Screen once after that many
// seconds, so the game writes Screenshots/ from the GL back buffer without needing screen-recording permission.
static void MacDebugScreenshot() {
    static uint32_t s_screenshotAt = 0xFFFFFFFF;

    if (s_screenshotAt == 0xFFFFFFFF) {
        const char* seconds = std::getenv("WOWDECOMP_MAC_SCREENSHOT");
        s_screenshotAt = seconds ? MacTickCount() + static_cast<uint32_t>(std::atoi(seconds)) * 1000 : 0;
    }

    if (s_screenshotAt && MacTickCount() >= s_screenshotAt) {
        s_screenshotAt = 0;
        OsQueuePut(OS_INPUT_KEY_DOWN, KEY_PRINTSCREEN, 1, 0, 0);
        OsQueuePut(OS_INPUT_KEY_UP, KEY_PRINTSCREEN, 1, 0, 0);
    }
}

int32_t OsInputGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    MacDebugScreenshot();

    *id = static_cast<OSINPUT>(-1);

    if (s_windowSize && !MacGameWindowIsMinimized()) {
        s_windowSize = 0;

        int32_t width;
        int32_t height;
        MacGameWindowGetSize(&width, &height);

        if (width != s_defaultWindowRect.right || height != s_defaultWindowRect.bottom) {
            s_defaultWindowRect.left = 0;
            s_defaultWindowRect.top = 0;
            s_defaultWindowRect.right = width;
            s_defaultWindowRect.bottom = height;

            *id = OS_INPUT_SIZE;
            *param0 = width;
            *param1 = height;
            *param2 = 0;
            *param3 = 0;

            return 1;
        }
    }

    if (s_queueHead == s_queueTail) {
        OsPollJoysticks(s_appActive);

        while (s_queueHead == s_queueTail) {
            // GetMessage returning 0 (WM_QUIT): here a quit request from the Dock or the application menu.
            if (s_quitRequested) {
                s_quitRequested = 0;
                *id = OS_INPUT_SHUTDOWN;
                return 1;
            }

            if (!MacPumpEvent() && !s_quitRequested) {
                return 0;
            }
        }
    }

    OsQueueGet(id, param0, param1, param2, param3);

    return 1;
}

// Win64: 0x0086A020
void OsInputSetMouseMode(OS_MOUSE_MODE mode) {
    if (mode >= OS_MOUSE_MODES) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return;
    }

    if (mode == s_osMouseMode) {
        return;
    }

    if (mode == OS_MOUSE_MODE_RELATIVE) {
        if (MacGameWindowView()) {
            int32_t x;
            int32_t y;
            MacGameWindowGetMousePosition(&x, &y);
            SaveMouse(x, y);
        }

        s_osMouseMode = mode;

        if (s_appActive) {
            CenterMouse();
        }
    } else if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        s_osMouseMode = OS_MOUSE_MODE_NORMAL;
        MacSetMouseAssociated(true);
        RestoreMouse();
    }
}

// Win64: 0x0086A070
void OsInputSetMouseSpeed(float speed) {
    float clamped = speed < 0.1f ? 0.1f : (speed >= 2.0f ? 2.0f : speed);
    s_mouseSpeed = static_cast<int32_t>(clamped * 10.0f);
}

// Win64: 0x0086A0D0
void OsInputGetMousePosition(int32_t* x, int32_t* y) {
    int32_t mouseX;
    int32_t mouseY;
    MacGameWindowGetMousePosition(&mouseX, &mouseY);
    SaveMouse(mouseX, mouseY);

    if (x) {
        *x = mouseX;
    }

    if (y) {
        *y = mouseY;
    }
}

// Win64: 0x0086A130
void OsInputSetMousePosition(int32_t x, int32_t y) {
    SaveMouse(x, y);

    // Win64 moves the cursor even when the game is in the background; the Mac build leaves it to the user then.
    if (s_appActive) {
        MacGameWindowSetMousePosition(x, y);
    }
}

// Win64: 0x0086A1A0
int32_t OsGetDefaultWindowRect(tagRECT* rect) {
    if (!rect) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!s_defaultWindowRect.right || !s_defaultWindowRect.bottom) {
        int32_t width;
        int32_t height;
        MacGameWindowGetSize(&width, &height);
        if (!width || !height) {
            return 0;
        }

        s_defaultWindowRect.left = 0;
        s_defaultWindowRect.top = 0;
        s_defaultWindowRect.right = width;
        s_defaultWindowRect.bottom = height;
    }

    *rect = s_defaultWindowRect;

    return 1;
}

// Win64: 0x0086A210
intptr_t OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    // No WM_* messages on macOS: the game view calls the MacInput* functions below directly. The Gx device still
    // receives this function as its window procedure and must not call it.
    return 0;
}

// The WM_SIZE, WM_ACTIVATE and WM_CLOSE cases of OsWindowProc.
void MacInputWindowEvent(MacWindowEvent event, int32_t width, int32_t height) {
    switch (event) {
        case MacWindowEvent_Resize:
        case MacWindowEvent_Restore:
            s_windowSize = 1;
            break;

        case MacWindowEvent_Minimize:
            s_windowSize = 0;
            break;

        case MacWindowEvent_FocusGained:
        case MacWindowEvent_FocusLost: {
            int32_t active = event == MacWindowEvent_FocusGained && !MacGameWindowIsMinimized() ? 1 : 0;

            // WM_CAPTURECHANGED: buttons held when the window loses focus are released by the game.
            if (!active && s_osButtonState) {
                int32_t x;
                int32_t y;
                MacGameWindowGetMousePosition(&x, &y);
                OsQueuePut(OS_INPUT_CAPTURE_CHANGED, 0, x, y, 0);
            }

            s_appActive = active;
            s_osButtonState = 0;
            // Key ups of an inactive window never arrive.
            std::memset(s_keyDown, 0, sizeof(s_keyDown));
            OsQueuePut(OS_INPUT_FOCUS, s_appActive != 0, 0, 0, 0);

            if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
                if (s_appActive) {
                    CenterMouse();
                } else {
                    MacSetMouseAssociated(true);
                }
            }
            break;
        }

        case MacWindowEvent_Close:
            OsQueuePut(OS_INPUT_CLOSE, 0, 0, 0, 0);
            break;

        default:
            break;
    }
}

void MacInputQuit() {
    s_quitRequested = 1;
}

// WM_KEYDOWN / WM_KEYUP. Auto-repeat sends more key downs, like Windows.
void MacInputKey(uint16_t keyCode, bool down) {
    if (keyCode < sizeof(s_keyDown)) {
        s_keyDown[keyCode] = down;
    }

    KEY key;
    if (!ConvertKeyCode(keyCode, &key)) {
        return;
    }

    OsQueuePut(down ? OS_INPUT_KEY_DOWN : OS_INPUT_KEY_UP, key, 1, 0, 0);
}

// flagsChanged: the modifier keys, which Windows sends as WM_KEYDOWN / WM_KEYUP.
void MacInputModifiers(uint16_t keyCode, uint64_t modifierFlags) {
    // Device-dependent bits (IOLLEvent.h NX_DEVICE*KEYMASK) tell the sides apart; events without them fall back
    // to the device-independent shift (1 << 17), control (1 << 18) and option (1 << 19) bits.
    uint64_t sideBit;
    uint64_t bothSides;
    uint64_t anyBit;

    switch (keyCode) {
        case kVK_Shift:
            sideBit = 0x2;
            bothSides = 0x2 | 0x4;
            anyBit = 1 << 17;
            break;

        case kVK_RightShift:
            sideBit = 0x4;
            bothSides = 0x2 | 0x4;
            anyBit = 1 << 17;
            break;

        case kVK_Control:
            sideBit = 0x1;
            bothSides = 0x1 | 0x2000;
            anyBit = 1 << 18;
            break;

        case kVK_RightControl:
            sideBit = 0x2000;
            bothSides = 0x1 | 0x2000;
            anyBit = 1 << 18;
            break;

        case kVK_Option:
            sideBit = 0x20;
            bothSides = 0x20 | 0x40;
            anyBit = 1 << 19;
            break;

        case kVK_RightOption:
            sideBit = 0x40;
            bothSides = 0x20 | 0x40;
            anyBit = 1 << 19;
            break;

        case kVK_CapsLock:
            // One flagsChanged per toggle; Windows sees a press and a release.
            MacInputKey(keyCode, true);
            MacInputKey(keyCode, false);
            return;

        default:
            // Command and Function: not game keys yet.
            if (keyCode < sizeof(s_keyDown)) {
                s_keyDown[keyCode] = !s_keyDown[keyCode];
            }
            return;
    }

    bool down = (modifierFlags & bothSides) ? (modifierFlags & sideBit) != 0 : (modifierFlags & anyBit) != 0;
    if (down == (s_keyDown[keyCode] != 0)) {
        return;
    }

    MacInputKey(keyCode, down);
}

// WM_CHAR: one Unicode code point of typed text. Control characters (Enter, Tab, Backspace...) are skipped like
// Windows does; the game reads those as keys.
void MacInputChar(uint32_t character) {
    if (character < 0x20 || character == 0x7F || (character >= 0xF700 && character <= 0xF8FF)) {
        return;
    }

    OsQueuePut(OS_INPUT_CHAR, character, 1, 0, 0);
}

// WM_?BUTTONDOWN / WM_?BUTTONUP. buttonNumber: 0 left, 1 right, 2 middle, 3 and 4 the extra buttons.
void MacInputMouseButton(int32_t buttonNumber, bool down, int32_t x, int32_t y) {
    if (down) {
        HandleMouseDown(buttonNumber, x, y);
    } else {
        HandleMouseUp(buttonNumber, x, y);
    }
}

// WM_MOUSEMOVE. inside: the point is in the view (Windows only sends client moves without capture).
void MacInputMouseMove(int32_t x, int32_t y, double deltaX, double deltaY, bool inside) {
    if (s_osMouseMode != OS_MOUSE_MODE_NORMAL) {
        if (!s_appActive) {
            return;
        }

        double scale = s_mouseSpeed * 0.1;
        s_relativeRemainderX += deltaX * scale;
        s_relativeRemainderY += deltaY * scale;

        int32_t dx = static_cast<int32_t>(s_relativeRemainderX);
        int32_t dy = static_cast<int32_t>(s_relativeRemainderY);
        s_relativeRemainderX -= dx;
        s_relativeRemainderY -= dy;

        if (dx || dy) {
            OsQueuePut(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, dx, dy, 0);
        }

        return;
    }

    if (!inside && !s_osButtonState) {
        return;
    }

    OsQueuePut(OS_INPUT_MOUSE_MOVE, 0, x, y, 0);
    SaveMouse(x, y);
}

// WM_MOUSEWHEEL: 120 per notch. Trackpads (precise deltas, in points) give one notch per 10 points.
void MacInputMouseWheel(double delta, bool precise, int32_t x, int32_t y) {
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        x = s_mousePos.x;
        y = s_mousePos.y;
    }

    int32_t distance;

    if (precise) {
        if ((delta > 0.0 && s_wheelRemainder < 0.0) || (delta < 0.0 && s_wheelRemainder > 0.0)) {
            s_wheelRemainder = 0.0;
        }

        s_wheelRemainder += delta;

        int32_t notches = static_cast<int32_t>(s_wheelRemainder / 10.0);
        if (!notches) {
            return;
        }

        s_wheelRemainder -= notches * 10.0;
        distance = notches * 120;
    } else {
        int32_t notches = static_cast<int32_t>(std::lround(delta));
        if (!notches) {
            if (delta == 0.0) {
                return;
            }

            notches = delta > 0.0 ? 1 : -1;
        }

        distance = notches * 120;
    }

    OsQueuePut(OS_INPUT_MOUSE_WHEEL, distance, x, y, 0);
}

// The SetTimer of OsWindowProc: the game keeps running while Cocoa tracks a live window resize.
void MacInputEventPoll() {
    if (s_eventPollProc) {
        OsInputCallEventPollProc();
    }
}

void OsInputEnableSecureMode(int32_t enable) {
}
