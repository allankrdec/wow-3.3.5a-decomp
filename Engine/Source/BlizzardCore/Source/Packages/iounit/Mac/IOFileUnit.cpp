// macOS counterpart of Packages/iounit/Win64/IOFileUnit.cpp. FILE_FLAG_NO_BUFFERING becomes F_NOCACHE, and the
// sector size is the file system's block size.

#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>
#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"
#include "Engine/Source/Storm/H/storm.h"

// Win64: 0x00434F20
bool IOFileUnit::ReadNative(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    int32_t file;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative native(path);
        file = open(native.m_data, O_RDONLY | O_CLOEXEC);
    }

    if (file < 0) {
        return false;
    }

    fcntl(file, F_NOCACHE, 1);

    uint32_t head = offset & 0xFFF;
    uint32_t size = (head + count + 0xFFF) & ~0xFFFu;
    bool result = false;
    uint8_t* data = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));

    ssize_t read = pread(file, data, size, static_cast<off_t>(offset - head));

    if (read >= 0) {
        uint32_t available = static_cast<uint32_t>(read) > head ? static_cast<uint32_t>(read) - head : 0;
        uint32_t amount = available >= count ? count : available;
        Blizzard::String::MemCopy(buffer, data + head, amount);

        if (bytesRead) {
            *bytesRead = amount;
        }

        result = true;
    }

    SMemFree(data, __FILE__, __LINE__, 0);
    close(file);

    return result;
}

// Win64: 0x00434CE0
IOUnbufferedFileUnit::IOUnbufferedFileUnit(Blizzard::File::StreamRecord* stream, bool ownsMemory) : IOFileUnit(stream, ownsMemory) {
    Blizzard::File::FileInfo* info = Blizzard::File::GetFileInfo(stream);
    this->m_size = info->m_size;

    struct statfs volume;
    bool valid;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative native(info->m_path);
        valid = statfs(native.m_data, &volume) == 0;
    }

    uint32_t bytesPerSector = valid ? static_cast<uint32_t>(volume.f_bsize) : 0;

    if (!valid || !bytesPerSector || ((bytesPerSector - 1) & bytesPerSector)) {
        this->m_alignMask = 0xFFFF;
    } else {
        this->m_alignMask = bytesPerSector - 1;
    }

    this->m_buffer = Blizzard::Memory::Allocate(this->m_alignMask + 0x100000);
    this->m_alignedBuffer = reinterpret_cast<void*>(~static_cast<uintptr_t>(this->m_alignMask) & (reinterpret_cast<uintptr_t>(this->m_buffer) + this->m_alignMask));
}
