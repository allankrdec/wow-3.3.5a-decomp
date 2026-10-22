// macOS counterpart of Battlenet/Win64/Battlenet.cpp.
//
// The Win64 file loads Battle.net.dll, a proprietary Windows binary that cannot be loaded on the Mac (the original
// Mac client shipped Battle.net.dylib, which is not available). Battle.net login (accounts with '@') is out of scope
// for the port (port-mac/docs/06-rede-login-anticheat.md), so Library::Load always fails: CreepTendril::Initialize
// then returns false and the Battle.net path reports itself unavailable. Grunt login does not use this file.

#include <new>
#include "WoW/Source/Battlenet/Battlenet.h"

// Win64: 0x009C28B0
void* Battlenet::WindowsLibrary::GetProc(const char* name) {
    return nullptr;
}

// Win64: 0x009C28D0
void* Battlenet::WindowsLibrary::GetProc(uint32_t ordinal) {
    return nullptr;
}

// Win64: 0x009C2900
Battlenet::Library* Battlenet::Library::Load(const char* path, Allocator& allocator) {
    return nullptr;
}

// Win64: 0x009C2A30
Battlenet::WindowsLibrary::~WindowsLibrary() {
}
