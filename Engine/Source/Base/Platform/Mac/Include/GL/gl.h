#ifndef ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_GL_GL_H
#define ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_GL_GL_H

// <GL/gl.h> on macOS: the OpenGL framework's legacy (2.1) header. GL_GLEXT_LEGACY keeps <OpenGL/glext.h> out, like
// the Windows <GL/gl.h> (GL 1.1 only): the backend declares its extension entry points itself, as function
// pointers with the same names (GlExt.h).

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif

#ifndef GL_GLEXT_LEGACY
#define GL_GLEXT_LEGACY
#endif

#include <OpenGL/gl.h>

#endif
