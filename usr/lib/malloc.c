#include <liballoc_config.h>
#include <extron/syscall.h>

/*
 * Userspace hooks for the shared allocator (kernel/mm/liballoc.c). The
 * kernel's equivalents live in kernel/mm/kheap.c and hand out kernel
 * pages; these hand out user pages via SYS_ANON_ALLOC, which the VMA
 * allocator (kernel/mm/uvm.c) backs.
 *
 * No locking: this kernel gives a process no way to create a second
 * thread yet, so there is nothing to exclude. Revisit when threads land
 * — liballoc calls these around every list mutation precisely so that
 * change is a two-function edit.
 */
#define PAGE_SIZE 4096

int liballoc_lock(void)   { return 0; }
int liballoc_unlock(void) { return 0; }

void *liballoc_alloc(size_t num_pages) {
    return sys_anon_alloc(num_pages * PAGE_SIZE);
}

int liballoc_free(void *addr, size_t num_pages) {
    return (int)sys_anon_free(addr, num_pages * PAGE_SIZE);
}
