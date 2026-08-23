#ifndef _EXTRON_LINK_H
#define _EXTRON_LINK_H

/* Extron uses mlibc's architecture-independent ELF link ABI. Keep this
 * sysdep shim only so the platform include path does not shadow it. */
#include_next <link.h>

#endif
