// macOS counterpart of Time/Win64/Time.cpp. The performance counter is the monotonic clock in nanoseconds
// (MacTimeNs), the tick count is MacTickCount, and the Win32 FILETIME/SYSTEMTIME conversions go through
// gmtime_r/timegm (UTC, like the Win32 calls).

#include <time.h>
#include <cmath>
#include <cstdint>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// Seconds between 1601-01-01 (FILETIME) and 1970-01-01 (Unix).
static const int64_t FILETIME_UNIX_SECONDS = 11644473600LL;

static void QueryPerformanceCounter(LARGE_INTEGER* counter) {
    counter->QuadPart = static_cast<int64_t>(MacTimeNs());
}

static bool QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
    frequency->QuadPart = 1000000000;
    return true;
}

static uint64_t GetSystemTimeAsFileTime() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return static_cast<uint64_t>(now.tv_sec + FILETIME_UNIX_SECONDS) * 10000000 + now.tv_nsec / 100;
}

const int32_t System_Time::s_daysBeforeMonth[13] = { 0, 31, 59, 90, 120, 151, 182, 212, 243, 273, 304, 334, 365 };

uint64_t System_Time::timeScaleCycles = 0xFFFFFFFFFFFFFFFF;

bool System_Time::s_usePerformanceCounter;

Blizzard::Lock::Atomic System_Time::s_tickLock;

int64_t System_Time::s_absBegin;

uint32_t System_Time::s_lastTickCount;

double System_Time::timeScaleMilliseconds;

Blizzard::Time::Timestamp System_Time::s_gmBegin;

uint64_t System_Time::s_tickCountHigh;

double System_Time::timeScaleNanoseconds;

double System_Time::timeScaleSeconds;

double System_Time::timeScaleMicroseconds;

// Win64: 0x00428F00
Blizzard::Time::Timestamp Blizzard::Time::FromFileTime(uint64_t fileTime) {
    if (fileTime <= 0x0077A5D6AA8BF851) {
        return static_cast<Timestamp>(0x8000000000000000);
    }

    if (fileTime <= 0x030701FFA04E87AE) {
        return static_cast<Timestamp>(fileTime * 100 + 0x5143382561530000);
    }

    return 0x7FFFFFFFFFFFFFFF;
}

// Win64: 0x00428F60
double System_Time::MeasureDrift(int64_t frequency, LARGE_INTEGER* counter, DWORD* tickCount) {
    DWORD tick;
    LARGE_INTEGER now;

    do {
        tick = MacTickCount();
        QueryPerformanceCounter(&now);
    } while (tick == *tickCount);

    double counterSeconds = static_cast<double>(now.QuadPart - counter->QuadPart) / frequency;
    double tickSeconds = static_cast<double>(tick - *tickCount);
    counter->QuadPart = now.QuadPart;
    *tickCount = tick;

    return fabs(counterSeconds - tickSeconds * 0.001) / counterSeconds;
}

// Win64: 0x00428FF0
uint64_t System_Time::GetTickCount64() {
    while (s_tickLock.Increment() != 1) {
        s_tickLock.Decrement();
        MacSleep(0);
    }

    DWORD tick = MacTickCount();

    if (tick < s_lastTickCount) {
        s_tickCountHigh += 0x100000000;
    }

    s_lastTickCount = tick;
    s_tickLock.Decrement();

    return s_tickCountHigh + s_lastTickCount;
}

// Win64: 0x00429070
void Blizzard::Time::BreakTime(Timestamp time, TimeRec& rec) {
    int32_t nanosecond = static_cast<int32_t>(time % 1000000000);

    if (nanosecond < 0) {
        nanosecond += 1000000000;
    }

    int64_t ticks = time / 100 + 125911584000000000;

    if (time < 0 && time % 100) {
        ticks = time / 100 + 125911583999999999;
    }

    // FileTimeToSystemTime: whole seconds of the FILETIME, rounded down.
    int64_t seconds = ticks / 10000000;

    if (ticks < 0 && ticks % 10000000) {
        seconds--;
    }

    time_t unixTime = static_cast<time_t>(seconds - FILETIME_UNIX_SECONDS);
    tm systemTime;
    gmtime_r(&unixTime, &systemTime);

    rec.day = systemTime.tm_mday;
    rec.hour = systemTime.tm_hour;
    rec.minute = systemTime.tm_min;
    rec.second = systemTime.tm_sec;
    rec.nanosecond = nanosecond;
    rec.weekday = systemTime.tm_wday;
    rec.year = systemTime.tm_year + 1900;
    rec.month = systemTime.tm_mon + 1;

    bool leap = !(rec.year % 400) || (rec.year % 100 && !(rec.year % 4));
    rec.yearday = System_Time::s_daysBeforeMonth[rec.month - 1] + systemTime.tm_mday - 1;

    if (leap && rec.month > 2) {
        rec.yearday++;
    }
}

// Win64: 0x00429170
Blizzard::Time::Timestamp Blizzard::Time::MakeTime(const TimeRec& rec) {
    tm systemTime = {};
    systemTime.tm_year = rec.year - 1900;
    systemTime.tm_mon = rec.month - 1;
    systemTime.tm_mday = rec.day;
    systemTime.tm_hour = rec.hour;
    systemTime.tm_min = rec.minute;
    systemTime.tm_sec = rec.second;

    // SystemTimeToFileTime: UTC.
    int64_t seconds = static_cast<int64_t>(timegm(&systemTime)) + FILETIME_UNIX_SECONDS;
    uint64_t fileTime = seconds > 0 ? static_cast<uint64_t>(seconds) * 10000000 : 0;
    Timestamp time = FromFileTime(fileTime);

    if (time != static_cast<Timestamp>(0x8000000000000000)) {
        return time + rec.nanosecond;
    }

    int64_t result = static_cast<int64_t>(rec.nanosecond) + 0x7FFFFFFFF7580E00;

    if (result > 0) {
        return static_cast<Timestamp>(0x8000000000000000);
    }

    return result;
}

// Win64: 0x0042E870
bool System_Time::CalibrateFrequency(LARGE_INTEGER* frequency) {
    if (!QueryPerformanceFrequency(frequency) || !frequency->QuadPart) {
        return false;
    }

    DWORD start = MacTickCount();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    DWORD tick;

    do {
        tick = MacTickCount();
        QueryPerformanceCounter(&counter);
    } while (tick == start);

    int32_t attempts = 0;

    while (MeasureDrift(frequency->QuadPart, &counter, &tick) >= 0.1) {
        if (++attempts >= 3) {
            return false;
        }
    }

    return true;
}

// Win64: 0x004337C0
bool System_Time::InitFrequency(LARGE_INTEGER* frequency) {
    // The monotonic clock is invariant, as QueryPerformanceCounter is on Windows Vista and later.
    return QueryPerformanceFrequency(frequency);
}

// Win64: 0x00435C80
bool System_Time::TimeInit() {
    LARGE_INTEGER frequency;
    s_usePerformanceCounter = InitFrequency(&frequency);

    if (!s_usePerformanceCounter) {
        timeScaleCycles = 0;
        return false;
    }

    timeScaleCycles = frequency.QuadPart;

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    s_absBegin = counter.QuadPart;

    s_gmBegin = Blizzard::Time::FromFileTime(GetSystemTimeAsFileTime());

    double scale = 1.0 / static_cast<double>(timeScaleCycles);
    timeScaleNanoseconds = 1000000000.0 * scale;
    timeScaleMicroseconds = 1000000.0 * scale;
    timeScaleMilliseconds = 1000.0 * scale;
    timeScaleSeconds = scale;

    return true;
}

// Win64: 0x00435D80
Blizzard::Time::Timestamp Blizzard::Time::GetTimestamp() {
    if (System_Time::timeScaleCycles == 0xFFFFFFFFFFFFFFFF) {
        System_Time::TimeInit();
    }

    if (System_Time::s_usePerformanceCounter) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        uint64_t elapsed = static_cast<uint64_t>(counter.QuadPart - System_Time::s_absBegin);
        return System_Time::s_gmBegin + static_cast<Timestamp>(static_cast<double>(elapsed) * System_Time::timeScaleNanoseconds);
    }

    return FromFileTime(GetSystemTimeAsFileTime());
}

// Win64: 0x00435E20
int64_t Blizzard::Time::Milliseconds() {
    if (System_Time::timeScaleCycles == 0xFFFFFFFFFFFFFFFF) {
        System_Time::TimeInit();
    }

    if (!System_Time::s_usePerformanceCounter) {
        return System_Time::GetTickCount64();
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    return static_cast<int64_t>(static_cast<double>(counter.QuadPart) * System_Time::timeScaleMilliseconds);
}
