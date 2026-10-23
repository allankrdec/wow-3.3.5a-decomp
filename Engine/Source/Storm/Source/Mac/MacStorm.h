#ifndef ENGINE_SOURCE_STORM_SOURCE_MAC_MACSTORM_H
#define ENGINE_SOURCE_STORM_SOURCE_MAC_MACSTORM_H

// Helpers shared by the macOS Storm platform files.
//
// Waitable handles: the HANDLE kept in SSyncObject::m_opaqueData. Events, semaphores, mutexes and threads share
// one representation so that SSyncObject::Wait and WaitMultiplePtr work on any of them with the Win32
// WaitForSingleObject/WaitForMultipleObjects semantics (auto/manual-reset events, counted semaphores, recursive
// owned mutexes, threads signaled when they exit, atomic wait-all). All waitable state is guarded by one process
// mutex and waiters sleep on one condition variable; Storm has only a handful of such objects, the hot SRWLock
// events use MacEvent instead.

#include <cstdint>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include "Engine/Source/Base/Platform/WinTypes.h"

#define MAC_WAIT_OBJECT_0 0x00000000
#define MAC_WAIT_ABANDONED 0x00000080
#define MAC_WAIT_TIMEOUT 0x00000102
#define MAC_WAIT_FAILED 0xFFFFFFFF

HANDLE MacWaitableCreateEvent(bool manualReset, bool initialState);
HANDLE MacWaitableCreateSemaphore(int32_t initialCount, int32_t maximumCount);
HANDLE MacWaitableCreateMutex(bool initialOwner);
// A thread handle starts with two references: the creator's and the running thread's (dropped by
// MacWaitableThreadExit).
HANDLE MacWaitableCreateThread();
void MacWaitableClose(HANDLE handle);

bool MacWaitableSetEvent(HANDLE handle);
bool MacWaitableResetEvent(HANDLE handle);
bool MacWaitableReleaseSemaphore(HANDLE handle, int32_t count);
bool MacWaitableReleaseMutex(HANDLE handle);
void MacWaitableSetThread(HANDLE handle, pthread_t thread);
bool MacWaitableGetThread(HANDLE handle, pthread_t* thread);
void MacWaitableThreadExit(HANDLE handle);

// Returns MAC_WAIT_OBJECT_0 + index, MAC_WAIT_TIMEOUT or MAC_WAIT_FAILED; timeout in ms (0xFFFFFFFF: none).
uint32_t MacWaitableWait(uint32_t count, const HANDLE* handles, bool waitAll, uint32_t timeout);

// Win32 thread priorities (THREAD_PRIORITY_*) mapped onto the pthread scheduling priority of a thread.
int32_t MacGetThreadPriority(pthread_t thread);
bool MacSetThreadPriority(pthread_t thread, int32_t priority);

// 32-bit thread id (Mach thread port) like GetCurrentThreadId.
uint32_t MacThreadId(pthread_t thread);

// Full path of the executable (GetModuleFileName(nullptr)).
void MacGetModuleFileName(char* buffer, uint32_t bufferchars);

// Directory the game treats as the executable's folder, with a trailing '/'. For an executable inside an
// application bundle (Foo.app/Contents/MacOS/foo) this is the folder containing Foo.app, so logs and error
// reports land next to the game data instead of inside the bundle.
void MacGetModuleDirectory(char* buffer, uint32_t bufferchars);

// Converts the '\\' separators of a Windows path to '/'.
void MacConvertPath(char* path);

// Creates every missing directory of a '/'-separated path, including the last component.
bool MacCreateDirectories(const char* path);

// OutputDebugString: stderr.
void MacOutputDebugString(const char* string);

// IsDebuggerPresent.
bool MacIsDebuggerPresent();

// Crash contexts. WinTypes.h leaves CONTEXT and EXCEPTION_POINTERS opaque; on macOS they wrap what a crash signal
// handler receives: the signal's siginfo and the ucontext of the interrupted thread (arm64: uc_mcontext->__ss
// holds x0-x28, fp, lr, sp, pc, cpsr and __es the fault status).
struct _CONTEXT {
    ucontext_t* ucontext;
};

struct _EXCEPTION_POINTERS {
    siginfo_t* ExceptionRecord;
    CONTEXT* ContextRecord;
};

// Alternate signal stack for the crash handler on the calling thread, so a stack overflow can still be reported.
// SErrCatchUnhandledExceptions installs one on its thread; Storm threads install their own while it is active.
void MacCrashInstallThreadStack();
void MacCrashRemoveThreadStack();

// Fills the caches MachineState uses from the crash handler (user name, executable path) and warms up backtrace()
// and dladdr(), so the first calls do not happen inside a signal handler.
void MacMachineStatePrepare();

#endif
