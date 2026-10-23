// macOS counterpart of Engine/Source/Os/Win64/OsClipboard.cpp: the general pasteboard (NSPasteboard, in
// MacWindow.mm) instead of the Windows clipboard.
// Win64 reads and writes CF_TEXT, in the ANSI code page, and converts it from and to the game's UTF-8. The
// pasteboard holds Unicode text, so the UTF-8 goes in and out unchanged (no character is lost to the code page);
// a string that is not valid UTF-8 is stored as Windows-1252, the code page GetACP reports. The window does not
// matter: the pasteboard has no owner window.

#include <cstdint>
#include "Engine/Source/Os/Mac/MacWindow.h"
#include "Engine/Source/Os/OsClipboard.h"
#include "Engine/Source/Storm/H/storm.h"

static void* ClipboardAlloc(uint32_t size) {
    return SMemAlloc(size, __FILE__, __LINE__, 0);
}

// Win64: 0x008726F0
char* IOsClipboardGetString(void* window) {
    return MacClipboardGetString(ClipboardAlloc);
}

// Win64: 0x008727C0
void OsClipboardFreeString(char* string) {
    if (string) {
        SMemFree(string, __FILE__, __LINE__, 0);
    }
}

// Win64: 0x008727E0
int32_t IOsClipboardPutString(const char* string, void* window) {
    if (!string) {
        return 0;
    }

    return MacClipboardPutString(string);
}
