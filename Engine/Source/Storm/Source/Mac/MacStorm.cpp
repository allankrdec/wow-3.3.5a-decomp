// Helpers shared by the macOS Storm platform files; see MacStorm.h.

#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

enum MacWaitableKind {
    MAC_WAITABLE_EVENT,
    MAC_WAITABLE_SEMAPHORE,
    MAC_WAITABLE_MUTEX,
    MAC_WAITABLE_THREAD,
};

struct MacWaitable {
    MacWaitableKind m_kind;
    int32_t m_refcount;
    bool m_manualReset;
    bool m_signaled;
    int32_t m_count;
    int32_t m_maximum;
    pthread_t m_owner;
    uint32_t m_recursion;
    pthread_t m_thread;
    bool m_hasThread;
};

static pthread_mutex_t s_waitMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_waitCond = PTHREAD_COND_INITIALIZER;

static MacWaitable* NewWaitable(MacWaitableKind kind) {
    MacWaitable* waitable = new MacWaitable;
    memset(waitable, 0, sizeof(*waitable));
    waitable->m_kind = kind;
    waitable->m_refcount = 1;
    return waitable;
}

static bool IsReady(const MacWaitable* waitable, pthread_t self) {
    switch (waitable->m_kind) {
        case MAC_WAITABLE_EVENT:
        case MAC_WAITABLE_THREAD:
            return waitable->m_signaled;
        case MAC_WAITABLE_SEMAPHORE:
            return waitable->m_count > 0;
        case MAC_WAITABLE_MUTEX:
            return !waitable->m_recursion || pthread_equal(waitable->m_owner, self);
    }

    return false;
}

static void Acquire(MacWaitable* waitable, pthread_t self) {
    switch (waitable->m_kind) {
        case MAC_WAITABLE_EVENT:
            if (!waitable->m_manualReset) {
                waitable->m_signaled = false;
            }
            break;
        case MAC_WAITABLE_SEMAPHORE:
            waitable->m_count--;
            break;
        case MAC_WAITABLE_MUTEX:
            waitable->m_owner = self;
            waitable->m_recursion++;
            break;
        case MAC_WAITABLE_THREAD:
            break;
    }
}

HANDLE MacWaitableCreateEvent(bool manualReset, bool initialState) {
    MacWaitable* waitable = NewWaitable(MAC_WAITABLE_EVENT);
    waitable->m_manualReset = manualReset;
    waitable->m_signaled = initialState;
    return waitable;
}

HANDLE MacWaitableCreateSemaphore(int32_t initialCount, int32_t maximumCount) {
    if (maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount) {
        return nullptr;
    }

    MacWaitable* waitable = NewWaitable(MAC_WAITABLE_SEMAPHORE);
    waitable->m_count = initialCount;
    waitable->m_maximum = maximumCount;
    return waitable;
}

HANDLE MacWaitableCreateMutex(bool initialOwner) {
    MacWaitable* waitable = NewWaitable(MAC_WAITABLE_MUTEX);

    if (initialOwner) {
        waitable->m_owner = pthread_self();
        waitable->m_recursion = 1;
    }

    return waitable;
}

HANDLE MacWaitableCreateThread() {
    MacWaitable* waitable = NewWaitable(MAC_WAITABLE_THREAD);
    waitable->m_manualReset = true;
    waitable->m_refcount = 2;
    return waitable;
}

void MacWaitableClose(HANDLE handle) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    if (!waitable) {
        return;
    }

    pthread_mutex_lock(&s_waitMutex);
    bool destroy = --waitable->m_refcount == 0;
    pthread_mutex_unlock(&s_waitMutex);

    if (destroy) {
        delete waitable;
    }
}

bool MacWaitableSetEvent(HANDLE handle) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    if (!waitable || waitable->m_kind != MAC_WAITABLE_EVENT) {
        return false;
    }

    pthread_mutex_lock(&s_waitMutex);
    waitable->m_signaled = true;
    pthread_cond_broadcast(&s_waitCond);
    pthread_mutex_unlock(&s_waitMutex);
    return true;
}

bool MacWaitableResetEvent(HANDLE handle) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    if (!waitable || waitable->m_kind != MAC_WAITABLE_EVENT) {
        return false;
    }

    pthread_mutex_lock(&s_waitMutex);
    waitable->m_signaled = false;
    pthread_mutex_unlock(&s_waitMutex);
    return true;
}

bool MacWaitableReleaseSemaphore(HANDLE handle, int32_t count) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    if (!waitable || waitable->m_kind != MAC_WAITABLE_SEMAPHORE || count <= 0) {
        return false;
    }

    pthread_mutex_lock(&s_waitMutex);
    bool result = waitable->m_count <= waitable->m_maximum - count;

    if (result) {
        waitable->m_count += count;
        pthread_cond_broadcast(&s_waitCond);
    }

    pthread_mutex_unlock(&s_waitMutex);
    return result;
}

bool MacWaitableReleaseMutex(HANDLE handle) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    if (!waitable || waitable->m_kind != MAC_WAITABLE_MUTEX) {
        return false;
    }

    pthread_mutex_lock(&s_waitMutex);
    bool result = waitable->m_recursion && pthread_equal(waitable->m_owner, pthread_self());

    if (result && --waitable->m_recursion == 0) {
        pthread_cond_broadcast(&s_waitCond);
    }

    pthread_mutex_unlock(&s_waitMutex);
    return result;
}

void MacWaitableSetThread(HANDLE handle, pthread_t thread) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    pthread_mutex_lock(&s_waitMutex);
    waitable->m_thread = thread;
    waitable->m_hasThread = true;
    pthread_mutex_unlock(&s_waitMutex);
}

bool MacWaitableGetThread(HANDLE handle, pthread_t* thread) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    if (!waitable || waitable->m_kind != MAC_WAITABLE_THREAD) {
        return false;
    }

    pthread_mutex_lock(&s_waitMutex);
    bool result = waitable->m_hasThread && !waitable->m_signaled;
    *thread = waitable->m_thread;
    pthread_mutex_unlock(&s_waitMutex);
    return result;
}

void MacWaitableThreadExit(HANDLE handle) {
    MacWaitable* waitable = static_cast<MacWaitable*>(handle);

    pthread_mutex_lock(&s_waitMutex);
    waitable->m_signaled = true;
    pthread_cond_broadcast(&s_waitCond);
    bool destroy = --waitable->m_refcount == 0;
    pthread_mutex_unlock(&s_waitMutex);

    if (destroy) {
        delete waitable;
    }
}

uint32_t MacWaitableWait(uint32_t count, const HANDLE* handles, bool waitAll, uint32_t timeout) {
    if (!count) {
        return MAC_WAIT_FAILED;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!handles[i]) {
            return MAC_WAIT_FAILED;
        }
    }

    timespec deadline = {};

    if (timeout != 0xFFFFFFFF) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000;
        deadline.tv_nsec += static_cast<long>(timeout % 1000) * 1000000;

        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    pthread_t self = pthread_self();
    uint32_t result = MAC_WAIT_TIMEOUT;
    bool timedOut = false;

    pthread_mutex_lock(&s_waitMutex);

    while (true) {
        if (waitAll) {
            bool ready = true;

            for (uint32_t i = 0; i < count && ready; i++) {
                ready = IsReady(static_cast<MacWaitable*>(handles[i]), self);
            }

            if (ready) {
                for (uint32_t i = 0; i < count; i++) {
                    Acquire(static_cast<MacWaitable*>(handles[i]), self);
                }

                result = MAC_WAIT_OBJECT_0;
                break;
            }
        } else {
            bool found = false;

            for (uint32_t i = 0; i < count; i++) {
                MacWaitable* waitable = static_cast<MacWaitable*>(handles[i]);

                if (IsReady(waitable, self)) {
                    Acquire(waitable, self);
                    result = MAC_WAIT_OBJECT_0 + i;
                    found = true;
                    break;
                }
            }

            if (found) {
                break;
            }
        }

        if (timeout == 0 || timedOut) {
            break;
        }

        if (timeout == 0xFFFFFFFF) {
            pthread_cond_wait(&s_waitCond, &s_waitMutex);
        } else if (pthread_cond_timedwait(&s_waitCond, &s_waitMutex, &deadline) == ETIMEDOUT) {
            // One last check of the objects, then give up.
            timedOut = true;
        }
    }

    pthread_mutex_unlock(&s_waitMutex);
    return result;
}

// THREAD_PRIORITY_NORMAL sits at the default SCHED_OTHER priority; the Win32 steps are one priority level apart.
static const int32_t s_normalPriority = 31;

int32_t MacGetThreadPriority(pthread_t thread) {
    int32_t policy;
    sched_param param;

    if (pthread_getschedparam(thread, &policy, &param)) {
        return 0;
    }

    int32_t priority = param.sched_priority - s_normalPriority;

    if (priority >= 15) {
        return 15;
    }

    if (priority <= -15) {
        return -15;
    }

    return priority < -2 ? -2 : (priority > 2 ? 2 : priority);
}

bool MacSetThreadPriority(pthread_t thread, int32_t priority) {
    int32_t policy;
    sched_param param;

    if (pthread_getschedparam(thread, &policy, &param)) {
        return false;
    }

    int32_t minimum = sched_get_priority_min(policy);
    int32_t maximum = sched_get_priority_max(policy);
    int32_t value = s_normalPriority + priority;
    param.sched_priority = value < minimum ? minimum : (value > maximum ? maximum : value);

    return pthread_setschedparam(thread, policy, &param) == 0;
}

uint32_t MacThreadId(pthread_t thread) {
    return pthread_mach_thread_np(thread);
}

void MacGetModuleFileName(char* buffer, uint32_t bufferchars) {
    if (!bufferchars) {
        return;
    }

    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    buffer[0] = 0;

    if (_NSGetExecutablePath(path, &size)) {
        return;
    }

    char resolved[PATH_MAX];
    const char* result = realpath(path, resolved) ? resolved : path;
    snprintf(buffer, bufferchars, "%s", result);
}

void MacGetModuleDirectory(char* buffer, uint32_t bufferchars) {
    MacGetModuleFileName(buffer, bufferchars);

    char* bundle = strstr(buffer, ".app/Contents/MacOS/");

    if (bundle) {
        *bundle = 0;
    }

    char* slash = strrchr(buffer, '/');

    if (slash) {
        slash[1] = 0;
    } else if (bufferchars >= 3) {
        snprintf(buffer, bufferchars, "./");
    }
}

void MacConvertPath(char* path) {
    for (char* c = path; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
}

bool MacCreateDirectories(const char* path) {
    char buffer[PATH_MAX];
    snprintf(buffer, sizeof(buffer), "%s", path);

    for (char* c = buffer + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            mkdir(buffer, 0755);
            *c = '/';
        }
    }

    if (mkdir(buffer, 0755) == 0) {
        return true;
    }

    struct stat info;
    return stat(buffer, &info) == 0 && S_ISDIR(info.st_mode);
}

void MacOutputDebugString(const char* string) {
    fputs(string, stderr);
}

bool MacIsDebuggerPresent() {
    kinfo_proc info = {};
    size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };

    if (sysctl(mib, 4, &info, &size, nullptr, 0)) {
        return false;
    }

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}
