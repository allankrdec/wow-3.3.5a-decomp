// macOS counterpart of Storm/Source/Win64/SCmd.cpp.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crt_externs.h>
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/Source/SCmd.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/H/storm.h"

#define SCMD_STRING_INVALID_ARGUMENT 0x5201
#define SCMD_STRING_SYNTAX_INCORRECT 0x5202
#define SCMD_STRING_RESPONSE_FILE 0x5203

#define ERROR_OPEN_FAILED 110

static const char* const s_errorstr[3] = {
    "Invalid argument: %s",
    "The syntax of the command is incorrect.",
    "Unable to open response file: %s",
};

// Win64: 0x00772C90
void GenerateError(void (STORMAPI* errorcallback)(_CMDERROR*), uint32_t errorcode, const char* itemstring) {
    uint32_t index;

    switch (errorcode) {
        case STORM_ERROR_BAD_ARGUMENT:
            index = 0;
            break;
        case STORM_ERROR_NOT_ENOUGH_ARGUMENTS:
            index = 1;
            break;
        case ERROR_OPEN_FAILED:
            index = 2;
            break;
        default:
            return;
    }

    // No string table resources on macOS: always the built-in English strings.
    char buffer[256];
    SStrCopy(buffer, s_errorstr[index], sizeof(buffer));

    char errorstr[256];

    if (strstr(buffer, "%s")) {
        snprintf(errorstr, sizeof(errorstr), buffer, itemstring);
    } else {
        SStrCopy(errorstr, buffer, sizeof(errorstr));
    }

    if (errorstr[0]) {
        SStrPack(errorstr, "\n", sizeof(errorstr));
    }

    SErrSetLastError(errorcode);

    _CMDERROR data;
    data.errorcode = errorcode;
    data.itemstr = itemstring;
    data.errorstr = errorstr;
    errorcallback(&data);
}

// Win64: 0x00772DA0
int32_t ProcessFile(const char* filename, _PROCESSING* processing, CMDDEF** nextarg, int32_t (STORMAPI* extracallback)(const char*), void (STORMAPI* errorcallback)(_CMDERROR*)) {
    char path[MAX_PATH];
    SStrCopy(path, filename, sizeof(path));
    MacConvertPath(path);

    FILE* file = fopen(path, "rb");

    if (!file) {
        if (errorcallback) {
            GenerateError(errorcallback, ERROR_OPEN_FAILED, filename);
        }

        return 0;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        size = 0;
    }

    char* data = static_cast<char*>(SMemAlloc(size + 1, __FILE__, __LINE__, 0));
    size_t bytesread = fread(data, 1, size, file);
    fclose(file);
    data[bytesread] = 0;

    const char* curr = data;
    int32_t result = ProcessString(&curr, processing, nextarg, extracallback, errorcallback);
    SMemFree(data, __FILE__, __LINE__, 0);

    return result;
}

// GetCommandLine: the process arguments joined back into one Windows-style command line, quoting the arguments
// that contain blanks.
static const char* GetCommandLine() {
    static char* s_commandline;

    if (s_commandline) {
        return s_commandline;
    }

    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    size_t length = 1;

    for (int i = 0; i < argc; i++) {
        length += strlen(argv[i]) + 3;
    }

    char* commandline = static_cast<char*>(malloc(length));
    char* curr = commandline;

    for (int i = 0; i < argc; i++) {
        bool quote = !argv[i][0] || strpbrk(argv[i], " \t");

        if (i) {
            *curr++ = ' ';
        }

        if (quote) {
            *curr++ = '"';
        }

        size_t arglength = strlen(argv[i]);
        memcpy(curr, argv[i], arglength);
        curr += arglength;

        if (quote) {
            *curr++ = '"';
        }
    }

    *curr = 0;
    s_commandline = commandline;

    return s_commandline;
}

// Win64: 0x00773990
int32_t STORMAPI SCmdProcessCommandLine(int32_t (STORMAPI* extracallback)(const char*), void (STORMAPI* errorcallback)(_CMDERROR*)) {
    return SCmdProcess(GetCommandLine(), 1, extracallback, errorcallback);
}
