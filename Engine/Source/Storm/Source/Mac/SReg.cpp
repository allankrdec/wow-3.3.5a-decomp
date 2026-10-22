// macOS counterpart of Storm/Source/Win64/SReg.cpp.
//
// The registry is a text file, ~/Library/Application Support/wowdecomp/Registry.ini (falls back to the working
// directory when $HOME is unset). Each key is a section named after its full Windows path, rooted at
// HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE; each value is "name=sz:text" or "name=dword:number". Backslash,
// newline, carriage return and '=' are escaped with a backslash in names and values. Keys and value names compare
// case-insensitively like the registry. The file is read on first use and rewritten (through a temporary file)
// on every save.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <pthread.h>
#include <string>
#include <vector>
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"

#define REG_SZ 1
#define REG_DWORD 4

#define REG_ROOT_CURRENT_USER 0
#define REG_ROOT_LOCAL_MACHINE 1

struct REGVALUE {
    int32_t root;
    std::string key;
    std::string name;
    uint32_t type;
    std::string string;
    uint32_t dword;
};

static pthread_mutex_t s_regMutex = PTHREAD_MUTEX_INITIALIZER;

static bool s_regLoaded;

static std::vector<REGVALUE>* s_regValues;

static const char* const s_regRootNames[2] = { "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE" };

static void GetRegistryPath(char* buffer, uint32_t bufferchars, bool createDirectory) {
    const char* home = getenv("HOME");

    if (!home || !*home) {
        SStrCopy(buffer, "Registry.ini", bufferchars);
        return;
    }

    snprintf(buffer, bufferchars, "%s/Library/Application Support/wowdecomp", home);

    if (createDirectory) {
        MacCreateDirectories(buffer);
    }

    SStrPack(buffer, "/Registry.ini", bufferchars);
}

static std::string Escape(const std::string& text) {
    std::string result;

    for (char c : text) {
        switch (c) {
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '=':
                result += "\\=";
                break;
            default:
                result += c;
                break;
        }
    }

    return result;
}

// Unescapes text up to the first unescaped stop character (0 for none); returns the position after it.
static const char* Unescape(const char* text, char stop, std::string* result) {
    result->clear();

    while (*text && *text != stop) {
        if (*text == '\\' && text[1]) {
            text++;
            result->push_back(*text == 'n' ? '\n' : (*text == 'r' ? '\r' : *text));
        } else {
            result->push_back(*text);
        }

        text++;
    }

    return *text ? text + 1 : text;
}

static void LoadRegistry() {
    if (s_regLoaded) {
        return;
    }

    s_regLoaded = true;
    s_regValues = new std::vector<REGVALUE>;

    char path[MAX_PATH * 2];
    GetRegistryPath(path, sizeof(path), false);

    FILE* file = fopen(path, "r");

    if (!file) {
        return;
    }

    int32_t root = -1;
    std::string key;
    char line[8192];

    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);

        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = 0;
        }

        if (line[0] == '[') {
            char* end = strrchr(line, ']');
            char* slash = strchr(line, '\\');
            root = -1;

            if (!end || !slash || slash > end) {
                continue;
            }

            *end = 0;
            *slash = 0;

            for (int32_t i = 0; i < 2; i++) {
                if (!strcasecmp(line + 1, s_regRootNames[i])) {
                    root = i;
                }
            }

            key = slash + 1;
            continue;
        }

        if (root < 0 || !line[0] || line[0] == ';') {
            continue;
        }

        REGVALUE value;
        value.root = root;
        value.key = key;
        value.dword = 0;

        const char* data = Unescape(line, '=', &value.name);

        if (!strncmp(data, "sz:", 3)) {
            value.type = REG_SZ;
            Unescape(data + 3, 0, &value.string);
        } else if (!strncmp(data, "dword:", 6)) {
            value.type = REG_DWORD;
            value.dword = static_cast<uint32_t>(strtoul(data + 6, nullptr, 0));
        } else {
            continue;
        }

        s_regValues->push_back(value);
    }

    fclose(file);
}

static bool SaveRegistry() {
    char path[MAX_PATH * 2];
    GetRegistryPath(path, sizeof(path), true);

    char temppath[MAX_PATH * 2 + 8];
    snprintf(temppath, sizeof(temppath), "%s.tmp", path);

    FILE* file = fopen(temppath, "w");

    if (!file) {
        return false;
    }

    std::vector<bool> written(s_regValues->size());

    for (size_t i = 0; i < s_regValues->size(); i++) {
        if (written[i]) {
            continue;
        }

        const REGVALUE& section = (*s_regValues)[i];
        fprintf(file, "[%s\\%s]\n", s_regRootNames[section.root], section.key.c_str());

        for (size_t j = i; j < s_regValues->size(); j++) {
            const REGVALUE& value = (*s_regValues)[j];

            if (written[j] || value.root != section.root || strcasecmp(value.key.c_str(), section.key.c_str())) {
                continue;
            }

            written[j] = true;

            if (value.type == REG_SZ) {
                fprintf(file, "%s=sz:%s\n", Escape(value.name).c_str(), Escape(value.string).c_str());
            } else {
                fprintf(file, "%s=dword:%u\n", Escape(value.name).c_str(), value.dword);
            }
        }

        fprintf(file, "\n");
    }

    bool result = fflush(file) == 0;
    result = fclose(file) == 0 && result;

    return result && rename(temppath, path) == 0;
}

static REGVALUE* FindValue(int32_t root, const char* key, const char* name) {
    for (REGVALUE& value : *s_regValues) {
        if (value.root == root && !strcasecmp(value.key.c_str(), key) && !strcasecmp(value.name.c_str(), name)) {
            return &value;
        }
    }

    return nullptr;
}

int32_t STORMAPI SRegGetBaseKey(uint32_t flags, char* buffer, uint32_t bufferchars) {
    if (flags & SREG_BATTLE_NET) {
        SStrCopy(buffer, "Software\\Battle.net\\", bufferchars);
    } else {
        SStrCopy(buffer, "Software\\Blizzard Entertainment\\", bufferchars);
    }

    return 1;
}

static void BuildFullKeyName(const char* keyname, uint32_t flags, char* buffer, uint32_t bufferchars) {
    *buffer = 0;

    if (!(flags & SREG_ABSOLUTE)) {
        SRegGetBaseKey(flags, buffer, bufferchars);
    }

    SStrPack(buffer, keyname, bufferchars);
}

// Win64: 0x007703A0
static bool ILoadValue(int32_t root, const char* subkeyname, const char* valuename, REGVALUE* result) {
    REGVALUE* value = FindValue(root, subkeyname, valuename);

    if (!value) {
        return false;
    }

    *result = *value;
    return true;
}

// Win64: 0x00770490
static int32_t InternalLoadEntry(const char* keyname, const char* valuename, uint32_t flags, REGVALUE* value) {
    char fullkeyname[MAX_PATH];
    BuildFullKeyName(keyname, flags, fullkeyname, MAX_PATH);

    pthread_mutex_lock(&s_regMutex);
    LoadRegistry();

    bool result = false;

    if (!(flags & SREG_EXCLUDE_CURRENT_USER)) {
        result = ILoadValue(REG_ROOT_CURRENT_USER, fullkeyname, valuename, value);
    }

    if (!result && !(flags & SREG_EXCLUDE_LOCAL_MACHINE)) {
        result = ILoadValue(REG_ROOT_LOCAL_MACHINE, fullkeyname, valuename, value);
    }

    pthread_mutex_unlock(&s_regMutex);

    return result;
}

// Win64: 0x00770580
static int32_t InternalSaveEntry(const char* keyname, const char* valuename, uint32_t flags, uint32_t datatype, const char* string, uint32_t dword) {
    char fullkeyname[MAX_PATH];
    BuildFullKeyName(keyname, flags, fullkeyname, MAX_PATH);

    int32_t root = flags & SREG_EXCLUDE_CURRENT_USER ? REG_ROOT_LOCAL_MACHINE : REG_ROOT_CURRENT_USER;

    pthread_mutex_lock(&s_regMutex);
    LoadRegistry();

    REGVALUE* value = FindValue(root, fullkeyname, valuename);

    if (!value) {
        REGVALUE newvalue;
        newvalue.root = root;
        newvalue.key = fullkeyname;
        newvalue.name = valuename;
        s_regValues->push_back(newvalue);
        value = &s_regValues->back();
    }

    value->type = datatype;
    value->string = string ? string : "";
    value->dword = dword;

    bool result = SaveRegistry();

    pthread_mutex_unlock(&s_regMutex);

    return result;
}

// Win64: 0x00770720
int32_t STORMAPI SRegLoadString(const char* keyname, const char* valuename, uint32_t flags, char* buffer, uint32_t buffersize) {
    if (!keyname || !*keyname || !valuename || !*valuename || !buffer || !buffersize) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    REGVALUE value;

    if (!InternalLoadEntry(keyname, valuename, flags, &value)) {
        return 0;
    }

    switch (value.type) {
        case REG_SZ:
            SStrCopy(buffer, value.string.c_str(), buffersize);
            break;
        case REG_DWORD:
            SStrPrintf(buffer, buffersize, "%u", value.dword);
            break;
    }

    return 1;
}

// Win64: 0x00770840
int32_t STORMAPI SRegLoadValue(const char* keyname, const char* valuename, uint32_t flags, uint32_t* value) {
    if (!keyname || !*keyname || !valuename || !*valuename || !value) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    REGVALUE entry;

    if (!InternalLoadEntry(keyname, valuename, flags, &entry)) {
        return 0;
    }

    switch (entry.type) {
        case REG_SZ:
            *value = static_cast<uint32_t>(strtoul(entry.string.c_str(), nullptr, 0));
            break;
        case REG_DWORD:
            *value = entry.dword;
            break;
    }

    return 1;
}

// Win64: 0x007708F0
int32_t STORMAPI SRegSaveString(const char* keyname, const char* valuename, uint32_t flags, const char* string) {
    if (!keyname || !*keyname || !valuename || !*valuename || !string) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    return InternalSaveEntry(keyname, valuename, flags, REG_SZ, string, 0);
}

// Win64: 0x007709A0
int32_t STORMAPI SRegSaveValue(const char* keyname, const char* valuename, uint32_t flags, uint32_t value) {
    if (!keyname || !*keyname || !valuename || !*valuename) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    return InternalSaveEntry(keyname, valuename, flags, REG_DWORD, nullptr, value);
}
