// Gx device factories for the macOS build. The Direct3D backends do not exist on macOS, and the OpenGL backend
// (CGxDeviceOpenGL) is Windows-only until stage E4 gives it a CGL context.

#include "Engine/Source/Base/Platform/Mac/TodoMac.h"
#include "Engine/Source/Gx/Gx.h"

CGxDevice* CreateCGxDeviceD3d() {
    return nullptr;
}

CGxDevice* CreateCGxDeviceD3d9Ex() {
    return nullptr;
}

CGxDevice* CreateCGxDeviceOpenGl() {
    TODO_MAC();
    return nullptr;
}
