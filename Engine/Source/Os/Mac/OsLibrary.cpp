#include <dlfcn.h>
#include "Engine/Source/Os/OsLibrary.h"

// Win64: 0x0086C4E0
void* OsLoadLibrary(const char* fileName) {
    return dlopen(fileName, RTLD_NOW | RTLD_LOCAL);
}

// Win64: 0x0086C4F0
void OsFreeLibrary(void* module) {
    if (module) {
        dlclose(module);
    }
}

// Win64: 0x0086C500
void* OsGetLibraryEntryPoint(void* module, const char* name) {
    if (!module) {
        return nullptr;
    }

    return dlsym(module, name);
}
