// macOS counterpart of Glue/Win64/PatchDownloadGlue.cpp.
//
// Windows Vista and later run the downloaded patcher elevated through ShellExecute("runas"); older Windows launch it
// as a plain child process. The Mac has no "runas" verb, so it takes the plain child-process path. There are no
// patches for 3.3.5a, so this is not expected to run.

#include <cstdint>
#include "WoW/Source/Glue/CGlueMgr.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"

// Win64: 0x00462190
bool CGlueMgr::PatchDownloadExecuteCommand(const char* path, const char* arguments) {
    return Blizzard::Process::LaunchChildProcessWithArguments(path, arguments, nullptr, nullptr);
}
