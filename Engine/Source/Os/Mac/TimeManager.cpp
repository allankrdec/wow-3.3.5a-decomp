#include <cstdint>
#include <mach/mach_time.h>
#include <pthread.h>
#include <cstdlib>
#include <new>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

static OsTimeManager* s_OsTimeMgr;

static int32_t QueryPerformanceFrequency(int64_t* frequency) {
    mach_timebase_info_data_t timebase;

    if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.numer) {
        return 0;
    }

    *frequency = static_cast<int64_t>(1000000000ull * timebase.denom / timebase.numer);
    return 1;
}

static int64_t QueryPerformanceCounter() {
    return static_cast<int64_t>(mach_absolute_time());
}

// Win64: 0x0086AB30
OsTimeManager::TimingMethod OsTimeManager::Calibrate() {
    // QueryPerformanceCounter is mach_absolute_time and GetTickCount is MacTickCount. Darwin has no thread
    // affinity, and mach_absolute_time is synchronized across cores, so the per-processor monotonicity test is
    // not run; the 250 ms comparison against the tick count is.
    if (!QueryPerformanceFrequency(&this->m_performanceFrequency)) {
        this->m_timingTestError = 1;
        return SystemMethod1;
    }

    if (!this->m_performanceFrequency) {
        this->m_timingTestError = 2;
        return SystemMethod1;
    }

    pthread_t thread = pthread_self();
    int32_t policy;
    struct sched_param priority;
    pthread_getschedparam(thread, &policy, &priority);
    struct sched_param highPriority = priority;
    highPriority.sched_priority = sched_get_priority_max(policy);
    pthread_setschedparam(thread, policy, &highPriority);
    OsSleep(0);

    this->m_timingTestError = 0;

    uint32_t tick = MacTickCount();
    uint32_t startTick;
    do {
        startTick = MacTickCount();
    } while (tick == startTick);

    int64_t startCount = QueryPerformanceCounter();

    uint32_t now;
    do {
        now = MacTickCount();
    } while (now - startTick < 250);

    uint32_t endTick;
    do {
        endTick = MacTickCount();
    } while (now == endTick);

    int64_t endCount = QueryPerformanceCounter();

    int64_t elapsed = static_cast<int64_t>(static_cast<double>(endCount - startCount) / static_cast<double>(this->m_performanceFrequency) * 1000.0);
    if (abs(static_cast<int32_t>(endTick - static_cast<uint32_t>(elapsed) - startTick)) >= 5) {
        this->m_timingTestError = 3;
    }

    pthread_setschedparam(thread, policy, &priority);
    OsSleep(0);

    return this->m_timingTestError ? SystemMethod1 : SystemMethod2;
}

// Win64: 0x0086AD50
uint32_t OsTimeGetTestError() {
    return s_OsTimeMgr->m_timingTestError;
}

// Win64: 0x0086AD60
OsTimeManager::TimingMethod OsTimeGetTimingMethod() {
    return s_OsTimeMgr->m_timingMethod;
}

// Win64: 0x0086AD70
const char* GetTimingMethodName(OsTimeManager::TimingMethod method) {
    switch (method) {
        case OsTimeManager::NotSet:
            return "[Not Set]";

        case OsTimeManager::BestAvailable:
            return "[Best Available]";

        case OsTimeManager::SystemMethod1:
            return "GetTickCount";

        case OsTimeManager::SystemMethod2:
            return "QueryPerformanceCounter";

        default:
            return "[Unknown]";
    }
}

// Win64: 0x0086ADC0
uint64_t OsTimeManager::Snapshot() {
    switch (this->m_timingMethod) {
        case SystemMethod1:
            break;

        case SystemMethod2: {
            int64_t count = QueryPerformanceCounter();
            return static_cast<int64_t>(static_cast<double>(count) * this->m_scaleToMs + this->m_timeBegin);
        }

        default:
            break;
    }

    int64_t ticks = MacTickCount();
    return static_cast<int64_t>(static_cast<double>(ticks) * this->m_scaleToMs + this->m_timeBegin);
}

// Win64: 0x0086AE20
uint32_t OsGetAsyncTimeMs() {
    return static_cast<uint32_t>(s_OsTimeMgr->Snapshot());
}

// Win64: 0x0086AE30
uint64_t OsGetAsyncTimeClocks() {
    switch (s_OsTimeMgr->m_timingMethod) {
        case OsTimeManager::SystemMethod1:
            break;

        case OsTimeManager::SystemMethod2: {
            return QueryPerformanceCounter();
        }

        default:
            break;
    }

    return MacTickCount();
}

// Win64: 0x0086AE70
int64_t OsGetAsyncClocksPerSecond() {
    switch (s_OsTimeMgr->m_timingMethod) {
        case OsTimeManager::SystemMethod1:
            break;

        case OsTimeManager::SystemMethod2:
            return s_OsTimeMgr->m_performanceFrequency;

        default:
            break;
    }

    return 1000;
}

// Win64: 0x0086AEA0
OsTimeManager::OsTimeManager(OsTimeManager::TimingMethod method) {
    this->m_timingMethod = NotSet;

    OsTimeManager::TimingMethod calibrated = this->Calibrate();
    if (method != BestAvailable && calibrated != method) {
        if (method == SystemMethod2 && calibrated == SystemMethod1) {
            this->m_timingTestError = 5;
        }

        calibrated = method;
    }

    this->m_timingMethod = calibrated;

    int64_t clocksPerSecond;
    switch (calibrated) {
        case SystemMethod1:
            clocksPerSecond = 1000;
            break;

        case SystemMethod2:
            clocksPerSecond = this->m_performanceFrequency;
            break;

        default:
            clocksPerSecond = 1000;
            break;
    }

    this->m_scaleToMs = 1000.0 / static_cast<double>(clocksPerSecond);
    this->m_timeBegin = 0.0;
}

// Win64: 0x0086AF20
void OsTimeManagerCreate(OsTimeManager::TimingMethod method) {
    s_OsTimeMgr = new (SMemAlloc(sizeof(OsTimeManager), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY)) OsTimeManager(method);
}

// Win64: 0x0086AF60
void OsTimeManagerDestroy() {
    if (s_OsTimeMgr) {
        delete s_OsTimeMgr;
        s_OsTimeMgr = nullptr;
    }
}

// Win64: 0x0086B280
void OsSleep(uint32_t milliseconds) {
    MacSleep(milliseconds);
}
