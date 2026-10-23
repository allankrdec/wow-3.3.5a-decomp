// The game window on macOS (MacWindow.h): an NSWindow whose content view, WowGameView, turns Cocoa events into the
// MacInput* calls of Input.cpp and window events into the Gx device's MacWindowEventProc.
// Manual reference counting (the build does not enable ARC). Main thread only.

#define GL_SILENCE_DEPRECATION 1

#import <Cocoa/Cocoa.h>
#include <cstdlib>
#include <Carbon/Carbon.h>
#include <cmath>
#include "Engine/Source/Os/Mac/MacInput.h"
#include "Engine/Source/Os/Mac/MacWindow.h"

@class WowGameWindow;
@class WowGameView;

static WowGameWindow* s_window;
static WowGameView* s_view;
static id s_windowDelegate;
static id s_appDelegate;
static MacWindowEventProc s_eventProc;
static void* s_eventParam;
static bool s_windowed = true;
static bool s_mouseAssociated = true;

static const NSWindowStyleMask s_windowedStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

static NSScreen* MainScreen() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count ? screens[0] : [NSScreen mainScreen];
}

static void ContentSize(int32_t* width, int32_t* height) {
    NSSize size = s_view ? [s_view bounds].size : NSZeroSize;
    *width = static_cast<int32_t>(size.width);
    *height = static_cast<int32_t>(size.height);
}

static void NotifyWindowEvent(MacWindowEvent event) {
    int32_t width = 0;
    int32_t height = 0;
    if (event == MacWindowEvent_Resize || event == MacWindowEvent_Restore) {
        ContentSize(&width, &height);
    }

    if (s_eventProc) {
        s_eventProc(s_eventParam, event, width, height);
    }

    MacInputWindowEvent(event, width, height);
}

static void ViewPoint(NSView* view, NSPoint pointInWindow, int32_t* x, int32_t* y) {
    NSPoint point = [view convertPoint:pointInWindow fromView:nil];
    *x = static_cast<int32_t>(std::floor(point.x));
    *y = static_cast<int32_t>(std::floor(point.y));
}

@interface WowGameWindow : NSWindow
@end

@implementation WowGameWindow

- (BOOL)canBecomeKeyWindow {
    return YES;
}

- (BOOL)canBecomeMainWindow {
    return YES;
}

// The fullscreen (borderless) window covers the menu bar area.
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
    if (!([self styleMask] & NSWindowStyleMaskTitled)) {
        return frameRect;
    }

    return [super constrainFrameRect:frameRect toScreen:screen];
}

@end

@interface WowGameView : NSView <NSTextInputClient> {
    NSMutableAttributedString* m_markedText;
    NSTimer* m_pollTimer;
}
@end

@implementation WowGameView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        m_markedText = [[NSMutableAttributedString alloc] init];
        // Points, not pixels: the compositor scales the GL surface on Retina screens.
        [self setWantsBestResolutionOpenGLSurface:NO];
    }

    return self;
}

- (void)dealloc {
    [m_pollTimer invalidate];
    [m_markedText release];
    [super dealloc];
}

// Origin top-left, y down: the client coordinates the game expects.
- (BOOL)isFlipped {
    return YES;
}

- (BOOL)isOpaque {
    return YES;
}

// The game draws its own cursor (CGxDevice::ICursorDraw), so over the game view the system cursor is a transparent
// one; it comes back outside the view and when another application is active.
+ (NSCursor*)blankCursor {
    static NSCursor* s_blankCursor;

    if (!s_blankCursor) {
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        s_blankCursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
        [image release];
    }

    return s_blankCursor;
}

- (void)resetCursorRects {
    [self addCursorRect:[self bounds] cursor:[WowGameView blankCursor]];
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

// A click that activates the window also reaches the game, like Windows.
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (void)drawRect:(NSRect)dirtyRect {
    [[NSColor blackColor] setFill];
    NSRectFill(dirtyRect);
}

- (void)viewWillStartLiveResize {
    [super viewWillStartLiveResize];

    if (!m_pollTimer) {
        m_pollTimer = [NSTimer timerWithTimeInterval:0.033 target:self selector:@selector(pollTimerFired:) userInfo:nil repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:m_pollTimer forMode:NSRunLoopCommonModes];
    }
}

- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];

    [m_pollTimer invalidate];
    m_pollTimer = nil;
}

- (void)pollTimerFired:(NSTimer*)timer {
    MacInputEventPoll();
}

// Keyboard

- (void)keyDown:(NSEvent*)event {
    MacInputKey([event keyCode], true);

    // Control and Command combinations type nothing (on Windows Ctrl+letter is a control character, skipped).
    if (!([event modifierFlags] & (NSEventModifierFlagControl | NSEventModifierFlagCommand))) {
        [self interpretKeyEvents:@[ event ]];
    }
}

- (void)keyUp:(NSEvent*)event {
    MacInputKey([event keyCode], false);
}

- (void)flagsChanged:(NSEvent*)event {
    MacInputModifiers([event keyCode], [event modifierFlags]);
}

// Command shortcuts are not handled yet: everything goes to keyDown:.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    return NO;
}

// Enter, Tab, Backspace, arrows...: the game reads them as keys. No beep.
- (void)doCommandBySelector:(SEL)selector {
}

// NSTextInputClient: dead keys arrive as marked text, then the composed character through insertText:. The marked
// text is not displayed (no IME support yet).

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString* text = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    NSUInteger length = [text length];

    for (NSUInteger i = 0; i < length; i++) {
        uint32_t character = [text characterAtIndex:i];

        if (character >= 0xD800 && character <= 0xDBFF && i + 1 < length) {
            uint32_t low = [text characterAtIndex:i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                character = 0x10000 + ((character - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }

        MacInputChar(character);
    }

    [self unmarkText];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    if ([string isKindOfClass:[NSAttributedString class]]) {
        [m_markedText setAttributedString:string];
    } else {
        [m_markedText setAttributedString:[[[NSAttributedString alloc] initWithString:string] autorelease]];
    }
}

- (void)unmarkText {
    [m_markedText setAttributedString:[[[NSAttributedString alloc] init] autorelease]];
    [[self inputContext] discardMarkedText];
}

- (NSRange)selectedRange {
    return NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange {
    return [m_markedText length] ? NSMakeRange(0, [m_markedText length]) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
    return [m_markedText length] != 0;
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    NSRect rect = [[self window] convertRectToScreen:[self convertRect:[self bounds] toView:nil]];
    return NSMakeRect(NSMinX(rect), NSMinY(rect), 0, 0);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return NSNotFound;
}

// Mouse

- (void)mouseButton:(NSEvent*)event number:(int32_t)number down:(bool)down {
    int32_t x;
    int32_t y;
    ViewPoint(self, [event locationInWindow], &x, &y);
    MacInputMouseButton(number, down, x, y);
}

- (void)mouseDown:(NSEvent*)event {
    [self mouseButton:event number:0 down:true];
}

- (void)mouseUp:(NSEvent*)event {
    [self mouseButton:event number:0 down:false];
}

- (void)rightMouseDown:(NSEvent*)event {
    [self mouseButton:event number:1 down:true];
}

- (void)rightMouseUp:(NSEvent*)event {
    [self mouseButton:event number:1 down:false];
}

- (void)otherMouseDown:(NSEvent*)event {
    [self mouseButton:event number:static_cast<int32_t>([event buttonNumber]) down:true];
}

- (void)otherMouseUp:(NSEvent*)event {
    [self mouseButton:event number:static_cast<int32_t>([event buttonNumber]) down:false];
}

- (void)mouseMove:(NSEvent*)event {
    int32_t x;
    int32_t y;
    ViewPoint(self, [event locationInWindow], &x, &y);

    NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
    bool inside = NSPointInRect(point, [self bounds]);

    MacInputMouseMove(x, y, [event deltaX], [event deltaY], inside);
}

- (void)mouseMoved:(NSEvent*)event {
    [self mouseMove:event];
}

- (void)mouseDragged:(NSEvent*)event {
    [self mouseMove:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
    [self mouseMove:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
    [self mouseMove:event];
}

- (void)scrollWheel:(NSEvent*)event {
    double delta = [event hasPreciseScrollingDeltas] ? [event scrollingDeltaY] : [event deltaY];
    if (delta == 0.0) {
        return;
    }

    int32_t x;
    int32_t y;
    ViewPoint(self, [event locationInWindow], &x, &y);
    MacInputMouseWheel(delta, [event hasPreciseScrollingDeltas], x, y);
}

@end

@interface WowGameWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation WowGameWindowDelegate

// The close button asks the game (OS_INPUT_CLOSE); the window stays open until the game destroys it.
- (BOOL)windowShouldClose:(NSWindow*)sender {
    NotifyWindowEvent(MacWindowEvent_Close);
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    NotifyWindowEvent(MacWindowEvent_Resize);
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    NotifyWindowEvent(MacWindowEvent_Minimize);
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    NotifyWindowEvent(MacWindowEvent_Restore);
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    NotifyWindowEvent(MacWindowEvent_FocusGained);
}

- (void)windowDidResignKey:(NSNotification*)notification {
    NotifyWindowEvent(MacWindowEvent_FocusLost);
}

@end

@interface WowAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WowAppDelegate

// Quit from the Dock or the system: the game shuts down from OsInputGet (OS_INPUT_SHUTDOWN) instead of exit().
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    MacInputQuit();
    return NSTerminateCancel;
}

@end

// Debugging aid: WOWDECOMP_MAC_BACKGROUND=1 keeps the game window behind the other windows and never activates the
// app, for automated runs on a machine someone is using.
static bool BackgroundMode() {
    static int32_t s_background = -1;
    if (s_background < 0) {
        s_background = std::getenv("WOWDECOMP_MAC_BACKGROUND") ? 1 : 0;
    }
    return s_background;
}

static void ApplyMode(int32_t width, int32_t height, bool windowed) {
    s_windowed = windowed;

    if (windowed) {
        [s_window setStyleMask:s_windowedStyle];
        [s_window setContentSize:NSMakeSize(width, height)];
        [s_window center];
        [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    } else {
        // Borderless window over the whole main screen; the display mode never changes.
        [s_window setStyleMask:NSWindowStyleMaskBorderless];
        [s_window setFrame:[MainScreen() frame] display:YES];
        if (!BackgroundMode()) {
            [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
        }
    }

    [s_window makeFirstResponder:s_view];
}

void* MacGameWindowCreate(int32_t width, int32_t height, bool windowed, MacWindowEventProc proc, void* param) {
    s_eventProc = proc;
    s_eventParam = param;

    if (s_window) {
        ApplyMode(width, height, windowed);
        return s_view;
    }

    [NSApplication sharedApplication];

    if (![NSApp delegate]) {
        s_appDelegate = [[WowAppDelegate alloc] init];
        [NSApp setDelegate:s_appDelegate];
    }

    // Holding a key repeats it instead of opening the accent menu.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"ApplePressAndHoldEnabled" : @NO }];

    // Cursor warps (OsInputSetMousePosition) must not swallow the next quarter second of mouse events.
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    if (source) {
        CGEventSourceSetLocalEventsSuppressionInterval(source, 0.0);
        CFRelease(source);
    }

    NSRect rect = NSMakeRect(0, 0, width, height);
    s_window = [[WowGameWindow alloc] initWithContentRect:rect styleMask:s_windowedStyle backing:NSBackingStoreBuffered defer:NO];
    [s_window setReleasedWhenClosed:NO];
    [s_window setTitle:@"World of Warcraft"];
    [s_window setBackgroundColor:[NSColor blackColor]];
    [s_window setAcceptsMouseMovedEvents:YES];
    [s_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenNone];

    s_view = [[WowGameView alloc] initWithFrame:rect];
    [s_window setContentView:s_view];

    s_windowDelegate = [[WowGameWindowDelegate alloc] init];
    [s_window setDelegate:s_windowDelegate];

    ApplyMode(width, height, windowed);

    // Like the WM_SIZE of CreateWindow: the first OsInputGet reports the client size.
    int32_t contentWidth;
    int32_t contentHeight;
    ContentSize(&contentWidth, &contentHeight);
    MacInputWindowEvent(MacWindowEvent_Resize, contentWidth, contentHeight);

    return s_view;
}

void MacGameWindowDestroy() {
    if (!s_window) {
        return;
    }

    [s_window setDelegate:nil];
    [s_window orderOut:nil];
    [s_window close];
    [s_window release];
    s_window = nil;

    [s_view release];
    s_view = nil;

    [s_windowDelegate release];
    s_windowDelegate = nil;

    s_eventProc = nullptr;
    s_eventParam = nullptr;

    [NSApp setPresentationOptions:NSApplicationPresentationDefault];
}

void MacGameWindowSetMode(int32_t width, int32_t height, bool windowed) {
    if (!s_window) {
        return;
    }

    ApplyMode(width, height, windowed);
}

void MacGameWindowShow() {
    if (!s_window) {
        return;
    }

    if (BackgroundMode()) {
        [s_window orderBack:nil];
        return;
    }

    [NSApp activateIgnoringOtherApps:YES];
    [s_window makeKeyAndOrderFront:nil];
    [s_window makeFirstResponder:s_view];
}

void MacGameWindowGetSize(int32_t* width, int32_t* height) {
    int32_t contentWidth;
    int32_t contentHeight;
    ContentSize(&contentWidth, &contentHeight);

    if (width) {
        *width = contentWidth;
    }

    if (height) {
        *height = contentHeight;
    }
}

void* MacGameWindowView() {
    return s_view;
}

void MacMainScreenSize(int32_t* width, int32_t* height) {
    NSSize size = [MainScreen() frame].size;

    if (width) {
        *width = static_cast<int32_t>(size.width);
    }

    if (height) {
        *height = static_cast<int32_t>(size.height);
    }
}

int32_t MacPumpEvent() {
    @autoreleasepool {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!event) {
            return 0;
        }

        // NSApplication drops key ups while Command is held; the game would see the key stuck down.
        if ([event type] == NSEventTypeKeyUp && ([event modifierFlags] & NSEventModifierFlagCommand) && [event window] == s_window) {
            [s_window sendEvent:event];
        } else {
            [NSApp sendEvent:event];
        }

        return 1;
    }
}

bool MacGameWindowIsMinimized() {
    return s_window && [s_window isMiniaturized];
}

void MacGameWindowSetTitle(void* window, const char* title) {
    if (!s_window || !title || (window != s_view && window != s_window)) {
        return;
    }

    NSString* string = [NSString stringWithUTF8String:title];
    if (string) {
        [s_window setTitle:string];
    }
}

void* MacGuiGetWindow(int32_t type) {
    if (type == 2 && ![NSApp isActive]) {
        return nullptr;
    }

    NSWindow* window = [NSApp keyWindow];
    if (window && window == s_window) {
        return s_view;
    }

    return window;
}

void MacGameWindowGetMousePosition(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;

    if (!s_window) {
        return;
    }

    NSPoint screenPoint = [NSEvent mouseLocation];
    NSRect windowRect = [s_window convertRectFromScreen:NSMakeRect(screenPoint.x, screenPoint.y, 0, 0)];
    ViewPoint(s_view, windowRect.origin, x, y);
}

void MacGameWindowSetMousePosition(int32_t x, int32_t y) {
    if (!s_window) {
        return;
    }

    NSPoint windowPoint = [s_view convertPoint:NSMakePoint(x, y) toView:nil];
    NSRect screenRect = [s_window convertRectToScreen:NSMakeRect(windowPoint.x, windowPoint.y, 0, 0)];

    // Quartz global coordinates: origin at the top-left of the main screen, y down.
    CGPoint point = CGPointMake(screenRect.origin.x, NSMaxY([MainScreen() frame]) - screenRect.origin.y);
    CGWarpMouseCursorPosition(point);
}

void MacSetMouseAssociated(bool associated) {
    if (associated == s_mouseAssociated) {
        return;
    }

    s_mouseAssociated = associated;
    CGAssociateMouseAndMouseCursorPosition(associated);
}

uint32_t MacPressedMouseButtons() {
    return static_cast<uint32_t>([NSEvent pressedMouseButtons]);
}

int32_t MacIsModifierKeyDown(int32_t key) {
    NSEventModifierFlags flags = [NSEvent modifierFlags];

    switch (key) {
        case 0:
            return (flags & NSEventModifierFlagControl) != 0;

        case 1:
            return (flags & NSEventModifierFlagShift) != 0;

        case 2:
            return (flags & NSEventModifierFlagOption) != 0;

        default:
            return 0;
    }
}

void MacBeep() {
    NSBeep();
}

uint32_t MacKeyCodeToCharacter(uint16_t keyCode) {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef layout = source ? static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)) : nullptr;

    // Input methods without a layout of their own (Japanese, Chinese...): their ASCII layout.
    if (!layout) {
        if (source) {
            CFRelease(source);
        }

        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        layout = source ? static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)) : nullptr;
    }

    uint32_t character = 0;

    if (layout) {
        UInt32 deadKeyState = 0;
        UniChar chars[4];
        UniCharCount length = 0;

        OSStatus status = UCKeyTranslate(reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layout)), keyCode, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysMask, &deadKeyState, 4, &length, chars);
        if (status == noErr && length == 1) {
            character = chars[0];
        }
    }

    if (source) {
        CFRelease(source);
    }

    return character;
}
