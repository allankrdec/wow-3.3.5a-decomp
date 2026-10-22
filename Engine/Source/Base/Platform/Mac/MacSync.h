#ifndef ENGINE_SOURCE_BASE_PLATFORM_MAC_MACSYNC_H
#define ENGINE_SOURCE_BASE_PLATFORM_MAC_MACSYNC_H

// Synchronization primitives shared by the macOS platform layers (Blizzard::Lock, Storm SCritSect/SLock/SThread,
// Os). They follow the Win32 semantics the Windows code relies on: critical sections are recursive, events are
// auto- or manual-reset with an initial state, and waits take a timeout in milliseconds (MAC_WAIT_INFINITE for
// none).

#include <cstdint>
#include "Engine/Source/Base/Platform/WinTypes.h"

constexpr uint32_t MAC_WAIT_INFINITE = 0xFFFFFFFF;

void MacCriticalSectionInit(CRITICAL_SECTION* section);
void MacCriticalSectionDelete(CRITICAL_SECTION* section);
void MacCriticalSectionEnter(CRITICAL_SECTION* section);
void MacCriticalSectionLeave(CRITICAL_SECTION* section);
bool MacCriticalSectionTryEnter(CRITICAL_SECTION* section);

// An event is a HANDLE (MacEvent*). MacEventWait returns true when the event was signaled before the timeout.
HANDLE MacEventCreate(bool manualReset, bool initialState);
void MacEventDestroy(HANDLE event);
void MacEventSet(HANDLE event);
void MacEventReset(HANDLE event);
bool MacEventWait(HANDLE event, uint32_t timeout);

void MacSleep(uint32_t milliseconds);

// Monotonic clock.
uint64_t MacTimeNs();
uint32_t MacTickCount();

#endif
