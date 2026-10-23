// The game window on macOS (MacWindow.h): an NSWindow whose content view, WowGameView, turns Cocoa events into the
// MacInput* calls of Input.cpp and window events into the Gx device's MacWindowEventProc.
// Manual reference counting (the build does not enable ARC). Main thread only.

#define GL_SILENCE_DEPRECATION 1

#import <Cocoa/Cocoa.h>
#include <cstdlib>
#include <cstring>
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
static bool s_cursorHidden;
// The windowed frame before switching to fullscreen, restored when the game goes back to a window.
static NSRect s_windowedFrame;
static bool s_haveWindowedFrame;
static int32_t s_commandAsControl = -1;
// Background mode: the cursor position the game sees (the real cursor belongs to the user).
static int32_t s_virtualMouseX;
static int32_t s_virtualMouseY;

static const NSWindowStyleMask s_windowedStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
// Miniaturizable so that Cmd+M (miniaturize:) also works without a title bar.
static const NSWindowStyleMask s_fullscreenStyle = NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable;

// Debugging aid: WOWDECOMP_MAC_BACKGROUND=1 keeps the game window behind the other windows, never activates the
// app and leaves the real cursor alone, for automated runs on a machine someone is using.
bool MacBackgroundMode() {
    static int32_t s_background = -1;
    if (s_background < 0) {
        s_background = std::getenv("WOWDECOMP_MAC_BACKGROUND") ? 1 : 0;
    }
    return s_background;
}

static bool BackgroundMode() {
    return MacBackgroundMode();
}

// The screen with the menu bar, origin of the Quartz global coordinates.
static NSScreen* MainScreen() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count ? screens[0] : [NSScreen mainScreen];
}

// The screen the game window is on; fullscreen covers this one.
static NSScreen* GameScreen() {
    NSScreen* screen = s_window ? [s_window screen] : nil;
    return screen ? screen : MainScreen();
}

// Fullscreen hides the Dock and the menu bar while the game is active and not minimized (AppKit restores them by
// itself when another application is active).
static void ApplyPresentation() {
    NSApplicationPresentationOptions options = NSApplicationPresentationDefault;

    if (s_window && !s_windowed && ![s_window isMiniaturized] && !BackgroundMode()) {
        options = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    }

    if ([NSApp presentationOptions] != options) {
        [NSApp setPresentationOptions:options];
    }
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

// Background mode: the position the game sees follows the view's events, not the real cursor. A frozen cursor
// (relative mode) does not move, so neither does the virtual one.
- (void)saveVirtualMouse:(NSEvent*)event {
    if (BackgroundMode() && s_mouseAssociated) {
        ViewPoint(self, [event locationInWindow], &s_virtualMouseX, &s_virtualMouseY);
    }
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
    // Command combinations that are not menu shortcuts reach the game only with MacCommandAsControl (Input.cpp).
    MacInputKey([event keyCode], true, [event modifierFlags]);

    // Control and Command combinations type nothing (on Windows Ctrl+letter is a control character, skipped).
    if (!([event modifierFlags] & (NSEventModifierFlagControl | NSEventModifierFlagCommand))) {
        [self interpretKeyEvents:@[ event ]];
    }
}

- (void)keyUp:(NSEvent*)event {
    MacInputKey([event keyCode], false, [event modifierFlags]);
}

- (void)flagsChanged:(NSEvent*)event {
    MacInputModifiers([event keyCode], [event modifierFlags]);
}

// The menu (Cmd+Q, Cmd+H, Cmd+M) has already had its turn; every other key goes to keyDown:.
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
    [self saveVirtualMouse:event];

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
    [self saveVirtualMouse:event];

    int32_t x;
    int32_t y;
    ViewPoint(self, [event locationInWindow], &x, &y);

    NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
    bool inside = NSPointInRect(point, [self bounds]);

    // The device motion before the system's pointer acceleration, for mouse-look.
    CGEventRef cgEvent = [event CGEvent];
    double rawX = cgEvent ? CGEventGetDoubleValueField(cgEvent, kCGEventUnacceleratedPointerMovementX) : 0.0;
    double rawY = cgEvent ? CGEventGetDoubleValueField(cgEvent, kCGEventUnacceleratedPointerMovementY) : 0.0;

    MacInputMouseMove(x, y, [event deltaX], [event deltaY], rawX, rawY, inside);
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
    ApplyPresentation();
    NotifyWindowEvent(MacWindowEvent_Minimize);
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    ApplyPresentation();
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

// Quit from the Dock or the system (logout, restart): the game shuts down from OsInputGet (OS_INPUT_SHUTDOWN, the
// WM_QUIT of GetMessage) instead of exit().
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    MacInputQuit();
    return NSTerminateCancel;
}

// Quit menu item (Cmd+Q): what Alt+F4 does on Windows (WM_SYSCOMMAND SC_CLOSE, OS_INPUT_CLOSE), so the game can ask
// for confirmation and log out cleanly.
- (void)quitGame:(id)sender {
    if (s_window) {
        NotifyWindowEvent(MacWindowEvent_Close);
    } else {
        MacInputQuit();
    }
}

// Minimize menu item (Cmd+M); performMiniaturize: would beep for the borderless fullscreen window.
- (void)minimizeGame:(id)sender {
    if (s_window && ![s_window isMiniaturized]) {
        [s_window miniaturize:sender];
    }
}

// A display was added, removed or changed resolution (WM_DISPLAYCHANGE): fullscreen follows its screen.
- (void)applicationDidChangeScreenParameters:(NSNotification*)notification {
    if (s_window && !s_windowed) {
        NSRect frame = [GameScreen() frame];
        if (!NSEqualRects(frame, [s_window frame])) {
            [s_window setFrame:frame display:YES];
        }
    }
}

@end

// Application menu (Hide, Hide Others, Show All, Quit) and Window menu (Minimize), so the standard shortcuts work.
static void InstallMenu() {
    if ([NSApp mainMenu]) {
        return;
    }

    NSString* name = [[NSProcessInfo processInfo] processName];
    NSMenu* menuBar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:name action:nil keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quit = [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(quitGame:) keyEquivalent:@"q"];
    [quit setTarget:s_appDelegate];
    [appItem setSubmenu:appMenu];
    [menuBar addItem:appItem];

    NSMenuItem* windowItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    NSMenuItem* minimize = [windowMenu addItemWithTitle:@"Minimize" action:@selector(minimizeGame:) keyEquivalent:@"m"];
    [minimize setTarget:s_appDelegate];
    [windowItem setSubmenu:windowMenu];
    [menuBar addItem:windowItem];

    [NSApp setMainMenu:menuBar];

    [appMenu release];
    [appItem release];
    [windowMenu release];
    [windowItem release];
    [menuBar release];
}

// windowed: a titled window with the given content size; the first time centered, when coming back from fullscreen
// where it was, otherwise keeping its top-left corner. Fullscreen: a borderless window over the whole screen the
// window is on; the display mode never changes.
static void ApplyMode(int32_t width, int32_t height, bool windowed) {
    bool wasWindowed = s_windowed;
    bool visible = [s_window isVisible];
    NSScreen* screen = GameScreen();

    s_windowed = windowed;

    if (windowed) {
        [s_window setStyleMask:s_windowedStyle];

        NSRect frame = [s_window frameRectForContentRect:NSMakeRect(0, 0, width, height)];
        NSRect previous = !wasWindowed && s_haveWindowedFrame ? s_windowedFrame : [s_window frame];

        if (visible || s_haveWindowedFrame) {
            frame.origin.x = previous.origin.x;
            frame.origin.y = NSMaxY(previous) - frame.size.height;
            frame = [s_window constrainFrameRect:frame toScreen:screen];
        } else {
            NSRect area = [screen visibleFrame];
            frame.origin.x = std::floor(NSMidX(area) - frame.size.width / 2);
            frame.origin.y = std::floor(NSMidY(area) - frame.size.height / 2);
        }

        [s_window setFrame:frame display:YES];
    } else {
        if (wasWindowed && visible) {
            s_windowedFrame = [s_window frame];
            s_haveWindowedFrame = true;
        }

        [s_window setStyleMask:s_fullscreenStyle];
        [s_window setFrame:[screen frame] display:YES];
    }

    ApplyPresentation();
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

    InstallMenu();

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
    s_windowed = true;
    s_haveWindowedFrame = false;

    MacSetMouseAssociated(true);
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

static void ScreenSize(NSScreen* screen, int32_t* width, int32_t* height) {
    NSSize size = [screen frame].size;

    if (width) {
        *width = static_cast<int32_t>(size.width);
    }

    if (height) {
        *height = static_cast<int32_t>(size.height);
    }
}

void MacMainScreenSize(int32_t* width, int32_t* height) {
    ScreenSize(MainScreen(), width, height);
}

void MacGameScreenSize(int32_t* width, int32_t* height) {
    ScreenSize(GameScreen(), width, height);
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

    if (BackgroundMode()) {
        *x = s_virtualMouseX;
        *y = s_virtualMouseY;
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

    if (BackgroundMode()) {
        s_virtualMouseX = x;
        s_virtualMouseY = y;
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

    if (BackgroundMode()) {
        return;
    }

    CGAssociateMouseAndMouseCursorPosition(associated);

    // The view's blank cursor already hides it over the game; hiding it too covers a cursor frozen on the edge.
    if (!associated && !s_cursorHidden) {
        [NSCursor hide];
        s_cursorHidden = true;
    } else if (associated && s_cursorHidden) {
        [NSCursor unhide];
        s_cursorHidden = false;
    }
}

bool MacIsMouseAssociated() {
    return s_mouseAssociated;
}

uint32_t MacPressedMouseButtons() {
    return static_cast<uint32_t>([NSEvent pressedMouseButtons]);
}

int32_t MacIsModifierKeyDown(int32_t key) {
    NSEventModifierFlags flags = [NSEvent modifierFlags];

    switch (key) {
        case 0:
            if (MacCommandAsControl() && (flags & NSEventModifierFlagCommand)) {
                return 1;
            }

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

bool MacCommandAsControl() {
    if (s_commandAsControl < 0) {
        const char* value = std::getenv("WOWDECOMP_MAC_CMD_AS_CTRL");
        s_commandAsControl = value && *value && *value != '0' ? 1 : 0;
    }

    return s_commandAsControl;
}

void MacSetCommandAsControl(bool enable) {
    s_commandAsControl = enable ? 1 : 0;
}

static NSString* StringFromUTF8(const char* string) {
    if (!string) {
        return @"";
    }

    NSString* result = [NSString stringWithUTF8String:string];

    // Not valid UTF-8: the bytes as Windows-1252, the client's ANSI code page (GetACP).
    if (!result) {
        result = [[[NSString alloc] initWithBytes:string length:std::strlen(string) encoding:NSWindowsCP1252StringEncoding] autorelease];
    }

    return result ? result : @"";
}

int32_t MacMessageBox(int32_t style, const char* message, const char* title) {
    // Automated runs must not put a modal window in front of the user; the caller gets the default button.
    if (BackgroundMode()) {
        return -1;
    }

    @autoreleasepool {
        // A frozen, hidden cursor could not click the buttons. The game window loses the key status to the alert,
        // so Input.cpp sees the focus change and captures the mouse again afterwards.
        MacSetMouseAssociated(true);

        NSString* titleText = StringFromUTF8(title);
        NSString* messageText = StringFromUTF8(message);

        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        [alert setAlertStyle:NSAlertStyleWarning];

        // MessageBox shows the title in the caption and the message in the body; an alert has no caption.
        if ([titleText length]) {
            [alert setMessageText:titleText];
            [alert setInformativeText:messageText];
        } else {
            [alert setMessageText:messageText];
        }

        switch (style) {
            case 1:
                [alert addButtonWithTitle:@"OK"];
                [alert addButtonWithTitle:@"Cancel"];
                break;

            case 2:
                [alert addButtonWithTitle:@"Yes"];
                [alert addButtonWithTitle:@"No"];
                break;

            case 3:
                [alert addButtonWithTitle:@"Yes"];
                [alert addButtonWithTitle:@"No"];
                [alert addButtonWithTitle:@"Cancel"];
                break;

            default:
                [alert addButtonWithTitle:@"OK"];
                break;
        }

        // Escape: Cancel when there is one; No for a Yes/No box.
        if (style == 2) {
            [[[alert buttons] objectAtIndex:1] setKeyEquivalent:@"\033"];
        }

        [NSApp activateIgnoringOtherApps:YES];
        [[NSCursor arrowCursor] set];

        NSModalResponse response = [alert runModal];

        if (response == NSAlertSecondButtonReturn) {
            return 1;
        }

        if (response == NSAlertThirdButtonReturn) {
            return 2;
        }

        return 0;
    }
}

char* MacClipboardGetString(void* (*alloc)(uint32_t size)) {
    @autoreleasepool {
        NSString* string = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char* utf8 = string ? [string UTF8String] : nullptr;

        if (!utf8) {
            return nullptr;
        }

        uint32_t size = static_cast<uint32_t>(std::strlen(utf8) + 1);
        char* result = static_cast<char*>(alloc(size));

        if (result) {
            std::memcpy(result, utf8, size);
        }

        return result;
    }
}

int32_t MacClipboardPutString(const char* string) {
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        return [pasteboard setString:StringFromUTF8(string) forType:NSPasteboardTypeString] ? 1 : 0;
    }
}

bool MacOpenURL(const char* url) {
    if (!url || !*url) {
        return false;
    }

    @autoreleasepool {
        NSString* string = StringFromUTF8(url);
        NSURL* target = nil;

        // Anything with a scheme ("http:", "file:", "mailto:"...) is a URL; the rest a file path, as with
        // /usr/bin/open and ShellExecute.
        NSRange colon = [string rangeOfString:@":"];
        if (colon.location != NSNotFound && colon.location > 1) {
            target = [NSURL URLWithString:string];

            // OsLaunchURL has decoded the %XX escapes, so spaces and the like must be escaped again.
            if (!target) {
                NSMutableCharacterSet* allowed = [[[NSCharacterSet URLQueryAllowedCharacterSet] mutableCopy] autorelease];
                [allowed addCharactersInString:@"#%"];
                target = [NSURL URLWithString:[string stringByAddingPercentEncodingWithAllowedCharacters:allowed]];
            }
        }

        if (!target) {
            target = [NSURL fileURLWithPath:string];
        }

        return target && [[NSWorkspace sharedWorkspace] openURL:target];
    }
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
