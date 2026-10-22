// Stage E4 test for the Mac OpenGL device: creates the Gx device the way ConsoleDeviceInitialize does (GxDefaultApi,
// CreateGxDevice with OsWindowProc and a windowed CGxFormat), clears the window to a color that changes over time
// and presents it for a few seconds, reading the back buffer back to check the clear color.
//
//   GxTest [seconds]
//
// Exit code 0 when everything passed. Needs a display (it opens a window).

#import <Cocoa/Cocoa.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxDeviceOpenGl.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/GlExt.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/CiRect.h"

static int32_t s_failures;

static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);

    if (!ok) {
        s_failures++;
    }
}

static void PumpEvents() {
    @autoreleasepool {
        NSEvent* event;

        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

int main(int argc, char* argv[]) {
    double seconds = argc > 1 ? std::atof(argv[1]) : 3.0;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    StormRtlInitialize();
    StormInitialize();
    // ScenePresent's frame cap reads the OS clock (Client.cpp starts it the same way).
    OsTimeStartup(OsTimeManager::BestAvailable);

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }

    CGxMonitorMode desktop;
    Check(GxAdapterDesktopMode(desktop) != 0, "GxAdapterDesktopMode");
    std::printf("     desktop %dx%d %d Hz %d bpp\n", desktop.m_size.x, desktop.m_size.y, desktop.m_refreshRate, desktop.m_bpp);

    TSGrowableArray<CGxMonitorMode> modes;
    Check(GxAdapterMonitorModes(modes) != 0, "GxAdapterMonitorModes");
    std::printf("     %d monitor modes\n", modes.Count());

    uint16_t deviceID = 0;
    Check(GxAdapterInfer(deviceID) != 0 && deviceID == 3, "GxAdapterInfer");

    TSGrowableArray<CGxFormat> formats;
    Check(CGxDevice::AdapterFormats(GxApi_OpenGl, formats) != 0, "CGxDevice::AdapterFormats(GxApi_OpenGl)");

    for (uint32_t i = 0; i < formats.Count(); i++) {
        std::printf("     format color %d depth %d samples %d\n", formats[i].m_colorFormat, formats[i].m_depthFormat, formats[i].m_multisampleCount);
    }

    // ConsoleDeviceInitialize's desktop format, windowed at 800x600.
    CGxFormat format(true, { 800, 600 }, CGxFormat::Fmt_ArgbX888, CGxFormat::Fmt_Ds24X, 60, 1, true, false, true, true, 0);

    // GxDefaultApi() is GxApi_D3d9; on macOS its factory returns the OpenGL device.
    CGxDevice* device = CreateGxDevice(GxDefaultApi(), OsWindowProc, format);
    Check(device != nullptr, "CreateGxDevice(GxDefaultApi())");

    if (!device) {
        return 1;
    }

    CGxDeviceOpenGl* glDevice = static_cast<CGxDeviceOpenGl*>(device);
    Check(device->DeviceApi() == GxApi_OpenGl, "DeviceApi() == GxApi_OpenGl");
    Check(device->DeviceWindow() != nullptr, "DeviceWindow()");
    std::printf("     GL %s | %s | %s\n", glDevice->m_glVendor, glDevice->m_glRenderer, glDevice->m_glVersion);
    std::printf("     tmus %d, streams %d, vs target %d, ps target %d, FBO %d, ARB vp %d, ARB fp %d, VBO %d, S3TC %d\n", device->Caps().m_numTmus, device->Caps().m_numStreams, device->Caps().m_shaderTargets[GxSh_Vertex], device->Caps().m_shaderTargets[GxSh_Pixel], glExtFramebufferObject, glARBVertexProgram, glARBFragmentProgram, glARBVertexBufferObject, glExtTextureCompressionS3tc);
    Check(device->Caps().m_numTmus >= 2 && device->Caps().m_numStreams, "dual TMUs and streams (ConsoleDeviceInitialize's checks)");
    Check(glARBVertexProgram && glARBFragmentProgram, "ARB vertex and fragment programs");
    Check(glDevice->m_renderTargetMode == 1, "render targets through FBOs");

    CRect window;
    device->CapsWindowSize(window);
    std::printf("     window %gx%g\n", window.maxX - window.minX, window.maxY - window.minY);
    Check(window.maxX - window.minX == 800.0f && window.maxY - window.minY == 600.0f, "window size 800x600");

    double start = [NSDate timeIntervalSinceReferenceDate];
    uint32_t frames = 0;
    bool readBackOk = true;

    for (double now = start; now - start < seconds; now = [NSDate timeIntervalSinceReferenceDate]) {
        PumpEvents();

        double t = now - start;
        CImVector color(static_cast<uint8_t>(64 + 64 * (frames % 3 == 2)), static_cast<uint8_t>(t * 255.0 / seconds), 200, 255);
        GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, color);

        if (frames % 30 == 0) {
            CiRect rect(100, 100, 101, 101);
            TSGrowableArray<CImVector> pixels;
            device->DeviceReadPixels(rect, pixels);

            if (pixels.Count() != 1 || pixels[0].r != color.r || pixels[0].g != color.g || pixels[0].b != color.b) {
                std::printf("     read back %d,%d,%d, cleared %d,%d,%d\n", pixels.Count() ? pixels[0].r : -1, pixels.Count() ? pixels[0].g : -1, pixels.Count() ? pixels[0].b : -1, color.r, color.g, color.b);
                readBackOk = false;
            }
        }

        GxScenePresent();
        frames++;
    }

    double elapsed = [NSDate timeIntervalSinceReferenceDate] - start;
    std::printf("     %u frames in %.2f s (%.1f fps)\n", frames, elapsed, frames / elapsed);
    Check(frames > 0, "frames presented");
    Check(readBackOk, "back buffer holds the clear color");

    GxDevDestroy(device);
    Check(g_theGxDevicePtr == nullptr, "GxDevDestroy");

    OsTimeShutdown();

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}
