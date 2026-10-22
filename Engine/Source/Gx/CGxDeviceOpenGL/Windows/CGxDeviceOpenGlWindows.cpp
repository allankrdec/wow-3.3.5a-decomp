// The Win32/WGL part of CGxDeviceOpenGl: game window and window procedure, pixel formats, WGL context, pbuffer,
// display modes, gamma and buffer swap. The functions are the original ones, moved here unchanged from
// CGxDeviceOpenGl.cpp; the macOS versions are in Mac/CGxDeviceOpenGlMac.mm.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <new>
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxDeviceOpenGl.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxDeviceOpenGlPlatform.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxVboBroker1.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/Types.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Storm/H/storm.h"

// STATIC: 0x00C60334
static bool s_dummyContextCreated;

// STATIC: 0x00C60338
static int32_t s_deviceChanging;

// VERIFY: 63ba8840 manual
// OFFSET: 0x0068C670
static void CreateDummyContext() {
    if (s_dummyContextCreated) {
        return;
    }

    s_dummyContextCreated = true;

    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "hwacc";

    ATOM atom = RegisterClassExA(&windowClass);

    if (!atom) {
        return;
    }

    HWND window = CreateWindowExA(0, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(atom)), "hwacc", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, nullptr, nullptr);

    if (window) {
        HDC dc = GetDC(window);

        if (dc) {
            int32_t count = DescribePixelFormat(dc, 0, 0, nullptr);

            PIXELFORMATDESCRIPTOR pfd;
            memset(&pfd.dwFlags, 0, sizeof(pfd) - offsetof(PIXELFORMATDESCRIPTOR, dwFlags));
            pfd.nSize = sizeof(pfd);
            pfd.nVersion = 1;

            for (int32_t i = 1; i <= count; i++) {
                DescribePixelFormat(dc, i, sizeof(pfd), &pfd);

                if ((pfd.dwFlags & (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL)) && !(pfd.dwFlags & (PFD_GENERIC_FORMAT | PFD_SUPPORT_GDI))) {
                    SetPixelFormat(dc, i, &pfd);

                    HGLRC context = wglCreateContext(dc);

                    if (context) {
                        wglMakeCurrent(dc, context);
                        BindGlExtensions();
                        BindWglExtensions();
                        wglMakeCurrent(nullptr, nullptr);
                    }

                    wglDeleteContext(context);
                    break;
                }
            }

            ReleaseDC(window, dc);
        }

        DestroyWindow(window);
    }

    UnregisterClassA(reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(atom)), instance);
}

// VERIFY: f09cd8b3 auto
// OFFSET: 0x0068C810
void CGxDeviceOpenGl::DeviceDestroyPbuffer() {
    if (this->m_pbuffer) {
        wglReleasePbufferDCARB(this->m_pbuffer, this->m_pbufferDC);
        wglDestroyPbufferARB(this->m_pbuffer);
    }

    this->m_pbuffer = nullptr;
    this->m_pbufferDC = nullptr;
    this->m_pbufferRC = nullptr;
}

// VERIFY: f0025cb1 manual
// OFFSET: 0x0068C860
void CGxDeviceOpenGl::IDevSetFocus(int32_t focus, const CGxFormat& format) {
    this->m_windowFocus = focus;

    if (!format.m_window || format.m_maximize == 2) {
        if (focus) {
            DISPLAY_DEVICEA device;
            memset(&device, 0, sizeof(device));
            device.cb = sizeof(device);

            DEVMODEA mode;
            memset(&mode, 0, sizeof(mode));
            mode.dmSize = sizeof(mode);

            EnumDisplayDevicesA(nullptr, 0, &device, 0);
            EnumDisplaySettingsA(device.DeviceName, format.m_apiSpecificModeID, &mode);
            ChangeDisplaySettingsExA(device.DeviceName, &mode, nullptr, CDS_FULLSCREEN, nullptr);
            SetWindowPos(this->m_hwnd, nullptr, 0, 0, format.m_size.x, format.m_size.y, SWP_NOREDRAW | SWP_NOCOPYBITS | SWP_DEFERERASE);
            ShowWindow(this->m_hwnd, SW_MAXIMIZE);

            RECT windowRect;
            GetWindowRect(this->m_hwnd, &windowRect);
            ClipCursor(&windowRect);
        } else {
            ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
            ShowWindow(this->m_hwnd, SW_MINIMIZE);
        }
    } else if (focus) {
        ShowWindow(this->m_hwnd, SW_SHOW);
    }
}

// VERIFY: dc5af953 auto
// OFFSET: 0x0068C9B0
int32_t CGxDeviceOpenGl::SetFormatMode(CGxFormat& format) {
    if (format.m_window) {
        format.m_apiSpecificModeID = 0;
        return 1;
    }

    DISPLAY_DEVICEA device;
    device.cb = sizeof(device);
    EnumDisplayDevicesA(nullptr, 0, &device, 0);

    uint32_t modeNum = 0;
    uint32_t bpp = (format.m_colorFormat != CGxFormat::Fmt_Rgb565 ? 16 : 0) + 16;

    DEVMODEA mode;
    mode.dmSize = sizeof(mode);

    if (!EnumDisplaySettingsA(device.DeviceName, 0, &mode)) {
        return 0;
    }

    while (mode.dmBitsPerPel != bpp || mode.dmDisplayFrequency != format.m_refreshRate || mode.dmPelsWidth != static_cast<DWORD>(format.m_size.x) || mode.dmPelsHeight != static_cast<DWORD>(format.m_size.y)) {
        if (!EnumDisplaySettingsA(device.DeviceName, ++modeNum, &mode)) {
            return 0;
        }
    }

    format.m_apiSpecificModeID = modeNum;
    return 1;
}

// VERIFY: 21483c6a auto
// OFFSET: 0x006A2660
static BOOL WglDestroyContext(HGLRC context) {
    wglMakeCurrent(nullptr, nullptr);
    return wglDeleteContext(context);
}

// VERIFY: a18dccdf manual
// OFFSET: 0x0068CA80
void CGxDeviceOpenGl::IDevRemoveGlContext() {
    this->DeviceDestroyPbuffer();

    if (this->m_hglrc) {
        this->ICursorDestroy();
        this->DeviceDestroyPoolAPI();
        this->ITexForceRecreation();
        this->IShaderForceRecreation();
        this->IQueryRelease();
        UnbindGlExtensions();
        WglDestroyContext(this->m_hglrc);
        this->m_hglrc = nullptr;
    }

    if (this->m_hdc) {
        SetDeviceGammaRamp(this->m_hdc, &this->m_systemGammaRamp);
        ReleaseDC(this->m_hwnd, this->m_hdc);
        this->m_hdc = nullptr;
    }

    this->m_context = 0;
}

// VERIFY: 72b6a45b manual
// OFFSET: 0x0068CB40
void CGxDeviceOpenGl::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
    this->DeviceDestroyPbuffer();
    s_deviceChanging = 1;

    if (this->m_hwnd) {
        if (this->m_ownhwnd) {
            DestroyWindow(this->m_hwnd);
            this->m_hwnd = nullptr;
        }

        if (this->m_hwnd && !this->m_ownhwnd) {
            this->IDevRemoveGlContext();
        }
    }

    if (this->m_hwndClass) {
        UnregisterClassA(reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(this->m_hwndClass)), GetModuleHandleA(nullptr));
        this->m_hwndClass = 0;
    }

    s_deviceChanging = 0;
}

// VERIFY: 01f4b10a auto
// OFFSET: 0x0068CC20
void CGxDeviceOpenGl::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);

    if (!this->m_format.m_window) {
        SetDeviceGammaRamp(this->m_hdc, &this->m_gammaRamp);
    }
}

// VERIFY: 0bfbf04f auto
// OFFSET: 0x0068CC60
void CGxDeviceOpenGl::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);

    if (!this->m_format.m_window) {
        SetDeviceGammaRamp(this->m_hdc, &this->m_gammaRamp);
    }
}

// VERIFY: 8a9d5950 manual
// OFFSET: 0x0068CCB0
static HWND CreateGameWindow(CGxFormat& format, void* param) {
    HINSTANCE instance = GetModuleHandleA(nullptr);

    DWORD style;

    if (!format.m_window) {
        style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_MAXIMIZE;
    } else if (format.m_maximize == 1) {
        style = WS_POPUP | WS_VISIBLE;
    } else if (format.m_maximize == 2) {
        style = WS_POPUP;
    } else {
        style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW;
    }

    float aspectRatio = 0.0f;

    if (format.m_size.x && format.m_size.y) {
        aspectRatio = static_cast<float>(format.m_size.x) / static_cast<float>(format.m_size.y);
    }

    RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = format.m_size.x;
    rect.bottom = format.m_size.y;
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    CGxDevice::s_clientAdjust.x = rect.right - format.m_size.x - rect.left;
    CGxDevice::s_clientAdjust.y = rect.bottom - format.m_size.y - rect.top;

    if (format.m_window) {
        if (format.m_maximize == 1) {
            format.m_pos.x = 0;
            format.m_pos.y = 0;
            format.m_size.x = GetSystemMetrics(SM_CXSCREEN);
            format.m_size.y = GetSystemMetrics(SM_CYSCREEN);
        } else {
            RECT workRect;
            workRect.left = 0;
            workRect.top = 0;
            workRect.right = format.m_size.x;
            workRect.bottom = format.m_size.y;

            if (CGxDevice::AdjustWindowRectToWorkArea(aspectRatio, &CGxDevice::s_clientAdjust, &workRect)) {
                format.m_pos.x = workRect.left;
                format.m_pos.y = workRect.top;
                format.m_size.x = workRect.right - workRect.left;
                format.m_size.y = workRect.bottom - workRect.top;
            } else {
                format.m_pos.x = 0;
                format.m_pos.y = 0;
            }
        }
    }

    int32_t width = format.m_size.x;
    int32_t height = format.m_size.y;

    if (!width) {
        width = CW_USEDEFAULT;
    }

    if (!height) {
        height = CW_USEDEFAULT;
    }

    if (format.m_window && format.m_maximize != 1 && format.m_size.x && format.m_size.y) {
        width += CGxDevice::s_clientAdjust.x;
        height += CGxDevice::s_clientAdjust.y;
    }

    return CreateWindowExA(WS_EX_APPWINDOW, "GxWindowClassOpenGl", "World of Warcraft", style, format.m_pos.x, format.m_pos.y, width, height, nullptr, nullptr, instance, param);
}

// VERIFY: 23cf22a4 auto
// OFFSET: 0x0068CE40
void CGxDeviceOpenGl::DeviceCreatePbuffer() {
    if (!wglArbPbuffer) {
        return;
    }

    int32_t pixelFormat = GetPixelFormat(this->m_hdc);
    const CRect& window = this->DeviceDefWindow();

    int32_t attributes[2];
    attributes[0] = 0;
    attributes[1] = 0;

    this->m_pbuffer = wglCreatePbufferARB(this->m_hdc, pixelFormat, static_cast<int32_t>(window.maxX - window.minX), static_cast<int32_t>(window.maxY - window.minY), attributes);
    this->m_pbufferDC = wglGetPbufferDCARB(this->m_pbuffer);

    PIXELFORMATDESCRIPTOR pfd;
    DescribePixelFormat(this->m_pbufferDC, pixelFormat, sizeof(pfd), &pfd);

    if (pfd.cRedBits == 8) {
        if (pfd.cGreenBits == 8 && pfd.cBlueBits == 8) {
            this->m_caps.m_rttFormat[GxTex_Argb8888] = 1;
        }
    } else if (pfd.cRedBits == 5 && pfd.cGreenBits == 6 && pfd.cBlueBits == pfd.cRedBits) {
        this->m_caps.m_rttFormat[GxTex_Rgb565] = 1;
    }

    this->m_pbufferRC = wglGetCurrentContext();
}

// VERIFY: 57f4221f auto
// OFFSET: 0x0068CF20
void CGxDeviceOpenGl::DeviceQueryPbuffer() {
    if (!this->m_pbuffer) {
        return;
    }

    int32_t lost;
    wglQueryPbufferARB(this->m_pbuffer, WGL_PBUFFER_LOST_ARB, &lost);

    if (lost) {
        this->DeviceDestroyPbuffer();
        this->DeviceCreatePbuffer();
    }
}

// VERIFY: b30d7051 manual
// OFFSET: 0x006A2410
static int32_t SetupPixelFormat(HDC dc, const CGxFormat& format) {
    if (format.m_multisampleCount > 1) {
        return 0;
    }

    int32_t match = 0;
    uint8_t colorBits = 16;
    uint8_t blueBits = 5;
    uint8_t greenBits = 6;
    uint8_t redBits = 5;
    uint8_t alphaBits = 0;
    uint8_t depthBits = 16;
    uint8_t stencilBits = 0;

    switch (format.m_colorFormat) {
        case CGxFormat::Fmt_Rgb565:
            colorBits = 16;
            blueBits = 5;
            greenBits = 6;
            redBits = 5;
            alphaBits = 0;
            break;

        case CGxFormat::Fmt_ArgbX888:
            colorBits = 24;
            redBits = 8;
            greenBits = 8;
            blueBits = 8;
            alphaBits = 0;
            break;

        case CGxFormat::Fmt_Argb8888:
            colorBits = 24;
            redBits = 8;
            greenBits = 8;
            blueBits = 8;
            alphaBits = 8;
            break;

        case CGxFormat::Fmt_Argb2101010:
            colorBits = 30;
            redBits = 10;
            greenBits = 10;
            blueBits = 10;
            alphaBits = 2;
            break;

        default:
            break;
    }

    switch (format.m_depthFormat) {
        case CGxFormat::Fmt_Ds160:
            depthBits = 16;
            stencilBits = 0;
            break;

        case CGxFormat::Fmt_Ds24X:
            depthBits = 24;
            stencilBits = 0;
            break;

        case CGxFormat::Fmt_Ds248:
            depthBits = 24;
            stencilBits = 8;
            break;

        case CGxFormat::Fmt_Ds320:
            depthBits = 32;
            stencilBits = 0;
            break;

        default:
            break;
    }

    PIXELFORMATDESCRIPTOR pfd;
    pfd.cAlphaBits = alphaBits;
    pfd.cColorBits = colorBits;
    pfd.cDepthBits = depthBits;
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cRedBits = 0;
    pfd.cRedShift = 0;
    pfd.cGreenBits = 0;
    pfd.cGreenShift = 0;
    pfd.cBlueBits = 0;
    pfd.cBlueShift = 0;
    pfd.cAlphaShift = 0;
    pfd.cAccumBits = 0;
    pfd.cAccumRedBits = 0;
    pfd.cAccumGreenBits = 0;
    pfd.cAccumBlueBits = 0;
    pfd.cAccumAlphaBits = 0;
    pfd.cStencilBits = stencilBits;
    memset(&pfd.cAuxBuffers, 0, 15);

    int32_t pixelFormat = ChoosePixelFormat(dc, &pfd);

    if (!pixelFormat) {
        CGxDevice::Log("ChoosePixelFormat failed, color = %d, alpha = %d, depth = %d\n", colorBits, alphaBits, depthBits);
        return 0;
    }

    DescribePixelFormat(dc, pixelFormat, sizeof(pfd), &pfd);

    int32_t matches = 1;

    if (!format.m_window) {
        if (format.m_colorFormat != CGxFormat::Fmt_ArgbX888) {
            matches = pfd.cAlphaBits == alphaBits;
        }

        if (pfd.cRedBits == redBits && pfd.cBlueBits == blueBits && pfd.cGreenBits == greenBits) {
            match = 1;
        }

        matches &= match;
    }

    if (format.m_depthFormat != CGxFormat::Fmt_Ds24X) {
        matches &= pfd.cStencilBits == stencilBits;
    }

    if (!((pfd.cDepthBits == depthBits) & matches)) {
        CGxDevice::Log("SetupPixelFormat(): pfd does not match requested");
        return 0;
    }

    if (SetPixelFormat(dc, pixelFormat, &pfd)) {
        return 1;
    }

    CGxDevice::Log("SetPixelFormat failed, color = %d, alpha = %d, depth = %d, fmt = %d\n", colorBits, alphaBits, depthBits, pixelFormat);
    return 0;
}

// VERIFY: c2b2597a manual
// OFFSET: 0x006A2680
static int32_t SetupPixelFormatWGL(HDC dc, const CGxFormat& format) {
    int32_t* attributes = static_cast<int32_t*>(alloca(512));
    attributes[0] = WGL_DRAW_TO_WINDOW_ARB;
    attributes[1] = 1;
    attributes[2] = WGL_ACCELERATION_ARB;
    attributes[3] = WGL_FULL_ACCELERATION_ARB;
    attributes[4] = WGL_SUPPORT_OPENGL_ARB;
    attributes[5] = 1;
    attributes[6] = WGL_DOUBLE_BUFFER_ARB;
    attributes[7] = 1;

    int32_t alphaBits = 0;
    int32_t redBits = 0;
    int32_t greenBits = 0;
    int32_t blueBits = 0;

    switch (format.m_colorFormat) {
        case CGxFormat::Fmt_Rgb565:
            greenBits = 6;
            alphaBits = 0;
            blueBits = 5;
            redBits = 5;
            break;

        case CGxFormat::Fmt_ArgbX888:
            greenBits = 8;
            alphaBits = 0;
            blueBits = 8;
            redBits = 8;
            break;

        case CGxFormat::Fmt_Argb8888:
            alphaBits = 8;
            blueBits = 8;
            greenBits = 8;
            redBits = 8;
            break;

        default:
            break;
    }

    attributes[8] = WGL_PIXEL_TYPE_ARB;
    attributes[9] = WGL_TYPE_RGBA_ARB;
    attributes[10] = WGL_RED_BITS_ARB;
    attributes[11] = redBits;
    attributes[12] = WGL_GREEN_BITS_ARB;
    attributes[13] = greenBits;
    attributes[14] = WGL_BLUE_BITS_ARB;
    attributes[15] = blueBits;
    attributes[16] = WGL_ALPHA_BITS_ARB;
    attributes[17] = alphaBits;

    int32_t depthBits = 0;
    int32_t stencilBits = 0;

    switch (format.m_depthFormat) {
        case CGxFormat::Fmt_Ds160:
            depthBits = 16;
            stencilBits = 0;
            break;

        case CGxFormat::Fmt_Ds24X:
            depthBits = 24;
            stencilBits = 0;
            break;

        case CGxFormat::Fmt_Ds248:
            depthBits = 24;
            stencilBits = 8;
            break;

        case CGxFormat::Fmt_Ds320:
            depthBits = 32;
            stencilBits = 0;
            break;

        default:
            break;
    }

    attributes[18] = WGL_DEPTH_BITS_ARB;
    attributes[19] = depthBits;
    attributes[20] = WGL_STENCIL_BITS_ARB;
    attributes[21] = stencilBits;

    uint32_t count = 22;

    if (glARBMultisample && format.m_multisampleCount > 1) {
        attributes[22] = WGL_SAMPLE_BUFFERS_ARB;
        attributes[23] = 1;
        attributes[24] = WGL_SAMPLES_ARB;
        count = 26;
        attributes[25] = format.m_multisampleCount;

        if (glNVMultisampleFilterHint) {
            glHint(GL_MULTISAMPLE_FILTER_HINT_NV, format.m_multisampleQuality <= 0.5f ? GL_FASTEST : GL_NICEST);
        }
    }

    attributes[count] = 0;
    attributes[count + 1] = 0;

    int32_t pixelFormat;
    UINT numFormats;

    if (!wglChoosePixelFormatARB(dc, attributes, nullptr, 1, &pixelFormat, &numFormats)) {
        return 0;
    }

    int32_t matches = 1;

    if (!numFormats) {
        return 0;
    }

    int32_t attribute;
    int32_t value;

    if (!format.m_window) {
        if (format.m_colorFormat != CGxFormat::Fmt_ArgbX888) {
            attribute = WGL_ALPHA_BITS_ARB;
            matches = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == alphaBits;
        }

        attribute = WGL_RED_BITS_ARB;
        int32_t red = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == redBits;
        matches = red & matches;

        attribute = WGL_GREEN_BITS_ARB;
        int32_t green = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == greenBits;
        matches = green & matches;

        attribute = WGL_BLUE_BITS_ARB;
        int32_t blue = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == blueBits;
        matches = blue & matches;
    }

    if (format.m_depthFormat != CGxFormat::Fmt_Ds24X) {
        attribute = WGL_STENCIL_BITS_ARB;
        int32_t stencil = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == stencilBits;
        matches &= stencil;
    }

    attribute = WGL_DEPTH_BITS_ARB;
    int32_t depth = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == depthBits;
    matches = depth & matches;

    if (glARBMultisample && format.m_multisampleCount > 1) {
        attribute = WGL_SAMPLES_ARB;
        int32_t samples = wglGetPixelFormatAttribivARB(dc, pixelFormat, 0, 1, &attribute, &value) && value == static_cast<int32_t>(format.m_multisampleCount);
        matches &= samples;
    }

    if (!matches) {
        CGxDevice::Log("SetupPixelFormatWGL(): pixelFormat does not match requested");
        return 0;
    }

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd.nVersion, 0, sizeof(pfd) - sizeof(pfd.nSize));
    pfd.nSize = sizeof(pfd);

    return SetPixelFormat(dc, pixelFormat, &pfd);
}

// VERIFY: 10007263 auto
// OFFSET: 0x006A2A50
static HGLRC CreateGLContext(HDC dc, const CGxFormat& format) {
    int32_t result;

    if (wglArbPixelFormat) {
        result = SetupPixelFormatWGL(dc, format);
    } else {
        result = SetupPixelFormat(dc, format);
    }

    if (!result) {
        return nullptr;
    }

    return wglCreateContext(dc);
}

// VERIFY: 80ad3a5a auto
// OFFSET: 0x0068CF60
int32_t CGxDeviceOpenGl::IDevAttachGlContext(const CGxFormat& format) {
    CreateDummyContext();

    this->m_hdc = GetDC(this->m_hwnd);

    if (this->m_hdc && (this->m_hglrc = CreateGLContext(this->m_hdc, format)) != nullptr) {
        wglMakeCurrent(this->m_hdc, this->m_hglrc);
        this->m_context = 1;

        if (wglExtSwapControl) {
            wglSwapIntervalEXT(format.m_vsync != 0);
        }

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

// VERIFY: 11254ee2 manual
// OFFSET: 0x0068D050
LRESULT CALLBACK CGxDeviceOpenGl::WindowProcGl(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    CGxDeviceOpenGl* device = reinterpret_cast<CGxDeviceOpenGl*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE:
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
            return 0;

        case WM_DESTROY:
            device->DeviceWM(GxWM_Destroy, 0, 0);
            return 0;

        case WM_SIZE: {
            CRect rect;
            rect.minY = 0.0f;
            rect.minX = 0.0f;
            rect.maxY = HIWORD(lparam);
            rect.maxX = LOWORD(lparam);

            if (wparam == SIZE_MINIMIZED) {
                device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 1);
            } else {
                uint32_t resizeType = 0;

                if (wparam == SIZE_MAXHIDE) {
                    resizeType = 2;
                }

                device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), resizeType);
            }

            break;
        }

        case WM_ACTIVATE:
            if (wparam) {
                if (wparam == WA_ACTIVE && !device->IDevIsWindowed()) {
                    CRect rect;
                    rect.minY = 0.0f;
                    rect.minX = 0.0f;
                    rect.maxY = 0.0f;
                    rect.maxX = 0.0f;
                    GxCapsWindowSizeInScreenCoords(rect);
                    device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 3);
                }
            } else if (!device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = 0.0f;
                rect.maxX = 0.0f;
                device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 1);
            }

            break;

        case WM_SETFOCUS:
            device->DeviceWM(GxWM_SetFocus, 0, 0);
            return 0;

        case WM_KILLFOCUS:
            device->DeviceWM(GxWM_KillFocus, 0, 0);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 0;

        case WM_DISPLAYCHANGE:
            if (!device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = HIWORD(lparam);
                rect.maxX = LOWORD(lparam);
                device->DeviceWM(GxWM_DisplayChange, reinterpret_cast<uintptr_t>(&rect), 0);
            }

            break;

        case WM_SYSCOMMAND:
            if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER) {
                return 0;
            }

            break;

        case WM_SIZING: {
            RECT* sizingRect = reinterpret_cast<RECT*>(lparam);

            CiRect rect;
            rect.minX = sizingRect->left;
            rect.minY = sizingRect->top;
            rect.maxX = sizingRect->right;
            rect.maxY = sizingRect->bottom;

            if (rect.maxX - rect.minX <= 0 || rect.maxY - rect.minY <= 0) {
                return 0;
            }

            switch (wparam) {
                case WMSZ_LEFT:
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    break;

                case WMSZ_RIGHT:
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_TOP:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPLEFT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPRIGHT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOM:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOMLEFT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_BOTTOMRIGHT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                default:
                    break;
            }

            sizingRect->left = rect.minX;
            sizingRect->bottom = rect.maxY;
            sizingRect->top = rect.minY;
            sizingRect->right = rect.maxX;

            return 1;
        }

        default:
            break;
    }

    if (device && device->m_windowProc) {
        return device->m_windowProc(hwnd, message, wparam, lparam);
    }

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

// VERIFY: bb87ce15 manual
// OFFSET: 0x0068D460
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

            if (!this->m_format.m_window || this->m_format.m_maximize == 2) {
                ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
                ShowWindow(this->m_hwnd, SW_MINIMIZE);
            }

            break;

        case GxWM_SetFocus:
            if (!s_deviceChanging) {
                this->IDevSetFocus(1, this->m_format);
            }

            break;

        case GxWM_KillFocus:
            if (!s_deviceChanging) {
                this->m_windowFocus = 0;

                if (!this->m_format.m_window || this->m_format.m_maximize == 2) {
                    ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
                    ShowWindow(this->m_hwnd, SW_MINIMIZE);
                }
            }

            break;

        default:
            break;
    }

    return 0;
}

// VERIFY: 075c29d3 auto
// OFFSET: 0x0068D610
int32_t CGxDeviceOpenGl::DeviceCreate(void* window, const CGxFormat& format) {
    s_deviceChanging = 1;
    this->m_ownhwnd = 0;

    HDC dc = GetDC(nullptr);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(nullptr, dc);

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

// VERIFY: 9f9c690c auto
// OFFSET: 0x0068D6D0
int32_t CGxDeviceOpenGl::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceOpenGl::DeviceSetFormat():");
    this->Log(format);

    s_deviceChanging = 1;
    this->IDevRemoveGlContext();
    DestroyWindow(this->m_hwnd);
    this->m_hwnd = nullptr;

    CGxFormat newFormat = format;

    if (this->SetFormatMode(newFormat) && (this->m_hwnd = CreateGameWindow(newFormat, this)) != nullptr) {
        this->IDevSetFocus(1, newFormat);

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

// VERIFY: 12cc2e1f manual
// OFFSET: 0x0068D7B0
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

    HDC dc = this->m_hdc;
    HGLRC context = this->m_hglrc;

    if (this->m_textureTarget[GxBuffers_Color].m_apiSpecific || this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        dc = this->m_pbufferDC;
        context = this->m_pbufferRC;
    }

    wglMakeCurrent(dc, context);

    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
    g_theGxDevicePtr->XformSetProjection(this->m_projection);
}

// VERIFY: ae538796 manual
// OFFSET: 0x0068D950
void CGxDeviceOpenGl::CapsWindowSizeInScreenCoords(CRect& rect) {
    if (this->IDevIsWindowed()) {
        const CRect& window = this->DeviceCurWindow();

        POINT points[2];
        points[0].x = 0;
        points[0].y = 0;
        points[1].x = static_cast<LONG>(window.maxX);
        points[1].y = static_cast<LONG>(window.maxY);
        MapWindowPoints(this->m_hwnd, nullptr, points, 2);

        rect.minY = static_cast<float>(points[0].y);
        rect.minX = static_cast<float>(points[0].x);
        rect.maxY = static_cast<float>(points[1].y);
        rect.maxX = static_cast<float>(points[1].x);
    } else {
        const CRect& window = this->DeviceCurWindow();
        rect.minY = window.minY;
        rect.minX = window.minX;
        rect.maxY = window.maxY;
        rect.maxX = window.maxX;
    }
}

// VERIFY: a93b9ae1 manual
// OFFSET: 0x0068DA10
static ATOM WindowClassCreate() {
    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = CGxDeviceOpenGl::WindowProcGl;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "GxWindowClassOpenGl";
    windowClass.hIcon = static_cast<HICON>(LoadImageA(instance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hCursor = LoadCursorA(instance, "BlizzardCursor.cur");

    if (!windowClass.hCursor) {
        windowClass.hCursor = LoadCursorA(instance, IDC_ARROW);
    }

    return RegisterClassExA(&windowClass);
}

// VERIFY: 495b2371 auto
// OFFSET: 0x0068DAA0
int32_t CGxDeviceOpenGl::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    HDC dc = GetDC(nullptr);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(nullptr, dc);

    this->m_hwndClass = WindowClassCreate();

    if (this->m_hwndClass && CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// VERIFY: a77c2696 manual
// OFFSET: 0x0068DB40
static void WglAdapterFormats(HDC dc, TSGrowableArray<CGxFormat>& formats) {
    wglGetPixelFormatAttribivARB = reinterpret_cast<PFNWGLGETPIXELFORMATATTRIBIVARBPROC>(wglGetProcAddress("wglGetPixelFormatAttribivARB"));

    if (!wglGetPixelFormatAttribivARB) {
        return;
    }

    int32_t numberAttrib = WGL_NUMBER_PIXEL_FORMATS_ARB;
    int32_t supportAttrib = WGL_SUPPORT_OPENGL_ARB;
    int32_t accelerationAttrib = WGL_ACCELERATION_ARB;
    int32_t pixelTypeAttrib = WGL_PIXEL_TYPE_ARB;
    int32_t colorBitsAttrib = WGL_COLOR_BITS_ARB;
    int32_t alphaBitsAttrib = WGL_ALPHA_BITS_ARB;
    int32_t depthBitsAttrib = WGL_DEPTH_BITS_ARB;
    int32_t stencilBitsAttrib = WGL_STENCIL_BITS_ARB;
    int32_t samplesAttrib = WGL_SAMPLES_ARB;

    int32_t count;

    if (!wglGetPixelFormatAttribivARB(dc, 0, 0, 1, &numberAttrib, &count)) {
        return;
    }

    for (int32_t i = 1; i < count; i++) {
        CGxFormat format;
        memset(&format, 0, sizeof(format));

        int32_t support;
        int32_t acceleration;
        int32_t pixelType;
        int32_t colorBits;
        int32_t alphaBits;
        int32_t depthBits;
        int32_t stencilBits;
        int32_t samples;

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &supportAttrib, &support) || !support) {
            continue;
        }

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &accelerationAttrib, &acceleration) || acceleration != WGL_FULL_ACCELERATION_ARB) {
            continue;
        }

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &pixelTypeAttrib, &pixelType) || pixelType != WGL_TYPE_RGBA_ARB) {
            continue;
        }

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &colorBitsAttrib, &colorBits)) {
            continue;
        }

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &depthBitsAttrib, &depthBits)) {
            continue;
        }

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &stencilBitsAttrib, &stencilBits)) {
            continue;
        }

        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &samplesAttrib, &samples) || !samples) {
            samples = 1;
        }

        if (colorBits == 16) {
            format.m_colorFormat = CGxFormat::Fmt_Rgb565;
        } else {
            if (colorBits != 32) {
                continue;
            }

            if (!wglGetPixelFormatAttribivARB(dc, i, 0, 1, &alphaBitsAttrib, &alphaBits)) {
                continue;
            }

            if (alphaBits) {
                if (alphaBits != 8) {
                    continue;
                }

                format.m_colorFormat = CGxFormat::Fmt_Argb8888;
            } else {
                format.m_colorFormat = CGxFormat::Fmt_ArgbX888;
            }
        }

        if (depthBits == 16) {
            if (stencilBits) {
                continue;
            }

            format.m_depthFormat = CGxFormat::Fmt_Ds160;
        } else if (depthBits == 24) {
            if (!stencilBits) {
                format.m_depthFormat = CGxFormat::Fmt_Ds24X;
            } else if (stencilBits == 8) {
                format.m_depthFormat = CGxFormat::Fmt_Ds248;
            } else {
                continue;
            }
        } else {
            if (depthBits != 32 || stencilBits) {
                continue;
            }

            format.m_depthFormat = CGxFormat::Fmt_Ds320;
        }

        format.m_multisampleCount = samples;

        if (!CGxDevice::FormatExists(formats, format)) {
            *formats.New() = format;
        }
    }
}

// VERIFY: 52975109 manual
// OFFSET: 0x0068DDD0
static void PfdAdapterFormats(HDC dc, TSGrowableArray<CGxFormat>& formats) {
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd.dwFlags, 0, sizeof(pfd) - offsetof(PIXELFORMATDESCRIPTOR, dwFlags));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;

    int32_t count = DescribePixelFormat(dc, 0, 0, nullptr);

    for (int32_t i = 1; i < count; i++) {
        DescribePixelFormat(dc, i, sizeof(pfd), &pfd);

        if (pfd.iPixelType != PFD_TYPE_RGBA) {
            continue;
        }

        if (!(pfd.dwFlags & (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL)) || (pfd.dwFlags & (PFD_GENERIC_FORMAT | PFD_SUPPORT_GDI))) {
            continue;
        }

        CGxFormat format;
        memset(&format, 0, sizeof(format));

        if (pfd.cRedBits == 8) {
            if (pfd.cBlueBits != 8 || pfd.cGreenBits != pfd.cRedBits) {
                continue;
            }

            if (pfd.cAlphaBits == 8) {
                format.m_colorFormat = CGxFormat::Fmt_Argb8888;
            } else if (!pfd.cAlphaBits) {
                format.m_colorFormat = CGxFormat::Fmt_ArgbX888;
            }
        } else {
            if (pfd.cRedBits != 5) {
                continue;
            }

            if (pfd.cGreenBits != 5 && pfd.cGreenBits != 6) {
                continue;
            }

            if (pfd.cBlueBits != 5) {
                continue;
            }

            format.m_colorFormat = CGxFormat::Fmt_Rgb565;
        }

        if (pfd.cDepthBits == 16) {
            if (pfd.cStencilBits) {
                continue;
            }

            format.m_depthFormat = CGxFormat::Fmt_Ds160;
        } else if (pfd.cDepthBits == 24) {
            if (!pfd.cStencilBits) {
                format.m_depthFormat = CGxFormat::Fmt_Ds24X;
            } else if (pfd.cStencilBits == 8) {
                format.m_depthFormat = CGxFormat::Fmt_Ds248;
            } else {
                continue;
            }
        } else {
            if (pfd.cDepthBits != 32 || pfd.cStencilBits) {
                continue;
            }

            format.m_depthFormat = CGxFormat::Fmt_Ds320;
        }

        format.m_multisampleCount = 1;

        if (!CGxDevice::FormatExists(formats, format)) {
            *formats.New() = format;
        }
    }
}

// VERIFY: fafeb5f2 manual
// OFFSET: 0x0068DF60
int32_t CGxDevice::OpenGlAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "adfmt";

    ATOM atom = RegisterClassExA(&windowClass);

    if (!atom) {
        return formats.Count() != 0;
    }

    HWND window = CreateWindowExA(0, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(atom)), "adfmt", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, nullptr, nullptr);

    if (window) {
        HDC dc = GetDC(window);

        if (dc) {
            int32_t count = DescribePixelFormat(dc, 0, 0, nullptr);

            PIXELFORMATDESCRIPTOR pfd;
            memset(&pfd.dwFlags, 0, sizeof(pfd) - offsetof(PIXELFORMATDESCRIPTOR, dwFlags));
            pfd.nSize = sizeof(pfd);
            pfd.nVersion = 1;

            for (int32_t i = 1; i <= count; i++) {
                DescribePixelFormat(dc, i, sizeof(pfd), &pfd);

                if (!(pfd.dwFlags & (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL)) || (pfd.dwFlags & (PFD_GENERIC_FORMAT | PFD_SUPPORT_GDI))) {
                    continue;
                }

                SetPixelFormat(dc, i, &pfd);

                HGLRC context = wglCreateContext(dc);

                if (!context) {
                    continue;
                }

                HGLRC currentContext = wglGetCurrentContext();
                HDC currentDC = wglGetCurrentDC();
                wglMakeCurrent(dc, context);

                wglGetExtensionsStringARB = reinterpret_cast<PFNWGLGETEXTENSIONSSTRINGARBPROC>(wglGetProcAddress("wglGetExtensionsStringARB"));

                if (HasWGLExtension("WGL_ARB_pixel_format")) {
                    WglAdapterFormats(dc, formats);
                } else {
                    PfdAdapterFormats(dc, formats);
                }

                wglMakeCurrent(currentDC, currentContext);
                wglDeleteContext(context);
                break;
            }

            ReleaseDC(window, dc);
        }

        DestroyWindow(window);
    }

    UnregisterClassA(reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(atom)), instance);

    return formats.Count() != 0;
}

// VERIFY: a13dc98e manual
// OFFSET: 0x0069E220
void CGxDeviceOpenGl::ScenePresent() {
    int32_t screenShot = this->m_scrShotClick;
    CGxDevice::ScenePresent();

    if (screenShot) {
        this->DeviceScreenShot();
    }

    this->ICursorDraw();

    if (this->m_hwMasterEnables & 0x40) {
        wglSwapLayerBuffers(this->m_hdc, WGL_SWAP_MAIN_PLANE);
    } else {
        glFinish();
    }

    this->WaitForFPSCap();
    this->IShaderConstantsClear();
}
