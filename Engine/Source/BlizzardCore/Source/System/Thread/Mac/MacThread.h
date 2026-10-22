#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_THREAD_MAC_MACTHREAD_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_THREAD_MAC_MACTHREAD_H

// What ThreadRecord::m_handle points to on macOS. A Win32 thread handle is a waitable object that stays valid
// until it is closed, whether or not the thread is still running; this mirrors that with a manual-reset "exited"
// event and a reference held by the handle owner and one by the running thread.

#include <pthread.h>
#include <cstdint>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"

namespace Blizzard { namespace Thread { struct ThreadRecord; }}

struct MacThreadHandle {
    pthread_t m_thread;
    HANDLE m_exited;
    volatile int32_t m_refCount;
    Blizzard::Thread::ThreadRecord* m_record;
};

inline MacThreadHandle* MacThreadHandleCreate(Blizzard::Thread::ThreadRecord* record) {
    MacThreadHandle* handle = new MacThreadHandle;
    handle->m_thread = nullptr;
    handle->m_exited = MacEventCreate(true, false);
    handle->m_refCount = 2;
    handle->m_record = record;
    return handle;
}

inline void MacThreadHandleRelease(HANDLE handle) {
    MacThreadHandle* thread = static_cast<MacThreadHandle*>(handle);

    if (__atomic_sub_fetch(&thread->m_refCount, 1, __ATOMIC_ACQ_REL) == 0) {
        MacEventDestroy(thread->m_exited);
        delete thread;
    }
}

inline bool MacThreadHandleWait(HANDLE handle, uint32_t timeout) {
    return MacEventWait(static_cast<MacThreadHandle*>(handle)->m_exited, timeout);
}

inline pthread_t MacThreadHandleThread(HANDLE handle) {
    return static_cast<MacThreadHandle*>(handle)->m_thread;
}

#endif
