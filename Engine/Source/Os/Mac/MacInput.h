#ifndef ENGINE_SOURCE_OS_MAC_MACINPUT_H
#define ENGINE_SOURCE_OS_MAC_MACINPUT_H

// What the game view (MacWindow.mm) reports to the input layer (Input.cpp); the Mac counterpart of the WM_* cases
// of OsWindowProc. Positions are content view coordinates (points, origin top-left, y down). Main thread only.

#include <cstdint>
#include "Engine/Source/Os/Mac/MacWindow.h"

void MacInputWindowEvent(MacWindowEvent event, int32_t width, int32_t height);
void MacInputQuit();
// modifierFlags: the NSEvent's, to tell Command shortcuts apart.
void MacInputKey(uint16_t keyCode, bool down, uint64_t modifierFlags);
void MacInputModifiers(uint16_t keyCode, uint64_t modifierFlags);
void MacInputChar(uint32_t character);
void MacInputMouseButton(int32_t buttonNumber, bool down, int32_t x, int32_t y);
// delta: the accelerated motion (NSEvent deltaX/Y); raw: the unaccelerated device motion, 0 when the event has none.
void MacInputMouseMove(int32_t x, int32_t y, double deltaX, double deltaY, double rawX, double rawY, bool inside);
void MacInputMouseWheel(double delta, bool precise, int32_t x, int32_t y);
void MacInputEventPoll();

#endif
