#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <stdbool.h>
#include <stdint.h>

/* Kernel command line as handed over by the loader (a multiboot2 CMDLINE tag,
 * read by the x86-64 UEFI loader from \extron\cmdline.txt). */
void cmdline_init(uint64_t mb2_addr);
const char *cmdline_raw(void);

/* "key=value": copies the value (up to the next space) into `out` and returns
 * true. */
bool cmdline_get(const char *key, char *out, unsigned size);
/* A bare word such as "ro" or "quiet". */
bool cmdline_has(const char *word);

#endif
