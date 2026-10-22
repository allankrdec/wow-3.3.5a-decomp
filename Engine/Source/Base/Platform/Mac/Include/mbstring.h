#ifndef ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_MBSTRING_H
#define ENGINE_SOURCE_BASE_PLATFORM_MAC_INCLUDE_MBSTRING_H

// MSVC multibyte-character CRT functions used by the common code. The client runs with a single-byte code
// page (SBCS), where the _ismbc* functions reduce to their <ctype.h> counterparts.
#include <ctype.h>

#define _ismbcspace(c) isspace(c)

#endif
