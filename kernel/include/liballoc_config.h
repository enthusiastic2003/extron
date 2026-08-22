#ifndef LIBALLOC_CONFIG_H
#define LIBALLOC_CONFIG_H

/*
 * Kernel-side configuration for kernel/mm/liballoc.c.
 *
 * The userspace build supplies its own file of this name on its own
 * include path (usr/include/liballoc_config.h) — same allocator source,
 * different names and different page supply. Found via -Ikernel/include.
 */

#include <stdint.h>
#include <stddef.h>
#include <kernel/mm/vmm.h>

/* Kernel allocations are kmalloc/kfree, keeping them visibly distinct
 * from any userspace malloc that ends up linked into the same tree. */
#define PREFIX(func) k ## func

int   liballoc_lock(void);
int   liballoc_unlock(void);
void *liballoc_alloc(size_t num_pages);
int   liballoc_free(void *addr, size_t num_pages);

void *PREFIX(malloc)(size_t);
void *PREFIX(realloc)(void *, size_t);
void *PREFIX(calloc)(size_t, size_t);
void  PREFIX(free)(void *);

#endif
