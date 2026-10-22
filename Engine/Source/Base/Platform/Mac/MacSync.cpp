#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include <cerrno>
#include <ctime>
#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>

struct MacEvent {
    pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
    bool m_manualReset;
    bool m_signaled;
};

void MacCriticalSectionInit(CRITICAL_SECTION* section) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&section->m_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void MacCriticalSectionDelete(CRITICAL_SECTION* section) {
    pthread_mutex_destroy(&section->m_mutex);
}

void MacCriticalSectionEnter(CRITICAL_SECTION* section) {
    pthread_mutex_lock(&section->m_mutex);
}

void MacCriticalSectionLeave(CRITICAL_SECTION* section) {
    pthread_mutex_unlock(&section->m_mutex);
}

bool MacCriticalSectionTryEnter(CRITICAL_SECTION* section) {
    return pthread_mutex_trylock(&section->m_mutex) == 0;
}

HANDLE MacEventCreate(bool manualReset, bool initialState) {
    MacEvent* event = new MacEvent;
    pthread_mutex_init(&event->m_mutex, nullptr);
    pthread_cond_init(&event->m_cond, nullptr);
    event->m_manualReset = manualReset;
    event->m_signaled = initialState;
    return event;
}

void MacEventDestroy(HANDLE handle) {
    MacEvent* event = static_cast<MacEvent*>(handle);
    if (!event) {
        return;
    }

    pthread_cond_destroy(&event->m_cond);
    pthread_mutex_destroy(&event->m_mutex);
    delete event;
}

void MacEventSet(HANDLE handle) {
    MacEvent* event = static_cast<MacEvent*>(handle);

    pthread_mutex_lock(&event->m_mutex);
    event->m_signaled = true;
    if (event->m_manualReset) {
        pthread_cond_broadcast(&event->m_cond);
    } else {
        pthread_cond_signal(&event->m_cond);
    }
    pthread_mutex_unlock(&event->m_mutex);
}

void MacEventReset(HANDLE handle) {
    MacEvent* event = static_cast<MacEvent*>(handle);

    pthread_mutex_lock(&event->m_mutex);
    event->m_signaled = false;
    pthread_mutex_unlock(&event->m_mutex);
}

bool MacEventWait(HANDLE handle, uint32_t timeout) {
    MacEvent* event = static_cast<MacEvent*>(handle);

    timespec deadline;
    if (timeout != MAC_WAIT_INFINITE) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000;
        deadline.tv_nsec += static_cast<long>(timeout % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&event->m_mutex);
    while (!event->m_signaled) {
        if (timeout == MAC_WAIT_INFINITE) {
            pthread_cond_wait(&event->m_cond, &event->m_mutex);
        } else if (timeout == 0 || pthread_cond_timedwait(&event->m_cond, &event->m_mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }

    bool signaled = event->m_signaled;
    if (signaled && !event->m_manualReset) {
        event->m_signaled = false;
    }
    pthread_mutex_unlock(&event->m_mutex);

    return signaled;
}

void MacSleep(uint32_t milliseconds) {
    if (milliseconds == 0) {
        sched_yield();
        return;
    }

    usleep(static_cast<useconds_t>(milliseconds) * 1000);
}

uint64_t MacTimeNs() {
    static mach_timebase_info_data_t s_timebase;
    if (!s_timebase.denom) {
        mach_timebase_info(&s_timebase);
    }

    return mach_absolute_time() * s_timebase.numer / s_timebase.denom;
}

uint32_t MacTickCount() {
    return static_cast<uint32_t>(MacTimeNs() / 1000000);
}
