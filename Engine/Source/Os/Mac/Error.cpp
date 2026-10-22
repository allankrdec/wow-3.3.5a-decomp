#include <cerrno>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Os/Error.h"

// The Mac platform layers report failures through errno, the counterpart of GetLastError.

// Win64: 0x008714E0
char* OsGetLastErrorStr() {
    return strdup(strerror(errno));
}

// Win64: 0x00871510
void OsFreeLastErrorStr(char* str) {
    free(str);
}
