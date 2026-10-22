#ifndef ENGINE_SOURCE_OS_MAC_MACWINDOW_H
#define ENGINE_SOURCE_OS_MAC_MACWINDOW_H

// The game window on macOS. Os/Mac owns the NSWindow, its content view and the event loop (OsInputGet turns
// keyboard and mouse events into OsQueuePut calls). The Mac Gx device asks for the window here and attaches its
// NSOpenGLContext to the returned view; window events that Windows delivers to CGxDeviceOpenGl::WindowProcGl
// come back through the MacWindowEventProc callback. Sizes are in points (the view does not ask for a Retina
// backing store, so the compositor scales).
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
// (an NSView*). windowed=false makes it a borderless window covering the main screen.
void* MacGameWindowCreate(int32_t width, int32_t height, bool windowed, MacWindowEventProc proc, void* param);
void MacGameWindowDestroy();
void MacGameWindowSetMode(int32_t width, int32_t height, bool windowed);
void MacGameWindowShow();
void MacGameWindowGetSize(int32_t* width, int32_t* height);
void* MacGameWindowView();

// Main screen size in points, for the adapter's display mode.
void MacMainScreenSize(int32_t* width, int32_t* height);

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
// false freezes the cursor; mouse events still carry deltas (relative mouse mode).
void MacSetMouseAssociated(bool associated);
// [NSEvent pressedMouseButtons]: bit 0 left, 1 right, 2 middle, 3 and 4 the extra buttons.
uint32_t MacPressedMouseButtons();
// OsGuiIsModifierKeyDown keys: 0 control, 1 shift, 2 option (alt).
int32_t MacIsModifierKeyDown(int32_t key);
void MacBeep();
// Unshifted character of a virtual key (kVK_*) in the current keyboard layout, 0 if none.
uint32_t MacKeyCodeToCharacter(uint16_t keyCode);

#endif
