// The Win32 functions declared in Platform/WinTypes.h for the macOS build.

#include "Engine/Source/Base/Platform/WinTypes.h"

uint32_t GetACP() {
    // Windows-1252, the ANSI code page of the Western-language clients.
    return 1252;
}
