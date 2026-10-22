#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_CGXDEVICEOPENGLPLATFORM_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_CGXDEVICEOPENGLPLATFORM_H

// Private to the OpenGL backend. CGxDeviceOpenGl.cpp has the portable GL code; the window, context, pixel format,
// pbuffer and present functions are in Windows/CGxDeviceOpenGlWindows.cpp (WGL) and Mac/CGxDeviceOpenGlMac.mm
// (NSOpenGL). These are the tables of CGxDeviceOpenGl.cpp that both parts use.

#include <cstdint>
#include "Engine/Source/Gx/CGxDeviceOpenGL/GlExt.h"
#include "Engine/Source/Gx/Types.h"

extern const GLenum s_glTexTarget[4];
extern const GLint s_glInternalFormat[GxTexFormats_Last];

#endif
