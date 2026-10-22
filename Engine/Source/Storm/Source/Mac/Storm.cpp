// macOS counterpart of Storm/Source/Storm.cpp.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mach-o/ldsyms.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_exitcode;

static int32_t s_exitcount;

// Win64: 0x007739B0
static void StormAtExit() {
    if (s_exitcount++ == 0) {
        // _cexit has no counterpart that is legal inside an atexit handler: flush stdio and leave without running
        // the remaining handlers and static destructors.
        fflush(nullptr);
        StormRtlDestroy();
        _exit(s_exitcode);
    }
}

// Win64: 0x007739E0
void STORMAPI StormInitialize() {
    SStrInitialize();
    SErrInitialize();
    SServerInitialize();
    SFile::Initialize();
}

// Win64: 0x00773A00
void STORMAPI StormDestroy() {
    SRgnDestroy();
    SMsgDestroy();
    SEvtDestroy();
    SCmdDestroy();
    SFile::Destroy();
    SCompDestroy();
    SStrDestroy();
    atexit(StormAtExit);
}

// Win64: 0x00773A30
HINSTANCE__* STORMAPI StormGetInstance() {
    // GetModuleHandle(nullptr): the base of the main executable image.
    return reinterpret_cast<HINSTANCE__*>(const_cast<mach_header_64*>(&_mh_execute_header));
}

// Win64: 0x00773A40
void StormExit(int32_t code) {
    s_exitcode = code;
    exit(code);
}

// Win64: 0x00773A60
void StormExitProcess(int32_t code) {
    s_exitcode = code;
    fflush(nullptr);
    _exit(code);
}
