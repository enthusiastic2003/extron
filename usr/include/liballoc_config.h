#ifndef LIBALLOC_CONFIG_H
#define LIBALLOC_CONFIG_H

/*
 * Userspace configuration for kernel/mm/liballoc.c — the SAME allocator
 * source the kernel uses, compiled a second time with different names
 * and a different page supply. See usr/lib/malloc.c for the hooks.
 *
 * Found via -Iusr/include, which shadows the kernel's file of this name.
 */

#include <stdint.h>
#include <stddef.h>

/* Userspace gets the unprefixed names — this is the real malloc. */
#define PREFIX(func) func

int   liballoc_lock(void);
int   liballoc_unlock(void);
void *liballoc_alloc(size_t num_pages);
int   liballoc_free(void *addr, size_t num_pages);

void *malloc(size_t);
void *realloc(void *, size_t);
void *calloc(size_t, size_t);
void  free(void *);

#endif
