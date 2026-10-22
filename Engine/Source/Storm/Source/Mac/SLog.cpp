// macOS counterpart of Storm/Source/Win64/SLog.cpp.

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/Source/SLog.h"
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_logsysteminit;

static uintptr_t s_sequence;

static char s_defaultdir[MAX_PATH];

static CRITICAL_SECTION s_defaultdir_critsect;

static _LOG* s_loghead[4];

static CRITICAL_SECTION s_critsect[4];

// The _LOG file is a POSIX descriptor stored in the void* slot.
static void* const INVALID_HANDLE_VALUE = reinterpret_cast<void*>(-1);

static int LogFileDescriptor(void* file) {
    return static_cast<int>(reinterpret_cast<intptr_t>(file));
}

// Win64: 0x007750D0
static uint32_t PathGetRootChars(const char* path) {
    return *path == '/' ? 1 : 0;
}

// Win64: 0x00775140
static void FlushLog(_LOG* logptr) {
    if (logptr->bufferused) {
        int fd = LogFileDescriptor(logptr->file);
        const char* data = logptr->buffer;
        uint32_t remaining = logptr->bufferused;

        while (remaining) {
            ssize_t written = write(fd, data, remaining);

            if (written <= 0) {
                break;
            }

            data += written;
            remaining -= static_cast<uint32_t>(written);
        }

        fsync(fd);
        logptr->bufferused = 0;
        logptr->pendpoint = 0;
    }
}

// Win64: 0x00775190
static _LOG* LockLog(HSLOG log, HLOCKEDLOG* lockedhandle, int32_t createifnecessary) {
    if (!log) {
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    uintptr_t index = reinterpret_cast<uintptr_t>(log) & 3;
    MacCriticalSectionEnter(&s_critsect[index]);
    *lockedhandle = reinterpret_cast<HLOCKEDLOG>(index);

    _LOG** prevptr = &s_loghead[index];

    for (_LOG* logptr = s_loghead[index]; logptr; logptr = logptr->next) {
        if (logptr->log == log) {
            return logptr;
        }

        prevptr = &logptr->next;
    }

    if (!createifnecessary) {
        MacCriticalSectionLeave(&s_critsect[index]);
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    _LOG* logptr = static_cast<_LOG*>(calloc(1, sizeof(_LOG)));
    *prevptr = logptr;

    if (!logptr) {
        MacCriticalSectionLeave(&s_critsect[index]);
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    logptr->log = log;
    logptr->next = nullptr;
    logptr->filename[0] = 0;
    logptr->file = INVALID_HANDLE_VALUE;
    logptr->bufferused = 0;
    logptr->pendpoint = 0;

    return logptr;
}

// Win64: 0x00775250
static void OutputIndent(_LOG* logptr) {
    int32_t indent = logptr->indent;

    if (indent > 0) {
        if (indent >= 128) {
            indent = 128;
        }

        memset(&logptr->buffer[logptr->bufferused], ' ', indent);
        logptr->buffer[logptr->bufferused + indent] = 0;
        logptr->bufferused += indent;
    }
}

static void OutputReturn(_LOG* logptr) {
    strcpy(&logptr->buffer[logptr->bufferused], "\r\n");
    logptr->bufferused += 2;
}

// Win64: 0x007752A0
static void OutputTime(_LOG* logptr, int32_t show) {
    static uint32_t lasttime;
    static char timestr[64];
    static uint32_t timestrlen;

    if (!logptr->timeStamp) {
        return;
    }

    uint32_t time = MacTickCount();

    if (time != lasttime) {
        lasttime = time;

        timeval now;
        gettimeofday(&now, nullptr);
        tm systime;
        localtime_r(&now.tv_sec, &systime);
        snprintf(timestr, sizeof(timestr), "%u/%u %02u:%02u:%02u.%03u  ", systime.tm_mon + 1, systime.tm_mday, systime.tm_hour, systime.tm_min, systime.tm_sec, static_cast<uint32_t>(now.tv_usec / 1000));
        timestrlen = SStrLen(timestr);
    }

    if (show) {
        memcpy(&logptr->buffer[logptr->bufferused], timestr, timestrlen + 1);
    } else {
        memset(&logptr->buffer[logptr->bufferused], ' ', timestrlen);
        logptr->buffer[logptr->bufferused + timestrlen] = 0;
    }

    logptr->bufferused += timestrlen;
}

// Win64: 0x00775380
static void UnlockDeleteLog(_LOG* logptr, HLOCKEDLOG lockedhandle) {
    uintptr_t index = reinterpret_cast<uintptr_t>(lockedhandle);
    _LOG** prevptr = &s_loghead[index];

    for (_LOG* curr = s_loghead[index]; curr; curr = curr->next) {
        if (curr == logptr) {
            *prevptr = curr->next;
            free(curr);
            break;
        }

        prevptr = &curr->next;
    }

    MacCriticalSectionLeave(&s_critsect[index]);
}

static void UnlockLog(HLOCKEDLOG lockedhandle) {
    MacCriticalSectionLeave(&s_critsect[reinterpret_cast<uintptr_t>(lockedhandle)]);
}

// Win64: 0x007753E0
static const char* PrependDefaultDir(char* newfilename, uint32_t newfilenamesize, const char* filename) {
    if (!filename || !*filename || SStrChr(filename, '\\') || SStrChr(filename, '/')) {
        SStrCopy(newfilename, filename ? filename : "", newfilenamesize);
        MacConvertPath(newfilename);
        return newfilename;
    }

    MacCriticalSectionEnter(&s_defaultdir_critsect);

    if (s_defaultdir[0]) {
        size_t chars = SStrCopy(newfilename, s_defaultdir, newfilenamesize);
        SStrCopy(&newfilename[chars], filename, newfilenamesize - chars);
    } else {
        MacGetModuleDirectory(newfilename, newfilenamesize);
        SStrPack(newfilename, filename, newfilenamesize);
    }

    MacCriticalSectionLeave(&s_defaultdir_critsect);

    return newfilename;
}

// Win64: 0x007754A0
void STORMAPI SLogClose(HSLOG log) {
    if (!s_logsysteminit) {
        return;
    }

    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (!logptr) {
        return;
    }

    if (logptr->file != INVALID_HANDLE_VALUE) {
        FlushLog(logptr);
        close(LogFileDescriptor(logptr->file));
    }

    UnlockDeleteLog(logptr, lockedhandle);
}

// Win64: 0x00775500
void STORMAPI SLogFlush(HSLOG log) {
    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (logptr) {
        if (logptr->file != INVALID_HANDLE_VALUE) {
            FlushLog(logptr);
        }

        UnlockLog(lockedhandle);
    }
}

// Win64: 0x00775550
void STORMAPI SLogFlushAll() {
    for (int32_t i = 0; i < 4; i++) {
        MacCriticalSectionEnter(&s_critsect[i]);

        for (_LOG* logptr = s_loghead[i]; logptr; logptr = logptr->next) {
            if (logptr->file != INVALID_HANDLE_VALUE) {
                FlushLog(logptr);
            }
        }

        MacCriticalSectionLeave(&s_critsect[i]);
    }
}

// Win64: 0x007755F0
void STORMAPI SLogInitialize() {
    if (s_logsysteminit) {
        return;
    }

    for (int32_t i = 0; i < 4; i++) {
        MacCriticalSectionInit(&s_critsect[i]);
    }

    MacCriticalSectionInit(&s_defaultdir_critsect);
    s_logsysteminit = 1;
}

static void PathStripFilename(char* path) {
    char* last = SStrChrR(path, '/');

    if (last) {
        char* root = &path[PathGetRootChars(path)];

        if (last >= root) {
            last[1] = 0;
        } else {
            *root = 0;
        }
    } else {
        *path = 0;
    }
}

// Win64: 0x00775630
static int32_t CreateFileDirectory(const char* path) {
    if (!path) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    char buffer[MAX_PATH];
    SStrCopy(buffer, path, sizeof(buffer));
    MacConvertPath(buffer);
    PathStripFilename(buffer);

    if (!buffer[0]) {
        return 1;
    }

    return MacCreateDirectories(buffer);
}

// Win64: 0x00775740
static int32_t OpenLogFile(const char* filename, void** file, uint32_t flags) {
    if (!filename || !*filename) {
        *file = INVALID_HANDLE_VALUE;
        return 0;
    }

    char newfilename[MAX_PATH];
    const char* path = PrependDefaultDir(newfilename, sizeof(newfilename), filename);
    int32_t append = (flags & SLOG_FLAG_APPEND) != 0;
    CreateFileDirectory(path);

    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? 0 : O_TRUNC), 0644);
    *file = fd >= 0 ? reinterpret_cast<void*>(static_cast<intptr_t>(fd)) : INVALID_HANDLE_VALUE;
    int32_t result = *file != INVALID_HANDLE_VALUE;

    if (result && append) {
        lseek(fd, 0, SEEK_END);
    }

    return result;
}

// Win64: 0x007757E0
int32_t STORMAPI SLogCreate(const char* filename, uint32_t flags, HSLOG* log) {
    if (!filename || !*filename || !log) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    *log = nullptr;

    if (flags & SLOG_FLAG_NOFILE) {
        filename = "";
        flags &= ~SLOG_FLAG_OPENFILE;
    }

    void* file = INVALID_HANDLE_VALUE;

    if ((flags & SLOG_FLAG_OPENFILE) && !OpenLogFile(filename, &file, flags)) {
        return 0;
    }

    *log = reinterpret_cast<HSLOG>(++s_sequence);

    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(*log, &lockedhandle, 1);

    if (!logptr) {
        *log = nullptr;
        return 0;
    }

    logptr->file = file;
    SStrCopy(logptr->filename, filename, sizeof(logptr->filename));
    logptr->flags = flags;
    logptr->timeStamp = 1;
    logptr->indent = 0;
    UnlockLog(lockedhandle);

    return 1;
}

// Win64: 0x007758E0
void STORMAPI SLogDestroy() {
    SLogFlushAll();

    for (int32_t i = 0; i < 4; i++) {
        MacCriticalSectionEnter(&s_critsect[i]);

        while (s_loghead[i]) {
            char filename[MAX_PATH];
            SStrCopy(filename, s_loghead[i]->filename, STORM_MAX_STR);
            SLogClose(s_loghead[i]->log);
            SErrReportNamedResourceLeak("HSLOG", filename);
        }

        MacCriticalSectionLeave(&s_critsect[i]);
    }

    s_logsysteminit = 0;

    for (int32_t i = 0; i < 4; i++) {
        MacCriticalSectionDelete(&s_critsect[i]);
    }

    MacCriticalSectionDelete(&s_defaultdir_critsect);
}

static int32_t PrepareLog(_LOG* logptr) {
    if (logptr->file != INVALID_HANDLE_VALUE) {
        return 1;
    }

    if (OpenLogFile(logptr->filename, &logptr->file, logptr->flags)) {
        return 1;
    }

    logptr->filename[0] = 0;
    return 0;
}

// Win64: 0x00775A90
void STORMAPI SLogVWrite(HSLOG log, const char* format, va_list arglist) {
    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (!logptr) {
        return;
    }

    if (PrepareLog(logptr)) {
        OutputTime(logptr, 1);
        OutputIndent(logptr);
        SStrVPrintf(&logptr->buffer[logptr->bufferused], sizeof(logptr->buffer) - logptr->bufferused, format, arglist);
        logptr->bufferused += SStrLen(&logptr->buffer[logptr->bufferused]);
        OutputReturn(logptr);

        if (g_opt.echotooutputdebugstring) {
            MacOutputDebugString(&logptr->buffer[logptr->pendpoint]);
        }

        logptr->pendpoint = logptr->bufferused;

        if (logptr->bufferused >= 0xC000) {
            FlushLog(logptr);
        }
    }

    UnlockLog(lockedhandle);
}

// Win64: 0x00775BB0
void SLogWrite(HSLOG log, const char* format, ...) {
    va_list arglist;
    va_start(arglist, format);
    SLogVWrite(log, format, arglist);
    va_end(arglist);
}
