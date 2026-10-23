// macOS counterpart of Storm/Source/Win64/SThread.cpp.

#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach/mach.h>
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/Source/SThread.h"
#include "Engine/Source/Storm/H/storm.h"

extern char** environ;

struct PROCESSCOMPLETION {
    /* 0000 */ void (*callback)(void*);
    /* 0004 */ void* data;
    /* 0008 */ pid_t process;
};

struct THREADLAUNCH {
    /* 0000 */ uint32_t (STORMAPI* threadProc)(void*);
    /* 0004 */ void* param;
    /* 0008 */ HANDLE threadH;
};

namespace S_Thread {
    SThreadTrack s_threads[s_maxthreads];

    int32_t s_numthreads;

    CInitCritSect s_threadCrit;
}

// Win64: 0x0076FDB0
uint32_t STORMAPI SGetCurrentThreadId() {
    return MacThreadId(pthread_self());
}

// Win64: 0x0076FDC0
uint32_t STORMAPI SGetCurrentProcessId() {
    return getpid();
}

// Win64: 0x0076FDD0
int32_t STORMAPI SGetCurrentThreadPriority() {
    return MacGetThreadPriority(pthread_self());
}

// Win64: 0x0076FDE0
int32_t STORMAPI SSetCurrentThreadPriority(int32_t priority) {
    return MacSetThreadPriority(pthread_self(), priority);
}

// Win64: 0x0076FE00
int32_t STORMAPI SSetCurrentProcessAffinityMask(size_t mask) {
    // macOS has no processor affinity; the scheduler places the threads.
    return 1;
}

// Win64: 0x0076FE20
static void SetThreadName(uint32_t threadId, const char* threadName) {
    // pthread_setname_np only names the calling thread, which is the thread being named here.
    if (threadName && *threadName) {
        pthread_setname_np(threadName);
    }
}

// Win64: 0x0076FEA0
int32_t CInitCritSect::Enter() {
    int32_t result = 0;

    // Unlike the Windows code, the critical section is constructed before it is published, and the pointer is
    // read and written with acquire/release ordering for arm64.
    if (!__atomic_load_n(&this->m_critsect, __ATOMIC_ACQUIRE)) {
        while (InterlockedExchange(&this->m_spinLock, 1)) {
        }

        if (!this->m_critsect) {
            new (this->m_critsectData) CCritSect();
            __atomic_store_n(&this->m_critsect, reinterpret_cast<CCritSect*>(this->m_critsectData), __ATOMIC_RELEASE);
            result = 1;
        }

        __atomic_store_n(&this->m_spinLock, 0, __ATOMIC_RELEASE);
    }

    this->m_critsect->Enter();
    return result;
}

void CInitCritSect::Leave() {
    this->m_critsect->Leave();
}

// Win64: 0x0076FEF0
static uint32_t STORMAPI ProcessCompletionCallbackThread(void* vdata) {
    PROCESSCOMPLETION* completion = static_cast<PROCESSCOMPLETION*>(vdata);
    int status;
    waitpid(completion->process, &status, 0);
    completion->callback(completion->data);
    SMemFree(completion, __FILE__, __LINE__, 0);
    return 0;
}

// Win64: 0x0076FF30
uint32_t STORMAPI S_Thread::s_SLaunchThread(void* threadParam) {
    s_threadCrit.Enter();

    THREADLAUNCH* launch = static_cast<THREADLAUNCH*>(threadParam);
    uint32_t (STORMAPI* threadProc)(void*) = launch->threadProc;
    void* param = launch->param;
    uint32_t threadId = SGetCurrentThreadId();
    const char* threadName = nullptr;

    for (int32_t i = 0; i < s_numthreads; i++) {
        if (s_threads[i].threadId == threadId) {
            threadName = s_threads[i].name;
            break;
        }
    }

    SMemFree(launch, __FILE__, __LINE__, 0);
    SetThreadName(threadId, threadName);
    s_threadCrit.Leave();

    uint32_t threadVal = threadProc(param);

    s_threadCrit.Enter();

    int32_t numthreads = s_numthreads;

    for (int32_t i = 0; i < numthreads; i++) {
        if (s_threads[i].live && s_threads[i].threadId == threadId) {
            memcpy(&s_threads[i], &s_threads[i + 1], (s_maxthreads - 1 - i) * sizeof(SThreadTrack));
            numthreads--;
        }
    }

    s_numthreads = numthreads;
    s_threadCrit.Leave();

    return threadVal;
}

// pthread entry: gives the thread a crash signal stack, runs s_SLaunchThread, then signals the thread handle (a
// Win32 thread handle is signaled when the thread exits) and drops the thread's reference to it.
static void* MacThreadStart(void* threadParam) {
    HANDLE handle = static_cast<THREADLAUNCH*>(threadParam)->threadH;
    MacCrashInstallThreadStack();
    S_Thread::s_SLaunchThread(threadParam);
    MacCrashRemoveThreadStack();
    MacWaitableThreadExit(handle);
    return nullptr;
}

// Win64: 0x007700A0
HANDLE SCreateThread(uint32_t dwStackSize, uint32_t (STORMAPI* lpStartAddress)(void*), void* lpParameter, uint32_t dwCreationFlags, uint32_t* lpThreadId, char* threadName) {
    if (!threadName) {
        threadName = const_cast<char*>("");
    }

    S_Thread::s_threadCrit.Enter();

    if (!S_Thread::s_numthreads) {
        S_Thread::SThreadTrack* track = &S_Thread::s_threads[S_Thread::s_numthreads];
        track->threadId = SGetCurrentThreadId();
        track->threadH = nullptr;
        track->live = 1;
        track->suspended = 0;
        SStrCopy(track->name, "main", sizeof(track->name));
        S_Thread::s_numthreads++;
    }

    S_Thread::s_threadCrit.Leave();

    THREADLAUNCH* launch = static_cast<THREADLAUNCH*>(SMemAlloc(sizeof(THREADLAUNCH), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
    launch->threadProc = lpStartAddress;
    launch->param = lpParameter;

    S_Thread::s_threadCrit.Enter();

    // CreateThread reserves 1 MB of stack by default; secondary pthreads only get 512 KB.
    size_t stackSize = dwStackSize ? dwStackSize : 0x100000;
    stackSize = (stackSize + vm_page_size - 1) & ~(static_cast<size_t>(vm_page_size) - 1);

    if (stackSize < PTHREAD_STACK_MIN) {
        stackSize = PTHREAD_STACK_MIN;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, stackSize);

    // CREATE_SUSPENDED is not supported: the thread always starts running.
    HANDLE hThread = MacWaitableCreateThread();
    launch->threadH = hThread;

    pthread_t thread;
    uint32_t threadId = 0;

    if (pthread_create(&thread, &attr, MacThreadStart, launch) == 0) {
        MacWaitableSetThread(hThread, thread);
        threadId = MacThreadId(thread);
    } else {
        MacWaitableClose(hThread);
        MacWaitableClose(hThread);
        SMemFree(launch, __FILE__, __LINE__, 0);
        hThread = nullptr;
    }

    pthread_attr_destroy(&attr);

    if (lpThreadId) {
        *lpThreadId = threadId;
    }

    if (hThread) {
        S_Thread::SThreadTrack* track = &S_Thread::s_threads[S_Thread::s_numthreads];
        track->threadId = threadId;
        track->threadH = hThread;
        track->live = 1;
        track->suspended = 0;

        if (threadName) {
            SStrCopy(track->name, threadName, sizeof(track->name));
        }

        S_Thread::s_numthreads++;
    }

    S_Thread::s_threadCrit.Leave();

    return hThread;
}

// Win64: 0x007701C0
int32_t STORMAPI SGetThreadName(uint32_t threadId, char* buffer, int32_t bufferchars) {
    bool found = false;

    if (!buffer || bufferchars < 1) {
        return 0;
    }

    if (bufferchars > static_cast<int32_t>(sizeof(S_Thread::s_threads[0].name))) {
        bufferchars = sizeof(S_Thread::s_threads[0].name);
    }

    *buffer = 0;

    S_Thread::s_threadCrit.Enter();

    for (int32_t i = 0; i < S_Thread::s_numthreads; i++) {
        if (S_Thread::s_threads[i].threadId == threadId) {
            SStrCopy(buffer, S_Thread::s_threads[i].name, bufferchars);
            found = true;
            break;
        }
    }

    S_Thread::s_threadCrit.Leave();

    return found;
}

// Win64: 0x00770260
HANDLE STORMAPI SCreateThread(uint32_t (STORMAPI* lpStartAddress)(void*), void* lpParameter, uint32_t* lpThreadId, void* linuxData, char* threadName, uint32_t stackSize) {
    return SCreateThread(stackSize, lpStartAddress, lpParameter, 0, lpThreadId, threadName);
}

// Splits a Windows command line into arguments (CommandLineToArgv rules for quotes, without the backslash
// escapes). Returns the argument count; the strings point into buffer.
static int32_t SplitCommandLine(char* buffer, char** argv, int32_t maxargs) {
    int32_t argc = 0;
    char* curr = buffer;

    while (*curr && argc < maxargs) {
        while (*curr == ' ' || *curr == '\t') {
            curr++;
        }

        if (!*curr) {
            break;
        }

        argv[argc++] = curr;
        char* dest = curr;
        bool quoted = false;

        while (*curr && (quoted || (*curr != ' ' && *curr != '\t'))) {
            if (*curr == '"') {
                quoted = !quoted;
            } else {
                *dest++ = *curr;
            }

            curr++;
        }

        if (*curr) {
            curr++;
        }

        *dest = 0;
    }

    argv[argc] = nullptr;
    return argc;
}

// Win64: 0x00770290
int32_t STORMAPI SCreateProcess(const char* appName, char* commandLine, void (*callbackWhenProcessCompletes)(void*), void* callbackData) {
    char buffer[4096];
    char* argv[64];
    SStrCopy(buffer, commandLine ? commandLine : "", sizeof(buffer));
    int32_t argc = SplitCommandLine(buffer, argv, 63);

    char path[MAX_PATH];

    if (appName) {
        SStrCopy(path, appName, sizeof(path));
    } else if (argc) {
        SStrCopy(path, argv[0], sizeof(path));
    } else {
        return 0;
    }

    MacConvertPath(path);

    char* defaultargv[2] = { path, nullptr };
    pid_t process;

    if (posix_spawnp(&process, path, nullptr, nullptr, argc ? argv : defaultargv, environ)) {
        return 0;
    }

    if (callbackWhenProcessCompletes) {
        PROCESSCOMPLETION* completion = static_cast<PROCESSCOMPLETION*>(SMemAlloc(sizeof(PROCESSCOMPLETION), __FILE__, __LINE__, 0));
        completion->data = callbackData;
        completion->callback = callbackWhenProcessCompletes;
        completion->process = process;

        uint32_t threadId;
        SCreateThread(ProcessCompletionCallbackThread, completion, &threadId, nullptr, nullptr, 0);
    }

    return 1;
}
