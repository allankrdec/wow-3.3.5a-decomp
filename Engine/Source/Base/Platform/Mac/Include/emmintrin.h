#ifndef ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_EMMINTRIN_H
#define ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_EMMINTRIN_H

// arm64 has no SSE: the SSE/SSE2 intrinsics come from sse2neon, which maps them to NEON.
#include "ThirdParty/sse2neon/sse2neon.h"

#endif
