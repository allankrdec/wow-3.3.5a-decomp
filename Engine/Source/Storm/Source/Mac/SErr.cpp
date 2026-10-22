// macOS counterpart of Storm/Source/Win64/SErr.cpp. Error state, handlers, error text and the error log files
// (<game folder>/Errors/) work; the error dialog, the unhandled-exception filter and minidumps belong to the crash
// handler (stage E7). Until then an error is reported on stderr and in the log, and the dialog's answer is taken
// as "keep running" for recoverable errors, and for the others "break to the debugger" when one is attached,
// "terminate" otherwise.

#include "Engine/Source/Base/Platform/Mac/TodoMac.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <malloc.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/MachineState.h"

#define STORM_FACILITY 0x510
#define SERR_STRING_BASE 0x5100

// DirectDraw and DirectSound facilities, registered like on Windows.
#define _FACDD 0x876
#define _FACDS 0x878

#define SERR_STR_ERROR 0
#define SERR_STR_CRITICAL 1
#define SERR_STR_PROGRAM 2
#define SERR_STR_FILE_LINE 3
#define SERR_STR_FUNCTION 4
#define SERR_STR_OBJECT 5
#define SERR_STR_HANDLE 6
#define SERR_STR_EXPRESSION 7
#define SERR_STR_DESCRIPTION 8
#define SERR_STR_TERMINATE 9
#define SERR_STR_KEEP_RUNNING 10
#define SERR_STR_FILE 11
#define SERR_STR_BREAK 12
#define SERR_STR_BREAK_OR_RUN 13
#define SERR_STR_EXCEPTION 14
#define SERR_STR_ACCESS_VIOLATION 15
#define SERR_STR_READ 16
#define SERR_STR_WRITTEN 17

// MessageBox results.
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7

template <class T, class TGetLink = TSGetLink<T> >
class TSListWinHeap : public TSList<T, TGetLink> {
    public:
    T* NewNode(uint32_t location, size_t extrabytes, uint32_t flags);
};

struct APPFATINFO {
    /* 0000 */ const char* filename;
    /* 0004 */ int32_t linenumber;
    /* 0008 */ uint32_t threadId;
};

struct HANDLER : TSLinkedNode<HANDLER> {
    /* 0008 */ int32_t (STORMAPI* handler)(uint32_t, const char*, const char*, int32_t, const char*);
};

struct _MSGSRC {
    /* 0000 */ uint16_t facility;
    /* 0002 */ uint16_t reserved;
    /* 0004 */ HINSTANCE module;
    /* 0008 */ _MSGSRC* next;
};

static const char* const s_displaystr[20] = {
    "ERROR #%u (0x%08x) %s",
    "This application has encountered a critical error:\n\n%s\n",
    "Program:\t%s\n",
    "File:\t%s\nLine:\t%d\n",
    "Function:\t%s\n",
    "Object:\t%s\n",
    "Handle:\t%s\n",
    "Expr:\t%s\n\n",
    "\n%s\n\n",
    "Press OK to terminate the application.",
    "Do you wish to keep running anyway?\n\tOK:\tKeep running\n\tCancel:\tTerminate application",
    "File:\t%s\n",
    "Do you wish to break to the debugger?\n\tYes:\tBreak to debugger\n\tNo:\tTerminate application",
    "Do you wish to break to the debugger?\n\tYes:\tBreak to debugger\n\tNo:\tTerminate application\n\tCancel:\tKeep running",
    "Exception:\t%s\n",
    "The instruction at \"0x%p\" referenced memory at \"0x%p\".\nThe memory could not be \"%s\".",
    "read",
    "written",
    "Couldn't locate the \"DbgHelp.dll\" debugging DLL.\n\nSome debugging information will be missing from error log files. Press OK to continue, or press Cancel to terminate the program.",
    "Missing Debugging DLL"
};

static LONG s_critsectinit = -1;

static TSListWinHeap<HANDLER> s_handlerlist;

static EXCEPTION_POINTERS* s_exceptionPointers;

static CRITICAL_SECTION s_exceptioncritsect;

static int32_t s_displaying;

static CRITICAL_SECTION s_critsect;

static APPFATINFO s_appFatInfo;

static _MSGSRC* s_msgsrchead;

static int32_t s_msgsrcinit;

static uint32_t s_lasterror;

static char s_logtitle[128];

static int32_t (STORMAPI* s_logCallback)(char*, uint32_t);

static uint32_t s_noMiniDumps;

static char s_logLastFile[MAX_PATH];

template <class T, class TGetLink>
T* TSListWinHeap<T, TGetLink>::NewNode(uint32_t location, size_t extrabytes, uint32_t flags) {
    void* memory = calloc(1, sizeof(T) + extrabytes);

    if (!memory) {
        abort();
    }

    T* node = new (memory) T;
    memset(node, 0, sizeof(T) + extrabytes);

    if (location) {
        this->LinkNode(node, location, nullptr);
    }

    return node;
}

// Win64: 0x00427010
static void Breakpoint() {
    if (MacIsDebuggerPresent()) {
        __builtin_debugtrap();
    }
}

// Win64: 0x00770F70
static const char* GetErrorString(uint32_t id) {
    // No string table resources on macOS.
    return s_displaystr[id];
}

// Win64: 0x00770FA0
static void AddStormFacility(uint16_t facility) {
    _MSGSRC** tail = &s_msgsrchead;

    for (_MSGSRC* src = s_msgsrchead; src; src = src->next) {
        tail = &src->next;
    }

    *tail = static_cast<_MSGSRC*>(calloc(1, sizeof(_MSGSRC)));
    (*tail)->facility = facility;
    (*tail)->module = StormGetInstance();
    (*tail)->next = nullptr;
}

inline void AddStormMessages() {
    AddStormFacility(STORM_FACILITY);
    AddStormFacility(_FACDD);
    AddStormFacility(_FACDS);
}

// Win64: 0x00770FF0
static void InternalEnterCriticalSection(CRITICAL_SECTION* critsect) {
    if (InterlockedIncrement(&s_critsectinit)) {
        InterlockedDecrement(&s_critsectinit);
    } else {
        MacCriticalSectionInit(&s_critsect);
        MacCriticalSectionInit(&s_exceptioncritsect);
    }

    MacCriticalSectionEnter(critsect);
}

inline void InternalLeaveCriticalSection(CRITICAL_SECTION* critsect) {
    MacCriticalSectionLeave(critsect);
}

// Win64: 0x00771040
void SErrInitialize() {
    g_opt.serrleaksilentwarning = 1;
    SRegLoadValue("Internal", "No Minidumps", 0, &s_noMiniDumps);
}

// Win64: 0x00771070
static int32_t CanBreakToDebugger() {
    return MacIsDebuggerPresent();
}

static void WriteFileText(int fd, const char* text, size_t length) {
    while (length) {
        ssize_t written = write(fd, text, length);

        if (written <= 0) {
            break;
        }

        text += written;
        length -= written;
    }
}

// Win64: 0x007710B0
static void WriteLine(void* param, const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 3, format ? format : "", args);
    va_end(args);
    buffer[sizeof(buffer) - 3] = 0;
    SStrPack(buffer, "\r\n", sizeof(buffer));

    WriteFileText(static_cast<int>(reinterpret_cast<intptr_t>(param)), buffer, SStrLen(buffer));
}

// Win64: 0x00771130
static void WriteMessageToLog(int logfile, const char* message) {
    uint32_t length = static_cast<uint32_t>(SStrLen(message));
    char* buffer = static_cast<char*>(alloca(2 * length + 4));
    char prevc = 0;
    char c = 0;
    uint32_t pos = 2;
    buffer[0] = '\r';
    buffer[1] = '\n';

    for (uint32_t i = 0; i < length; ++i) {
        c = message[i];

        if (c == '\n' && prevc != '\r') {
            buffer[pos++] = '\r';
        }

        buffer[pos++] = c;
        prevc = c;
    }

    if (!length || c != '\n') {
        buffer[pos++] = '\r';
        buffer[pos++] = '\n';
    }

    buffer[pos] = 0;

    WriteFileText(logfile, buffer, pos);
}

inline int32_t MakeDirectory(const char* path) {
    return MacCreateDirectories(path);
}

// Win64: 0x007711C0
static int CreateErrorLogFile(const char* suffix, const char* ext, char* logpath, uint32_t logpathchars, const tm* time) {
    char logfilename[MAX_PATH];

    if (!suffix) {
        suffix = " Log";
    }

    MacGetModuleDirectory(logpath, logpathchars);

    char* slash = &logpath[SStrLen(logpath) - 1];

    SStrPack(logpath, "Errors/", logpathchars);

    if (!MakeDirectory(logpath)) {
        slash[1] = 0;
    }

    snprintf(logfilename, sizeof(logfilename), "%04d-%02d-%02d %02d.%02d.%02d %s.%s", time->tm_year + 1900, time->tm_mon + 1, time->tm_mday, time->tm_hour, time->tm_min, time->tm_sec, suffix, ext);
    SStrPack(logpath, logfilename, logpathchars);

    return open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

// Win64: 0x007712D0
static int CreateErrorLog(const char* suffix, const char* message, const tm* time) {
    char logpath[MAX_PATH];
    tm localTime;

    if (!time) {
        time_t now = ::time(nullptr);
        localtime_r(&now, &localTime);
        time = &localTime;
    }

    int logfile = CreateErrorLogFile(suffix, "txt", logpath, sizeof(logpath), time);

    if (logfile < 0) {
        return logfile;
    }

    LogComputerInfoHeader(LOG_OPTION_DEFAULT, WriteLine, reinterpret_cast<void*>(static_cast<intptr_t>(logfile)), s_logtitle, nullptr);
    WriteMessageToLog(logfile, message);

    if (s_logCallback) {
        char* buffer = static_cast<char*>(alloca(5200));
        buffer[0] = 0;
        s_logCallback(buffer, 5200);

        if (strlen(buffer)) {
            WriteMessageToLog(logfile, buffer);
        }
    }

    SStrCopy(s_logLastFile, logpath, sizeof(s_logLastFile));

    return logfile;
}

// Win64: 0x007713B0
static const char* SErrGetErrorText(uint32_t errorcode) {
    switch (errorcode) {
        case STORM_ERROR_ASSERTION:
            return "Assertion Failure";
        case STORM_ERROR_NOT_ARCHIVE:
            return "Not Archive";
        case STORM_ERROR_VERSION_MISMATCH:
            return "Version Mismatch";
        case STORM_ERROR_MEM_NOT_ALLOCATED:
            return "Memory Already Freed";
        case STORM_ERROR_MEM_CORRUPTED:
            return "Memory Corrupt";
        case STORM_ERROR_MEM_INVALID:
            return "Memory Invalid Block";
        case STORM_ERROR_MEM_NOT_FREED:
            return "Memory Never Released";
        case STORM_ERROR_RESOURCES_NOT_RELEASED:
            return "Handle Never Released";
        case STORM_ERROR_OUT_OF_BOUNDS:
            return "Access Out Of Bounds";
        case STORM_ERROR_FILE_CORRUPTED:
            return "File Corrupt";
        case STORM_ERROR_FATAL:
            return "Fatal Exception";
        case STORM_ERROR_FATAL_CONDITION:
            return "Fatal Condition";
        default:
            return "";
    }
}

// The FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM) text of the Win32 error codes Storm reports.
static const char* GetSystemErrorText(uint32_t errorcode) {
    switch (errorcode) {
        case 0:
            return "The operation completed successfully.\r\n";
        case 2:
            return "The system cannot find the file specified.\r\n";
        case 3:
            return "The system cannot find the path specified.\r\n";
        case 5:
            return "Access is denied.\r\n";
        case 6:
            return "The handle is invalid.\r\n";
        case 8:
            return "Not enough storage is available to process this command.\r\n";
        case 87:
            return "The parameter is incorrect.\r\n";
        case 110:
            return "The system cannot open the device or file specified.\r\n";
        case 112:
            return "There is not enough space on the disk.\r\n";
        default:
            return nullptr;
    }
}

// Win64: 0x007717E0
uint32_t STORMAPI SErrGetLastError() {
    return s_lasterror;
}

// Win64: 0x007717F0
int32_t STORMAPI SErrIsDisplayingError() {
    return s_displaying;
}

// Win64: 0x00771800
void STORMAPI SErrPrepareAppFatal(const char* filename, int32_t linenumber) {
    InternalEnterCriticalSection(&s_critsect);
    s_appFatInfo.filename = filename;
    s_appFatInfo.linenumber = linenumber;
    s_appFatInfo.threadId = SGetCurrentThreadId();
    InternalLeaveCriticalSection(&s_critsect);
}

// Win64: 0x00771870
void STORMAPI SErrSetLastError(uint32_t errorcode) {
    s_lasterror = errorcode;
}

// Win64: 0x00771890
void STORMAPI SErrSetLogTitleString(const char* title) {
    InternalEnterCriticalSection(&s_critsect);
    SStrCopy(s_logtitle, title, sizeof(s_logtitle));
    InternalLeaveCriticalSection(&s_critsect);
}

// Win64: 0x00771900
void STORMAPI SErrSetLogCallback(int32_t (STORMAPI* callback)(char*, uint32_t)) {
    InternalEnterCriticalSection(&s_critsect);
    s_logCallback = callback;
    InternalLeaveCriticalSection(&s_critsect);
}

// Win64: 0x00771960
int32_t STORMAPI SErrGetLogLastPath(char* buf, uint32_t size) {
    InternalEnterCriticalSection(&s_critsect);
    int32_t result = 0;

    if (s_logLastFile[0]) {
        SStrCopy(buf, s_logLastFile, size);
        result = 1;
    }

    InternalLeaveCriticalSection(&s_critsect);
    return result;
}

// Win64: 0x007719E0
static int32_t UndecorateObjectName(const char* source, char* dest, uint32_t destchars) {
    if (SStrLen(source) < 6) {
        return 0;
    }

    if (source[0] != '.') {
        return 0;
    }

    if (source[3] != 'U') {
        return 0;
    }

    if (!strpbrk(source + 4, "?@")) {
        return 0;
    }

    SStrCopy(dest, source + 4, destchars);

    char* end = strpbrk(dest, "?@");

    if (!end) {
        return 0;
    }

    *end = 0;

    for (char* c = end - 1; c >= dest && *c == '_'; --c) {
        *c = 0;
    }

    SStrPack(dest, " (", destchars);
    SStrPack(dest, source, destchars);
    SStrPack(dest, ")", destchars);

    return 1;
}

// Win64: 0x00771A80
int32_t STORMAPI SErrGetErrorStr(uint32_t errorcode, char* buffer, uint32_t bufferchars) {
    if (!buffer || !bufferchars) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    InternalEnterCriticalSection(&s_critsect);

    if (!s_msgsrcinit) {
        s_msgsrcinit = 1;
        AddStormMessages();
    }

    HINSTANCE module = nullptr;

    for (_MSGSRC* src = s_msgsrchead; src; src = src->next) {
        if (src->facility == ((errorcode >> 16) & 0xFFF)) {
            module = src->module;
            break;
        }
    }

    InternalLeaveCriticalSection(&s_critsect);
    *buffer = 0;

    if (errorcode == 108) {
        SStrPrintf(buffer, bufferchars, "%s", "Invalid or corrupt archive.\n");
        return 1;
    }

    // The Storm, DirectDraw and DirectSound message tables are not available on macOS: callers fall back to the
    // generic "ERROR #" text, as they do on Windows for codes missing from the tables.
    const char* text = module ? nullptr : GetSystemErrorText(errorcode);

    if (!text) {
        return 0;
    }

    SStrCopy(buffer, text, bufferchars);
    return 1;
}

// Win64: 0x00771B80
void STORMAPI SErrRegisterHandler(int32_t (STORMAPI* handler)(uint32_t, const char*, const char*, int32_t, const char*)) {
    InternalEnterCriticalSection(&s_critsect);
    s_handlerlist.NewNode(1, 0, 0x8000000)->handler = handler;
    InternalLeaveCriticalSection(&s_critsect);
}

// Win64: 0x00771C90
int32_t STORMAPI SErrDestroy() {
    s_handlerlist.Clear();

    _MSGSRC* src = s_msgsrchead;
    s_msgsrcinit = 0;

    while (src) {
        _MSGSRC* next = src->next;
        free(src);
        src = next;
        s_msgsrchead = next;
    }

    if (s_critsectinit != -1) {
        MacCriticalSectionDelete(&s_critsect);
        MacCriticalSectionDelete(&s_exceptioncritsect);
        s_critsectinit = -1;
    }

    return 1;
}

// Win64: 0x00771D10
int32_t STORMAPI SErrDisplayError(uint32_t errorcode, const char* filename, int32_t linenumber, const char* description, int32_t recoverable, uint32_t exitcode, uint32_t unknown) {
    static int32_t lastlinenumber;
    static const char* lastfilename;

    if (g_opt.unk_0028) {
        if (filename == lastfilename && linenumber == lastlinenumber) {
            return 1;
        }

        lastfilename = filename;
        lastlinenumber = linenumber;
    }

    if (s_displaying) {
        return 0;
    }

    s_displaying = 1;

    static char appfilename[MAX_PATH];
    static char appname[MAX_PATH];
    appfilename[0] = 0;
    appname[0] = 0;
    MacGetModuleFileName(appfilename, sizeof(appfilename));

    const char* leaf = SStrChrR(appfilename, '/');
    SStrCopy(appname, leaf ? leaf + 1 : appfilename, sizeof(appname));

    if (SStrChrR(appname, '.')) {
        *SStrChrR(appname, '.') = 0;
    }

    static char errorstr[256];
    errorstr[0] = 0;
    SErrGetErrorStr(errorcode, errorstr, sizeof(errorstr));

    if (!errorstr[0]) {
        SStrPrintf(errorstr, sizeof(errorstr), GetErrorString(SERR_STR_ERROR), errorcode, errorcode, SErrGetErrorText(errorcode));
    }

    static const char* localnameptr = filename;
    static char localnamebuffer[256];

    if (localnameptr && *localnameptr && (linenumber == SLOG_OBJECT || linenumber == SLOG_HANDLE) && UndecorateObjectName(localnameptr, localnamebuffer, sizeof(localnamebuffer))) {
        localnameptr = localnamebuffer;
    }

    static char outstr[2048];
    static char* outptr = outstr;
    static char* outend = outstr + sizeof(outstr);

    if (localnameptr) {
        outptr += SStrCopy(outptr, localnameptr, outend - outptr);
    }

    if (linenumber > 0) {
        SStrPrintf(outptr, outend - outptr, "(%u)", linenumber);
        outptr += SStrLen(outptr);
    }

    SStrPrintf(outptr, outend - outptr, " : error %u: ", errorcode);
    outptr += SStrLen(outptr);
    SStrCopy(outptr, errorstr, outend - outptr);
    SDebugPrintf("%s\n", outstr);

    outptr = outstr;
    SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_CRITICAL), errorstr);
    outptr += SStrLen(outptr);
    SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_PROGRAM), appfilename);
    outptr += SStrLen(outptr);

    if (localnameptr && *localnameptr) {
        switch (linenumber) {
            case SLOG_EXCEPTION:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_EXCEPTION), localnameptr);
                break;
            case SLOG_FILE:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_FILE), localnameptr);
                break;
            case SLOG_HANDLE:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_HANDLE), localnameptr);
                break;
            case SLOG_OBJECT:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_OBJECT), localnameptr);
                break;
            case SLOG_FUNCTION:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_FUNCTION), localnameptr);
                break;
            default:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_FILE_LINE), localnameptr, linenumber);
                break;
        }

        outptr += SStrLen(outptr);
    }

    if (errorcode == STORM_ERROR_ASSERTION) {
        SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_EXPRESSION), description ? description : "");
    } else {
        SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_DESCRIPTION), description ? description : "");
    }

    outptr += SStrLen(outptr);

    if (!g_opt.serrsuppresslogs) {
        LoadMachineStateSymbols();
        InternalEnterCriticalSection(&s_exceptioncritsect);

        static EXCEPTION_POINTERS* exceptionPointers = s_exceptionPointers;
        s_exceptionPointers = nullptr;
        InternalLeaveCriticalSection(&s_exceptioncritsect);

        // Crash contexts come from the exception handler (stage E7); until then every log is an "Error" log.
        CONTEXT* context = nullptr;

        static tm time;
        time_t now = ::time(nullptr);
        localtime_r(&now, &time);

        const char* suffix = context ? "Crash" : "Error";
        int logfile = CreateErrorLog(suffix, outstr, &time);

        if (logfile >= 0) {
            LogMachineState(LOG_OPTION_DEFAULT | LOG_OPTION_FRAMEPARAMS, WriteLine, reinterpret_cast<void*>(static_cast<intptr_t>(logfile)), context ? 0 : 2, context);
            close(logfile);
        }

        char* details = SStrStr(outstr, "Debug Details:");

        if (details) {
            outptr = details;
        }

        if (LogMiniDumpIsAvailable() && !s_noMiniDumps) {
            static char logpath[MAX_PATH];
            int dumpfile = CreateErrorLogFile(suffix, "dmp", logpath, sizeof(logpath), &time);

            if (dumpfile >= 0) {
                const char* title = s_logtitle;
                LogMiniDump(reinterpret_cast<void*>(static_cast<intptr_t>(dumpfile)), exceptionPointers, 1, &title);
                close(dumpfile);
            }
        }

        UnloadMachineStateSymbols();
    }

    if (g_opt.unk_0028) {
        s_displaying = 0;
        return 1;
    }

    int32_t defaultresult;
    int32_t keeprunning;
    int32_t breakresult;

    if (!CanBreakToDebugger()) {
        if (recoverable) {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_KEEP_RUNNING), outend - outptr);
            defaultresult = IDCANCEL;
            keeprunning = IDOK;
            breakresult = 0;
        } else {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_TERMINATE), outend - outptr);
            defaultresult = IDOK;
            keeprunning = 0;
            breakresult = 0;
        }
    } else {
        if (!recoverable) {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_BREAK), outend - outptr);
            defaultresult = IDNO;
            keeprunning = 0;
            breakresult = IDYES;
        } else {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_BREAK_OR_RUN), outend - outptr);
            defaultresult = IDNO;
            keeprunning = IDCANCEL;
            breakresult = IDYES;
        }
    }

    InternalEnterCriticalSection(&s_critsect);

    int32_t result = keeprunning ? keeprunning : defaultresult;
    HANDLER* handler = s_handlerlist.Head();

    while (handler) {
        if (!handler->handler(errorcode, errorstr, localnameptr, linenumber, description ? description : "")) {
            break;
        }

        handler = s_handlerlist.Next(handler);
    }

    if (!handler) {
        // The error dialog is stage E7: print the message and answer for the user.
        TODO_MAC();
        fprintf(stderr, "%s\n%s\n", appname, outstr);
        result = recoverable ? keeprunning : (breakresult ? breakresult : defaultresult);
    }

    InternalLeaveCriticalSection(&s_critsect);

    if (result == breakresult) {
        Breakpoint();
        s_displaying = 0;
        return 1;
    }

    if (result == keeprunning) {
        s_displaying = 0;
        return 1;
    }

    fflush(nullptr);
    _exit(exitcode);
}

// Win64: 0x00772670
void STORMAPI SErrReportNamedResourceLeak(const char* handlename, const char* resourcename) {
    static int32_t checked;
    static uint32_t debugmode;

    if (!checked) {
        checked = 1;
        SRegLoadValue("Internal", "Debug Memory", 0, &debugmode);
    }

    if (!debugmode) {
        return;
    }

    char errormessage[256];

    if (resourcename && *resourcename) {
        SStrPrintf(errormessage, sizeof(errormessage), "%s (%s)", handlename, resourcename);
    } else {
        SStrPrintf(errormessage, sizeof(errormessage), "%s", handlename);
    }

    if (g_opt.serrleaksilentwarning) {
        char szMessage[200];
        SStrPrintf(szMessage, sizeof(szMessage), "Storm Error : handle never released -- %s\n", errormessage);
        MacOutputDebugString(szMessage);
    } else {
        SErrDisplayError(STORM_ERROR_RESOURCES_NOT_RELEASED, errormessage, SLOG_HANDLE, nullptr, 1, 1, 0x11111111);
    }
}

// Win64: 0x007729B0
void SErrDisplayAppFatalCustomV(uint32_t errorcode, const char* format, va_list args) {
    const char* filename = nullptr;
    int32_t linenumber = 0;

    InternalEnterCriticalSection(&s_critsect);

    if (s_appFatInfo.threadId == SGetCurrentThreadId()) {
        filename = s_appFatInfo.filename;
        linenumber = s_appFatInfo.linenumber;
        s_appFatInfo.filename = nullptr;
        s_appFatInfo.linenumber = 0;
        s_appFatInfo.threadId = 0;
    }

    InternalLeaveCriticalSection(&s_critsect);

    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    buffer[sizeof(buffer) - 1] = 0;
    SErrDisplayError(errorcode, filename, linenumber, buffer, 0, 1, 0x11111111);
    StormExitProcess(1);
}

// Win64: 0x00772A80
void SErrDisplayAppFatalCustom(uint32_t errorcode, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(errorcode, format, args);
}

// Win64: 0x00772AA0
void SErrDisplayAppFatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(STORM_ERROR_FATAL_CONDITION, format, args);
}

// Win64: 0x00772AC0
int32_t SErrDisplayErrorFmt(uint32_t errorcode, const char* filename, int32_t linenumber, int32_t recoverable, uint32_t exitcode, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;

    return SErrDisplayError(errorcode, filename, linenumber, buffer, recoverable, exitcode, 0x11111111);
}

// Win64: 0x00772B20
void STORMAPI SErrCatchUnhandledExceptions() {
    // Unhandled exception filter (signal handlers + Crash log): stage E7.
    TODO_MAC();
}

// Win64: 0x008889B0
void SErrDisplayAppFatalMessage(const char* message, const char* filename, int32_t linenumber) {
    SErrPrepareAppFatal(filename, linenumber);
    SErrDisplayAppFatal("%s", message);
}
