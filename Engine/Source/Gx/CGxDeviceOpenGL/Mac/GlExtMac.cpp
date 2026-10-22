// The WGL calls of GlExt.cpp on macOS. Every GL entry point of the legacy (2.1) context, core or extension, is an
// exported symbol of the OpenGL framework, so it is looked up by name there; a missing one gives nullptr, which is
// what wglGetProcAddress returns for an entry point the driver lacks. The lookup goes through the framework's handle,
// not RTLD_DEFAULT: GlExt.cpp's function pointer variables have the same names as the framework's functions
// (glActiveTextureARB...), and RTLD_DEFAULT would find those variables in the executable first.
// There is no WGL: wglGetExtensionsStringARB is never found, so HasWGLExtension() is always false and the wgl*
// extension flags stay 0.

#include <dlfcn.h>
#include "Engine/Source/Gx/CGxDeviceOpenGL/GlExt.h"

PROC wglGetProcAddress(const char* name) {
    static void* s_openGl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);

    if (!s_openGl) {
        return nullptr;
    }

    return reinterpret_cast<PROC>(dlsym(s_openGl, name));
}

HDC wglGetCurrentDC() {
    return nullptr;
}
