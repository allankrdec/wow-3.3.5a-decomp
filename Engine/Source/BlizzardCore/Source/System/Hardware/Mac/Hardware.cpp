// macOS counterpart of Hardware/Win64/Hardware.cpp. arm64 has no CPUID: the counts come from sysctl, and the
// feature bits report the x86 baseline the game checks for (TSC, MMX, SSE, SSE2), which NEON covers.

#include <sys/sysctl.h>
#include <cstdint>
#include <cstring>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

static uint32_t GetSysctlCount(const char* name) {
    int32_t value = 0;
    size_t size = sizeof(value);

    if (sysctlbyname(name, &value, &size, nullptr, 0) || value <= 0) {
        return 0;
    }

    return static_cast<uint32_t>(value);
}

Blizzard::Hardware::ProcessorFeatures System_Hardware::s_processorFeatures;

bool System_Hardware::s_initialized;

Blizzard::Lock::Atomic System_Hardware::s_initCount;

// Win64: 0x0042E120
bool System_Hardware::ReadCPUID(CPUIDInfo* info) {
    memset(info, 0, sizeof(*info));

    return false;
}

// Win64: 0x00432D90
void System_Hardware::InitProcessorFeatures() {
    s_processorFeatures.m_processorCount = GetSysctlCount("hw.logicalcpu");

    if (!s_processorFeatures.m_processorCount) {
        s_processorFeatures.m_processorCount = 1;
    }

    s_processorFeatures.m_coreCount = 1;
    s_processorFeatures.m_vendor = 0;

    uint32_t features = 0x1 | 0x2 | 0x4 | 0x10;
    uint32_t cores = GetSysctlCount("hw.physicalcpu");

    if (cores > 1) {
        s_processorFeatures.m_features = features | 0x40;
        s_processorFeatures.m_coreCount = cores;
        return;
    }

    s_processorFeatures.m_features = features;
}

// Win64: 0x00435BB0
uint32_t Blizzard::Hardware::GetProcessorFeatures() {
    if (!System_Hardware::s_initialized) {
        if (System_Hardware::s_initCount.Increment() == 1) {
            System_Hardware::InitProcessorFeatures();
            System_Hardware::s_initialized = true;
            return System_Hardware::s_processorFeatures.m_features;
        }

        do {
            MacSleep(1);
        } while (!System_Hardware::s_initialized);
    }

    return System_Hardware::s_processorFeatures.m_features;
}
