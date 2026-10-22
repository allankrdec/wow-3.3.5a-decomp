// macOS counterpart of Process/Win64/Process.cpp.
//
// - The process directory of a bundled build is the folder that holds the .app, where Data/, WTF/ and the
//   other game folders live (the executable itself is in X.app/Contents/MacOS/).
// - Child processes start with posix_spawn. The completion callback runs on a watcher thread once the child
//   exits; Windows polls from a 250 ms timer on the thread's message loop instead.

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/TempBuffer.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"

extern char** environ;

struct ChildProcess {
    pid_t m_process;
    void (*m_callback)(void*);
    void* m_param;
};

static uint32_t ChildWatcher(void* param) {
    ChildProcess* child = static_cast<ChildProcess*>(param);
    int32_t status;

    while (waitpid(child->m_process, &status, 0) < 0 && errno == EINTR) {
    }

    child->m_callback(child->m_param);
    delete child;

    return 0;
}

// Win64: 0x00435C20
bool Blizzard::Process::GetProcessPath(char* buffer, int32_t buffersize) {
    if (!buffer || buffersize <= 0) {
        return false;
    }

    char filename[PATH_MAX];
    char resolved[PATH_MAX];
    uint32_t size = sizeof(filename);
    filename[0] = '\0';

    if (!_NSGetExecutablePath(filename, &size) && realpath(filename, resolved)) {
        Blizzard::String::Copy(filename, resolved, sizeof(filename));
    }

    System_File::FromNativeName(buffer, buffersize, filename);

    return true;
}

// Win64: 0x004389C0
bool Blizzard::Process::GetProcessDirectory(char* buffer, int32_t buffersize) {
    Blizzard::Memory::TempBuffer<char, 1024> path(buffersize);

    if (!GetProcessPath(path.m_data, buffersize)) {
        return false;
    }

    if (path.m_data) {
        if (buffer) {
            int32_t length = static_cast<int32_t>(Blizzard::String::FindFilename(path.m_data) - path.m_data + 1);
            static const char s_bundleSuffix[] = ".app/Contents/MacOS/";
            int32_t suffix = sizeof(s_bundleSuffix) - 1;

            if (length - 1 >= suffix && !strncasecmp(path.m_data + length - 1 - suffix, s_bundleSuffix, suffix)) {
                path.m_data[length - 1 - suffix + 4] = '\0';
                length = static_cast<int32_t>(Blizzard::String::FindFilename(path.m_data) - path.m_data + 1);
            }

            Blizzard::String::Copy(buffer, path.m_data, std::min(length, buffersize));
        }
    } else if (buffer && buffersize > 0) {
        *buffer = '\0';
    }

    return true;
}

// Win64: 0x00440D80
bool Blizzard::Process::LaunchChildProcessWithArguments(const char* path, const char* arguments, void (*callback)(void*), void* param) {
    if (!arguments) {
        arguments = "";
    }

    // Each line of `arguments` is one argument; Windows quotes the ones with blanks into a command line.
    std::vector<std::string> list;

    while (*arguments) {
        size_t length = strcspn(arguments, "\n");
        list.push_back(std::string(arguments, length));
        arguments += length;
        arguments += strspn(arguments, "\n");
    }

    Blizzard::StringConversion_CodepageToUTF8::ToNative nativePath(path);
    std::vector<char*> argv;
    argv.push_back(nativePath.m_data);

    for (std::vector<std::string>::iterator it = list.begin(); it != list.end(); ++it) {
        argv.push_back(const_cast<char*>(it->c_str()));
    }

    argv.push_back(nullptr);

    pid_t process;

    if (posix_spawn(&process, nativePath.m_data, nullptr, nullptr, argv.data(), environ)) {
        return false;
    }

    if (!callback) {
        return true;
    }

    ChildProcess* child = new ChildProcess;
    child->m_process = process;
    child->m_callback = callback;
    child->m_param = param;

    if (!Blizzard::Thread::CreateDetached(ChildWatcher, child, "Child Process Watcher")) {
        delete child;
        return false;
    }

    return true;
}

void Blizzard::Process::Sleep(uint32_t milliseconds) {
    MacSleep(milliseconds);
}
