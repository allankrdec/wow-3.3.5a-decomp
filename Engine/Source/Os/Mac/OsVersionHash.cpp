// macOS counterpart of Os/Win64/OsVersionHash.cpp (versionproof checksum, see port-mac/docs/06-rede-login-anticheat.md).
//
// The only caller is ChecksumExecutables (WowSvcs), which feeds an HMAC-SHA1 with:
//   1. the running executable  (path nullptr, loaded),
//   2. "DivxDecoder.dll"       (loaded),
//   3. "unicows.dll"           (file on disk).
// The 20-byte result goes to the login server in AUTH_LOGON_PROOF. The output shape is unchanged; only the bytes fed
// to the callback differ:
//   - Loaded images: Win64 walks the PE in memory (headers, sections, relocations undone, IAT zeroed) so the hash
//     does not depend on the load address. The Mac equivalent is the image's __TEXT segment as mapped (Mach-O header,
//     load commands, code and read-only data). __TEXT is never written by dyld (fixups live in __DATA_CONST/__DATA),
//     so it is identical on every run and at any ASLR slide; it changes only when the binary is rebuilt. The image is
//     found by file name among the loaded dyld images, like GetModuleHandle; DivxDecoder.dll is never loaded on the
//     Mac, so that entry contributes nothing (as on a Windows machine without it).
//   - Files on disk: read and hashed whole, exactly like Win64 (unicows.dll does not exist here either).
// No server can expect this value for a Mac build, so login only works with servers that do not enforce the
// executable checksum (the default for TrinityCore/AzerothCore unless strict version checking is turned on), which
// is also the situation of the Windows x64 build of this tree.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Storm/H/storm.h"

// Win64: 0x00871520
OsVersionHash::~OsVersionHash() {
    if (this->m_entries) {
        SMemFree(this->m_entries, __FILE__, __LINE__, 0);
    }
}

// Win64: 0x00871540
bool OsVersionHash::AddEntry(const OsVersionHashEntry& entry) {
    if (!this->m_entries) {
        return false;
    }

    if (this->m_count >= this->m_capacity) {
        this->m_capacity *= 2;
        this->m_entries = static_cast<OsVersionHashEntry*>(SMemReAlloc(this->m_entries, this->m_capacity * sizeof(OsVersionHashEntry), __FILE__, __LINE__, 0));

        if (!this->m_entries) {
            return false;
        }
    }

    this->m_entries[this->m_count] = entry;
    this->m_count++;

    return true;
}

// Win64: 0x00871BE0
OsVersionHash::OsVersionHash() : m_entries(nullptr) {
    this->m_entries = static_cast<OsVersionHashEntry*>(SMemAlloc(256 * sizeof(OsVersionHashEntry), __FILE__, __LINE__, 0));
    this->m_count = 0;
    this->m_capacity = 256;
}

// GetModuleHandle: nullptr is the main executable, otherwise a loaded image whose file name matches (case-insensitive,
// with or without directory).
static const mach_header_64* FindLoadedImage(const char* path) {
    if (!path) {
        return reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    }

    const char* name = path;
    for (const char* c = path; *c; c++) {
        if (*c == '/' || *c == '\\') {
            name = c + 1;
        }
    }

    uint32_t count = _dyld_image_count();

    for (uint32_t i = 0; i < count; i++) {
        const char* imagePath = _dyld_get_image_name(i);
        if (!imagePath) {
            continue;
        }

        const char* imageName = strrchr(imagePath, '/');
        imageName = imageName ? imageName + 1 : imagePath;

        if (!SStrCmpI(imageName, name, 0x7FFFFFFF)) {
            return reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        }
    }

    return nullptr;
}

// Win64: 0x00871F90
int32_t IOsVersionChecksum(IOsVersionHashCallback& callback, const char* path, int32_t loaded) {
    if (!path && !loaded) {
        return 0;
    }

    if (loaded) {
        const mach_header_64* header = FindLoadedImage(path);

        if (!header || header->magic != MH_MAGIC_64) {
            return 0;
        }

        unsigned long size = 0;
        const uint8_t* text = getsegmentdata(header, SEG_TEXT, &size);

        if (!text || !size || size != static_cast<uint32_t>(size)) {
            return 0;
        }

        callback.Process(text, static_cast<uint32_t>(size));
        return 1;
    }

    int32_t result = 0;
    void* buffer = nullptr;

    HOSFILE file = OsCreateFile(path, OS_GENERIC_READ, 1, OS_OPEN_EXISTING, OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);

    if (file != OS_INVALID_FILE) {
        uint64_t size = OsGetFileSize(file);

        if (size == static_cast<uint32_t>(size)) {
            buffer = SMemAlloc(static_cast<uint32_t>(size), __FILE__, __LINE__, 0);

            uint32_t bytesRead;
            if (buffer && OsReadFile(file, buffer, static_cast<uint32_t>(size), &bytesRead) && bytesRead == size) {
                callback.Process(buffer, static_cast<uint32_t>(size));
                result = 1;
            }
        }
    }

    if (buffer) {
        SMemFree(buffer, __FILE__, __LINE__, 0);
    }

    if (file != OS_INVALID_FILE) {
        OsCloseFile(file);
    }

    return result;
}

// Win64: 0x00872170
OsFileVersionInfo::OsFileVersionInfo(const char* filename) : m_fileVersion(), m_productVersion() {
    // Windows reads the VS_VERSIONINFO resource of a PE file. Mach-O files have none, so this always reports "no
    // version information" (empty m_data), what Windows does for a file without the resource. The only caller is
    // GetDllVersionString (CheckExecutableSignature.cpp), for downloaded Windows DLLs that never exist on the Mac.
    m_data.Clear();
}
