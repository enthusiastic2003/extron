#include <kernel/mm/kheap.h>
#include <kernel/sync/spinlock.h>

static spinlock_t heap_lock = SPINLOCK_INIT;

int liballoc_lock() {
    spin_lock(&heap_lock);
    return 0;
}

int liballoc_unlock() {
    spin_unlock(&heap_lock);
    return 0;
}

void* liballoc_alloc(size_t num_pages) {
    return (void*)vmm_alloc_pages(num_pages);
}

int liballoc_free(void* addr, size_t num_pages) {
    return vmm_free_pages((virt_addr_t)addr, num_pages);
}

/* The allocator itself lives in kernel/mm/liballoc.c — this file is only
 * the OS-specific hooks it calls back into. See that file's header. */
