// macOS counterpart of Thread/Win64/Thread.cpp: Blizzard threads over pthreads.

#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Mac/MacThread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"

// Win32 thread priorities; GetPriorityLevel keeps the Windows switch on these values.
static const int32_t THREAD_PRIORITY_IDLE = -15;
static const int32_t THREAD_PRIORITY_LOWEST = -2;
static const int32_t THREAD_PRIORITY_BELOW_NORMAL = -1;
static const int32_t THREAD_PRIORITY_NORMAL = 0;
static const int32_t THREAD_PRIORITY_ABOVE_NORMAL = 1;
static const int32_t THREAD_PRIORITY_HIGHEST = 2;
static const int32_t THREAD_PRIORITY_TIME_CRITICAL = 15;

// The default reserve of a Windows executable; macOS gives secondary threads 512 KB.
static const int32_t DEFAULT_STACK_SIZE = 0x100000;

// Runs the TLS destructors of threads Blizzard::Thread did not create when they exit (the Windows code starts a
// watcher thread per such thread instead).
static pthread_key_t s_externalExitKey;
static bool s_externalExitKeyCreated;

bool System_Thread::s_initialized;

Blizzard::Lock::Mutex System_Thread::s_mutex;

Blizzard::Lock::Mutex System_Thread::s_registryMutex;

Blizzard::Thread::TLSSlot System_Thread::s_threadRecordTLS;

Blizzard::Thread::TLSSlot System_Thread::s_stackTraceEntryPointTLS;

Blizzard::Thread::ThreadRecord* System_Thread::s_mainThread;

std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>* System_Thread::s_ThreadRegistry;

int32_t System_Thread::s_DefaultStackSize;

System_Thread::ExternalTLSRecord System_Thread::s_externalTLS[32];

int32_t System_Thread::s_externalTLSCount;

static uint32_t CurrentThreadId() {
    return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
}

static int32_t GetSchedPriority(int32_t priority) {
    int32_t low = sched_get_priority_min(SCHED_OTHER);
    int32_t high = sched_get_priority_max(SCHED_OTHER);
    int32_t normal = (low + high) / 2;

    switch (priority) {
        case THREAD_PRIORITY_IDLE:
            return low;
        case THREAD_PRIORITY_LOWEST:
            return normal - 6;
        case THREAD_PRIORITY_BELOW_NORMAL:
            return normal - 3;
        case THREAD_PRIORITY_ABOVE_NORMAL:
            return normal + 3;
        case THREAD_PRIORITY_HIGHEST:
            return normal + 6;
        case THREAD_PRIORITY_TIME_CRITICAL:
            return high;
        default:
            return normal;
    }
}

static bool SetThreadPriority(HANDLE handle, int32_t priority) {
    if (!handle) {
        return false;
    }

    sched_param param;
    param.sched_priority = GetSchedPriority(priority);

    return pthread_setschedparam(MacThreadHandleThread(handle), SCHED_OTHER, &param) == 0;
}

static int32_t GetThreadPriority(HANDLE handle) {
    int32_t policy;
    sched_param param;

    if (!handle || pthread_getschedparam(MacThreadHandleThread(handle), &policy, &param)) {
        return THREAD_PRIORITY_NORMAL;
    }

    static const int32_t s_priorities[] = { THREAD_PRIORITY_IDLE, THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST, THREAD_PRIORITY_TIME_CRITICAL };
    int32_t best = THREAD_PRIORITY_NORMAL;
    int32_t bestDistance = 0x7FFFFFFF;

    for (int32_t priority : s_priorities) {
        int32_t distance = abs(GetSchedPriority(priority) - param.sched_priority);

        if (distance < bestDistance) {
            best = priority;
            bestDistance = distance;
        }
    }

    return best;
}

static void ExternalThreadExit(void* value) {
    System_Thread::RunExternalTLSDestructors(reinterpret_cast<HANDLE>(pthread_self()));
}

static void* ThreadEntry(void* param) {
    MacThreadHandle* handle = static_cast<MacThreadHandle*>(param);
    System_Thread::ThreadStarter(handle->m_record);
    MacEventSet(handle->m_exited);
    MacThreadHandleRelease(handle);

    return nullptr;
}

// Win64: 0x004284E0
bool Blizzard::Thread::Sleep(uint32_t milliseconds) {
    MacSleep(milliseconds);
    return true;
}

// Win64: 0x00428CA0
bool Blizzard::Thread::IsRunning(ThreadRecord* thread) {
    return thread->m_state >= 0;
}

// Win64: 0x00428CB0
bool System_Thread::SetPriorityLevel(Blizzard::Thread::ThreadRecord* thread, int32_t level) {
    switch (level) {
        case 0:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_IDLE);
        case 1:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_LOWEST);
        case 2:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_BELOW_NORMAL);
        case 3:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_NORMAL);
        case 4:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_ABOVE_NORMAL);
        case 5:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_HIGHEST);
        case 6:
            return SetThreadPriority(thread->m_handle, THREAD_PRIORITY_TIME_CRITICAL);
        default:
            return false;
    }
}

// Win64: 0x00428D80
bool System_Thread::GetPriorityLevel(Blizzard::Thread::ThreadRecord* thread, int32_t* level) {
    // Same fall-through as the Windows code: any known priority ends up as 6.
    switch (GetThreadPriority(thread->m_handle)) {
        case THREAD_PRIORITY_IDLE:
            *level = 0;
        case THREAD_PRIORITY_LOWEST:
            *level = 1;
        case THREAD_PRIORITY_BELOW_NORMAL:
            *level = 2;
        case THREAD_PRIORITY_NORMAL:
            *level = 3;
        case THREAD_PRIORITY_ABOVE_NORMAL:
            *level = 4;
        case THREAD_PRIORITY_HIGHEST:
            *level = 5;
        case THREAD_PRIORITY_TIME_CRITICAL:
            *level = 6;
    }

    return false;
}

// Win64: 0x00428E20
void System_Thread::ShutdownThreadLocalStorage() {
    if (!s_initialized) {
        return;
    }

    // Threads that are still alive: run their destructors now, as the Windows watchers do when signaled.
    Blizzard::Lock::MutexEnter(s_mutex);

    while (s_externalTLSCount > 0) {
        HANDLE thread = s_externalTLS[0].m_thread;
        Blizzard::Lock::MutexLeave(s_mutex);
        RunExternalTLSDestructors(thread);
        Blizzard::Lock::MutexEnter(s_mutex);
    }

    Blizzard::Lock::MutexLeave(s_mutex);
}

// Win64: 0x0042E7A0
void System_Thread::RunExternalTLSDestructors(HANDLE thread) {
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    for (int32_t i = s_externalTLSCount; i != 0;) {
        i--;
        ExternalTLSRecord& record = s_externalTLS[i];

        if (record.m_thread == thread) {
            if (record.m_destructor) {
                record.m_destructor(record.m_value);
            }

            s_externalTLSCount--;
            record = s_externalTLS[s_externalTLSCount];
        }
    }
}

// Win64: 0x00448260
void System_Thread::AddToRegistry(Blizzard::Thread::ThreadRecord* thread) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);

    if (!s_ThreadRegistry) {
        s_ThreadRegistry = new std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>();
    }

    s_ThreadRegistry->insert(thread);
}

// Win64: 0x00449D70
Blizzard::Thread::ThreadRecord* System_Thread::NewThread(uint32_t (*function)(void*), void* param, const char* name) {
    if (!name) {
        name = "";
    }

    uint32_t length = static_cast<uint32_t>(strlen(name));
    Blizzard::Thread::ThreadRecord* thread = static_cast<Blizzard::Thread::ThreadRecord*>(malloc(sizeof(Blizzard::Thread::ThreadRecord) + length + 1));
    memset(thread, 0, sizeof(Blizzard::Thread::ThreadRecord));
    thread->m_param = param;
    thread->m_function = function;
    thread->m_state = 0;
    thread->m_refCount.m_value = 2;
    Blizzard::String::Copy(thread->m_name, name, length + 1);
    AddToRegistry(thread);

    return thread;
}

// Win64: 0x00449DF0
void System_Thread::RemoveFromRegistry(Blizzard::Thread::ThreadRecord* thread) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);
    s_ThreadRegistry->erase(thread);

    if (s_ThreadRegistry->empty()) {
        delete s_ThreadRegistry;
        s_ThreadRegistry = nullptr;
    }
}

// Win64: 0x0044B6E0
void System_Thread::InitMainThread() {
    s_mainThread = NewThread(nullptr, nullptr, nullptr);
    InternalSetLocalStorage(&s_threadRecordTLS, s_mainThread);
    s_mainThread->m_state = 1;
    s_mainThread->m_refCount.m_value = 1;
}

// Win64: 0x0044CCE0
void System_Thread::InitThreadSystem() {
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    Blizzard::Lock::MutexCreate(s_mutex);
    Blizzard::Lock::MutexCreate(s_registryMutex);
    AllocateLocalStorage(&s_threadRecordTLS, nullptr);
    AllocateLocalStorage(&s_stackTraceEntryPointTLS, nullptr);
    InitMainThread();
}

// Win64: 0x0044CD30
void System_Thread::SetStackTraceEntryPoint(void* entryPoint) {
    InitThreadSystem();
    InternalSetLocalStorage(&s_stackTraceEntryPointTLS, entryPoint);
}

// Win64: 0x0044CD90
Blizzard::Thread::ThreadRecord* System_Thread::GetThreadRecord() {
    InitThreadSystem();

    return static_cast<Blizzard::Thread::ThreadRecord*>(pthread_getspecific(static_cast<pthread_key_t>(~s_threadRecordTLS.m_key)));
}

// Win64: 0x0044CDF0
bool System_Thread::AllocateTLSSlot(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    InitThreadSystem();
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    return slot->m_key || AllocateLocalStorage(slot, destructor);
}

// Win64: 0x0044CEC0
void System_Thread::InitStackTraceEntryPoint() {
    void* frames[3];

    if (backtrace(frames, 3) == 3) {
        SetStackTraceEntryPoint(frames[2]);
    }
}

// Win64: 0x0044CF50
uint32_t System_Thread::ExternalTLSWatcher(void* param) {
    // No watcher threads on macOS: the s_externalExitKey destructor runs when the external thread exits.
    ExternalTLSRecord* record = static_cast<ExternalTLSRecord*>(param);
    RunExternalTLSDestructors(record->m_thread);

    return 0;
}

// Win64: 0x0044DEE0
uint32_t __stdcall System_Thread::ThreadStarter(void* param) {
    Blizzard::Thread::ThreadRecord* thread = static_cast<Blizzard::Thread::ThreadRecord*>(param);

    InternalSetLocalStorage(&s_threadRecordTLS, thread);
    InitStackTraceEntryPoint();

    if (thread->m_name[0]) {
        pthread_setname_np(thread->m_name);
    }

    while (!__atomic_load_n(&thread->m_state, __ATOMIC_ACQUIRE)) {
        MacSleep(1);
    }

    uint32_t result = thread->m_function(thread->m_param);
    thread->m_result = result;
    RunTLSDestructors();
    thread->m_state = -1;
    Blizzard::Thread::Close(thread);

    return result;
}

// Win64: 0x00450AC0
bool Blizzard::Thread::Create(uint32_t (*function)(void*), void* param, ThreadRecord** thread, const char* name, int32_t flags, int32_t stackSize) {
    System_Thread::InitThreadSystem();
    ThreadRecord* record = System_Thread::NewThread(function, param, name);
    *thread = record;

    if (!stackSize) {
        stackSize = System_Thread::s_DefaultStackSize;
    }

    if (stackSize <= 0) {
        stackSize = DEFAULT_STACK_SIZE;
    }

    size_t pageSize = static_cast<size_t>(getpagesize());
    size_t size = (static_cast<size_t>(stackSize) + pageSize - 1) & ~(pageSize - 1);

    if (size < PTHREAD_STACK_MIN) {
        size = PTHREAD_STACK_MIN;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, size);

    // A suspended thread (flags & 1) waits in ThreadStarter until m_state is set, as on Windows.
    MacThreadHandle* handle = MacThreadHandleCreate(record);

    if (pthread_create(&handle->m_thread, &attr, ThreadEntry, handle) != 0) {
        MacEventDestroy(handle->m_exited);
        delete handle;
        handle = nullptr;
    } else {
        record->m_threadId = static_cast<uint32_t>(pthread_mach_thread_np(handle->m_thread));
    }

    pthread_attr_destroy(&attr);

    record->m_handle = handle;
    __atomic_store_n(&(*thread)->m_state, (flags & 1) == 0, __ATOMIC_RELEASE);

    return true;
}

// Win64: 0x00450BF0
void System_Thread::RegisterExternalTLSDestructor(void* value, Blizzard::Thread::TLSDestructor destructor) {
    uint32_t threadId = CurrentThreadId();
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    if (s_externalTLSCount >= 32) {
        return;
    }

    int32_t i;

    for (i = 0; i < s_externalTLSCount; i++) {
        if (s_externalTLS[i].m_threadId == threadId) {
            break;
        }
    }

    if (i < s_externalTLSCount) {
        ExternalTLSRecord& record = s_externalTLS[s_externalTLSCount++];
        record.m_thread = s_externalTLS[i].m_thread;
        record.m_watcher = s_externalTLS[i].m_watcher;
        record.m_event = s_externalTLS[i].m_event;
        record.m_threadId = threadId;
        record.m_value = value;
        record.m_destructor = destructor;
    } else {
        if (!s_externalExitKeyCreated) {
            if (pthread_key_create(&s_externalExitKey, ExternalThreadExit)) {
                return;
            }

            s_externalExitKeyCreated = true;
        }

        ExternalTLSRecord& record = s_externalTLS[s_externalTLSCount];
        record.m_event = nullptr;
        record.m_value = value;
        record.m_thread = reinterpret_cast<HANDLE>(pthread_self());
        record.m_watcher = nullptr;
        record.m_threadId = threadId;
        record.m_destructor = destructor;
        s_externalTLSCount++;

        pthread_setspecific(s_externalExitKey, reinterpret_cast<void*>(1));
    }
}

// Win64: 0x00451C40
void System_Thread::InternalSetLocalStorage(const Blizzard::Thread::TLSSlot* slot, const void* value) {
    pthread_setspecific(static_cast<pthread_key_t>(~slot->m_key), value);

    if (slot->m_destructor && value) {
        Blizzard::Thread::ThreadRecord* thread = GetThreadRecord();

        if (thread && thread->m_worker) {
            thread = thread->m_worker->m_task;
        }

        if (!thread) {
            RegisterExternalTLSDestructor(const_cast<void*>(value), slot->m_destructor);
        }
    }
}
