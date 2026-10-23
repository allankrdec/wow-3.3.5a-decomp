// The macOS part of CGxDeviceOpenGl: the counterparts of the WGL functions in Windows/CGxDeviceOpenGlWindows.cpp.
//
// - Window: Os/Mac owns it (MacWindow.h). m_hwnd holds its content view (NSView*), which is also what DeviceWindow()
//   returns; window events arrive in WindowEventGl, which does what WindowProcGl does with the matching Win32
//   messages.
// - Context: a legacy (OpenGL 2.1) NSOpenGLContext attached to that view. m_hglrc holds it (owned, manual
//   retain/release); m_hdc is unused. The legacy profile has the fixed-function pipeline, the ARB assembly programs
//   (arbvp1/arbfp1) and EXT_framebuffer_object, so CGxDeviceOpenGl.cpp runs unchanged on it.
// - Render targets: there are no pbuffers. When EXT_framebuffer_object exists the device renders to textures through
//   FBOs (IDeviceSetRenderTargetFBO); otherwise IDeviceSetRenderTargetCopy copies from the back buffer.
// - Display modes: the display mode never changes; fullscreen is a borderless window over the main screen.
// - Gamma: like SetDeviceGammaRamp on Windows, only in fullscreen: the ramp goes to the window's display while the app
//   is active, composed over the display's own (ColorSync) transfer table so gamma 1.0 leaves the display untouched.
//   Leaving fullscreen, deactivating (Cmd+Tab) or destroying the device restores the display.
// - Vsync: the GL swap interval is set, but the GL-over-Metal renderer does not hold flushBuffer to the refresh
//   (swap interval 1 gives ~80 fps on a 60 Hz screen, frontmost or not), so with vsync on ScenePresent also waits for
//   the next refresh of the window's screen, counted by a CVDisplayLink.
// All of it runs on the main thread, like the rest of the Mac window code.

#define GL_SILENCE_DEPRECATION

#import <Cocoa/Cocoa.h>
#include <CoreVideo/CoreVideo.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach_time.h>
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxDeviceOpenGl.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxDeviceOpenGlPlatform.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Mac/MacWindow.h"

static bool s_extensionsBound;

static int32_t s_deviceChanging;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Gamma: the display the ramp is on, and that display's transfer table from before (sampled at the ramp's 256 inputs).
static CGDirectDisplayID s_gammaDisplay;
static CGGammaValue s_systemGamma[3][256];

static void GammaRestore() {
    if (s_gammaDisplay) {
        CGDisplayRestoreColorSyncSettings();
        s_gammaDisplay = 0;
    }
}

static void GammaUpdate(CGxDeviceOpenGl* device) {
    NSWindow* window = device->m_hwnd ? [reinterpret_cast<NSView*>(device->m_hwnd) window] : nil;

    if (device->m_format.m_window || !window || ![NSApp isActive]) {
        GammaRestore();
        return;
    }

    CGDirectDisplayID display = [[[[window screen] deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

    if (!display) {
        GammaRestore();
        return;
    }

    if (display != s_gammaDisplay) {
        GammaRestore();

        uint32_t count = 0;
        if (CGGetDisplayTransferByTable(display, 256, s_systemGamma[0], s_systemGamma[1], s_systemGamma[2], &count) != kCGErrorSuccess || count != 256) {
            for (uint32_t i = 0; i < 256; i++) {
                s_systemGamma[0][i] = s_systemGamma[1][i] = s_systemGamma[2][i] = i / 255.0f;
            }
        }
    }

    // The game's ramp maps each input level to an output level; the display's table then applies to that output.
    const uint16_t* ramp[3] = { device->m_gammaRamp.m_red, device->m_gammaRamp.m_green, device->m_gammaRamp.m_blue };
    CGGammaValue table[3][256];

    for (uint32_t c = 0; c < 3; c++) {
        for (uint32_t i = 0; i < 256; i++) {
            float position = ramp[c][i] * (255.0f / 65535.0f);
            uint32_t index = static_cast<uint32_t>(position);
            uint32_t next = index < 255 ? index + 1 : 255;
            float t = position - index;
            table[c][i] = s_systemGamma[c][index] + (s_systemGamma[c][next] - s_systemGamma[c][index]) * t;
        }
    }

    if (CGSetDisplayTransferByTable(display, 256, table[0], table[1], table[2]) == kCGErrorSuccess) {
        s_gammaDisplay = display;
    }
}

// Vsync: s_refreshCount counts the refreshes of the screen (CVDisplayLink thread); ScenePresent waits until it passes
// the count of the previous present.
static CVDisplayLinkRef s_displayLink;
static std::mutex s_refreshMutex;
static std::condition_variable s_refreshSignal;
static uint64_t s_refreshCount;
static uint64_t s_presentRefresh;
static bool s_vsync;

static CVReturn DisplayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void*) {
    {
        std::lock_guard<std::mutex> lock(s_refreshMutex);
        s_refreshCount++;
    }

    s_refreshSignal.notify_all();
    return kCVReturnSuccess;
}

// Starts counting the refreshes of the screen showing the view, or stops it when vsync is off.
static void VsyncSet(bool vsync, NSView* view) {
    s_vsync = vsync;

    if (!vsync) {
        if (s_displayLink) {
            CVDisplayLinkStop(s_displayLink);
        }

        return;
    }

    if (!s_displayLink) {
        if (CVDisplayLinkCreateWithActiveCGDisplays(&s_displayLink) != kCVReturnSuccess) {
            s_displayLink = nullptr;
            s_vsync = false;
            return;
        }

        CVDisplayLinkSetOutputCallback(s_displayLink, DisplayLinkCallback, nullptr);
    }

    NSScreen* screen = [[view window] screen] ?: [NSScreen mainScreen];
    NSNumber* displayID = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];

    if (displayID) {
        CVDisplayLinkSetCurrentCGDisplay(s_displayLink, [displayID unsignedIntValue]);
    }

    if (!CVDisplayLinkIsRunning(s_displayLink)) {
        CVDisplayLinkStart(s_displayLink);
    }
}

// Waits for the first refresh after the previous present. A frame that took longer than a refresh does not wait.
// The timeout covers a display that stops refreshing (asleep, disconnected).
static void VsyncWait() {
    if (!s_vsync) {
        return;
    }

    std::unique_lock<std::mutex> lock(s_refreshMutex);
    s_refreshSignal.wait_for(lock, std::chrono::milliseconds(100), [] { return s_refreshCount > s_presentRefresh; });
    s_presentRefresh = s_refreshCount;
}

#pragma clang diagnostic pop

static NSOpenGLContext* GlContext(HGLRC context) {
    return reinterpret_cast<NSOpenGLContext*>(context);
}

static NSView* GlView(HWND window) {
    return reinterpret_cast<NSView*>(window);
}

// The pixel format for a CGxFormat, or nil when the renderer has none. Same color/depth/stencil bits as
// SetupPixelFormatWGL.
static NSOpenGLPixelFormat* PixelFormatCreate(const CGxFormat& format) {
    NSOpenGLPixelFormatAttribute attributes[32];
    uint32_t count = 0;

    attributes[count++] = NSOpenGLPFAOpenGLProfile;
    attributes[count++] = NSOpenGLProfileVersionLegacy;
    attributes[count++] = NSOpenGLPFAAccelerated;
    attributes[count++] = NSOpenGLPFADoubleBuffer;

    NSOpenGLPixelFormatAttribute colorBits = 24;
    NSOpenGLPixelFormatAttribute alphaBits = 0;

    switch (format.m_colorFormat) {
        case CGxFormat::Fmt_Rgb565:
            colorBits = 16;
            break;

        case CGxFormat::Fmt_Argb8888:
            alphaBits = 8;
            break;

        case CGxFormat::Fmt_Argb2101010:
            colorBits = 30;
            alphaBits = 2;
            break;

        default:
            break;
    }

    NSOpenGLPixelFormatAttribute depthBits = 24;
    NSOpenGLPixelFormatAttribute stencilBits = 0;

    switch (format.m_depthFormat) {
        case CGxFormat::Fmt_Ds160:
            depthBits = 16;
            break;

        case CGxFormat::Fmt_Ds248:
            stencilBits = 8;
            break;

        case CGxFormat::Fmt_Ds320:
            depthBits = 32;
            break;

        default:
            break;
    }

    attributes[count++] = NSOpenGLPFAColorSize;
    attributes[count++] = colorBits;
    attributes[count++] = NSOpenGLPFAAlphaSize;
    attributes[count++] = alphaBits;
    attributes[count++] = NSOpenGLPFADepthSize;
    attributes[count++] = depthBits;
    attributes[count++] = NSOpenGLPFAStencilSize;
    attributes[count++] = stencilBits;

    if (format.m_multisampleCount > 1) {
        attributes[count++] = NSOpenGLPFAMultisample;
        attributes[count++] = NSOpenGLPFASampleBuffers;
        attributes[count++] = 1;
        attributes[count++] = NSOpenGLPFASamples;
        attributes[count++] = format.m_multisampleCount;
    }

    attributes[count++] = NSOpenGLPFANoRecovery;
    attributes[count] = 0;

    NSOpenGLPixelFormat* pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];

    if (!pixelFormat) {
        return nil;
    }

    // Like the WGL path, a format that does not give the requested bits is a failure, so the caller can step down
    // (ConsoleDeviceInitialize lowers multisampling, then depth and color).
    GLint samples = 0;
    GLint depth = 0;
    GLint stencil = 0;
    [pixelFormat getValues:&samples forAttribute:NSOpenGLPFASamples forVirtualScreen:0];
    [pixelFormat getValues:&depth forAttribute:NSOpenGLPFADepthSize forVirtualScreen:0];
    [pixelFormat getValues:&stencil forAttribute:NSOpenGLPFAStencilSize forVirtualScreen:0];

    bool multisampleOk = format.m_multisampleCount <= 1 || samples == static_cast<GLint>(format.m_multisampleCount);

    if (!multisampleOk || depth < static_cast<GLint>(depthBits) || stencil < static_cast<GLint>(stencilBits)) {
        [pixelFormat release];
        return nil;
    }

    return pixelFormat;
}

// WindowProcGl: the window events of Os/Mac, as the Win32 messages they replace.
static void WindowEventGl(void* param, MacWindowEvent event, int32_t width, int32_t height) {
    CGxDeviceOpenGl* device = static_cast<CGxDeviceOpenGl*>(param);

    if (!device) {
        return;
    }

    CRect rect;
    rect.minY = 0.0f;
    rect.minX = 0.0f;
    rect.maxY = static_cast<float>(height);
    rect.maxX = static_cast<float>(width);

    switch (event) {
        case MacWindowEvent_Resize:
        case MacWindowEvent_Restore:
            if (device->m_hglrc) {
                [GlContext(device->m_hglrc) update];
            }

            device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
            break;

        case MacWindowEvent_Minimize:
            device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 1);
            break;

        case MacWindowEvent_FocusGained:
            device->DeviceWM(GxWM_SetFocus, 0, 0);
            GammaUpdate(device);
            break;

        case MacWindowEvent_FocusLost:
            device->DeviceWM(GxWM_KillFocus, 0, 0);
            GammaUpdate(device);
            break;

        // WM_CLOSE goes to the application's window procedure on Windows; Os/Mac turns the close into the quit
        // event itself, and the device is destroyed through DeviceDestroy.
        case MacWindowEvent_Close:
        default:
            break;
    }
}

// CreateGameWindow: windowed at the requested size, or a borderless window covering the main screen (fullscreen and
// "maximized" windows), whose size becomes the format's size.
static HWND CreateGameWindow(CGxFormat& format, void* param) {
    bool windowed = format.m_window && format.m_maximize != 1;

    if (!windowed) {
        int32_t width = 0;
        int32_t height = 0;
        MacGameScreenSize(&width, &height);

        if (width && height) {
            format.m_size.x = width;
            format.m_size.y = height;
        }
    }

    format.m_pos.x = 0;
    format.m_pos.y = 0;

    CGxDevice::s_clientAdjust.x = 0;
    CGxDevice::s_clientAdjust.y = 0;

    return reinterpret_cast<HWND>(MacGameWindowCreate(format.m_size.x, format.m_size.y, windowed, WindowEventGl, param));
}

void CGxDeviceOpenGl::DeviceDestroyPbuffer() {
    this->m_pbuffer = nullptr;
    this->m_pbufferDC = nullptr;
    this->m_pbufferRC = nullptr;
}

void CGxDeviceOpenGl::IDevSetFocus(int32_t focus, const CGxFormat& format) {
    this->m_windowFocus = focus;

    if (focus) {
        MacGameWindowShow();
    }
}

// No display mode switch on macOS.
int32_t CGxDeviceOpenGl::SetFormatMode(CGxFormat& format) {
    format.m_apiSpecificModeID = 0;
    return 1;
}

void CGxDeviceOpenGl::IDevRemoveGlContext() {
    this->DeviceDestroyPbuffer();
    GammaRestore();

    if (this->m_hglrc) {
        NSOpenGLContext* context = GlContext(this->m_hglrc);
        [context makeCurrentContext];

        this->ICursorDestroy();
        this->DeviceDestroyPoolAPI();
        this->ITexForceRecreation();
        this->IShaderForceRecreation();
        this->IQueryRelease();
        UnbindGlExtensions();

        VsyncSet(false, nil);
        [NSOpenGLContext clearCurrentContext];
        [context clearDrawable];
        [context release];
        this->m_hglrc = nullptr;
    }

    this->m_hdc = nullptr;
    this->m_context = 0;
}

void CGxDeviceOpenGl::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
    this->DeviceDestroyPbuffer();
    s_deviceChanging = 1;

    // On Windows DestroyWindow sends WM_DESTROY, which removes the context; here it is removed first.
    this->IDevRemoveGlContext();

    if (this->m_hwnd && this->m_ownhwnd) {
        MacGameWindowDestroy();
    }

    this->m_hwnd = nullptr;
    s_deviceChanging = 0;
}

void CGxDeviceOpenGl::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);
    GammaUpdate(this);
}

void CGxDeviceOpenGl::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);
    GammaUpdate(this);
}

// No pbuffers on macOS: render targets are FBOs when the extension exists (always, in the legacy context).
void CGxDeviceOpenGl::DeviceCreatePbuffer() {
    if (!glExtFramebufferObject) {
        return;
    }

    this->m_renderTargetMode = 1;
    this->m_caps.m_rttFormat[GxTex_Argb8888] = 1;
}

void CGxDeviceOpenGl::DeviceQueryPbuffer() {
}

int32_t CGxDeviceOpenGl::IDevAttachGlContext(const CGxFormat& format) {
    NSView* view = GlView(this->m_hwnd);
    NSOpenGLPixelFormat* pixelFormat = view ? PixelFormatCreate(format) : nil;

    if (pixelFormat) {
        NSOpenGLContext* context = [[NSOpenGLContext alloc] initWithFormat:pixelFormat shareContext:nil];
        [pixelFormat release];
        this->m_hglrc = reinterpret_cast<HGLRC>(context);
    } else {
        CGxDevice::Log("NSOpenGLPixelFormat failed, color = %d, depth = %d, samples = %d\n", format.m_colorFormat, format.m_depthFormat, format.m_multisampleCount);
    }

    if (this->m_hglrc) {
        NSOpenGLContext* context = GlContext(this->m_hglrc);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [context setView:view];
#pragma clang diagnostic pop
        [context makeCurrentContext];
        this->m_context = 1;

        // CreateDummyContext: the extension entry points are looked up once, with the first context.
        if (!s_extensionsBound) {
            s_extensionsBound = true;
            BindGlExtensions();
            BindWglExtensions();
        }

        GLint swapInterval = format.m_vsync != 0;
        [context setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
        VsyncSet(swapInterval != 0, view);

        this->ISetCaps(format);
        this->DeviceCreatePbuffer();
        this->ICursorCreate(format);
        this->IStateSetContextDefaults();
        this->IRsSync(1);
        this->IShaderConstantsClear();
        this->m_glVendor = glGetString(GL_VENDOR);
        this->m_glRenderer = glGetString(GL_RENDERER);
        this->m_glVersion = glGetString(GL_VERSION);

        return 1;
    }

    this->IDevRemoveGlContext();
    return 0;
}

int32_t CGxDeviceOpenGl::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
            } else {
                this->m_windowVisible = 1;

                const CRect& window = this->DeviceDefWindow();
                const CRect* rect = reinterpret_cast<const CRect*>(param1);
                bool grew = window.maxX - window.minX < rect->maxX - rect->minX || window.maxY - window.minY < rect->maxY - rect->minY;
                this->DeviceSetDefWindow(*rect);

                if (grew) {
                    this->DeviceDestroyPbuffer();
                    this->DeviceCreatePbuffer();
                } else {
                    this->DeviceQueryPbuffer();
                }

                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_Destroy:
            this->IDevRemoveGlContext();
            this->m_windowFocus = 0;
            break;

        case GxWM_SetFocus:
            if (!s_deviceChanging) {
                this->IDevSetFocus(1, this->m_format);
            }

            break;

        case GxWM_KillFocus:
            if (!s_deviceChanging) {
                this->m_windowFocus = 0;
            }

            break;

        default:
            break;
    }

    return 0;
}

int32_t CGxDeviceOpenGl::DeviceCreate(void* window, const CGxFormat& format) {
    s_deviceChanging = 1;
    this->m_ownhwnd = 0;
    this->m_hwnd = static_cast<HWND>(window);

    if (this->IDevAttachGlContext(format)) {
        s_deviceChanging = 0;

        if (CGxDevice::DeviceCreate(window, format)) {
            return 1;
        }
    }

    this->DeviceDestroy();
    s_deviceChanging = 0;
    return 0;
}

int32_t CGxDeviceOpenGl::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceOpenGl::DeviceSetFormat():");
    this->Log(format);

    s_deviceChanging = 1;
    this->IDevRemoveGlContext();

    // The window is kept (MacGameWindowCreate reuses it), so the new format resizes it instead of recreating it.
    CGxFormat newFormat = format;

    if (this->SetFormatMode(newFormat) && (this->m_hwnd = CreateGameWindow(newFormat, this)) != nullptr) {
        this->IDevSetFocus(1, newFormat);

        // Windows gets WM_SIZE while it creates and shows the window.
        int32_t width = 0;
        int32_t height = 0;
        MacGameWindowGetSize(&width, &height);
        WindowEventGl(this, MacWindowEvent_Resize, width, height);

        if (this->IDevAttachGlContext(newFormat)) {
            s_deviceChanging = 0;

            if (CGxDevice::DeviceSetFormat(newFormat)) {
                this->DeviceSetGamma(this->m_gammaRamp);
                return 1;
            }
        }
    }

    s_deviceChanging = 0;
    return 0;
}

// IDeviceSetRenderTargetCopy without the pbuffer: the scene is drawn to the back buffer and copied into the texture
// when the target changes. Only used when there is no EXT_framebuffer_object.
void CGxDeviceOpenGl::IDeviceSetRenderTargetCopy(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context || !this->m_windowVisible) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);

    if (this->m_textureTarget[GxBuffers_Color].m_texture || this->m_textureTarget[GxBuffers_Depth].m_texture) {
        this->DeviceSetCurWindow(this->DeviceDefWindow());
    }

    CGxTex* previous = static_cast<CGxTex*>(target.m_apiSpecific);

    if (previous) {
        if (!previous->m_apiSpecificData) {
            glGenTextures(1, reinterpret_cast<GLuint*>(&previous->m_apiSpecificData));
        }

        this->BindTexture(previous, 0xFFFFFFFF);

        if (previous->m_needsFlagUpdate) {
            this->ITexSetFlags(previous);
        }

        GLenum glTarget = s_glTexTarget[previous->m_target];

        if (previous->m_needsCreation) {
            glCopyTexImage2D(glTarget, 0, s_glInternalFormat[previous->m_format], 0, 0, previous->m_width, previous->m_height, 0);
        } else {
            glCopyTexSubImage2D(glTarget, 0, 0, 0, 0, 0, previous->m_width, previous->m_height);
        }

        previous->m_needsCreation = 0;
    }

    target.m_apiSpecific = texture;

    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
    g_theGxDevicePtr->XformSetProjection(this->m_projection);
}

// Screen coordinates with the origin at the top left of the main screen, like Windows.
void CGxDeviceOpenGl::CapsWindowSizeInScreenCoords(CRect& rect) {
    const CRect& window = this->DeviceCurWindow();
    NSView* view = GlView(this->m_hwnd);

    if (this->IDevIsWindowed() && view && [view window]) {
        NSRect viewRect = NSMakeRect(0.0, 0.0, window.maxX, window.maxY);
        NSRect screenRect = [[view window] convertRectToScreen:[view convertRect:viewRect toView:nil]];

        int32_t screenWidth = 0;
        int32_t screenHeight = 0;
        MacMainScreenSize(&screenWidth, &screenHeight);

        rect.minX = static_cast<float>(screenRect.origin.x);
        rect.maxX = static_cast<float>(screenRect.origin.x + screenRect.size.width);
        rect.minY = static_cast<float>(screenHeight - (screenRect.origin.y + screenRect.size.height));
        rect.maxY = static_cast<float>(screenHeight - screenRect.origin.y);
    } else {
        rect.minY = window.minY;
        rect.minX = window.minX;
        rect.maxY = window.maxY;
        rect.maxX = window.maxX;
    }
}

// WOWDECOMP_MAC_FPSLOG=1: prints the presented frames per second (and the swap interval in effect) to stderr once
// a second.
static void LogFrameRate(NSOpenGLContext* context) {
    static int32_t s_enabled = -1;

    if (s_enabled < 0) {
        const char* value = getenv("WOWDECOMP_MAC_FPSLOG");
        s_enabled = value && *value && *value != '0';
    }

    if (!s_enabled) {
        return;
    }

    static mach_timebase_info_data_t s_timebase;
    static uint64_t s_start;
    static uint32_t s_frames;

    if (!s_timebase.denom) {
        mach_timebase_info(&s_timebase);
    }

    uint64_t now = mach_absolute_time();

    if (!s_start) {
        s_start = now;
    }

    s_frames++;

    double seconds = static_cast<double>(now - s_start) * s_timebase.numer / s_timebase.denom / 1e9;

    if (seconds >= 1.0) {
        GLint swapInterval = -1;
        [context getValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];

        NSWindow* window = [[context view] window];
        bool visible = window && ([window occlusionState] & NSWindowOcclusionStateVisible);

        fprintf(stderr, "fps: %.1f (swap interval %d, window %s)\n", s_frames / seconds, swapInterval, visible ? "visible" : "occluded");
        s_start = now;
        s_frames = 0;
    }
}

void CGxDeviceOpenGl::ScenePresent() {
    int32_t screenShot = this->m_scrShotClick;
    CGxDevice::ScenePresent();

    if (screenShot) {
        this->DeviceScreenShot();
    }

    this->ICursorDraw();

    if (this->m_hwMasterEnables & 0x40) {
        VsyncWait();
        [GlContext(this->m_hglrc) flushBuffer];
    } else {
        glFinish();
    }

    LogFrameRate(GlContext(this->m_hglrc));

    this->WaitForFPSCap();
    this->IShaderConstantsClear();
}

int32_t CGxDeviceOpenGl::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    if (CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// The formats the renderer accepts, found by asking for each one (OpenGlAdapterFormats asks the WGL pixel formats).
int32_t CGxDevice::OpenGlAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    static const CGxFormat::Format colorFormats[] = { CGxFormat::Fmt_ArgbX888, CGxFormat::Fmt_Argb8888 };
    static const CGxFormat::Format depthFormats[] = { CGxFormat::Fmt_Ds160, CGxFormat::Fmt_Ds24X, CGxFormat::Fmt_Ds248, CGxFormat::Fmt_Ds320 };
    static const uint32_t multisampleCounts[] = { 1, 2, 4, 8 };

    @autoreleasepool {
        for (CGxFormat::Format colorFormat : colorFormats) {
            for (CGxFormat::Format depthFormat : depthFormats) {
                for (uint32_t multisampleCount : multisampleCounts) {
                    CGxFormat format;
                    memset(&format, 0, sizeof(format));
                    format.m_colorFormat = colorFormat;
                    format.m_depthFormat = depthFormat;
                    format.m_multisampleCount = multisampleCount;

                    NSOpenGLPixelFormat* pixelFormat = PixelFormatCreate(format);

                    if (!pixelFormat) {
                        continue;
                    }

                    [pixelFormat release];

                    if (!CGxDevice::FormatExists(formats, format)) {
                        *formats.New() = format;
                    }
                }
            }
        }
    }

    return formats.Count() != 0;
}
