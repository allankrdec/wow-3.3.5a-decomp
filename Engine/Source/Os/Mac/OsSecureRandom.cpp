#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <Security/SecRandom.h>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include "Engine/Source/Storm/H/storm.h"

// The Win64 entropy sources, taken from their Darwin counterparts.
struct OsSecureRandomEntropy {
    uint32_t m_tickCount;
    uint64_t m_absoluteTime;
    struct timeval m_systemTime;
    vm_statistics64_data_t m_memoryStatus;
    struct statfs m_diskStatus;
    pid_t m_processId;
    uint64_t m_threadId;
    LONG m_counter;
    void* m_heapPointer;
};

static volatile LONG s_counter;

// Win64: 0x0086D4C0
void IOsSecureRandom(uint8_t* buffer, uint32_t size) {
    OsSecureRandomEntropy entropy;
    memset(&entropy, 0, sizeof(entropy));

    entropy.m_tickCount = MacTickCount();
    entropy.m_absoluteTime = mach_absolute_time();
    gettimeofday(&entropy.m_systemTime, nullptr);

    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&entropy.m_memoryStatus), &count);

    statfs(".", &entropy.m_diskStatus);

    entropy.m_processId = getpid();
    pthread_threadid_np(nullptr, &entropy.m_threadId);

    entropy.m_heapPointer = SMemAlloc(1, __FILE__, __LINE__, 0);

    if (entropy.m_heapPointer) {
        SMemFree(entropy.m_heapPointer, __FILE__, __LINE__, 0);
    }

    entropy.m_counter = InterlockedIncrement(&s_counter);

    SHA1_CONTEXT context;
    uint8_t digest[20];

    SHA1_Init(&context);
    SHA1_Update(&context, buffer, size);
    SHA1_Update(&context, reinterpret_cast<const uint8_t*>(&entropy), sizeof(entropy));
    SHA1_Final(digest, &context);

    while (true) {
        uint32_t count = size > 10 ? 10 : size;
        memcpy(buffer, digest, count);
        size -= count;
        buffer += count;

        if (!size) {
            break;
        }

        SHA1_Init(&context);
        SHA1_Update(&context, digest, sizeof(digest));
        SHA1_Final(digest, &context);
    }
}

// Win64: 0x0086D640
void OsSecureRandom(uint8_t* buffer, uint32_t size) {
    if (SecRandomCopyBytes(kSecRandomDefault, size, buffer) == errSecSuccess) {
        return;
    }

    IOsSecureRandom(buffer, size);
}
