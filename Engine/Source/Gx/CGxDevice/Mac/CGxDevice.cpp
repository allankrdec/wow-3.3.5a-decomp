// Adapter and display functions of CGxDevice on macOS (Win64/CGxDevice.cpp on Windows). There is one adapter, the
// OpenGL renderer, and one display, the main one. Sizes are in points, like the game window (Os/Mac/MacWindow.h).
// Direct3D does not exist here: its format queries return the OpenGL formats, like the Direct3D device factories
// return the OpenGL device (Gx/Mac/CGxDeviceFactory.cpp).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <CoreGraphics/CoreGraphics.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

int32_t CGxDevice::AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    formats.m_chunk = 64;
    formats.Reserve(256, 0);

    switch (api) {
        case GxApi_OpenGl:
            CGxDevice::OpenGlAdapterFormats(formats);
            break;

        case GxApi_D3d9:
            CGxDevice::D3dAdapterFormats(formats);
            break;

        case GxApi_D3d9Ex:
            CGxDevice::D3d9ExAdapterFormats(formats);
            break;

        default:
            break;
    }

    return formats.Count() != 0;
}

// Only the Windows game window creation uses it; Os/Mac places the Mac window.
int32_t CGxDevice::AdjustWindowRectToWorkArea(float aspectRatio, C2iVector* adjust, tagRECT* rect) {
    return 0;
}

// No PCI IDs on Apple Silicon: a generic adapter. 0xFFFF is what Windows reports when the ID is unknown.
int32_t CGxDevice::DeviceAdapterID(uint16_t& vendorID, uint16_t& deviceID, uint32_t& driverVersionHi, uint32_t& driverVersionLo) {
    vendorID = 0xFFFF;
    deviceID = 0xFFFF;
    driverVersionHi = 0;
    driverVersionLo = 0;

    CGxDevice::Log("CGxDevice::DeviceAdapterID(): RET: %d, VID: %x, DID: %x, DVER: %x.%x", 0, vendorID, deviceID, driverVersionHi, driverVersionLo);

    return 0;
}

// The Windows version grades the Direct3D caps: 3 = hardware T&L, 3+ texture units and pixel shader 2.0. Every Mac
// that runs the legacy OpenGL context has that.
int32_t CGxDevice::DeviceAdapterInfer(uint16_t& deviceID) {
    deviceID = 3;

    CGxDevice::Log("CGxDevice::DeviceAdapterInfer(): RET: %d, DID: %x", 1, deviceID);

    return 1;
}

static int CGxMonitorModeSort(const void* a, const void* b) {
    const CGxMonitorMode* modeA = static_cast<const CGxMonitorMode*>(a);
    const CGxMonitorMode* modeB = static_cast<const CGxMonitorMode*>(b);

    return modeA->m_size.x * modeA->m_size.y - modeB->m_size.x * modeB->m_size.y;
}

static uint32_t DisplayModeRefreshRate(CGDisplayModeRef mode) {
    double rate = CGDisplayModeGetRefreshRate(mode);

    // Built-in panels report 0 (and ProMotion panels up to 120); the game expects a real rate.
    return rate > 0.0 ? static_cast<uint32_t>(std::lround(rate)) : 60;
}

int32_t CGxDevice::AdapterDesktopMode(CGxMonitorMode& mode) {
    CGDisplayModeRef displayMode = CGDisplayCopyDisplayMode(CGMainDisplayID());

    if (!displayMode) {
        return 0;
    }

    mode.m_size.x = static_cast<int32_t>(CGDisplayModeGetWidth(displayMode));
    mode.m_size.y = static_cast<int32_t>(CGDisplayModeGetHeight(displayMode));
    mode.m_refreshRate = DisplayModeRefreshRate(displayMode);
    mode.m_bpp = 32;

    CGDisplayModeRelease(displayMode);

    return 1;
}

int32_t CGxDevice::D3dAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    return CGxDevice::OpenGlAdapterFormats(formats);
}

int32_t CGxDevice::D3d9ExAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    return CGxDevice::OpenGlAdapterFormats(formats);
}

// The main display's modes. The game window never changes the display mode (fullscreen is a borderless window), so
// this list only feeds the resolution choices.
int32_t CGxDevice::AdapterMonitorModes(TSGrowableArray<CGxMonitorMode>& modes) {
    modes.SetCount(0);

    CFArrayRef displayModes = CGDisplayCopyAllDisplayModes(CGMainDisplayID(), nullptr);

    if (!displayModes) {
        return 0;
    }

    CFIndex count = CFArrayGetCount(displayModes);

    for (CFIndex i = 0; i < count; i++) {
        CGDisplayModeRef displayMode = static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(displayModes, i)));
        int32_t width = static_cast<int32_t>(CGDisplayModeGetWidth(displayMode));
        int32_t height = static_cast<int32_t>(CGDisplayModeGetHeight(displayMode));

        if (width < 640 || height < 480 || !CGDisplayModeIsUsableForDesktopGUI(displayMode)) {
            continue;
        }

        CGxMonitorMode mode;
        mode.m_size.x = width;
        mode.m_size.y = height;
        mode.m_bpp = 32;
        mode.m_refreshRate = DisplayModeRefreshRate(displayMode);

        bool exists = false;

        for (uint32_t j = 0; j < modes.Count(); j++) {
            if (modes[j].m_size.x == mode.m_size.x && modes[j].m_size.y == mode.m_size.y && modes[j].m_refreshRate == mode.m_refreshRate) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            *modes.New() = mode;
        }
    }

    CFRelease(displayModes);

    qsort(modes.Ptr(), modes.Count(), sizeof(CGxMonitorMode), CGxMonitorModeSort);

    return modes.Count() != 0;
}
