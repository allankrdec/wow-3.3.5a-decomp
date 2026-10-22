// macOS counterpart of Storm/Source/Win64/MachineState.cpp. The header block and the stack/module listing of
// error logs work; register dumps, code/stack memory dumps and minidumps of a crash context belong to the crash
// handler (stage E7).

#include "Engine/Source/Base/Platform/Mac/TodoMac.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/MachineState.h"

#define LOG_PTR "%016llX"

struct MEMDUMP {
    /* 0000 */ void (*logLineProc)(void*, const char*, ...);
    /* 0004 */ void* logLineProcParam;
};

static char s_monthNames[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

int32_t g_memFullError;

static volatile LONG sgRecursionLevel;

// Win64: 0x0077CBD0
static void sLogSeparatorLine(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, char character, int32_t longLine) {
    char line[80];
    uint32_t length = longLine ? 78 : 40;
    memset(line, character, length);
    line[length] = 0;
    logLineProc(logLineProcParam, "%s", line);
}

// Win64: 0x0077CC10
static void sLogHeader(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const char* headerString) {
    logLineProc(logLineProcParam, "");
    sLogSeparatorLine(logLineProc, logLineProcParam, '-', 0);
    logLineProc(logLineProcParam, "    %s", headerString);
    sLogSeparatorLine(logLineProc, logLineProcParam, '-', 0);
    logLineProc(logLineProcParam, "");
}

// Win64: 0x0077CE70
static const char* sGetPathLeaf(const char* path) {
    const char* leaf = nullptr;
    const char* slash = strrchr(path, '/');

    if (slash) {
        leaf = slash;
    }

    const char* backslash = strrchr(path, '\\');

    if (backslash && backslash > leaf) {
        leaf = backslash;
    }

    const char* colon = strrchr(path, ':');

    if (colon && colon > leaf) {
        leaf = colon;
    }

    return leaf ? leaf + 1 : path;
}

// Win64: 0x0077CEC0
static void sLogExeFile(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    char exeFullPath[MAX_PATH];
    MacGetModuleFileName(exeFullPath, sizeof(exeFullPath));
    logLineProc(logLineProcParam, "%-10s%s", "Exe:", exeFullPath);
}

// Win64: 0x0077CF00
static void sLogDateTimeString(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const tm* time, uint32_t milliseconds) {
    int32_t hour = time->tm_hour;
    char ampm = hour < 12 ? 'A' : 'P';

    if (hour > 12) {
        hour -= 12;
    } else if (!hour) {
        hour = 12;
    }

    logLineProc(logLineProcParam, "%-10s%3s %2d, %4d %2d:%02d:%02d.%03d %cM", "Time:", s_monthNames[time->tm_mon], time->tm_mday, time->tm_year + 1900, hour, time->tm_min, time->tm_sec, milliseconds, ampm);
}

// Win64: 0x0077CF70
static void sLogUserName(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    char userName[MAX_PATH];
    passwd* entry = getpwuid(getuid());

    if (entry && entry->pw_name) {
        SStrCopy(userName, entry->pw_name, sizeof(userName));
    } else {
        strcpy(userName, "<unknown>");
    }

    logLineProc(logLineProcParam, "%-10s%s", "User:", userName);
}

// Win64: 0x0077CFE0
static void sLogComputerName(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    char computerName[256];

    if (gethostname(computerName, sizeof(computerName))) {
        strcpy(computerName, "<unknown>");
    }

    computerName[sizeof(computerName) - 1] = 0;
    logLineProc(logLineProcParam, "%-10s%s", "Computer:", computerName);
}

// Win64: 0x0077D690
static void STORMAPI CmdMemOutput(void* hOutput, const MEMREPORTENTRY* entry) {
    MEMDUMP* dump = static_cast<MEMDUMP*>(hOutput);
    dump->logLineProc(dump->logLineProcParam, "%5d %5d %s(%d)", entry->unk_0010, entry->unk_0014, entry->unk_0024, entry->unk_0020 >> 1);
}

// Win64: 0x0077D6C0
static void sShowOutOfMemory(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    if (!g_memFullError) {
        return;
    }

    MEMDUMP dump;
    dump.logLineProc = logLineProc;
    dump.logLineProcParam = logLineProcParam;
    SMemGenerateReport(SMEM_REPORT_BY_CALLER, CmdMemOutput, &dump);
    sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
}

void SMemGenerateReport(SMEMREPORTTYPE type, void (STORMAPI* callback)(void*, const MEMREPORTENTRY*), void* context) {
    StormCallService(STORM_SERVICE_GENERATEREPORT, type, callback, context);
}

// The current thread's stack, symbolized with dladdr (the dbghelp stack walk of the Windows version).
static void sLogStackTrace(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint32_t stackFramesToSkip) {
    sLogHeader(logLineProc, logLineProcParam, "Stack Trace (Manual)");

    void* frames[128];
    int32_t count = backtrace(frames, 128);

    logLineProc(logLineProcParam, "Address           Frame             Logical addr  Module");

    for (int32_t i = static_cast<int32_t>(stackFramesToSkip); i < count; i++) {
        Dl_info info;

        if (dladdr(frames[i], &info) && info.dli_fname) {
            uintptr_t offset = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase);
            const char* symbol = info.dli_sname ? info.dli_sname : "???";
            uintptr_t displacement = info.dli_saddr ? reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_saddr) : 0;
            logLineProc(logLineProcParam, LOG_PTR "  %02d  0x%08llX  %s  %s+%llu", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])), i, static_cast<unsigned long long>(offset), sGetPathLeaf(info.dli_fname), symbol, static_cast<unsigned long long>(displacement));
        } else {
            logLineProc(logLineProcParam, LOG_PTR "  %02d  <unknown module>", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])), i);
        }
    }
}

// Win64: 0x0077DE10
void LoadMachineStateSymbols() {
    // dladdr needs nothing loaded.
}

// Win64: 0x0077DE20
void UnloadMachineStateSymbols() {
}

// Win64: 0x0077DE30
void LogComputerInfoHeader(uint32_t logOptions, void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const char* headerTitleLine, const SYSTEMTIME* time) {
    if (InterlockedIncrement(&sgRecursionLevel) == 1) {
        if (!logOptions || (logOptions & LOG_OPTION_DEFAULT)) {
            logOptions |= LOG_OPTION_EXEFILE | LOG_OPTION_DATETIME | LOG_OPTION_COMPUTERNAME | LOG_OPTION_USERNAME;

            if (headerTitleLine && *headerTitleLine) {
                logOptions |= LOG_OPTION_TITLE;
            }
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '=', 1);

        if ((logOptions & LOG_OPTION_TITLE) && headerTitleLine && *headerTitleLine) {
            logLineProc(logLineProcParam, "%s", headerTitleLine);
            logLineProc(logLineProcParam, "");
        }

        if (logOptions & LOG_OPTION_EXEFILE) {
            sLogExeFile(logLineProc, logLineProcParam);
        }

        if (logOptions & LOG_OPTION_DATETIME) {
            // SYSTEMTIME is opaque on macOS: the header always shows the current local time.
            timeval now;
            gettimeofday(&now, nullptr);
            tm currentTime;
            localtime_r(&now.tv_sec, &currentTime);
            sLogDateTimeString(logLineProc, logLineProcParam, &currentTime, static_cast<uint32_t>(now.tv_usec / 1000));
        }

        if (logOptions & LOG_OPTION_USERNAME) {
            sLogUserName(logLineProc, logLineProcParam);
        }

        if (logOptions & LOG_OPTION_COMPUTERNAME) {
            sLogComputerName(logLineProc, logLineProcParam);
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
    }

    InterlockedDecrement(&sgRecursionLevel);
}

// Win64: 0x0077E040
int32_t LogMiniDump(void* logfile, EXCEPTION_POINTERS* exceptionpointers, uint32_t count, const char** strings) {
    // Minidumps (or a macOS equivalent) are part of the crash handler, stage E7.
    TODO_MAC();
    return 0;
}

// Win64: 0x0077E0E0
int32_t LogMiniDumpIsAvailable() {
    // No minidump writer until the crash handler (stage E7).
    return 0;
}

// Win64: 0x0077E3D0
static void sLogLoadedModules(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    sLogHeader(logLineProc, logLineProcParam, "Loaded Modules");

    uint32_t count = _dyld_image_count();

    for (uint32_t i = 0; i < count; i++) {
        const char* name = _dyld_get_image_name(i);
        const void* base = _dyld_get_image_header(i);
        logLineProc(logLineProcParam, "0x" LOG_PTR " %s", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base)), name ? name : "<unknown>");
    }
}

// Win64: 0x0077E670
void LogMachineState(uint32_t logOptions, void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint32_t stackFramesToSkip, CONTEXT* context) {
    if (InterlockedIncrement(&sgRecursionLevel) == 1) {
        if (!logOptions || (logOptions & LOG_OPTION_DEFAULT)) {
            logOptions |= LOG_OPTION_DBGHELPSTACK | LOG_OPTION_MANUALSTACK | LOG_OPTION_STACK | LOG_OPTION_MODULES;
        }

        if (context) {
            // Registers, the crashing thread's stack and memory dumps of a crash context: stage E7.
            TODO_MAC();
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);

        if (logOptions & (LOG_OPTION_MANUALSTACK | LOG_OPTION_DBGHELPSTACK)) {
            sLogStackTrace(logLineProc, logLineProcParam, stackFramesToSkip + 1);
        }

        if (logOptions & LOG_OPTION_MODULES) {
            sLogLoadedModules(logLineProc, logLineProcParam);
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
        sShowOutOfMemory(logLineProc, logLineProcParam);
    }

    InterlockedDecrement(&sgRecursionLevel);
}
