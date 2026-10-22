/* expat configuration for the macOS build; the same settings as lib/winconfig.h. */
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H

#include <string.h>

#define XML_NS 1
#define XML_DTD 1
#define XML_CONTEXT_BYTES 1024

/* arm64 is little endian */
#define BYTEORDER 1234

#define HAVE_MEMMOVE

#endif
