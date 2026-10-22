#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include "Engine/Source/Os/Path.h"
#include "Engine/Source/Storm/H/storm.h"

static CommandLineUTF8 s_commandLineUTF8;

// Path of the main executable. Inside an application bundle (<dir>/X.app/Contents/MacOS/X) the bundle stands for
// the executable: the game folder (Data, WTF, Interface) is <dir>, the folder that contains X.app, so OsGetExePath
// (which strips the file name) yields "<dir>/" just like it yields the folder of Wow.exe on Windows.
static bool GetMainExecutablePath(char* path, uint32_t size) {
    uint32_t bufferSize = size;
    char* buffer = static_cast<char*>(alloca(bufferSize));

    if (_NSGetExecutablePath(buffer, &bufferSize)) {
        buffer = static_cast<char*>(alloca(bufferSize));

        if (_NSGetExecutablePath(buffer, &bufferSize)) {
            return false;
        }
    }

    char resolved[PATH_MAX];

    if (realpath(buffer, resolved)) {
        buffer = resolved;
    }

    const char* marker = ".app/Contents/MacOS/";
    char* bundle = strstr(buffer, marker);

    if (bundle && !strchr(bundle + strlen(marker), '/')) {
        bundle[4] = '\0';
    }

    SStrCopy(path, buffer, size);
    return true;
}

// Path of the loaded image whose dlopen handle is module.
static bool GetImagePath(void* module, char* path, uint32_t size) {
    uint32_t count = _dyld_image_count();

    for (uint32_t i = 0; i < count; i++) {
        const char* name = _dyld_get_image_name(i);

        if (!name) {
            continue;
        }

        void* handle = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);

        if (!handle) {
            continue;
        }

        dlclose(handle);

        if (handle == module) {
            SStrCopy(path, name, size);
            return true;
        }
    }

    return false;
}

// Win64: 0x0086BBD0
uint32_t GetModuleFileNameUTF8(void* module, char* path, uint32_t size) {
    if (!size) {
        return 0;
    }

    path[0] = '\0';

    if (module ? !GetImagePath(module, path, size) : !GetMainExecutablePath(path, size)) {
        path[0] = '\0';
        return 0;
    }

    return static_cast<uint32_t>(strlen(path));
}

// Win64: 0x0086BDB0
CommandLineUTF8::CommandLineUTF8() {
    // GetCommandLineW form: the arguments joined by spaces, quoted when they contain blanks. Darwin argv is UTF-8
    // already. Finder's "-psn_" process serial number argument is not part of the game command line.
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();

    size_t length = 0;

    for (int i = 0; i < argc; i++) {
        length += strlen(argv[i]) + 3;
    }

    uint32_t size = static_cast<uint32_t>(length + 1);
    m_commandLine = static_cast<char*>(SMemAlloc(size, __FILE__, __LINE__, 0));

    char* dst = m_commandLine;

    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];

        if (i && !strncmp(arg, "-psn_", 5)) {
            continue;
        }

        if (dst != m_commandLine) {
            *dst++ = ' ';
        }

        bool quote = !*arg || strpbrk(arg, " \t");

        if (quote) {
            *dst++ = '"';
        }

        size_t argLength = strlen(arg);
        memcpy(dst, arg, argLength);
        dst += argLength;

        if (quote) {
            *dst++ = '"';
        }
    }

    *dst = '\0';
}

CommandLineUTF8::~CommandLineUTF8() {
    delete[] m_commandLine;
}
