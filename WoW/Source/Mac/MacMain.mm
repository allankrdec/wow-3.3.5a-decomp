// macOS entry point; counterpart of WoW/Source/Win64/WinMain.cpp.
//
// WinMain runs main() on an 8 MB fiber. Here CommonMain runs on the main thread, whose stack the linker sets to
// 8 MB (-stack_size 0x800000), because AppKit wants windows and events on the main thread. OsInputGet pumps the
// NSApplication event queue the way the Windows build pumps PeekMessage (port-mac/docs/02-build-e-compilador.md).

#import <Cocoa/Cocoa.h>
#include <cfenv>
#include <cstdlib>
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Client.h"

int main() {
    StormRtlInitialize();

    @autoreleasepool {
        [NSApplication sharedApplication];
        // WOWDECOMP_MAC_BACKGROUND=1 (automated runs): no Dock icon, never takes focus, no App Nap throttling.
        if (std::getenv("WOWDECOMP_MAC_BACKGROUND")) {
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [[[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"wowdecomp background run"] retain];
        } else {
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }
        [NSApp finishLaunching];
    }

    // Windows clears pending x87/SSE exceptions and masks them all (_clearfp/_control87); arm64 traps nothing by
    // default, so clearing the flags is enough.
    std::feclearexcept(FE_ALL_EXCEPT);

    return CommonMain();
}
