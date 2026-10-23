#ifndef ENGINE_SOURCE_OS_MAC_MACWINDOW_H
#define ENGINE_SOURCE_OS_MAC_MACWINDOW_H

// The game window on macOS. Os/Mac owns the NSWindow, its content view and the event loop (OsInputGet turns
// keyboard and mouse events into OsQueuePut calls). The Mac Gx device asks for the window here and attaches its
// NSOpenGLContext to the returned view; window events that Windows delivers to CGxDeviceOpenGl::WindowProcGl
// come back through the MacWindowEventProc callback.
// Retina: everything is in points. The view does not ask for a Retina backing store
// (wantsBestResolutionOpenGLSurface NO), so the GL drawable is as large as the view's bounds in points and the
// compositor scales it up; mouse positions are view points too, so they match drawable pixels one to one.
// All functions must be called on the main thread.

#include <cstdint>

enum MacWindowEvent {
    MacWindowEvent_Resize = 0,      // width/height: new content size
    MacWindowEvent_Minimize = 1,
    MacWindowEvent_Restore = 2,     // width/height: content size
    MacWindowEvent_FocusGained = 3,
    MacWindowEvent_FocusLost = 4,
    MacWindowEvent_Close = 5,
};

typedef void (*MacWindowEventProc)(void* param, MacWindowEvent event, int32_t width, int32_t height);

// Creates the window (or reuses it if it already exists) with the given content size and returns its content view
// (an NSView*). windowed=false makes it a borderless window covering the screen the window is on (the main screen
// the first time). Calling it again, or MacGameWindowSetMode, switches between windowed and fullscreen at runtime.
void* MacGameWindowCreate(int32_t width, int32_t height, bool windowed, MacWindowEventProc proc, void* param);
void MacGameWindowDestroy();
void MacGameWindowSetMode(int32_t width, int32_t height, bool windowed);
void MacGameWindowShow();
void MacGameWindowGetSize(int32_t* width, int32_t* height);
void* MacGameWindowView();

// Main screen size in points, for the adapter's display mode.
void MacMainScreenSize(int32_t* width, int32_t* height);
// Size in points of the screen the game window is on (the main screen without a window): the size a fullscreen
// window gets.
void MacGameScreenSize(int32_t* width, int32_t* height);

// Os/Mac internals, used by Input.cpp and Gui.cpp.

// Dispatches one pending NSEvent ([NSApp sendEvent:]) without blocking. Returns 0 when the queue is empty.
int32_t MacPumpEvent();
bool MacGameWindowIsMinimized();
void MacGameWindowSetTitle(void* window, const char* title);
// OsGuiGetWindow types 1 (active) and 2 (foreground): the game view when the game window is key.
void* MacGuiGetWindow(int32_t type);
// Cursor position in content view coordinates (points, origin top-left, y down).
void MacGameWindowGetMousePosition(int32_t* x, int32_t* y);
void MacGameWindowSetMousePosition(int32_t x, int32_t y);
// false freezes and hides the cursor; mouse events still carry deltas (relative mouse mode).
void MacSetMouseAssociated(bool associated);
bool MacIsMouseAssociated();
// [NSEvent pressedMouseButtons]: bit 0 left, 1 right, 2 middle, 3 and 4 the extra buttons.
uint32_t MacPressedMouseButtons();
// OsGuiIsModifierKeyDown keys: 0 control (or Command with MacCommandAsControl), 1 shift, 2 option (alt).
int32_t MacIsModifierKeyDown(int32_t key);
// Command as Control: off by default (Command only drives the AppKit shortcuts Cmd+Q, Cmd+H, Cmd+M); on with
// WOWDECOMP_MAC_CMD_AS_CTRL=1, the Command keys reach the game as the Control keys of the same side.
bool MacCommandAsControl();
void MacSetCommandAsControl(bool enable);
// WOWDECOMP_MAC_BACKGROUND=1 (automated runs): the window stays behind, the app never activates, and the real
// cursor is never moved or frozen (the game's cursor position is the last one the view saw).
bool MacBackgroundMode();
// Modal NSAlert for OsGuiMessageBox. style: 0 OK, 1 OK/Cancel, 2 Yes/No, 3 Yes/No/Cancel. Returns the button
// pressed in order (0 first, 1 second, 2 third), or -1 when no alert was shown (background mode).
int32_t MacMessageBox(int32_t style, const char* message, const char* title);
// General pasteboard as UTF-8. Get returns a string allocated with alloc, or nullptr when there is no text.
char* MacClipboardGetString(void* (*alloc)(uint32_t size));
int32_t MacClipboardPutString(const char* string);
// Opens a URL (or a file path) with LaunchServices (NSWorkspace). Returns false if nothing was opened.
bool MacOpenURL(const char* url);
void MacBeep();
// Unshifted character of a virtual key (kVK_*) in the current keyboard layout, 0 if none.
uint32_t MacKeyCodeToCharacter(uint16_t keyCode);

#endif
