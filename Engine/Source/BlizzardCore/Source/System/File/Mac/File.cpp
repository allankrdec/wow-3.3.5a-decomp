// macOS counterpart of File/Win64/File.cpp.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"
#include "Engine/Source/BlizzardCore/Source/System/Debug/ErrorStack.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"

// Win64: 0x0042D500
void System_File::Stacked::Throttle(int64_t bytes, int64_t elapsed) {
    if (s_throttleDelay > 0) {
        MacSleep(s_throttleDelay);
    }

    s_throttleDebt += static_cast<int64_t>(static_cast<double>(bytes) / static_cast<double>(s_throttleRate) * 1000000000.0f) - elapsed;

    if (s_throttleDebt < -1000000000) {
        s_throttleDebt = -1000000000;
        return;
    }

    if (s_throttleDebt > 10000000) {
        int32_t milliseconds = static_cast<int32_t>(s_throttleDebt / 1000000);
        s_throttleDebt -= milliseconds * 1000000;
        MacSleep(milliseconds);
    }
}
