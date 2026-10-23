// Stage E7 test for the macOS crash handler (SErrCatchUnhandledExceptions).
//
//   CrashTest
//
// Each case runs in a forked child that initializes Storm the way the game does (StormRtlInitialize,
// StormInitialize, SErrCatchUnhandledExceptions, SErrSetLogTitleString, SErrSetLogCallback, SErrRegisterHandler)
// and then crashes:
//  1. null pointer write on the main thread (SIGSEGV),
//  2. abort() on a Storm thread (SIGABRT),
//  3. stack overflow on the main thread (SIGSEGV/SIGBUS, reported from the alternate signal stack).
// The parent checks that the child died by the signal, that the registered handler ran, and that
// <test folder>/Errors/<date> Crash.txt has the title, the log callback text, the exception, the arm64 registers,
// a stack trace with the crashing function, the loaded modules and the memory dump.
// Exit code 0 when everything passed.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dirent.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_failures;
static int s_handlerPipe = -1;

static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);

    if (!ok) {
        s_failures++;
    }
}

static int32_t STORMAPI TestLogHeader(char* buffer, uint32_t bufferchars) {
    SStrPrintf(buffer, bufferchars, "CrashTestBuild: %d\r\n", 12340);
    return 1;
}

// The game's SendErrorLog: runs after the log is written.
static int32_t STORMAPI TestErrorHandler(uint32_t errorcode, const char* description, const char* filename, int32_t linenumber, const char* errorstring) {
    char path[260];
    char line[400];
    int length = snprintf(line, sizeof(line), "handler 0x%08X %s|%s\n", errorcode, filename ? filename : "", SErrGetLogLastPath(path, sizeof(path)) ? path : "");
    write(s_handlerPipe, line, length);
    return 1;
}

extern "C" __attribute__((noinline)) void CrashTestNullDeref() {
    volatile int* pointer = nullptr;
    *pointer = 0x1234;
}

static uint32_t STORMAPI CrashTestAbortThread(void* param) {
    abort();
    return 0;
}

extern "C" __attribute__((noinline)) int CrashTestRecurse(int depth) {
    volatile char buffer[4096];
    buffer[0] = static_cast<char>(depth);
    return CrashTestRecurse(depth + 1) + buffer[0];
}

static void InitStormLikeTheGame() {
    StormRtlInitialize();
    StormInitialize();
    SErrCatchUnhandledExceptions();
    SErrSetLogTitleString("World of WarCraft (build 12340)");
    SErrSetLogCallback(TestLogHeader);
    SErrRegisterHandler(TestErrorHandler);
}

static std::string ErrorsDirectory() {
    char exe[1024];
    uint32_t size = sizeof(exe);
    std::string dir = ".";

    if (_NSGetExecutablePath(exe, &size) == 0) {
        char resolved[1024];

        if (realpath(exe, resolved)) {
            dir = resolved;
            dir = dir.substr(0, dir.rfind('/'));
        }
    }

    return dir + "/Errors";
}

static void DeleteCrashLogs(const std::string& dir) {
    DIR* d = opendir(dir.c_str());

    if (!d) {
        return;
    }

    while (dirent* entry = readdir(d)) {
        std::string name = entry->d_name;

        if (name.find("Crash.txt") != std::string::npos || name == "exception-failed.txt") {
            unlink((dir + "/" + name).c_str());
        }
    }

    closedir(d);
}

static std::string ReadCrashLog(const std::string& dir, std::string* pathOut) {
    DIR* d = opendir(dir.c_str());
    std::string text;

    if (!d) {
        return text;
    }

    while (dirent* entry = readdir(d)) {
        std::string name = entry->d_name;

        if (name.size() > 9 && name.compare(name.size() - 9, 9, "Crash.txt") == 0) {
            *pathOut = dir + "/" + name;
            FILE* file = fopen(pathOut->c_str(), "rb");

            if (file) {
                char buffer[4096];
                size_t read;

                while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                    text.append(buffer, read);
                }

                fclose(file);
            }
        }
    }

    closedir(d);
    return text;
}

static bool Contains(const std::string& text, const char* what) {
    return text.find(what) != std::string::npos;
}

static void RunCase(const char* title, void (*crash)(), const int* signals, int numSignals, const char* exceptionName, const char* function) {
    std::printf("\n== %s\n", title);
    std::string dir = ErrorsDirectory();
    DeleteCrashLogs(dir);

    int pipeFds[2];
    pipe(pipeFds);
    fflush(stdout);
    pid_t pid = fork();

    if (pid == 0) {
        close(pipeFds[0]);
        s_handlerPipe = pipeFds[1];
        // Keep the child's error report off the test output.
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);
        InitStormLikeTheGame();
        crash();
        _exit(100);
    }

    close(pipeFds[1]);
    std::string handlerOutput;
    char buffer[512];
    ssize_t read;

    while ((read = ::read(pipeFds[0], buffer, sizeof(buffer))) > 0) {
        handlerOutput.append(buffer, read);
    }

    close(pipeFds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    bool bySignal = WIFSIGNALED(status);
    int signal = bySignal ? WTERMSIG(status) : -1;
    bool expectedSignal = false;

    for (int i = 0; i < numSignals; i++) {
        expectedSignal |= signal == signals[i];
    }

    char what[256];
    snprintf(what, sizeof(what), "child died by signal %d (%s)", signal, bySignal ? strsignal(signal) : (WIFEXITED(status) ? "exited" : "?"));
    Check(bySignal && expectedSignal, what);
    std::printf("     handler: %s", handlerOutput.empty() ? "(none)\n" : handlerOutput.c_str());
    Check(Contains(handlerOutput, "handler 0x85100084"), "SErrRegisterHandler callback ran with STORM_ERROR_FATAL");

    std::string path;
    std::string log = ReadCrashLog(dir, &path);
    std::printf("     log: %s (%zu bytes)\n", path.empty() ? "(none)" : path.c_str(), log.size());

    // A copy that the next case does not delete, for reading after the run.
    static int s_case;
    FILE* copy = fopen((dir + "/CrashTest-" + std::to_string(++s_case) + ".log").c_str(), "wb");

    if (copy) {
        fwrite(log.data(), 1, log.size(), copy);
        fclose(copy);
    }

    Check(!log.empty(), "Errors/<date> Crash.txt written");
    Check(Contains(handlerOutput, path.c_str()) && !path.empty(), "SErrGetLogLastPath names the crash log");
    Check(Contains(log, "World of WarCraft (build 12340)"), "log title");
    Check(Contains(log, "CrashTestBuild: 12340"), "log callback text");
    Check(Contains(log, "Exception:") && Contains(log, exceptionName), exceptionName);
    Check(Contains(log, "arm64 Registers") && Contains(log, "PC =") && Contains(log, "CPSR="), "arm64 registers");
    Check(Contains(log, "[Current Thread]"), "stack trace of the current thread");
    snprintf(what, sizeof(what), "stack trace has %s", function);
    Check(Contains(log, function), what);
    Check(Contains(log, "Loaded Modules") && Contains(log, "libsystem_kernel.dylib"), "loaded modules");
    Check(Contains(log, "Code: 16 bytes") && Contains(log, "Stack: 1024 bytes"), "memory dump");

    struct stat info;
    Check(stat((dir + "/exception-failed.txt").c_str(), &info) != 0, "the crash handler did not crash");
}

static void CrashNullDeref() {
    CrashTestNullDeref();
}

static void CrashAbortThread() {
    uint32_t threadId;
    SCreateThread(0, CrashTestAbortThread, nullptr, 0, &threadId, const_cast<char*>("CrashTestAbortThread"));

    for (;;) {
        pause();
    }
}

static void CrashStackOverflow() {
    CrashTestRecurse(0);
}

int main(int argc, char** argv) {
    const int segv[] = { SIGSEGV };
    const int abrt[] = { SIGABRT };
    const int overflow[] = { SIGSEGV, SIGBUS };

    RunCase("null pointer write (main thread)", CrashNullDeref, segv, 1, "ACCESS_VIOLATION", "CrashTestNullDeref");
    RunCase("abort() on a Storm thread", CrashAbortThread, abrt, 1, "FATAL_APP_EXIT", "CrashTestAbortThread");
    RunCase("stack overflow (main thread)", CrashStackOverflow, overflow, 2, "ACCESS_VIOLATION", "CrashTestRecurse");

    std::printf("\n%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}
