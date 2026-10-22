// Stage E4 manual test of the macOS window and input layer (Os/Mac): opens the game window, pumps OsInputGet and
// prints every OS input event.
//
//   WindowTest [seconds] [--synth] [--fullscreen]
//
// seconds: how long to run (default 10). --synth posts a few synthesized keyboard and mouse events into the
// NSApp queue first and checks that they come out of OsInputGet with the expected KEY_ / button values; the exit
// code is then the number of missing events. A quit request (OS_INPUT_SHUTDOWN) or the close button (OS_INPUT_CLOSE)
// ends the test early. --fullscreen opens the borderless window over the main screen.

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Mac/MacWindow.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

struct Expected {
    OSINPUT id;
    int32_t param0;
    bool seen;
};

static std::vector<Expected> s_expected;

static const char* s_inputNames[] = {
    "CAPTURE_CHANGED", "CHAR", "STRING", "IME", "SIZE", "CLOSE", "FOCUS", "KEY_DOWN", "KEY_UP", "MOUSE_DOWN",
    "MOUSE_MOVE", "MOUSE_WHEEL", "MOUSE_MOVE_RELATIVE", "MOUSE_UP", "14", "15", "16", "17", "18", "SHUTDOWN",
};

static const char* s_windowEventNames[] = { "Resize", "Minimize", "Restore", "FocusGained", "FocusLost", "Close" };

static void WindowEventProc(void* param, MacWindowEvent event, int32_t width, int32_t height) {
    std::printf("  window event %s %dx%d\n", s_windowEventNames[event], width, height);
}

static void Expect(OSINPUT id, int32_t param0) {
    s_expected.push_back({ id, param0, false });
}

static void PostKey(NSWindow* window, NSEventType type, uint16_t keyCode, NSString* chars, NSEventModifierFlags flags) {
    NSEvent* event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber] context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:keyCode];
    [NSApp postEvent:event atStart:NO];
}

// x, y: view coordinates (origin top-left).
static void PostMouse(NSWindow* window, NSEventType type, int32_t x, int32_t y, NSInteger buttonNumber) {
    NSView* view = [window contentView];
    NSPoint point = [view convertPoint:NSMakePoint(x + 0.5, y + 0.5) toView:nil];

    NSEvent* event = [NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber] context:nil eventNumber:0 clickCount:1 pressure:1.0f];

    // mouseEventWithType has no button number; other buttons go through CGEvent.
    if (buttonNumber > 1) {
        CGEventRef cgEvent = CGEventCreateCopy([event CGEvent]);
        CGEventSetIntegerValueField(cgEvent, kCGMouseEventButtonNumber, buttonNumber);
        event = [NSEvent eventWithCGEvent:cgEvent];
        CFRelease(cgEvent);
    }

    [NSApp postEvent:event atStart:NO];
}

static void PostSynthesizedEvents(NSWindow* window) {
    // 'A' with its character, then Shift+'1', F1, Return, keypad Enter, keypad 5, left Option.
    PostKey(window, NSEventTypeKeyDown, kVK_ANSI_A, @"a", 0);
    PostKey(window, NSEventTypeKeyUp, kVK_ANSI_A, @"a", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_A);
    Expect(OS_INPUT_CHAR, 'a');
    Expect(OS_INPUT_KEY_UP, KEY_A);

    PostKey(window, NSEventTypeFlagsChanged, kVK_Shift, @"", NSEventModifierFlagShift | 0x2);
    PostKey(window, NSEventTypeKeyDown, kVK_ANSI_1, @"!", NSEventModifierFlagShift | 0x2);
    PostKey(window, NSEventTypeKeyUp, kVK_ANSI_1, @"!", NSEventModifierFlagShift | 0x2);
    PostKey(window, NSEventTypeFlagsChanged, kVK_Shift, @"", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_LSHIFT);
    Expect(OS_INPUT_KEY_DOWN, KEY_1);
    Expect(OS_INPUT_CHAR, '!');
    Expect(OS_INPUT_KEY_UP, KEY_1);
    Expect(OS_INPUT_KEY_UP, KEY_LSHIFT);

    PostKey(window, NSEventTypeKeyDown, kVK_F1, [NSString stringWithFormat:@"%C", static_cast<unichar>(NSF1FunctionKey)], NSEventModifierFlagFunction);
    PostKey(window, NSEventTypeKeyUp, kVK_F1, [NSString stringWithFormat:@"%C", static_cast<unichar>(NSF1FunctionKey)], NSEventModifierFlagFunction);
    Expect(OS_INPUT_KEY_DOWN, KEY_F1);
    Expect(OS_INPUT_KEY_UP, KEY_F1);

    PostKey(window, NSEventTypeKeyDown, kVK_Return, @"\r", 0);
    PostKey(window, NSEventTypeKeyDown, kVK_ANSI_KeypadEnter, @"\x03", NSEventModifierFlagNumericPad);
    PostKey(window, NSEventTypeKeyDown, kVK_ANSI_Keypad5, @"5", NSEventModifierFlagNumericPad);
    Expect(OS_INPUT_KEY_DOWN, KEY_ENTER);
    Expect(OS_INPUT_KEY_DOWN, KEY_ENTER);
    Expect(OS_INPUT_KEY_DOWN, KEY_NUMPAD5);
    Expect(OS_INPUT_CHAR, '5');

    PostKey(window, NSEventTypeFlagsChanged, kVK_RightOption, @"", NSEventModifierFlagOption | 0x40);
    PostKey(window, NSEventTypeFlagsChanged, kVK_RightOption, @"", 0);
    Expect(OS_INPUT_KEY_DOWN, KEY_RALT);
    Expect(OS_INPUT_KEY_UP, KEY_RALT);

    PostMouse(window, NSEventTypeMouseMoved, 100, 50, 0);
    PostMouse(window, NSEventTypeLeftMouseDown, 100, 50, 0);
    PostMouse(window, NSEventTypeLeftMouseUp, 100, 50, 0);
    PostMouse(window, NSEventTypeRightMouseDown, 10, 20, 1);
    PostMouse(window, NSEventTypeRightMouseUp, 10, 20, 1);
    PostMouse(window, NSEventTypeOtherMouseDown, 30, 40, 2);
    PostMouse(window, NSEventTypeOtherMouseUp, 30, 40, 2);
    PostMouse(window, NSEventTypeOtherMouseDown, 30, 40, 3);
    PostMouse(window, NSEventTypeOtherMouseUp, 30, 40, 3);
    Expect(OS_INPUT_MOUSE_MOVE, 0);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_LEFT);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_RIGHT);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_RIGHT);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_MIDDLE);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_MIDDLE);
    Expect(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_XBUTTON1);
    Expect(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_XBUTTON1);

    // One wheel notch up, as a line-based (mouse) scroll.
    CGEventRef wheel = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, 1);
    NSEvent* wheelEvent = [NSEvent eventWithCGEvent:wheel];
    CFRelease(wheel);
    // eventWithCGEvent has no window: route it the way a real wheel event over the view is.
    [[window contentView] scrollWheel:wheelEvent];
    Expect(OS_INPUT_MOUSE_WHEEL, 120);

    // Quit from the Dock: OsInputGet returns OS_INPUT_SHUTDOWN instead of the process exiting.
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
    Expect(OS_INPUT_SHUTDOWN, -1);
}

static void CheckExpected(OSINPUT id, int32_t param0) {
    for (Expected& expected : s_expected) {
        if (!expected.seen && expected.id == id && (expected.param0 == param0 || expected.param0 == -1)) {
            expected.seen = true;
            return;
        }
    }
}

int main(int argc, char** argv) {
    double seconds = 10.0;
    bool synth = false;
    bool windowed = true;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--synth")) {
            synth = true;
        } else if (!std::strcmp(argv[i], "--fullscreen")) {
            windowed = false;
        } else {
            seconds = std::atof(argv[i]);
        }
    }

    StormRtlInitialize();
    OsTimeManagerCreate(OsTimeManager::BestAvailable);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }

    OsInputInitialize();

    NSView* view = static_cast<NSView*>(MacGameWindowCreate(800, 600, windowed, WindowEventProc, nullptr));
    if (!view) {
        std::printf("FAIL MacGameWindowCreate\n");
        return 1;
    }

    MacGameWindowShow();
    std::printf("window open, %.1f s (close it or quit from the Dock to stop early)\n", seconds);

    uint32_t start = OsGetAsyncTimeMs();
    bool posted = false;
    bool done = false;

    while (!done && OsGetAsyncTimeMs() - start < seconds * 1000.0) {
        if (synth && !posted && OsGetAsyncTimeMs() - start > 500) {
            posted = true;
            PostSynthesizedEvents([view window]);
        }

        OSINPUT id;
        int32_t param[4];

        while (OsInputGet(&id, &param[0], &param[1], &param[2], &param[3])) {
            const char* name = static_cast<uint32_t>(id) < sizeof(s_inputNames) / sizeof(s_inputNames[0]) ? s_inputNames[id] : "?";

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

            CheckExpected(id, param[0]);

            if (id == OS_INPUT_CLOSE || id == OS_INPUT_SHUTDOWN) {
                done = true;
            }
        }

        usleep(5000);
    }

    MacGameWindowDestroy();
    OsInputDestroy();
    OsTimeManagerDestroy();

    int32_t missing = 0;
    for (const Expected& expected : s_expected) {
        if (!expected.seen) {
            std::printf("MISSING %s %d (0x%X)\n", s_inputNames[expected.id], expected.param0, expected.param0);
            missing++;
        }
    }

    if (synth) {
        std::printf("%s: %d of %zu synthesized events missing\n", missing ? "FAIL" : "OK", missing, s_expected.size());
    }

    return missing;
}
