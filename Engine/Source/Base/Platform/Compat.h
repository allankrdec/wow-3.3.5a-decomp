#ifndef ENGINE_SOURCE_BASE_PLATFORM_COMPAT_H
#define ENGINE_SOURCE_BASE_PLATFORM_COMPAT_H

// MSVC compiler and CRT idioms for non-MSVC builds (macOS). Force-included before every source file of those
// builds; MSVC never sees it. Only compiler/CRT spellings belong here, not the Win32 API: platform code lives in
// the Mac/ folders. See port-mac/docs/02-build-e-compilador.md.

#if !defined(_MSC_VER)

#include <alloca.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

// Calling conventions: arm64 and x86_64 SysV have a single one.
#define __stdcall
#define __cdecl
#define __fastcall
#define __thiscall
#define WINAPI
#define CALLBACK
#define APIENTRY

#define __forceinline inline __attribute__((always_inline))

#define __declspec(x) WOWDECOMP_DECLSPEC_##x
#define WOWDECOMP_DECLSPEC_noinline __attribute__((noinline))
#define WOWDECOMP_DECLSPEC_noreturn __attribute__((noreturn))
#define WOWDECOMP_DECLSPEC_naked __attribute__((naked))
#define WOWDECOMP_DECLSPEC_align(n) __attribute__((aligned(n)))
#define WOWDECOMP_DECLSPEC_selectany __attribute__((weak))
#define WOWDECOMP_DECLSPEC_thread __thread
#define WOWDECOMP_DECLSPEC_dllimport
#define WOWDECOMP_DECLSPEC_dllexport __attribute__((visibility("default")))

#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long

// Windows LONG is 32 bits on every target; only used to spell Interlocked* operands.
typedef int32_t LONG;

#define MAX_PATH 260

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _strdup strdup
#define _strtoi64 strtoll
#define _strtoui64 strtoull
#define _atoi64 atoll
#define _alloca alloca

static inline char* _strupr(char* string) {
    for (char* c = string; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    return string;
}

static inline char* _strlwr(char* string) {
    for (char* c = string; *c; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
    return string;
}

// The Windows CRT takes '\\' as a path separator; the game passes paths like "Screenshots\\x.jpg" straight to fopen.
static inline FILE* WowCompatFopen(const char* filename, const char* mode) {
    char path[1024];
    size_t i = 0;
    for (; filename[i] && i < sizeof(path) - 1; i++) {
        path[i] = filename[i] == '\\' ? '/' : filename[i];
    }
    path[i] = '\0';
    return (fopen)(path, mode);
}
#define fopen WowCompatFopen

static inline int fopen_s(FILE** stream, const char* filename, const char* mode) {
    *stream = WowCompatFopen(filename, mode);
    return *stream ? 0 : errno;
}

static inline unsigned int _rotl(unsigned int value, int shift) {
    return __builtin_rotateleft32(value, (unsigned int)shift);
}

static inline unsigned int _rotr(unsigned int value, int shift) {
    return __builtin_rotateright32(value, (unsigned int)shift);
}

#if defined(__cplusplus)

#include <cmath>

static inline int _finite(double x) {
    return std::isfinite(x);
}

static inline int _isnan(double x) {
    return std::isnan(x);
}

// MSVC typeid(T).raw_name() is the decorated name; callers only pass it to the allocator as a debug tag.
#define raw_name name

// Interlocked*: sequentially consistent like the Win32 functions. Templates so the operand keeps whatever
// integer type the caller declared (long, LONG, int32_t).
template <class T>
inline T InterlockedIncrement(volatile T* addend) {
    return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

template <class T>
inline T InterlockedDecrement(volatile T* addend) {
    return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

template <class T, class V>
inline T InterlockedExchangeAdd(volatile T* addend, V value) {
    return __atomic_fetch_add(addend, static_cast<T>(value), __ATOMIC_SEQ_CST);
}

template <class T, class V>
inline T InterlockedExchange(volatile T* target, V value) {
    return __atomic_exchange_n(target, static_cast<T>(value), __ATOMIC_SEQ_CST);
}

template <class T, class V, class C>
inline T InterlockedCompareExchange(volatile T* destination, V exchange, C comparand) {
    T expected = static_cast<T>(comparand);
    __atomic_compare_exchange_n(destination, &expected, static_cast<T>(exchange), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

inline void* InterlockedCompareExchangePointer(void* volatile* destination, void* exchange, void* comparand) {
    void* expected = comparand;
    __atomic_compare_exchange_n(destination, &expected, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

inline void* InterlockedExchangePointer(void* volatile* target, void* value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

#else

#include <math.h>

#define _finite(x) isfinite(x)
#define _isnan(x) isnan(x)

#endif

#endif

#endif
