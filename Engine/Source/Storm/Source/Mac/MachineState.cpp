// macOS counterpart of Storm/Source/Win64/MachineState.cpp. Error and crash logs get the header block, the arm64
// registers of a crash context, a frame-pointer stack walk of every thread (Mach thread states, symbolized with
// dladdr; it stands in for both the manual and the dbghelp walks of Windows), the loaded images and code/stack
// memory dumps. Memory is read with vm_read_overwrite, so a bad pointer fails the read instead of faulting (the
// IsBadReadPtr of Windows). There are no minidumps: the crash handler re-raises the signal and the macOS crash
// reporter writes its own report.
//
// LogMachineState runs inside the crash signal handler: nothing here allocates, and the values that need
// non-reentrant calls (user name, executable path) are cached by MacMachineStatePrepare.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <pwd.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/MachineState.h"

#define LOG_PTR "%016llX"
#define LOG_PTR_EXTRA 8

struct MEMDUMP {
    /* 0000 */ void (*logLineProc)(void*, const char*, ...);
    /* 0004 */ void* logLineProcParam;
};

static char s_monthNames[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

int32_t g_memFullError;

static volatile LONG sgRecursionLevel;

// getpwuid may talk to opendirectoryd: not something to do from a signal handler.
static char s_userName[MAX_PATH];

// A frame of a stack walk.
struct STACKWALKFRAME {
    uint64_t pc;
    uint64_t fp;
};

// Strips pointer authentication bits (arm64e system code) off a code address.
static uint64_t sStripCodePointer(uint64_t address) {
    return address & 0x0000FFFFFFFFFFFFULL;
}

// Copies memory of this process without faulting on unmapped or protected addresses.
static bool sSafeRead(uint64_t address, void* buffer, size_t size) {
    vm_size_t outsize = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(address), size, reinterpret_cast<vm_address_t>(buffer), &outsize);
    return result == KERN_SUCCESS && outsize == size;
}

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

// Win64: 0x0077CCA0
static void sLogMemoryHexDump(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint8_t* address, uint32_t numBytes, int32_t alignedLines) {
    char buffer[80 + LOG_PTR_EXTRA];
    uint32_t numLines = numBytes >> 4;
    uint32_t offset = reinterpret_cast<uintptr_t>(address) & 0xF;

    if (alignedLines) {
        memset(buffer, ' ', 78 + LOG_PTR_EXTRA);
        buffer[0] = '*';
        char* marker = &buffer[10 + LOG_PTR_EXTRA + 3 * offset + (offset >> 2)];
        buffer[78 + LOG_PTR_EXTRA] = 0;
        buffer[2] = '=';
        memcpy(&buffer[4], "addr", 4);
        marker[0] = '*';
        marker[1] = '*';
        buffer[62 + LOG_PTR_EXTRA + offset] = '*';
        logLineProc(logLineProcParam, "%s", buffer);

        if (offset) {
            address -= offset;
            ++numLines;
        }
    }

    for (; numLines; --numLines) {
        char* text = buffer + snprintf(buffer, sizeof(buffer), LOG_PTR ": ", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
        uint8_t bytes[16];

        if (!sSafeRead(reinterpret_cast<uintptr_t>(address), bytes, sizeof(bytes))) {
            strcpy(text, "<can't read from this address>");
            logLineProc(logLineProcParam, "%s", buffer);
            return;
        }

        for (uint32_t i = 0; i < 16; i += 4) {
            text += snprintf(text, buffer + sizeof(buffer) - text, "%02X %02X %02X %02X  ", bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3]);
        }

        for (uint32_t i = 0; i < 16; ++i) {
            text[i] = isprint(bytes[i]) ? bytes[i] : '.';
        }

        text[16] = 0;
        logLineProc(logLineProcParam, "%s", buffer);
        address += 16;
    }
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

    if (s_userName[0]) {
        SStrCopy(userName, s_userName, sizeof(userName));
    } else {
        passwd* entry = getpwuid(getuid());

        if (entry && entry->pw_name) {
            SStrCopy(userName, entry->pw_name, sizeof(userName));
        } else {
            strcpy(userName, "<unknown>");
        }
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

// Win64: 0x0077D040
static void sLogMemory(uint32_t logOptions, void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint8_t* instructionPointer, uint8_t* stackPointer) {
    sLogHeader(logLineProc, logLineProcParam, "Memory Dump");

    if (logOptions & LOG_OPTION_CODE) {
        logLineProc(logLineProcParam, "Code: %d bytes starting at (PC = " LOG_PTR ")", 16, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(instructionPointer)));
        logLineProc(logLineProcParam, "");
        sLogMemoryHexDump(logLineProc, logLineProcParam, instructionPointer, 16, 0);
        logLineProc(logLineProcParam, "");
        logLineProc(logLineProcParam, "");
    }

    if (logOptions & LOG_OPTION_STACK) {
        logLineProc(logLineProcParam, "Stack: %d bytes starting at (SP = " LOG_PTR ")", 1024, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stackPointer)));
        logLineProc(logLineProcParam, "");
        sLogMemoryHexDump(logLineProc, logLineProcParam, stackPointer, 1024, 1);
        logLineProc(logLineProcParam, "");
        logLineProc(logLineProcParam, "");
    }
}

// The pc and frame pointer of the caller (RtlCaptureContext of the Windows version).
__attribute__((noinline)) static void sCaptureCallerFrame(uint64_t* pc, uint64_t* fp) {
    *pc = sStripCodePointer(reinterpret_cast<uint64_t>(__builtin_return_address(0)));
    *fp = *static_cast<uint64_t*>(__builtin_frame_address(0));
}

static void sGetFunctionStart(uint64_t address, uint64_t* start) {
    Dl_info info;
    *start = dladdr(reinterpret_cast<void*>(address), &info) ? reinterpret_cast<uint64_t>(info.dli_saddr) : 0;
}

// Walks an arm64 frame-pointer chain: each frame record is { previous fp, return address }. With lr (a crash
// context), a leaf function interrupted before or without building a frame record still shows its caller.
static uint32_t sWalkStack(uint64_t pc, uint64_t fp, uint64_t lr, STACKWALKFRAME* frames, uint32_t maxFrames) {
    uint32_t count = 0;
    frames[count].pc = pc;
    frames[count].fp = fp;
    count++;

    uint64_t record[2];
    bool haveRecord = fp && !(fp & 7) && sSafeRead(fp, record, sizeof(record));

    if (lr) {
        lr = sStripCodePointer(lr);
        uint64_t pcStart;
        uint64_t lrStart;
        sGetFunctionStart(pc, &pcStart);
        sGetFunctionStart(lr, &lrStart);

        if (lr != (haveRecord ? sStripCodePointer(record[1]) : 0) && lrStart != pcStart && count < maxFrames) {
            frames[count].pc = lr;
            frames[count].fp = fp;
            count++;
        }
    }

    while (haveRecord && count < maxFrames) {
        uint64_t returnAddress = sStripCodePointer(record[1]);
        uint64_t previous = record[0];

        if (!returnAddress) {
            break;
        }

        frames[count].pc = returnAddress;
        frames[count].fp = previous;
        count++;

        if (!previous || (previous & 7) || previous <= fp) {
            break;
        }

        fp = previous;
        haveRecord = sSafeRead(fp, record, sizeof(record));
    }

    return count;
}

// The symbol of an address. dladdr only knows exported symbols, so for images outside the dyld shared cache (the
// game executable) the full symbol table in __LINKEDIT, local symbols included, is searched too.
static void sFindSymbol(uint64_t address, const Dl_info& info, const char** name, uint64_t* start) {
    *name = info.dli_sname;
    *start = reinterpret_cast<uint64_t>(info.dli_saddr);

    const mach_header_64* header = static_cast<const mach_header_64*>(info.dli_fbase);

    if (!header || header->magic != MH_MAGIC_64 || (header->flags & 0x80000000) /* MH_DYLIB_IN_CACHE */) {
        return;
    }

    const symtab_command* symtab = nullptr;
    const segment_command_64* text = nullptr;
    const segment_command_64* linkedit = nullptr;
    const load_command* command = reinterpret_cast<const load_command*>(header + 1);

    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (command->cmd == LC_SYMTAB) {
            symtab = reinterpret_cast<const symtab_command*>(command);
        } else if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64* segment = reinterpret_cast<const segment_command_64*>(command);

            if (!strcmp(segment->segname, SEG_TEXT)) {
                text = segment;
            } else if (!strcmp(segment->segname, SEG_LINKEDIT)) {
                linkedit = segment;
            }
        }

        command = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }

    if (!symtab || !text || !linkedit) {
        return;
    }

    uint64_t slide = reinterpret_cast<uint64_t>(header) - text->vmaddr;
    const uint8_t* linkeditBase = reinterpret_cast<const uint8_t*>(slide + linkedit->vmaddr - linkedit->fileoff);
    const nlist_64* symbols = reinterpret_cast<const nlist_64*>(linkeditBase + symtab->symoff);
    const char* strings = reinterpret_cast<const char*>(linkeditBase + symtab->stroff);

    for (uint32_t i = 0; i < symtab->nsyms; i++) {
        const nlist_64& symbol = symbols[i];

        if ((symbol.n_type & N_STAB) || (symbol.n_type & N_TYPE) != N_SECT || !symbol.n_un.n_strx || symbol.n_un.n_strx >= symtab->strsize) {
            continue;
        }

        uint64_t value = symbol.n_value + slide;

        if (value <= address && value > *start) {
            *start = value;
            *name = strings + symbol.n_un.n_strx;
        }
    }

    // The nlist names keep the leading underscore of C symbols; dladdr's do not.
    if (*name && **name == '_' && *name != info.dli_sname) {
        ++*name;
    }
}

static void sLogStackFrames(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const STACKWALKFRAME* frames, uint32_t count, uint32_t stackFramesToSkip) {
    for (uint32_t i = stackFramesToSkip; i < count; i++) {
        // Past frame 0 the addresses are return addresses: the call is the instruction before, and after a call
        // to a noreturn function the return address may already be the next function.
        uint64_t lookup = i ? frames[i].pc - 1 : frames[i].pc;
        Dl_info info;

        if (dladdr(reinterpret_cast<void*>(lookup), &info) && info.dli_fname) {
            unsigned long long offset = frames[i].pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
            const char* symbol;
            uint64_t symbolStart;
            sFindSymbol(lookup, info, &symbol, &symbolStart);
            unsigned long long displacement = symbolStart ? frames[i].pc - symbolStart : 0;

            if (!symbol) {
                symbol = "???";
            }
            logLineProc(logLineProcParam, LOG_PTR " " LOG_PTR " %08llX  %s  %s+%llu", static_cast<unsigned long long>(frames[i].pc), static_cast<unsigned long long>(frames[i].fp), offset, sGetPathLeaf(info.dli_fname), symbol, displacement);
        } else {
            logLineProc(logLineProcParam, LOG_PTR " " LOG_PTR " <unknown module>", static_cast<unsigned long long>(frames[i].pc), static_cast<unsigned long long>(frames[i].fp));
        }
    }
}

// Win64: 0x0077DB10
static void sLogManualStackTrace(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint64_t registerPc, uint64_t registerFp, uint64_t registerLr, uint32_t stackFramesToSkip) {
    sLogHeader(logLineProc, logLineProcParam, "Stack Trace (Manual)");
    logLineProc(logLineProcParam, "%s", "Address          Frame            Logical addr  Module  Symbol");
    logLineProc(logLineProcParam, "");

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t numThreads = 0;
    mach_port_t currentThread = pthread_mach_thread_np(pthread_self());

    if (task_threads(mach_task_self(), &threads, &numThreads) != KERN_SUCCESS) {
        threads = nullptr;
        numThreads = 0;
    }

    uint32_t shownThreads = numThreads < 128 ? numThreads : 128;
    logLineProc(logLineProcParam, "Showing %d/%d threads...", shownThreads ? shownThreads : 1, numThreads ? numThreads : 1);

    STACKWALKFRAME frames[100];

    // The current (crashing) thread first.
    logLineProc(logLineProcParam, "");
    logLineProc(logLineProcParam, "--- Thread ID: %d [Current Thread] ---", currentThread);
    sLogStackFrames(logLineProc, logLineProcParam, frames, sWalkStack(registerPc, registerFp, registerLr, frames, 100), stackFramesToSkip);

    uint32_t shown = 1;

    for (mach_msg_type_number_t i = 0; i < numThreads; ++i) {
        if (threads[i] != currentThread && shown < shownThreads) {
            shown++;
            logLineProc(logLineProcParam, "");
            logLineProc(logLineProcParam, "--- Thread ID: %d ---", threads[i]);

            if (thread_suspend(threads[i]) != KERN_SUCCESS) {
                logLineProc(logLineProcParam, "**** Unable to suspend thread");
            } else {
                arm_thread_state64_t state;
                mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;

                if (thread_get_state(threads[i], ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &stateCount) == KERN_SUCCESS) {
                    uint64_t pc = sStripCodePointer(arm_thread_state64_get_pc(state));
                    uint64_t fp = arm_thread_state64_get_fp(state);
                    uint64_t lr = arm_thread_state64_get_lr(state);
                    uint32_t count = sWalkStack(pc, fp, lr, frames, 100);
                    thread_resume(threads[i]);
                    sLogStackFrames(logLineProc, logLineProcParam, frames, count, 0);
                } else {
                    thread_resume(threads[i]);
                    logLineProc(logLineProcParam, "**** Unable to retrieve thread context");
                }
            }
        }

        mach_port_deallocate(mach_task_self(), threads[i]);
    }

    if (threads) {
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), numThreads * sizeof(thread_act_t));
    }
}

// Win64: 0x0077DD50
static void sLogArm64ContextRegisters(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const CONTEXT* context) {
    const __darwin_arm_thread_state64& ss = context->ucontext->uc_mcontext->__ss;
    const __darwin_arm_exception_state64& es = context->ucontext->uc_mcontext->__es;
    sLogHeader(logLineProc, logLineProcParam, "arm64 Registers");

    for (uint32_t i = 0; i < 28; i += 4) {
        logLineProc(logLineProcParam, "X%-2u=%016llX  X%-2u=%016llX  X%-2u=%016llX  X%-2u=%016llX", i, ss.__x[i], i + 1, ss.__x[i + 1], i + 2, ss.__x[i + 2], i + 3, ss.__x[i + 3]);
    }

    logLineProc(logLineProcParam, "X28=%016llX  FP =%016llX  LR =%016llX  SP =%016llX", ss.__x[28], static_cast<unsigned long long>(arm_thread_state64_get_fp(ss)), static_cast<unsigned long long>(arm_thread_state64_get_lr(ss)), static_cast<unsigned long long>(arm_thread_state64_get_sp(ss)));
    logLineProc(logLineProcParam, "PC =%016llX  CPSR=%08X  FAR=%016llX  ESR=%08X", static_cast<unsigned long long>(arm_thread_state64_get_pc(ss)), ss.__cpsr, es.__far, es.__esr);
    logLineProc(logLineProcParam, "");
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
    // No minidumps on macOS (LogMiniDumpIsAvailable is 0): the system crash report takes their place.
    return 0;
}

// Win64: 0x0077E0E0
int32_t LogMiniDumpIsAvailable() {
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

            if (context) {
                logOptions |= LOG_OPTION_REGISTERS | LOG_OPTION_CODE;
            }
        }

        uint64_t registerPc;
        uint64_t registerFp;
        uint64_t registerSp;
        uint64_t registerLr;

        if (context) {
            const __darwin_arm_thread_state64& ss = context->ucontext->uc_mcontext->__ss;
            registerPc = sStripCodePointer(arm_thread_state64_get_pc(ss));
            registerFp = arm_thread_state64_get_fp(ss);
            registerSp = arm_thread_state64_get_sp(ss);
            registerLr = arm_thread_state64_get_lr(ss);
            stackFramesToSkip = 0;
        } else {
            sCaptureCallerFrame(&registerPc, &registerFp);
            registerSp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
            registerLr = 0;
            logOptions &= ~static_cast<uint32_t>(LOG_OPTION_REGISTERS | LOG_OPTION_CODE);
            stackFramesToSkip = stackFramesToSkip + 1;
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);

        if ((logOptions & LOG_OPTION_REGISTERS) && context) {
            sLogArm64ContextRegisters(logLineProc, logLineProcParam, context);
        }

        if (logOptions & (LOG_OPTION_MANUALSTACK | LOG_OPTION_DBGHELPSTACK)) {
            sLogManualStackTrace(logLineProc, logLineProcParam, registerPc, registerFp, registerLr, stackFramesToSkip);
        }

        if (logOptions & LOG_OPTION_MODULES) {
            sLogLoadedModules(logLineProc, logLineProcParam);
        }

        if (logOptions & (LOG_OPTION_CODE | LOG_OPTION_STACK)) {
            sLogMemory(logOptions, logLineProc, logLineProcParam, reinterpret_cast<uint8_t*>(registerPc), reinterpret_cast<uint8_t*>(registerSp));
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
        sShowOutOfMemory(logLineProc, logLineProcParam);
    }

    InterlockedDecrement(&sgRecursionLevel);
}

void MacMachineStatePrepare() {
    passwd* entry = getpwuid(getuid());
    SStrCopy(s_userName, entry && entry->pw_name ? entry->pw_name : "<unknown>", sizeof(s_userName));

    char path[MAX_PATH];
    MacGetModuleFileName(path, sizeof(path));

    void* frames[4];
    backtrace(frames, 4);
    Dl_info info;
    dladdr(reinterpret_cast<void*>(&MacMachineStatePrepare), &info);

    tzset();
}
