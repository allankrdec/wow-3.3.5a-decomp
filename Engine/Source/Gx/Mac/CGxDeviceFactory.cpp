// Gx device factories for the macOS build. The Direct3D backends do not exist on macOS: CreateCGxDeviceOpenGl is the
// real one (CGxDeviceOpenGL/CGxDeviceOpenGl.cpp), and the Direct3D factories also return the OpenGL device, because
// GxDefaultApi() returns GxApi_D3d9 and a gxApi cvar saved by the Windows client may name either Direct3D API. The
// device reports GxApi_OpenGl through DeviceApi(), so the game takes its OpenGL paths.

#include "Engine/Source/Gx/Gx.h"

CGxDevice* CreateCGxDeviceD3d() {
    return CreateCGxDeviceOpenGl();
}

CGxDevice* CreateCGxDeviceD3d9Ex() {
    return CreateCGxDeviceOpenGl();
}
