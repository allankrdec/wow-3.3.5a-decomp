#include <cstdint>
#include <ctime>
#include "Engine/Source/Os/Time.h"

// Win64: 0x0086D450
int32_t OsGetTimeZone() {
    // GetTimeZoneInformation's Bias is the standard-time offset (UTC - local, in minutes), like POSIX timezone.
    tzset();
    return static_cast<int32_t>(-timezone / 60);
}

// Win64: 0x0086D490
void OsGetTimeStamp(char* buffer, uint32_t size) {
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    strftime(buffer, size, "%m%d%y_%H%M%S", &local);
}
