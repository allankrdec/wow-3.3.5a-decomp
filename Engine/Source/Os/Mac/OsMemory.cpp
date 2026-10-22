#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "Engine/Source/Os/OsMemory.h"

// Model2 keeps 32-bit offsets between a model's data and its extra buffers, so they are placed within
// s_nearRange of the anchor. Free address space is found with mach_vm_region and claimed with a fixed
// mach_vm_allocate, which fails instead of replacing a mapping that appeared in the meantime. VirtualFree needs no
// size, so each allocation starts with a header holding its total size; the returned pointer stays 16-byte aligned.

static const uintptr_t s_nearRange = 0x60000000;

// Lowest address above __PAGEZERO on 64-bit Darwin.
static const uintptr_t s_minimumAddress = 0x100000000;

static const size_t s_headerSize = 16;

static bool TryAllocate(uintptr_t address, uintptr_t bytes) {
    mach_vm_address_t base = address;
    return mach_vm_allocate(mach_task_self(), &base, bytes, VM_FLAGS_FIXED) == KERN_SUCCESS;
}

// Returns the mapped region containing or following address, or false when none is left.
static bool NextRegion(uintptr_t address, uintptr_t& base, uintptr_t& end) {
    mach_vm_address_t regionAddress = address;
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    if (mach_vm_region(mach_task_self(), &regionAddress, &regionSize, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
        return false;
    }

    base = regionAddress;
    end = regionAddress + regionSize;
    return true;
}

void* OsAllocNear(const void* anchor, size_t size) {
    uintptr_t granularity = vm_page_size;
    uintptr_t bytes = (size + s_headerSize + granularity - 1) & ~(granularity - 1);
    uintptr_t center = reinterpret_cast<uintptr_t>(anchor);
    uintptr_t minimum = s_minimumAddress;
    uintptr_t maximum = MACH_VM_MAX_ADDRESS;

    if (center > minimum + s_nearRange) {
        minimum = center - s_nearRange;
    }

    if (center + s_nearRange < maximum) {
        maximum = center + s_nearRange;
    }

    uint8_t* ptr = nullptr;

    // Free gaps above the anchor, nearest first.
    uintptr_t address = center & ~(granularity - 1);

    while (!ptr && address < maximum) {
        uintptr_t base;
        uintptr_t end;

        if (!NextRegion(address, base, end)) {
            base = maximum;
            end = maximum;
        }

        if (base > address) {
            uintptr_t gapEnd = base < maximum ? base : maximum;

            if (address + bytes <= gapEnd && TryAllocate(address, bytes)) {
                ptr = reinterpret_cast<uint8_t*>(address);
                break;
            }
        }

        address = (end + granularity - 1) & ~(granularity - 1);
    }

    // Free gaps below the anchor: mach_vm_region only walks upward, so collect the gaps from minimum and keep the
    // highest one that fits.
    if (!ptr) {
        uintptr_t best = 0;
        address = (minimum + granularity - 1) & ~(granularity - 1);
        uintptr_t limit = center & ~(granularity - 1);

        while (address < limit) {
            uintptr_t base;
            uintptr_t end;

            if (!NextRegion(address, base, end)) {
                base = limit;
                end = limit;
            }

            uintptr_t gapEnd = base < limit ? base : limit;

            if (gapEnd > address && gapEnd - address >= bytes) {
                best = (gapEnd - bytes) & ~(granularity - 1);
            }

            address = (end + granularity - 1) & ~(granularity - 1);
        }

        if (best && TryAllocate(best, bytes)) {
            ptr = reinterpret_cast<uint8_t*>(best);
        }
    }

    if (!ptr) {
        return nullptr;
    }

    *reinterpret_cast<uintptr_t*>(ptr) = bytes;
    return ptr + s_headerSize;
}

void OsFreeNear(void* ptr) {
    if (ptr) {
        uint8_t* base = static_cast<uint8_t*>(ptr) - s_headerSize;
        mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(base), *reinterpret_cast<uintptr_t*>(base));
    }
}
