// macOS counterpart of File/Win64/Filestack.cpp: the base file system over POSIX calls.
//
// - Names go through StringConversion_CodepageToUTF8::ToNative, which turns the game's '\' separators into '/'.
// - A lookup that fails with ENOENT is retried once with each path component matched ignoring case, for loose
//   files on case-sensitive volumes.
// - StreamRecord::m_fileHandle holds the descriptor plus one, so a zeroed record is never descriptor 0.
// - Attributes keep the Win32 FILE_ATTRIBUTE_* representation as the "native" one, synthesized from stat().
// - The descriptors are synchronous, like the Windows handles: a read or write with an OVERLAPPED is a pread or
//   pwrite at OVERLAPPED::m_offset that also moves the file position, as ReadFile does.

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/TempBuffer.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"
#include "Engine/Source/Storm/H/storm.h"

static const int32_t FILE_ATTRIBUTE_READONLY = 0x1;
static const int32_t FILE_ATTRIBUTE_HIDDEN = 0x2;
static const int32_t FILE_ATTRIBUTE_SYSTEM = 0x4;
static const int32_t FILE_ATTRIBUTE_DIRECTORY = 0x10;
static const int32_t FILE_ATTRIBUTE_ARCHIVE = 0x20;
static const int32_t FILE_ATTRIBUTE_NORMAL = 0x80;
static const int32_t FILE_ATTRIBUTE_TEMPORARY = 0x100;

// ToCreateFlags output: the FILE_FLAG_NO_BUFFERING request, applied with F_NOCACHE.
static const DWORD NATIVE_FLAG_NOCACHE = 0x1;

// Seconds between 1601-01-01 (FILETIME) and 1970-01-01 (Unix).
static const int64_t FILETIME_UNIX_SECONDS = 11644473600LL;

static void* const INVALID_HANDLE_VALUE = reinterpret_cast<void*>(-1);

static int32_t GetDescriptor(Blizzard::File::StreamRecord* file) {
    if (file->m_fileHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    return static_cast<int32_t>(reinterpret_cast<intptr_t>(file->m_fileHandle)) - 1;
}

static void* MakeHandle(int32_t fd) {
    return fd >= 0 ? reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1) : INVALID_HANDLE_VALUE;
}

// Rebuilds `path` with the on-disk spelling of its components, matched ignoring case. Components that do not
// exist in any case are kept as given. Returns false when nothing was changed.
static bool FindPathIgnoringCase(const char* path, char* result, size_t size) {
    size_t length = 0;
    bool changed = false;
    const char* cursor = path;

    if (*cursor == '/') {
        result[length++] = '/';
        cursor++;
    }

    result[length] = '\0';

    while (*cursor) {
        const char* end = strchr(cursor, '/');
        size_t component = end ? static_cast<size_t>(end - cursor) : strlen(cursor);

        if (length + component + 2 > size) {
            return false;
        }

        memcpy(result + length, cursor, component);
        result[length + component] = '\0';

        struct stat st;

        if (component && lstat(result, &st) != 0 && errno == ENOENT) {
            result[length] = '\0';
            DIR* dir = opendir(length ? result : ".");
            bool found = false;

            if (dir) {
                while (dirent* entry = readdir(dir)) {
                    if (entry->d_namlen == component && !strncasecmp(entry->d_name, cursor, component)) {
                        memcpy(result + length, entry->d_name, component);
                        found = true;
                        break;
                    }
                }

                closedir(dir);
            }

            if (found) {
                changed = true;
            } else {
                memcpy(result + length, cursor, component);
            }
        }

        length += component;
        result[length] = '\0';

        if (!end) {
            break;
        }

        result[length++] = '/';
        result[length] = '\0';
        cursor = end + 1;
    }

    return changed;
}

static int32_t OpenNative(const char* path, int32_t flags) {
    int32_t fd = open(path, flags | O_CLOEXEC, 0666);

    if (fd < 0 && errno == ENOENT) {
        char real[1024];

        if (FindPathIgnoringCase(path, real, sizeof(real))) {
            fd = open(real, flags | O_CLOEXEC, 0666);
        }
    }

    return fd;
}

static int32_t StatNative(const char* path, struct stat* st) {
    int32_t result = stat(path, st);

    if (result != 0 && errno == ENOENT) {
        char real[1024];

        if (FindPathIgnoringCase(path, real, sizeof(real))) {
            result = stat(real, st);
        }
    }

    return result;
}

static DIR* OpenDirNative(const char* path) {
    DIR* dir = opendir(path);

    if (!dir && errno == ENOENT) {
        char real[1024];

        if (FindPathIgnoringCase(path, real, sizeof(real))) {
            dir = opendir(real);
        }
    }

    return dir;
}

static uint64_t ToFileTime(const timespec& time) {
    return static_cast<uint64_t>(time.tv_sec + FILETIME_UNIX_SECONDS) * 10000000 + time.tv_nsec / 100;
}

static int32_t GetNativeAttributes(const char* name, const struct stat& st) {
    int32_t attributes = 0;

    if (!(st.st_mode & S_IWUSR)) {
        attributes |= FILE_ATTRIBUTE_READONLY;
    }

    const char* filename = strrchr(name, '/');
    filename = filename ? filename + 1 : name;

    if ((st.st_flags & UF_HIDDEN) || (filename[0] == '.' && filename[1] && strcmp(filename, ".."))) {
        attributes |= FILE_ATTRIBUTE_HIDDEN;
    }

    if (S_ISDIR(st.st_mode)) {
        attributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        attributes |= FILE_ATTRIBUTE_ARCHIVE;
    }

    return attributes;
}

static void FillFileInfo(Blizzard::File::FileInfo* info, const char* name, const struct stat& st) {
    info->m_createTime = Blizzard::Time::FromFileTime(ToFileTime(st.st_birthtimespec));
    info->m_modTime = Blizzard::Time::FromFileTime(ToFileTime(st.st_mtimespec));
    info->m_accessTime = Blizzard::Time::FromFileTime(ToFileTime(st.st_atimespec));
    info->m_attributes = System_File::FromNativeAttributes(GetNativeAttributes(name, st));
    info->m_regularFile = (info->m_attributes & 0x20) != 0;
    info->m_existence = ((info->m_attributes & 0x40) != 0) + 1;
}

System_File::IOFunc System_File::read_func = System_File::read_first;

System_File::IOFunc System_File::write_func = System_File::write_first;

bool System_File::s_read_init;

Blizzard::Lock::Atomic System_File::s_readInitCount;

bool System_File::s_write_init;

Blizzard::Lock::Atomic System_File::s_writeInitCount;

Blizzard::Lock::Mutex System_File::s_ioMutex;

Blizzard::Lock::Mutex* System_File::s_ioLock;

// Win64: 0x00427C20
int32_t System_File::ToNativeAttributes(int32_t attributes) {
    int32_t result = (attributes & 0x1) != 0 ? FILE_ATTRIBUTE_READONLY : 0;

    if (attributes & 0x2) {
        result |= FILE_ATTRIBUTE_HIDDEN;
    }

    if (attributes & 0x4) {
        result |= FILE_ATTRIBUTE_SYSTEM;
    }

    if (attributes & 0x8) {
        result |= FILE_ATTRIBUTE_ARCHIVE;
    }

    if (attributes & 0x10) {
        result |= FILE_ATTRIBUTE_TEMPORARY;
    }

    if (attributes & 0x20) {
        result |= FILE_ATTRIBUTE_NORMAL;
    }

    if (attributes & 0x40) {
        result |= FILE_ATTRIBUTE_DIRECTORY;
    }

    return result;
}

// Win64: 0x00427C70
int32_t System_File::FromNativeAttributes(int32_t attributes) {
    int32_t result = (attributes & FILE_ATTRIBUTE_READONLY) != 0 ? 0x1 : 0;

    if (attributes & FILE_ATTRIBUTE_HIDDEN) {
        result |= 0x2;
    }

    if (attributes & FILE_ATTRIBUTE_SYSTEM) {
        result |= 0x4;
    }

    if (attributes & FILE_ATTRIBUTE_ARCHIVE) {
        result |= 0x8;
    }

    if (attributes & FILE_ATTRIBUTE_TEMPORARY) {
        result |= 0x10;
    }

    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        result |= 0x40;
    }

    if ((attributes & FILE_ATTRIBUTE_NORMAL) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result |= 0x20;
    }

    return result;
}

// Win64: 0x00427CC0
bool System_File::ReadP(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    OVERLAPPED overlapped;
    overlapped.m_offset = static_cast<uint64_t>(parms->m_pos);
    overlapped.m_event = nullptr;
    overlapped.m_platform = nullptr;

    return read_func(parms->m_stream, parms->m_buffer, &overlapped, &parms->m_amount);
}

// Win64: 0x00427D10
bool System_File::WriteP(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    OVERLAPPED overlapped;
    overlapped.m_offset = static_cast<uint64_t>(parms->m_pos);
    overlapped.m_event = nullptr;
    overlapped.m_platform = nullptr;

    return write_func(parms->m_stream, parms->m_buffer, &overlapped, &parms->m_amount);
}

// Win64: 0x0042D620
bool System_File::Close(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    int32_t fd = GetDescriptor(stream);

    if (fd >= 0) {
        close(fd);
    }

    SMemFree(stream, __FILE__, __LINE__, 0);

    return true;
}

// Win64: 0x004325C0
bool System_File::GetRootChars(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;
    Blizzard::Memory::TempBuffer<char, 256> buffer((path ? static_cast<uint32_t>(strlen(path)) : 0) + 1);

    if (buffer.m_count < 2) {
        parms->m_pos = 0;
        return true;
    }

    Blizzard::String::MakeUnivPath(path, buffer.m_data, buffer.m_count);
    char* univ = buffer.m_data;

    if (univ[1] == ':') {
        parms->m_pos = (univ[2] == '/') + 2;
    } else if (univ[0] == '/' && univ[1] == '/') {
        const char* separator = univ + 1;
        int32_t count = 0;

        for (;;) {
            separator = strchr(separator + 1, '/');

            if (!separator) {
                parms->m_pos = buffer.m_count;
                break;
            }

            if (++count >= 2) {
                parms->m_pos = separator - univ + 1;
                break;
            }
        }
    } else if (univ[0] == '/') {
        parms->m_pos = 1;
    } else {
        parms->m_pos = 0;
    }

    return true;
}

// Win64: 0x00435580
void System_File::GetFileInfoByFile(Blizzard::File::StreamRecord* file) {
    if (file->m_infoValid) {
        return;
    }

    file->m_infoValid = false;
    struct stat st;

    if (fstat(GetDescriptor(file), &st) != 0) {
        return;
    }

    if (!file->m_streaming || !file->m_streaming->m_entry) {
        file->m_info.m_size = st.st_size;
    }

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(file->m_pathname);
        FillFileInfo(&file->m_info, name.m_data, st);
    }

    file->m_infoValid = true;
}

// Win64: 0x004356B0
bool System_File::Exists(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char path[1024];
    Blizzard::String::MakeBackslashPath(parms->m_path, path, sizeof(path));

    struct stat st;
    int32_t result;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);
        result = StatNative(name.m_data, &st);
    }

    if (result != 0) {
        parms->m_info->m_existence = 0;
    } else if (S_ISDIR(st.st_mode)) {
        parms->m_info->m_existence = 2;
    } else {
        parms->m_info->m_existence = 1;
    }

    return true;
}

// Win64: 0x00435790
bool System_File::MakeAbsolutePath(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    // _fullpath: prefix the working directory to a relative path and fold "." and ".." without touching the disk.
    char buffer[1024];
    char joined[2048];

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(parms->m_path);

        if (name.m_data[0] == '/') {
            Blizzard::String::Copy(joined, name.m_data, sizeof(joined));
        } else {
            char cwd[1024];

            if (!getcwd(cwd, sizeof(cwd))) {
                cwd[0] = '\0';
            }

            Blizzard::String::Format(joined, sizeof(joined), "%s/%s", cwd, name.m_data);
        }
    }

    size_t length = 0;
    const char* cursor = joined;

    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }

        if (!*cursor) {
            break;
        }

        const char* end = strchr(cursor, '/');
        size_t component = end ? static_cast<size_t>(end - cursor) : strlen(cursor);

        if (component == 1 && cursor[0] == '.') {
        } else if (component == 2 && cursor[0] == '.' && cursor[1] == '.') {
            while (length > 0 && buffer[length - 1] != '/') {
                length--;
            }

            if (length > 0) {
                length--;
            }
        } else if (length + component + 2 < sizeof(buffer)) {
            buffer[length++] = '/';
            memcpy(buffer + length, cursor, component);
            length += component;
        }

        cursor += component;
    }

    if (!length) {
        buffer[length++] = '/';
    }

    buffer[length] = '\0';

    FromNativeName(parms->m_cwdbuf, parms->m_cwdbuflen, buffer);
    Blizzard::String::MakeUnivPath(parms->m_cwdbuf, parms->m_cwdbuf, parms->m_cwdbuflen);

    const char* path = parms->m_path;
    size_t pathLength = path ? strlen(path) : 0;
    char last = pathLength ? path[pathLength - 1] : '\0';

    if (last == '/' || last == '\\') {
        Blizzard::String::ForceTrailingSeparator(parms->m_cwdbuf, parms->m_cwdbuflen, '/');
    }

    return true;
}

// Win64: 0x00435890
bool System_File::Move(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char src[1024];
    char dst[1024];
    Blizzard::String::MakeBackslashPath(parms->m_path, src, sizeof(src));
    Blizzard::String::MakeBackslashPath(parms->m_newpath, dst, sizeof(dst));

    Blizzard::StringConversion_CodepageToUTF8::ToNative newName(dst);
    Blizzard::StringConversion_CodepageToUTF8::ToNative oldName(src);

    // MoveFile does not replace an existing destination.
    struct stat st;

    if (lstat(newName.m_data, &st) == 0) {
        return false;
    }

    return rename(oldName.m_data, newName.m_data) == 0;
}

// Win64: 0x004359C0
bool System_File::Delete(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char path[1024];
    Blizzard::String::MakeBackslashPath(parms->m_path, path, sizeof(path));

    Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);

    return unlink(name.m_data) == 0;
}

// Win64: 0x00454870
bool System_File::read_overlapped(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    int32_t fd = GetDescriptor(file);

    if (fd < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (!*count) {
        return true;
    }

    int64_t total = 0;
    bool result = true;

    while (total < *count) {
        ssize_t bytes;

        if (overlapped) {
            bytes = pread(fd, static_cast<uint8_t*>(buffer) + total, *count - total, static_cast<off_t>(overlapped->m_offset + total));
        } else {
            bytes = read(fd, static_cast<uint8_t*>(buffer) + total, *count - total);
        }

        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }

            result = false;
            break;
        }

        if (!bytes) {
            break;
        }

        total += bytes;
    }

    *count = static_cast<int32_t>(total);

    if (result) {
        if (overlapped) {
            lseek(fd, static_cast<off_t>(overlapped->m_offset + total), SEEK_SET);
        }

        return true;
    }

    Blizzard::File::SetLastError(5);
    Blizzard::File::AddToLastErrorStack(5, Blizzard::String::QuickFormat<1024>("Mac Read - %s", file->m_pathname).m_buffer, 1);

    return false;
}

// Win64: 0x00454910
bool System_File::write_overlapped(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    int32_t fd = GetDescriptor(file);

    if (fd < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (!*count) {
        return true;
    }

    file->m_infoValid = false;
    int64_t total = 0;
    bool result = true;

    while (total < *count) {
        ssize_t bytes;

        if (overlapped) {
            bytes = pwrite(fd, static_cast<const uint8_t*>(buffer) + total, *count - total, static_cast<off_t>(overlapped->m_offset + total));
        } else {
            bytes = write(fd, static_cast<const uint8_t*>(buffer) + total, *count - total);
        }

        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }

            result = false;
            break;
        }

        total += bytes;
    }

    *count = static_cast<int32_t>(total);

    if (result) {
        if (overlapped) {
            lseek(fd, static_cast<off_t>(overlapped->m_offset + total), SEEK_SET);
        }

        return true;
    }

    Blizzard::File::SetLastError(6);
    Blizzard::File::AddToLastErrorStack(6, Blizzard::String::QuickFormat<1024>("Mac Write - %s", file->m_pathname).m_buffer, 1);

    return false;
}

// Win64: 0x004549B0
bool System_File::ToCreateFlags(uint32_t flags, DWORD* sharemode, DWORD* desiredaccess, DWORD* creationdisposition, DWORD* flagsandattributes) {
    // Same decisions as the Windows code, spelled as open(2) flags: bit 0 read, bit 1 write, bits 2-3 share mode
    // (no POSIX equivalent), 0x100/0x400 create always, 0x800 create new, 0x1000 open existing, otherwise open
    // always; 0x40 is FILE_FLAG_NO_BUFFERING.
    *sharemode = (flags >> 2) & 3;

    if ((flags & 1) && (flags & 2)) {
        *desiredaccess = O_RDWR;
    } else if (flags & 2) {
        *desiredaccess = O_WRONLY;
    } else {
        *desiredaccess = O_RDONLY;
    }

    if ((flags & 0x100) || (flags & 0x400)) {
        *creationdisposition = (flags & 0x800) ? (O_CREAT | O_EXCL) : (O_CREAT | O_TRUNC);
    } else if (flags & 0x800) {
        *creationdisposition = O_CREAT | O_EXCL;
    } else {
        *creationdisposition = (flags & 0x1000) ? 0 : O_CREAT;
    }

    *flagsandattributes = (flags & 0x40) ? NATIVE_FLAG_NOCACHE : 0;

    return true;
}

// Win64: 0x00454A50
bool System_File::SetWorkingDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!parms->m_path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    Blizzard::StringConversion_CodepageToUTF8::ToNative name(parms->m_path);

    if (chdir(name.m_data) == 0) {
        return true;
    }

    char real[1024];

    return errno == ENOENT && FindPathIgnoringCase(name.m_data, real, sizeof(real)) && chdir(real) == 0;
}

// Win64: 0x00454AC0
bool System_File::Create(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::SetLastError(9);

    return false;
}

// Win64: 0x00454AE0
bool System_File::GetWorkingDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!parms->m_cwdbuf || parms->m_cwdbuflen <= 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    *parms->m_cwdbuf = '\0';
    char buffer[1024];

    if (!getcwd(buffer, sizeof(buffer))) {
        buffer[0] = '\0';
    }

    FromNativeName(parms->m_cwdbuf, parms->m_cwdbuflen, buffer);

    return true;
}

// Win64: 0x00454B50
bool System_File::ProcessDirFast(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    void* cookie = parms->m_cookie;
    const char* path = parms->m_path;
    Blizzard::File::ProcessDirCallback callback = parms->m_dircallback;

    DIR* find;
    char directory[1024];

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);
        Blizzard::String::Copy(directory, name.m_data, sizeof(directory));
        find = OpenDirNative(directory);
    }

    if (!find) {
        Blizzard::Debug::Printf(Blizzard::Debug::StderrOutput, "failed to open %s\n", path);
        Blizzard::File::SetLastError(8);
        return false;
    }

    Blizzard::File::ProcessDirParms dirParms;
    dirParms.m_pathRoot = path;
    dirParms.m_cookie = cookie;

    while (dirent* entry = readdir(find)) {
        if (entry->d_name[0] != '.' || (entry->d_name[1] && (entry->d_name[1] != '.' || entry->d_name[2]))) {
            bool isDir = entry->d_type == DT_DIR;

            if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
                char full[2048];
                struct stat st;
                Blizzard::String::Format(full, sizeof(full), "%s/%s", directory, entry->d_name);
                isDir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            }

            char filename[1024];
            FromNativeName(filename, sizeof(filename), entry->d_name);
            dirParms.m_filename = filename;
            dirParms.m_isDir = isDir;

            if (!callback(dirParms)) {
                closedir(find);
                return false;
            }
        }
    }

    closedir(find);

    return true;
}

// Win64: 0x00454CB0
bool System_File::Flush(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int32_t fd = GetDescriptor(parms->m_stream);

    if (fd < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    return fsync(fd) == 0;
}

// Win64: 0x00454CF0
bool System_File::GetFileInfo(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (parms->m_path) {
        struct stat st;
        bool found;

        {
            Blizzard::StringConversion_CodepageToUTF8::ToNative name(parms->m_path);
            found = StatNative(name.m_data, &st) == 0;

            if (found) {
                FillFileInfo(parms->m_info, name.m_data, st);
            }
        }

        if (!found) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_info->m_size = st.st_size;
        parms->m_info->m_path = nullptr;

        return true;
    }

    if (parms->m_stream) {
        GetFileInfoByFile(parms->m_stream);
        Blizzard::File::StreamRecord* stream = parms->m_stream;
        parms->m_info = &stream->m_info;

        if (!stream->m_info.m_path) {
            stream->m_info.m_path = stream->m_pathname;
        }

        return parms->m_stream->m_infoValid;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

// Win64: 0x00454E70
bool System_File::GetFreeSpace(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (path && *path) {
        char directory[MAX_PATH];
        Blizzard::String::Copy(directory, path, std::min<int32_t>(static_cast<int32_t>(Blizzard::String::FindFilename(path) - path + 1), MAX_PATH));

        struct statfs info;
        int32_t result;

        {
            Blizzard::StringConversion_CodepageToUTF8::ToNative name(*directory ? directory : ".");
            result = statfs(name.m_data, &info);
        }

        if (result == 0) {
            parms->m_pos = static_cast<int64_t>(info.f_bavail) * info.f_bsize;
            return true;
        }
    }

    Blizzard::File::SetLastError(8);

    return false;
}

// Win64: 0x00454FD0
bool System_File::GetPos(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int32_t fd = GetDescriptor(parms->m_stream);
    off_t pos;

    if (fd < 0 || (pos = lseek(fd, 0, SEEK_CUR)) < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    parms->m_pos = pos;

    return true;
}

// Win64: 0x00455050
bool System_File::IsAbsolutePath(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    // POSIX roots, plus the Windows forms the game may still build.
    if (((path[0] == '\\' || path[0] == '/') && path[1]) || (isalpha(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/') && path[3])) {
        return true;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

// Win64: 0x004550C0
bool System_File::IsReadOnly(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    struct stat st;
    int32_t result;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(parms->m_path);
        result = StatNative(name.m_data, &st);
    }

    if (result == 0) {
        return !(st.st_mode & S_IWUSR);
    }

    Blizzard::File::SetLastError(8);

    return false;
}

// Win64: 0x00455130
bool System_File::CreateDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    bool recursive = parms->m_recursive;
    const char* path = parms->m_path;
    bool grantEveryone = parms->m_grantEveryone;

    if (!path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    // grantEveryone gives the Users group full access on Windows; here the directory gets 0777 before the umask.
    mode_t mode = grantEveryone ? 0777 : 0755;
    Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);

    if (recursive) {
        char* start = name.m_data;
        size_t length = strlen(start);

        for (char* end = start + 1; end <= start + length; end++) {
            if (*end && *end != '/') {
                continue;
            }

            if (end[-1] == '/') {
                continue;
            }

            char saved = *end;
            *end = '\0';
            bool created = mkdir(start, mode) == 0;
            int32_t error = errno;
            struct stat st;

            if (!created && (error != EEXIST && error != EACCES && error != EISDIR)) {
                Blizzard::File::SetLastError(8);
                return false;
            }

            if (!created && (stat(start, &st) != 0 || !S_ISDIR(st.st_mode))) {
                Blizzard::File::SetLastError(8);
                return false;
            }

            *end = saved;
        }

        return true;
    }

    if (mkdir(name.m_data, mode) == 0) {
        return true;
    }

    return errno == EEXIST || errno == EACCES;
}

// Win64: 0x004554C0
bool System_File::Copy(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    bool overwrite = parms->m_overwrite;
    char src[1024];
    char dst[1024];
    Blizzard::String::MakeBackslashPath(parms->m_path, src, sizeof(src));
    Blizzard::String::MakeBackslashPath(parms->m_newpath, dst, sizeof(dst));

    Blizzard::File::StreamRecord* input;

    if (!Blizzard::File::Open(src, 0x1005, input)) {
        Blizzard::File::SetLastError(2);
        return false;
    }

    Blizzard::File::StreamRecord* output;

    if (!Blizzard::File::Open(dst, (overwrite ? 0x400 : 0xC00) | 0x2, output)) {
        Blizzard::File::Close(input);
        Blizzard::File::SetLastError(4);
        return false;
    }

    int64_t size = Blizzard::File::GetFileInfo(input)->m_size;
    int64_t offset = 0;
    bool result = true;
    void* buffer = Blizzard::Memory::Allocate(size >= 0xA00000 ? 0xA00000 : static_cast<uint32_t>(size));

    if (size > 0) {
        for (;;) {
            int32_t count = size - offset >= 0xA00000 ? 0xA00000 : static_cast<int32_t>(size - offset);

            if (!Blizzard::File::Read(input, buffer, offset, &count)) {
                break;
            }

            if (!Blizzard::File::Write(output, buffer, offset, &count)) {
                break;
            }

            offset += count;

            if (offset >= size) {
                goto done;
            }
        }

        result = false;
    }

done:
    Blizzard::Memory::Free(buffer);
    Blizzard::File::Close(input);
    Blizzard::File::Close(output);

    if (!result) {
        Blizzard::File::Delete(dst);
    }

    return result;
}

// Win64: 0x00455730
bool System_File::Open(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int32_t mode = parms->m_mode;
    char path[1024];
    Blizzard::String::MakeBackslashPath(parms->m_path, path, sizeof(path));

    DWORD sharemode;
    DWORD desiredaccess;
    DWORD creationdisposition;
    DWORD flagsandattributes;

    if (!ToCreateFlags(mode, &sharemode, &desiredaccess, &creationdisposition, &flagsandattributes)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    int32_t fd;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);
        fd = OpenNative(name.m_data, desiredaccess | creationdisposition);
    }

    struct stat st;

    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        // CreateFile does not open directories without FILE_FLAG_BACKUP_SEMANTICS.
        close(fd);
        fd = -1;
    }

    if (fd < 0) {
        Blizzard::File::SetLastError(4);
        Blizzard::File::AddToLastErrorStack(4, Blizzard::String::QuickFormat<1024>("Mac Open - %s", path).m_buffer, 1);
        return false;
    }

    if (flagsandattributes & NATIVE_FLAG_NOCACHE) {
        fcntl(fd, F_NOCACHE, 1);
    }

    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (!stream) {
        uint32_t length = static_cast<uint32_t>(strlen(path));
        stream = static_cast<Blizzard::File::StreamRecord*>(SMemAlloc(sizeof(Blizzard::File::StreamRecord) + length + 1, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
        stream->m_pathname = reinterpret_cast<char*>(stream + 1);
        Blizzard::String::Copy(stream->m_pathname, path, length + 1);
    }

    stream->m_fileHandle = MakeHandle(fd);
    stream->m_mode = mode;
    stream->m_infoValid = false;
    stream->m_info.m_path = stream->m_pathname;
    GetFileInfoByFile(stream);

    if (mode & 0x200) {
        Blizzard::File::SetPos(stream, stream->m_info.m_size, 0);
    }

    parms->m_stream = stream;

    return true;
}

// Win64: 0x004558F0
bool System_File::Read(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    return read_overlapped(parms->m_stream, parms->m_buffer, nullptr, &parms->m_amount);
}

// Win64: 0x00455910
bool System_File::RemoveDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (parms->m_recursive) {
        return Blizzard::File::RemoveDirectoryAndContents(path, false);
    }

    if (!path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    Blizzard::StringConversion_CodepageToUTF8::ToNative name(path);

    return rmdir(name.m_data) == 0;
}

// Win64: 0x00455A30
bool System_File::SetCacheMode(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    // Windows reopens the file to change FILE_FLAG_NO_BUFFERING; F_NOCACHE switches it on the open descriptor.
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    stream->m_mode = parms->m_mode | (stream->m_mode & ~0x40);

    DWORD sharemode;
    DWORD desiredaccess;
    DWORD creationdisposition;
    DWORD flagsandattributes;

    if (!ToCreateFlags(stream->m_mode, &sharemode, &desiredaccess, &creationdisposition, &flagsandattributes)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    int32_t fd = GetDescriptor(stream);

    if (fd < 0 || fcntl(fd, F_NOCACHE, (flagsandattributes & NATIVE_FLAG_NOCACHE) ? 1 : 0) == -1) {
        Blizzard::File::SetLastError(4);
        Blizzard::File::AddToLastErrorStack(4, "Mac SetCacheMode", 1);
        return false;
    }

    return true;
}

// Win64: 0x00455B10
bool System_File::SetEOF(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int64_t pos = parms->m_pos;
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    int32_t whence = parms->m_posMode;
    int64_t offset;

    if (!Blizzard::File::GetPos(stream, &offset) || !Blizzard::File::SetPos(stream, pos, whence)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    int32_t fd = GetDescriptor(stream);
    off_t end = lseek(fd, 0, SEEK_CUR);
    bool result = end >= 0 && ftruncate(fd, end) == 0;
    stream->m_infoValid = false;
    Blizzard::File::SetPos(stream, offset, 0);

    return result;
}

// Win64: 0x00455BA0
bool System_File::SetAttributes(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (parms->m_has.m_all & 0x10) {
        Blizzard::Time::Timestamp time = parms->m_info->m_modTime;
        int64_t ticks = time / 100 + 125911584000000000;

        if (time < 0 && time % 100) {
            ticks = time / 100 + 125911583999999999;
        }

        timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
        times[1].tv_sec = static_cast<time_t>(ticks / 10000000 - FILETIME_UNIX_SECONDS);
        times[1].tv_nsec = static_cast<long>(ticks % 10000000) * 100;

        if (times[1].tv_nsec < 0) {
            times[1].tv_sec--;
            times[1].tv_nsec += 1000000000;
        }

        if (futimens(GetDescriptor(parms->m_stream), times) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_stream->m_info.m_modTime = time;
        parms->m_has.m_all &= ~0x10u;
    }

    if (parms->m_has.m_all & 0x4) {
        int32_t attributes = ToNativeAttributes(parms->m_info->m_attributes);
        bool result = false;

        {
            Blizzard::StringConversion_CodepageToUTF8::ToNative name(parms->m_path);
            struct stat st;

            if (StatNative(name.m_data, &st) == 0) {
                mode_t mode = st.st_mode & 07777;
                mode = (attributes & FILE_ATTRIBUTE_READONLY) ? (mode & ~0222) : (mode | S_IWUSR);
                uint32_t flags = (attributes & FILE_ATTRIBUTE_HIDDEN) ? (st.st_flags | UF_HIDDEN) : (st.st_flags & ~UF_HIDDEN);
                result = chmod(name.m_data, mode) == 0;

                if (result && flags != st.st_flags) {
                    chflags(name.m_data, flags);
                }
            }
        }

        if (!result) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_has.m_all &= ~0x4u;
    }

    return parms->m_has.m_all == 0;
}

// Win64: 0x00455CA0
bool System_File::SetPos(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    int32_t method = parms->m_posMode;
    int32_t fd = GetDescriptor(stream);

    if (fd < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (method) {
        method = method != 1 ? SEEK_END : SEEK_CUR;
    } else {
        method = SEEK_SET;
    }

    return lseek(fd, static_cast<off_t>(parms->m_pos), method) >= 0;
}

// Win64: 0x00455D20
bool System_File::Write(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    return write_overlapped(parms->m_stream, parms->m_buffer, nullptr, &parms->m_amount);
}

// Win64: 0x00457820
bool System_File::read_locked(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    Blizzard::Lock::ScopedMutex lock(*s_ioLock);

    if (!Blizzard::File::SetPos(file, static_cast<int64_t>(overlapped->m_offset), 0)) {
        *count = 0;
        Blizzard::File::SetLastError(9);
        return false;
    }

    return read_overlapped(file, buffer, nullptr, count);
}

// Win64: 0x00457910
bool System_File::write_locked(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    Blizzard::Lock::ScopedMutex lock(*s_ioLock);

    if (!Blizzard::File::SetPos(file, static_cast<int64_t>(overlapped->m_offset), 0)) {
        *count = 0;
        Blizzard::File::SetLastError(9);
        return false;
    }

    return write_overlapped(file, buffer, nullptr, count);
}

// Win64: 0x00459910
void System_File::detect_io_mode() {
    // pread/pwrite are always available (the Windows 9x branch used read_locked/write_locked).
    read_func = read_overlapped;
    write_func = write_overlapped;
}

// Win64: 0x0045B440
bool System_File::read_first(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!s_read_init) {
        if (s_readInitCount.Increment() == 1) {
            detect_io_mode();
            s_read_init = true;
        } else {
            do {
                MacSleep(1);
            } while (!s_read_init);
        }
    }

    return read_func(file, buffer, overlapped, count);
}

// Win64: 0x0045B490
bool System_File::write_first(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!s_write_init) {
        if (s_writeInitCount.Increment() == 1) {
            detect_io_mode();
            s_write_init = true;
        } else {
            do {
                MacSleep(1);
            } while (!s_write_init);
        }
    }

    return write_func(file, buffer, overlapped, count);
}

// Win64: 0x00433590
void System_File::FromNativeName(char* buffer, uint32_t buffersize, const char* str) {
    // Native names are UTF-8 already.
    Blizzard::String::Copy(buffer, str ? str : "", buffersize);
}
