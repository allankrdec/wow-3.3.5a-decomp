// Stages E4/E7 test of the macOS window and input layer (Os/Mac): opens the game window, pumps OsInputGet and
// prints every OS input event.
//
//   WindowTest [seconds] [--synth] [--fullscreen] [--alert]
//
// seconds: how long to run (default 10). --synth runs the synthetic checks (below) first and exits with the number
// of failed checks. A quit request (OS_INPUT_SHUTDOWN) or the close button / Cmd+Q (OS_INPUT_CLOSE) ends a manual
// run early. --fullscreen opens the borderless window over the main screen. --alert shows the four OsGuiMessageBox
// styles one after the other and prints what each returned (manual check).
//
// The synthetic checks hand the events straight to the game view (and post the window notifications), so they need
// neither the focus nor the real mouse; with WOWDECOMP_MAC_BACKGROUND=1 the window stays behind the others and the
// real cursor is never moved or frozen. Each step drains OsInputGet before the next one (the OS queue holds 16
// events). Checked: keys and characters, mouse buttons and wheel, Command shortcuts (dropped by default, Control
// with Command as Control, Cmd+Q from the menu), focus loss (buttons and keys released, relative mode suspended),
// mouse-look (frozen cursor, unaccelerated deltas, position restored), resize and the windowed/fullscreen switch,
// the clipboard (restored afterwards), message boxes without UI in background mode, quit from the Dock.

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Mac/MacWindow.h"
#include "Engine/Source/Os/OsClipboard.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

static const int32_t ANY = 0x7FFFFFFF;

struct Expected {
    OSINPUT id;
    int32_t param[3];
    bool seen;
};

static std::vector<Expected> s_expected;
static std::vector<Expected> s_forbidden;
static int32_t s_failures;
static bool s_quiet;

static const char* s_inputNames[] = {
    "CAPTURE_CHANGED", "CHAR", "STRING", "IME", "SIZE", "CLOSE", "FOCUS", "KEY_DOWN", "KEY_UP", "MOUSE_DOWN",
    "MOUSE_MOVE", "MOUSE_WHEEL", "MOUSE_MOVE_RELATIVE", "MOUSE_UP", "14", "15", "16", "17", "18", "SHUTDOWN",
};

static const char* s_windowEventNames[] = { "Resize", "Minimize", "Restore", "FocusGained", "FocusLost", "Close" };

static const char* InputName(OSINPUT id) {
    return static_cast<uint32_t>(id) < sizeof(s_inputNames) / sizeof(s_inputNames[0]) ? s_inputNames[id] : "?";
}

static void WindowEventProc(void* param, MacWindowEvent event, int32_t width, int32_t height) {
    if (!s_quiet) {
        std::printf("  window event %s %dx%d\n", s_windowEventNames[event], width, height);
    }
}

static void Expect(OSINPUT id, int32_t param0, int32_t param1 = ANY, int32_t param2 = ANY) {
    s_expected.push_back({ id, { param0, param1, param2 }, false });
}

static void Forbid(OSINPUT id, int32_t param0 = ANY) {
    s_forbidden.push_back({ id, { param0, ANY, ANY }, false });
}

static void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        s_failures++;
    }
}

static bool Matches(const Expected& expected, OSINPUT id, const int32_t* param) {
    if (expected.id != id) {
        return false;
    }

    for (int32_t i = 0; i < 3; i++) {
        if (expected.param[i] != ANY && expected.param[i] != param[i]) {
            return false;
        }
    }

    return true;
}

static void PrintEvent(OSINPUT id, const int32_t* param) {
    const char* name = InputName(id);

    switch (id) {
        case OS_INPUT_KEY_DOWN:
        case OS_INPUT_KEY_UP:
            std::printf("%-20s key=0x%03X", name, param[0]);
            if (param[0] >= 0x20 && param[0] < 0x7F) {
                std::printf(" '%c'", param[0]);
            }
            std::printf(" repeat=%d\n", param[1]);
            break;

        case OS_INPUT_CHAR:
            std::printf("%-20s U+%04X repeat=%d\n", name, param[0], param[1]);
            break;

        default:
            std::printf("%-20s %d %d %d\n", name, param[0], param[1], param[2]);
            break;
    }
}

// Pumps the event loop for a while and checks the events against the step's expectations.
static bool Drain(double milliseconds) {
    bool done = false;
    uint32_t start = OsGetAsyncTimeMs();

    do {
        OSINPUT id;
        int32_t param[4];

        while (OsInputGet(&id, &param[0], &param[1], &param[2], &param[3])) {
            if (!s_quiet) {
                PrintEvent(id, param);
            }

            for (Expected& expected : s_expected) {
                if (!expected.seen && Matches(expected, id, param)) {
                    expected.seen = true;
                    break;
                }
            }

            for (Expected& forbidden : s_forbidden) {
                if (Matches(forbidden, id, param)) {
                    forbidden.seen = true;
                }
            }

            if (id == OS_INPUT_CLOSE || id == OS_INPUT_SHUTDOWN) {
                done = true;
            }
        }

        usleep(2000);
    } while (OsGetAsyncTimeMs() - start < milliseconds);

    return done;
}

static void EndStep(const char* name) {
    Drain(60.0);

    int32_t failed = 0;

    for (const Expected& expected : s_expected) {
        if (!expected.seen) {
            std::printf("FAIL %s: missing %s %d %d %d\n", name, InputName(expected.id), expected.param[0], expected.param[1], expected.param[2]);
            failed++;
        }
    }

    for (const Expected& forbidden : s_forbidden) {
        if (forbidden.seen) {
            std::printf("FAIL %s: unexpected %s %d\n", name, InputName(forbidden.id), forbidden.param[0]);
            failed++;
        }
    }

    std::printf("%s %s\n", failed ? "FAIL" : "ok  ", name);
    s_failures += failed;
    s_expected.clear();
    s_forbidden.clear();
}

// Events go straight to the view, as the window would route them, so the checks do not need the key window.

static NSEvent* KeyEvent(NSWindow* window, NSEventType type, uint16_t keyCode, NSString* chars, NSEventModifierFlags flags) {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber] context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:keyCode];
}

static void SendKey(NSWindow* window, NSEventType type, uint16_t keyCode, NSString* chars, NSEventModifierFlags flags) {
    NSView* view = [window contentView];
    NSEvent* event = KeyEvent(window, type, keyCode, chars, flags);

    switch (type) {
        case NSEventTypeKeyDown:
            [view keyDown:event];
            break;

        case NSEventTypeKeyUp:
            [view keyUp:event];
            break;

        default:
            [view flagsChanged:event];
            break;
    }
}

// x, y: view coordinates (origin top-left). delta and raw: the accelerated and unaccelerated motion.
static NSEvent* MouseEvent(NSWindow* window, NSEventType type, int32_t x, int32_t y, NSInteger buttonNumber, double deltaX = 0.0, double deltaY = 0.0, double rawX = 0.0, double rawY = 0.0) {
    NSView* view = [window contentView];
    NSPoint point = [view convertPoint:NSMakePoint(x + 0.5, y + 0.5) toView:nil];

    NSEvent* event = [NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber] context:nil eventNumber:0 clickCount:1 pressure:1.0f];

    // mouseEventWithType has no button number and no deltas; those go through CGEvent. The CGEvent carries the
    // screen position, which eventWithCGEvent turns back into a window position only for a window under it, so the
    // location is checked by the button events, which keep the NSEvent.
    if (buttonNumber > 1 || deltaX != 0.0 || deltaY != 0.0 || rawX != 0.0 || rawY != 0.0) {
        CGEventRef cgEvent = CGEventCreateCopy([event CGEvent]);
        CGEventSetIntegerValueField(cgEvent, kCGMouseEventButtonNumber, buttonNumber);
        CGEventSetDoubleValueField(cgEvent, kCGMouseEventDeltaX, deltaX);
        CGEventSetDoubleValueField(cgEvent, kCGMouseEventDeltaY, deltaY);
        CGEventSetDoubleValueField(cgEvent, kCGEventUnacceleratedPointerMovementX, rawX);
        CGEventSetDoubleValueField(cgEvent, kCGEventUnacceleratedPointerMovementY, rawY);
        event = [NSEvent eventWithCGEvent:cgEvent];
        CFRelease(cgEvent);
    }

    return event;
}

static void SendMouse(NSWindow* window, NSEventType type, int32_t x, int32_t y, NSInteger buttonNumber, double deltaX = 0.0, double deltaY = 0.0, double rawX = 0.0, double rawY = 0.0) {
    NSView* view = [window contentView];
    NSEvent* event = MouseEvent(window, type, x, y, buttonNumber, deltaX, deltaY, rawX, rawY);

    switch (type) {
        case NSEventTypeLeftMouseDown:
            [view mouseDown:event];
            break;

        case NSEventTypeLeftMouseUp:
            [view mouseUp:event];
            break;

        case NSEventTypeRightMouseDown:
            [view rightMouseDown:event];
            break;

        case NSEventTypeRightMouseUp:
            [view rightMouseUp:event];
            break;

        case NSEventTypeOtherMouseDown:
            [view otherMouseDown:event];
            break;

        case NSEventTypeOtherMouseUp:
            [view otherMouseUp:event];
            break;

        case NSEventTypeRightMouseDragged:
            [view rightMouseDragged:event];
            break;

        default:
            [view mouseMoved:event];
            break;
    }
}

// The notifications NSWindow posts when it gains or loses the key status; its delegate observes them.
static void PostFocus(NSWindow* window, bool gained) {
    [[NSNotificationCenter defaultCenter] postNotificationName:gained ? NSWindowDidBecomeKeyNotification : NSWindowDidResignKeyNotification object:window];
}

static void RunSyntheticChecks(NSWindow* window) {
    NSView* view = [window contentView];
    bool background = MacBackgroundMode();

    // Start from a known state: active, no Command as Control, the first OS_INPUT_SIZE consumed.
    PostFocus(window, true);
    MacSetCommandAsControl(false);
    Drain(100.0);

    // Keys: 'A' with its character, then Shift+'1', F1, Return, keypad Enter, keypad 5, right Option.
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_A, @"a", 0);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_A, @"a", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_A);
    Expect(OS_INPUT_CHAR, 'a');
    Expect(OS_INPUT_KEY_UP, KEY_A);

    SendKey(window, NSEventTypeFlagsChanged, kVK_Shift, @"", NSEventModifierFlagShift | 0x2);
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_1, @"!", NSEventModifierFlagShift | 0x2);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_1, @"!", NSEventModifierFlagShift | 0x2);
    SendKey(window, NSEventTypeFlagsChanged, kVK_Shift, @"", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_LSHIFT);
    Expect(OS_INPUT_KEY_DOWN, KEY_1);
    Expect(OS_INPUT_CHAR, '!');
    Expect(OS_INPUT_KEY_UP, KEY_1);
    Expect(OS_INPUT_KEY_UP, KEY_LSHIFT);
    EndStep("keys and characters");

    SendKey(window, NSEventTypeKeyDown, kVK_F1, [NSString stringWithFormat:@"%C", static_cast<unichar>(NSF1FunctionKey)], NSEventModifierFlagFunction);
    SendKey(window, NSEventTypeKeyUp, kVK_F1, [NSString stringWithFormat:@"%C", static_cast<unichar>(NSF1FunctionKey)], NSEventModifierFlagFunction);
    Expect(OS_INPUT_KEY_DOWN, KEY_F1);
    Expect(OS_INPUT_KEY_UP, KEY_F1);

    SendKey(window, NSEventTypeKeyDown, kVK_Return, @"\r", 0);
    SendKey(window, NSEventTypeKeyUp, kVK_Return, @"\r", 0);
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_KeypadEnter, @"\x03", NSEventModifierFlagNumericPad);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_KeypadEnter, @"\x03", NSEventModifierFlagNumericPad);
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_Keypad5, @"5", NSEventModifierFlagNumericPad);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_Keypad5, @"5", NSEventModifierFlagNumericPad);
    Expect(OS_INPUT_KEY_DOWN, KEY_ENTER);
    Expect(OS_INPUT_KEY_DOWN, KEY_ENTER);
    Expect(OS_INPUT_KEY_DOWN, KEY_NUMPAD5);
    Expect(OS_INPUT_CHAR, '5');

    SendKey(window, NSEventTypeFlagsChanged, kVK_RightOption, @"", NSEventModifierFlagOption | 0x40);
    SendKey(window, NSEventTypeFlagsChanged, kVK_RightOption, @"", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_RALT);
    Expect(OS_INPUT_KEY_UP, KEY_RALT);
    EndStep("function, keypad and modifier keys");

    // Mouse buttons and wheel.
    SendMouse(window, NSEventTypeMouseMoved, 100, 50, 0);
    SendMouse(window, NSEventTypeLeftMouseDown, 100, 50, 0);
    SendMouse(window, NSEventTypeLeftMouseUp, 100, 50, 0);
    SendMouse(window, NSEventTypeRightMouseDown, 10, 20, 1);
    SendMouse(window, NSEventTypeRightMouseUp, 10, 20, 1);
    Expect(OS_INPUT_MOUSE_MOVE, 0, 100, 50);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_LEFT, 100, 50);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT, 100, 50);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_RIGHT, 10, 20);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_RIGHT, 10, 20);
    EndStep("left and right buttons");

    SendMouse(window, NSEventTypeOtherMouseDown, 30, 40, 2);
    SendMouse(window, NSEventTypeOtherMouseUp, 30, 40, 2);
    SendMouse(window, NSEventTypeOtherMouseDown, 30, 40, 3);
    SendMouse(window, NSEventTypeOtherMouseUp, 30, 40, 3);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_MIDDLE);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_MIDDLE);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_XBUTTON1);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_XBUTTON1);

    // One wheel notch up, as a line-based (mouse) scroll.
    CGEventRef wheel = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, 1);
    [view scrollWheel:[NSEvent eventWithCGEvent:wheel]];
    CFRelease(wheel);
    Expect(OS_INPUT_MOUSE_WHEEL, 120);
    EndStep("other buttons and wheel");

    // Command shortcuts are not game keys by default: Command itself and Cmd+W give nothing.
    SendKey(window, NSEventTypeFlagsChanged, kVK_Command, @"", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_W, @"w", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_W, @"w", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeFlagsChanged, kVK_Command, @"", 0);
    Forbid(OS_INPUT_KEY_DOWN);
    Forbid(OS_INPUT_KEY_UP);
    Forbid(OS_INPUT_CHAR);
    EndStep("Command + key ignored by default");

    // A key held before Command still gets its key up.
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_W, @"w", 0);
    SendKey(window, NSEventTypeFlagsChanged, kVK_Command, @"", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_W, @"w", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeFlagsChanged, kVK_Command, @"", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_W);
    Expect(OS_INPUT_KEY_UP, KEY_W);
    EndStep("key held before Command released");

    // Command as Control: left and right Command are the Control keys; Cmd+W is Ctrl+W (no character).
    MacSetCommandAsControl(true);
    SendKey(window, NSEventTypeFlagsChanged, kVK_Command, @"", NSEventModifierFlagCommand | 0x8);
    Check(OsInputIsKeyDown(KEY_LCONTROL), "Command as Control: OsInputIsKeyDown(KEY_LCONTROL)");
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_W, @"w", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeKeyUp, kVK_ANSI_W, @"w", NSEventModifierFlagCommand | 0x8);
    SendKey(window, NSEventTypeFlagsChanged, kVK_Command, @"", 0);
    SendKey(window, NSEventTypeFlagsChanged, kVK_RightCommand, @"", NSEventModifierFlagCommand | 0x10);
    SendKey(window, NSEventTypeFlagsChanged, kVK_RightCommand, @"", 0);
    Check(!OsInputIsKeyDown(KEY_LCONTROL), "Command as Control: KEY_LCONTROL up");
    Expect(OS_INPUT_KEY_DOWN, KEY_LCONTROL);
    Expect(OS_INPUT_KEY_DOWN, KEY_W);
    Expect(OS_INPUT_KEY_UP, KEY_W);
    Expect(OS_INPUT_KEY_UP, KEY_LCONTROL);
    Expect(OS_INPUT_KEY_DOWN, KEY_RCONTROL);
    Expect(OS_INPUT_KEY_UP, KEY_RCONTROL);
    Forbid(OS_INPUT_CHAR);
    EndStep("Command as Control");
    MacSetCommandAsControl(false);

    // The menu: Cmd+H and Cmd+M exist, Cmd+Q asks the game to close (the Alt+F4 path).
    NSMenu* menu = [NSApp mainMenu];
    Check(menu != nil, "main menu installed");
    bool hide = false;
    bool minimize = false;
    for (NSMenuItem* item in [menu itemArray]) {
        for (NSMenuItem* subitem in [[item submenu] itemArray]) {
            if ([[subitem keyEquivalent] isEqualToString:@"h"] && [subitem keyEquivalentModifierMask] == NSEventModifierFlagCommand) {
                hide = [subitem action] == @selector(hide:);
            }
            if ([[subitem keyEquivalent] isEqualToString:@"m"]) {
                minimize = true;
            }
        }
    }
    Check(hide, "Cmd+H menu item");
    Check(minimize, "Cmd+M menu item");
    NSEvent* cmdQ = KeyEvent(window, NSEventTypeKeyDown, kVK_ANSI_Q, @"q", NSEventModifierFlagCommand);
    Check([menu performKeyEquivalent:cmdQ], "Cmd+Q handled by the menu");
    Expect(OS_INPUT_CLOSE, 0);
    Forbid(OS_INPUT_SHUTDOWN);
    Forbid(OS_INPUT_KEY_DOWN);
    EndStep("Cmd+Q is OS_INPUT_CLOSE");

    // Focus loss with a button and a key down: CAPTURE_CHANGED, the key released, FOCUS 0.
    SendMouse(window, NSEventTypeRightMouseDown, 10, 20, 1);
    SendKey(window, NSEventTypeKeyDown, kVK_ANSI_W, @"w", 0);
    PostFocus(window, false);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_RIGHT);
    Expect(OS_INPUT_KEY_DOWN, KEY_W);
    Expect(OS_INPUT_CAPTURE_CHANGED, 0);
    Expect(OS_INPUT_KEY_UP, KEY_W);
    Expect(OS_INPUT_FOCUS, 0);
    EndStep("focus lost releases buttons and keys");
    Check(!OsInputIsKeyDown(KEY_W), "no key stuck after focus loss");

    SendMouse(window, NSEventTypeRightMouseUp, 10, 20, 1);
    PostFocus(window, true);
    Expect(OS_INPUT_FOCUS, 1);
    EndStep("focus gained");

    // Mouse-look: the cursor freezes where it is, the deltas are the unaccelerated ones, buttons report the saved
    // point, focus loss frees the cursor, focus gain freezes it again, leaving the mode puts it back.
    SendMouse(window, NSEventTypeMouseMoved, 120, 80, 0);
    Drain(20.0);
    OsInputSetMouseMode(OS_MOUSE_MODE_RELATIVE);
    Check(!MacIsMouseAssociated(), "relative mode freezes the cursor");
    SendMouse(window, NSEventTypeRightMouseDragged, 300, 200, 1, 9.0, -6.0, 5.0, -3.0);
    SendMouse(window, NSEventTypeMouseMoved, 300, 200, 0, 4.0, 2.0);
    SendMouse(window, NSEventTypeRightMouseDown, 300, 200, 1);
    SendMouse(window, NSEventTypeRightMouseUp, 300, 200, 1);
    Expect(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, 5, -3);
    Expect(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, 4, 2);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_RIGHT, 120, 80);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_RIGHT, 120, 80);
    Forbid(OS_INPUT_MOUSE_MOVE);
    EndStep("mouse-look deltas");

    Expect(OS_INPUT_FOCUS, 0);
    PostFocus(window, false);
    Drain(20.0);
    Check(MacIsMouseAssociated(), "focus loss frees the cursor");
    SendMouse(window, NSEventTypeMouseMoved, 300, 200, 0, 4.0, 2.0, 4.0, 2.0);
    Forbid(OS_INPUT_MOUSE_MOVE_RELATIVE);
    EndStep("mouse-look suspended without focus");

    Expect(OS_INPUT_FOCUS, 1);
    PostFocus(window, true);
    Drain(20.0);
    Check(!MacIsMouseAssociated(), "focus gain freezes the cursor again");
    OsInputSetMouseMode(OS_MOUSE_MODE_NORMAL);
    Check(MacIsMouseAssociated(), "normal mode frees the cursor");
    if (background) {
        int32_t x = -1;
        int32_t y = -1;
        OsInputGetMousePosition(&x, &y);
        Check(x == 120 && y == 80, "cursor restored to where mouse-look started");
        if (x != 120 || y != 80) {
            std::printf("  cursor at %d,%d\n", x, y);
        }
    }
    EndStep("mouse-look ends");

    // Resize, then fullscreen on the window's screen and back to the same windowed frame.
    NSRect before = [window frame];
    [window setContentSize:NSMakeSize(640, 480)];
    Expect(OS_INPUT_SIZE, 640, 480);
    EndStep("resize");

    NSRect windowed = [window frame];
    NSRect screen = [[window screen] frame];
    MacGameWindowSetMode(640, 480, false);
    Check(!([window styleMask] & NSWindowStyleMaskTitled), "fullscreen window is borderless");
    Check(NSEqualRects([window frame], screen), "fullscreen window covers its screen");
    Expect(OS_INPUT_SIZE, static_cast<int32_t>(screen.size.width), static_cast<int32_t>(screen.size.height));
    EndStep("windowed to fullscreen");

    MacGameWindowSetMode(640, 480, true);
    Check(([window styleMask] & NSWindowStyleMaskTitled) != 0, "windowed again");
    Check(NSEqualRects([window frame], windowed), "windowed frame restored");
    Expect(OS_INPUT_SIZE, 640, 480);
    EndStep("fullscreen to windowed");

    MacGameWindowSetMode(static_cast<int32_t>([window contentRectForFrameRect:before].size.width), static_cast<int32_t>([window contentRectForFrameRect:before].size.height), true);
    Drain(20.0);

    // Clipboard round trip with non-ASCII text; the user's clipboard comes back afterwards.
    int32_t failuresBefore = s_failures;
    NSString* saved = [[[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString] copy];
    const char* text = "Ol\xC3\xA1 \xE2\x9C\x93 \xE9\xAD\x94\xE5\x85\xBD\nline 2";
    // OsClipboardGetString/PutString pass the Gx device's window, and there is no Gx device here.
    Check(IOsClipboardPutString(text, view) == 1, "clipboard put");
    char* back = IOsClipboardGetString(view);
    Check(back && !std::strcmp(back, text), "clipboard get returns the same UTF-8");
    OsClipboardFreeString(back);
    [[NSPasteboard generalPasteboard] clearContents];
    Check(IOsClipboardGetString(view) == nullptr, "empty clipboard gives nullptr");
    if (saved) {
        [[NSPasteboard generalPasteboard] setString:saved forType:NSPasteboardTypeString];
        [saved release];
    }
    std::printf("%s clipboard\n", s_failures != failuresBefore ? "FAIL" : "ok  ");

    // Message boxes never show in background mode and answer the first button.
    if (background) {
        Check(OsGuiMessageBox(nullptr, 0, "WindowTest message", "WindowTest") == 0, "message box OK");
        Check(OsGuiMessageBox(nullptr, 1, "WindowTest message", "WindowTest") == 0, "message box OK/Cancel");
        Check(OsGuiMessageBox(nullptr, 2, "WindowTest message", "WindowTest") == 0, "message box Yes/No");
        Check(OsGuiMessageBox(nullptr, 3, "WindowTest message", "WindowTest") == 0, "message box Yes/No/Cancel");
        std::printf("ok   message boxes skipped in background mode\n");
    }

    // Quit from the Dock: OsInputGet returns OS_INPUT_SHUTDOWN instead of the process exiting.
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
    Expect(OS_INPUT_SHUTDOWN, ANY);
    Drain(100.0);
    EndStep("quit from the Dock is OS_INPUT_SHUTDOWN");
}

static void RunAlerts() {
    static const char* s_styles[] = { "OK", "OK/Cancel", "Yes/No", "Yes/No/Cancel" };

    for (int32_t style = 0; style < 4; style++) {
        char message[128];
        std::snprintf(message, sizeof(message), "Style %d (%s). Pick any button.", style, s_styles[style]);
        int32_t result = OsGuiMessageBox(nullptr, style, message, "WindowTest");
        std::printf("message box %s returned %d (0 OK/Yes, 1 No, 2 Cancel)\n", s_styles[style], result);
    }
}

int main(int argc, char** argv) {
    double seconds = 10.0;
    bool synth = false;
    bool windowed = true;
    bool alerts = false;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--synth")) {
            synth = true;
        } else if (!std::strcmp(argv[i], "--fullscreen")) {
            windowed = false;
        } else if (!std::strcmp(argv[i], "--alert")) {
            alerts = true;
        } else {
            seconds = std::atof(argv[i]);
        }
    }

    StormRtlInitialize();
    OsTimeManagerCreate(OsTimeManager::BestAvailable);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:MacBackgroundMode() ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }

    OsInputInitialize();

    NSView* view = static_cast<NSView*>(MacGameWindowCreate(800, 600, windowed, WindowEventProc, nullptr));
    if (!view) {
        std::printf("FAIL MacGameWindowCreate\n");
        return 1;
    }

    MacGameWindowShow();

    if (alerts) {
        RunAlerts();
    }

    if (synth) {
        s_quiet = !std::getenv("WINDOWTEST_VERBOSE");
        RunSyntheticChecks([view window]);
    } else {
        std::printf("window open, %.1f s (close it, Cmd+Q or quit from the Dock to stop early)\n", seconds);

        uint32_t start = OsGetAsyncTimeMs();
        while (OsGetAsyncTimeMs() - start < seconds * 1000.0) {
            if (Drain(10.0)) {
                break;
            }
        }
    }

    MacGameWindowDestroy();
    OsInputDestroy();
    OsTimeManagerDestroy();

    if (synth) {
        std::printf("%s: %d failed checks\n", s_failures ? "FAIL" : "OK", s_failures);
    }

    return s_failures;
}
