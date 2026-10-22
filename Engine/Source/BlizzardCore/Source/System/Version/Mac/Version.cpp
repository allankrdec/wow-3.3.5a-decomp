// macOS counterpart of Version/Win64/Version.cpp. There is no version.dll: the VS_FIXEDFILEINFO block of the
// Windows executable (e.g. Launcher.exe) is found by its signature and read directly.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"
#include "Engine/Source/BlizzardCore/Source/System/Version/Version.h"

struct VS_FIXEDFILEINFO {
    uint32_t dwSignature;
    uint32_t dwStrucVersion;
    uint32_t dwFileVersionMS;
    uint32_t dwFileVersionLS;
    uint32_t dwProductVersionMS;
    uint32_t dwProductVersionLS;
    uint32_t dwFileFlagsMask;
    uint32_t dwFileFlags;
    uint32_t dwFileOS;
    uint32_t dwFileType;
    uint32_t dwFileSubtype;
    uint32_t dwFileDateMS;
    uint32_t dwFileDateLS;
};

static const uint32_t VS_FFI_SIGNATURE = 0xFEEF04BD;

static bool ReadFixedFileInfo(const char* path, VS_FIXEDFILEINFO* info) {
    int32_t file = open(path, O_RDONLY | O_CLOEXEC);

    if (file < 0) {
        return false;
    }

    struct stat st;

    if (fstat(file, &st) || st.st_size <= 0 || st.st_size > 0x10000000) {
        close(file);
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    ssize_t size = pread(file, data.data(), data.size(), 0);
    close(file);

    for (ssize_t offset = 0; offset + static_cast<ssize_t>(sizeof(VS_FIXEDFILEINFO)) <= size; offset += 4) {
        uint32_t signature;
        memcpy(&signature, &data[offset], sizeof(signature));

        if (signature == VS_FFI_SIGNATURE) {
            memcpy(info, &data[offset], sizeof(VS_FIXEDFILEINFO));
            return true;
        }
    }

    return false;
}

// Win64: 0x004338F0
static bool GetVersionInfo(const char* path, Blizzard::Version::FileVersion& version, int32_t type) {
    version.m_major = 0;
    version.m_minor = 0;
    version.m_build = 0;
    version.m_revision = 0;

    if (Blizzard::File::Exists(path) != 1) {
        return false;
    }

    Blizzard::StringConversion_CodepageToUTF8::ToNative nativePath(path);

    VS_FIXEDFILEINFO info;

    if (!ReadFixedFileInfo(nativePath.m_data, &info)) {
        return false;
    }

    if (type == 0) {
        version.m_major = info.dwProductVersionMS >> 16;
        version.m_minor = info.dwProductVersionMS & 0xFFFF;
        version.m_build = info.dwProductVersionLS >> 16;
        version.m_revision = info.dwProductVersionLS & 0xFFFF;
    } else if (type == 1) {
        version.m_major = info.dwFileVersionMS >> 16;
        version.m_minor = info.dwFileVersionMS & 0xFFFF;
        version.m_build = info.dwFileVersionLS >> 16;
        version.m_revision = info.dwFileVersionLS & 0xFFFF;
    }

    return true;
}

// Win64: 0x00433A90
bool Blizzard::Version::GetFileVersion(const char* path, FileVersion& version) {
    return GetVersionInfo(path, version, 1);
}
