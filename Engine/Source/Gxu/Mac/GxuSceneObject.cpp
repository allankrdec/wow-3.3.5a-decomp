// GxuSceneObject declares its virtual functions without defining them. MSVC never needs the base class vtable;
// the Itanium C++ ABI emits the class's vtable and typeinfo with the first virtual function, so the macOS build
// defines them here. Derived classes override all of them.

#include "Engine/Source/Base/Platform/Mac/TodoMac.h"
#include "Engine/Source/Gxu/GxuScene.h"

void GxuSceneObject::Unk00() {
    TODO_MAC();
}

void GxuSceneObject::Release() {
    TODO_MAC();
}

bool GxuSceneObject::IsAlive() {
    TODO_MAC();
    return false;
}

void GxuSceneObject::Update(float elapsed) {
    TODO_MAC();
}

void GxuSceneObject::Render() {
    TODO_MAC();
}
