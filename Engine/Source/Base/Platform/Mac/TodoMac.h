#ifndef ENGINE_SOURCE_BASE_PLATFORM_MAC_TODOMAC_H
#define ENGINE_SOURCE_BASE_PLATFORM_MAC_TODOMAC_H

// Marks Mac platform code that is still a stub. The first call from each site logs "TODO_MAC: <function>" to
// stderr; the stub then returns a value-initialized result. Searching the log for TODO_MAC shows the next
// piece of platform work.

#include <cstdio>
#include <new>
#include <type_traits>

#define TODO_MAC()                                                                                  \
    do {                                                                                            \
        static bool s_todoMacLogged;                                                                \
        if (!s_todoMacLogged) {                                                                     \
            s_todoMacLogged = true;                                                                 \
            std::fprintf(stderr, "TODO_MAC: %s (%s:%d)\n", __PRETTY_FUNCTION__, __FILE__, __LINE__); \
        }                                                                                           \
    } while (0)

template <class T>
inline T TodoMacValue() {
    if constexpr (std::is_reference_v<T>) {
        alignas(std::remove_reference_t<T>) static unsigned char s_storage[sizeof(std::remove_reference_t<T>)];
        return *reinterpret_cast<std::remove_reference_t<T>*>(s_storage);
    } else {
        return T();
    }
}

#endif
