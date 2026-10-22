// macOS counterpart of Storm/Source/Win64/IOFileUnit.cpp. The unit handle is a POSIX file descriptor stored as
// fd + 1, so that nullptr still means no file.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/Storm/Source/Mac/MacStorm.h"
#include "Engine/Source/Storm/Source/IOUnit/IOFileUnit.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/SOption.h"

namespace Storm {

// Win32 CreateFile parameters kept in IOFileParam.
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 0x00000001
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_FLAG_NO_BUFFERING 0x20000000

static int FileDescriptor(void* handle) {
    return static_cast<int>(reinterpret_cast<intptr_t>(handle)) - 1;
}

// Win64: 0x00779C30
static void* OpenFileHandle(const IOFileParam* param) {
    char path[MAX_PATH];
    SStrCopy(path, param->m_name, sizeof(path));
    MacConvertPath(path);

    int flags = O_CLOEXEC;

    if ((param->m_access & GENERIC_READ) && (param->m_access & GENERIC_WRITE)) {
        flags |= O_RDWR;
    } else if (param->m_access & GENERIC_WRITE) {
        flags |= O_WRONLY;
    } else {
        flags |= O_RDONLY;
    }

    switch (param->m_creation) {
        case CREATE_NEW:
            flags |= O_CREAT | O_EXCL;
            break;
        case CREATE_ALWAYS:
            flags |= O_CREAT | O_TRUNC;
            break;
        case OPEN_ALWAYS:
            flags |= O_CREAT;
            break;
        case TRUNCATE_EXISTING:
            flags |= O_TRUNC;
            break;
        default:
            break;
    }

    int fd = open(path, flags, 0644);

    if (fd < 0) {
        return nullptr;
    }

    if (param->m_flags & FILE_FLAG_NO_BUFFERING) {
        fcntl(fd, F_NOCACHE, 1);
    }

    return reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
}

IOUnit::IOUnit() {
    this->m_refcount = nullptr;
    this->m_count = 1;
}

void IOUnit::Release() {
    IORefcount* refcount = this->m_refcount;

    if (--this->m_count == 0) {
        delete this;
    }

    if (refcount) {
        refcount->Release();
    }
}

IOWin32FileUnit::IOWin32FileUnit(void* handle) {
    this->m_handle = handle;
    this->m_position = -1;
    this->Seek(0);
}

// Win64: 0x00779CD0
bool IOWin32FileUnit::Seek(int64_t offset) {
    if (this->m_position != offset) {
        if (lseek(FileDescriptor(this->m_handle), offset, SEEK_SET) < 0) {
            this->m_position = -1;
            return false;
        }

        this->m_position = offset;
    }

    return true;
}

// Win64: 0x00779D30
bool IOWin32FileUnit::Read(void* buffer, int64_t offset, uint32_t count, uint32_t* bytesRead) {
    uint32_t minimum = 1;
    uint32_t localRead;

    if (!bytesRead) {
        minimum = count;
        bytesRead = &localRead;
    }

    *bytesRead = 0;

    for (int32_t tries = 0; tries < 3; tries++) {
        if (!this->Seek(offset)) {
            return false;
        }

        ssize_t bytes = read(FileDescriptor(this->m_handle), buffer, count);
        bool result = bytes >= 0;
        *bytesRead = result ? static_cast<uint32_t>(bytes) : 0;
        this->m_position = offset + *bytesRead;

        if (result) {
            return *bytesRead >= minimum;
        }

        this->m_position = -1;
        MacSleep(10);
    }

    return false;
}

// Win64: 0x00779E20
bool IOWin32FileUnit::Write(const void* buffer, int64_t offset, uint32_t count) {
    if (!this->Seek(offset)) {
        return false;
    }

    ssize_t written = write(FileDescriptor(this->m_handle), buffer, count);

    if (written == static_cast<ssize_t>(count)) {
        this->m_position = offset + written;
        return true;
    }

    this->m_position = -1;

    return false;
}

// Win64: 0x00779E90
bool IOWin32FileUnit::Flush() {
    return fsync(FileDescriptor(this->m_handle)) == 0;
}

// Win64: 0x00779EB0
bool IOWin32FileUnit::GetFileSize(int64_t* size) {
    struct stat info;

    if (fstat(FileDescriptor(this->m_handle), &info)) {
        *size = 0;
        return false;
    }

    *size = info.st_size;

    return true;
}

// Win64: 0x00779F10
bool IOWin32FileUnit::SetFileSize(int64_t size) {
    if (!this->Seek(size)) {
        return false;
    }

    return ftruncate(FileDescriptor(this->m_handle), size) == 0;
}

// Win64: 0x008C32F0
void* IOWin32FileUnit::Win32Handle() {
    return this->m_handle;
}

// Win64: 0x00779FB0
bool IOUnbufferedFileUnit::Read(void* buffer, int64_t offset, uint32_t count, uint32_t* bytesRead) {
    if (!(this->m_alignMask & reinterpret_cast<uintptr_t>(buffer)) && !(this->m_alignMask & count)) {
        return this->m_unit->Read(buffer, offset, count, bytesRead);
    }

    if (!this->m_buffer) {
        return false;
    }

    uint32_t total = 0;
    bool result = true;

    while (count) {
        uint32_t chunk = count >= 0x100000 ? 0x100000 : count;
        uint32_t size = chunk;

        if (chunk & this->m_alignMask) {
            size = (this->m_alignMask + chunk) & ~this->m_alignMask;
        }

        uint32_t read;
        result = this->m_unit->Read(this->m_buffer, offset, size, &read);

        if (!result) {
            break;
        }

        read = size < chunk ? size : chunk;
        memcpy(buffer, this->m_buffer, read);
        buffer = static_cast<uint8_t*>(buffer) + read;
        offset += read;
        count -= read;
        total += read;

        if (read < chunk) {
            break;
        }
    }

    if (!bytesRead) {
        return count == 0;
    }

    *bytesRead = total;

    return result;
}

// Win64: 0x0077A0C0
bool IOUnbufferedFileUnit::Write(const void* buffer, int64_t offset, uint32_t count) {
    if (!(this->m_alignMask & reinterpret_cast<uintptr_t>(buffer)) && !(this->m_alignMask & count)) {
        return this->m_unit->Write(buffer, offset, count);
    }

    if (!this->m_buffer) {
        return false;
    }

    bool result = true;

    while (count) {
        uint32_t chunk = count >= 0x100000 ? 0x100000 : count;
        memcpy(this->m_buffer, buffer, chunk);
        buffer = static_cast<const uint8_t*>(buffer) + chunk;

        uint32_t size = chunk;

        if (chunk & this->m_alignMask) {
            size = (this->m_alignMask + chunk) & ~this->m_alignMask;
        }

        result = this->m_unit->Write(this->m_buffer, offset, size);

        if (!result) {
            break;
        }

        offset += chunk;
        count -= chunk;
    }

    if (offset > this->m_size) {
        this->m_size = offset;
    }

    return result;
}

// Win64: 0x0077A190
bool IOUnbufferedFileUnit::Flush() {
    return this->m_unit->Flush();
}

// Win64: 0x0077A1A0
bool IOUnbufferedFileUnit::SetFileSize(int64_t size) {
    this->m_size = size;

    return this->m_unit->SetFileSize((size + this->m_alignMask) & ~this->m_alignMask);
}

// Win64: 0x0077A1E0
bool IOUnbufferedFileUnit::GetFileSize(int64_t* size) {
    *size = this->m_size;

    return true;
}

// Win64: 0x0077A200
IOUnit::~IOUnit() {
}

// Win64: 0x0077A230
IOWin32FileUnit::~IOWin32FileUnit() {
    close(FileDescriptor(this->m_handle));
    this->m_handle = nullptr;
}

// Win64: 0x0077A270
IOUnbufferedFileUnit::IOUnbufferedFileUnit(void* handle, const IOFileParam* param) {
    this->m_unit = new IOWin32FileUnit(handle);
    this->m_size = 0;
    this->m_unit->GetFileSize(&this->m_size);
    SStrCopy(this->m_path, param->m_name, sizeof(this->m_path));
    this->m_buffer = valloc(0x100000);

    // The sector size of the volume becomes the block size reported by fstatfs.
    struct statfs info;
    uint32_t bytesPerSector = 0;

    if (!fstatfs(FileDescriptor(handle), &info)) {
        bytesPerSector = info.f_bsize;
    }

    if (!bytesPerSector || ((bytesPerSector - 1) & bytesPerSector)) {
        this->m_alignMask = 0xFFFF;
    } else {
        this->m_alignMask = bytesPerSector - 1;
    }
}

// Win64: 0x0077A380
void* IOUnbufferedFileUnit::Win32Handle() {
    return this->m_unit->Win32Handle();
}

// Win64: 0x0077A390
IOUnbufferedFileUnit* IOUnbufferedFileUnit::Open(const IOFileParam* param) {
    HANDLE handle = OpenFileHandle(param);

    if (!handle) {
        return nullptr;
    }

    return new IOUnbufferedFileUnit(handle, param);
}

// Win64: 0x0077A3E0
IOWin32FileUnit* IOWin32FileUnit::Open(const IOFileParam* param) {
    HANDLE handle = OpenFileHandle(param);

    if (!handle) {
        return nullptr;
    }

    return new IOWin32FileUnit(handle);
}

// Win64: 0x0077A450
IOUnit* IOFileUnit::Create(const IOFileParam& param, IOUnit* next) {
    if (next) {
        return nullptr;
    }

    if (param.m_flags & FILE_FLAG_NO_BUFFERING) {
        return IOUnbufferedFileUnit::Open(&param);
    }

    return IOWin32FileUnit::Open(&param);
}

// Win64: [0x0077A480, 0x0077A530]
IOUnbufferedFileUnit::~IOUnbufferedFileUnit() {
    free(this->m_buffer);

    if (this->m_unit) {
        delete this->m_unit;
    }

    IOFileParam param;
    param.m_name = this->m_path;
    param.m_access = GENERIC_WRITE;
    param.m_share = FILE_SHARE_READ;
    param.m_creation = OPEN_EXISTING;
    param.m_flags = FILE_ATTRIBUTE_NORMAL;

    IOUnit* unit = IOWin32FileUnit::Open(&param);

    if (unit) {
        unit->SetFileSize(this->m_size);
        unit->Release();
    }
}

}
