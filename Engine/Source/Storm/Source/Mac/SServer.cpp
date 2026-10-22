// macOS counterpart of Storm/Source/Win64/SServer.cpp. The Windows version looks up TryEnterCriticalSection in
// kernel32.dll; pthread_mutex_trylock is always available, so there is nothing to load.

#include "Engine/Source/Storm/H/storm.h"

// Win64: 0x0077E9A0
void SServerInitialize() {
}

// Win64: 0x0077E9D0
void SServerDestroy() {
}
