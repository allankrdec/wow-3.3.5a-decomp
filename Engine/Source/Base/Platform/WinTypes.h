#ifndef ENGINE_SOURCE_BASE_PLATFORM_WINTYPES_H
#define ENGINE_SOURCE_BASE_PLATFORM_WINTYPES_H

// The Win32 types (and the few Win32 calls) that common code keeps in its declarations: the OS-layer headers
// (Lock, Thread, File, Time, SCritSect...) and headers that used to include <windows.h>. Windows gets <windows.h>
// exactly as before. Other platforms get same-named types whose content belongs to their Mac/ implementation:
// this is not a Win32 API emulation, nothing here has behavior.

#if defined(_WIN32)

#include <windows.h>

#else

#include <pthread.h>
#include <stdint.h>

typedef void* HANDLE;
typedef void* HWND;
typedef uint32_t DWORD;

// Recursive mutex; created and destroyed by the platform Lock/SCritSect code.
struct CRITICAL_SECTION {
    pthread_mutex_t m_mutex;
};

// Asynchronous I/O request; only the platform File code reads or writes its fields.
struct OVERLAPPED {
    uint64_t m_offset;
    void* m_event;
    void* m_platform;
};

struct tagRECT {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        int32_t HighPart;
    };
    int64_t QuadPart;
};

// Win32 functions that the common code still calls directly; each platform's Mac/ code implements them.
uint32_t GetACP();

#endif

#endif
