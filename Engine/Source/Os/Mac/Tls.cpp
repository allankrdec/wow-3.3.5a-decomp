#include <cstdint>
#include <pthread.h>
#include "Engine/Source/Os/Tls.h"

// Win64 TLS indices are small integers and TlsAlloc returns TLS_OUT_OF_INDEXES (0xFFFFFFFF) on failure. pthread
// keys are integers too (pthread_key_t is unsigned long on Darwin, well below 2^32), so the key is the index.

// Win64: 0x0086D6B0
uint32_t OsTlsAlloc() {
    pthread_key_t key;

    if (pthread_key_create(&key, nullptr)) {
        return 0xFFFFFFFF;
    }

    return static_cast<uint32_t>(key);
}

// Win64: 0x0086D6C0
void OsTlsFree(uint32_t index) {
    pthread_key_delete(static_cast<pthread_key_t>(index));
}

// Win64: 0x0086D6D0
void* OsTlsGetValue(uint32_t index) {
    return pthread_getspecific(static_cast<pthread_key_t>(index));
}

// Win64: 0x0086D6E0
int32_t OsTlsSetValue(uint32_t index, void* value) {
    return pthread_setspecific(static_cast<pthread_key_t>(index), value) == 0;
}
