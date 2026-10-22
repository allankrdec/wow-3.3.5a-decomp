// macOS counterpart of Debug/Win64/Debug.cpp. The Windows breakpoint is swallowed by SEH when no debugger is
// attached; here it is only raised when a debugger is attached. There is no dbghelp to unload.

#include <sys/sysctl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"

static bool s_continueAfterAssert;

static bool IsDebuggerPresent() {
    int32_t mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    kinfo_proc info = {};
    size_t size = sizeof(info);

    if (sysctl(mib, 4, &info, &size, nullptr, 0)) {
        return false;
    }

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

// Win64: 0x00427010
void Blizzard::Debug::Breakpoint() {
    if (IsDebuggerPresent()) {
        __builtin_debugtrap();
    }
}

// Win64: 0x00439880
void Blizzard::Debug::DefaultCallback(const char* expression, const char* message, const char* fileName, uint32_t lineNumber) {
    char text[2048];
    char path[512];
    char canonical[256];
    char directory[512];
    Blizzard::Time::TimeRec rec;

    Blizzard::Time::BreakTime(Blizzard::Time::GetTimestamp(), rec);
    Blizzard::String::Format(text, sizeof(text), "%04d/%02d/%02d %02d:%02d:%02d  %s(%d) : Assertion failed: %s  \"%s\"\n", rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, CanonicalStr(fileName, canonical, sizeof(canonical)), lineNumber, expression, message);
    fprintf(stderr, "%s", text);

    Blizzard::Process::GetProcessDirectory(directory, sizeof(directory));
    Blizzard::String::Append(directory, "Errors", sizeof(directory));
    Blizzard::File::CreateDirectory(directory, true);
    Blizzard::String::Append(directory, "/Assert.log", sizeof(directory));
    Blizzard::String::MakeBackslashPath(directory, path, sizeof(path));

    FILE* stream;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);
        stream = fopen(name.m_data, "a");
    }

    if (stream) {
        fprintf(stream, "%s", text);
        fclose(stream);
    }

    Breakpoint();

    if (!s_continueAfterAssert) {
        abort();
    }
}

// Win64: 0x004321B0
void System_Debug::ShutdownProc() {
}
