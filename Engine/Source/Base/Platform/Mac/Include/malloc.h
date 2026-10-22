#ifndef ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_MALLOC_H
#define ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_MALLOC_H

// The macOS SDK has no <malloc.h>; the common code includes it for malloc/free, _alloca and _msize.
#include <alloca.h>
#include <malloc/malloc.h>
#include <stdlib.h>

#define _msize malloc_size

#endif
