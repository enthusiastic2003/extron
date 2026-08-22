#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <kernel/sync/spinlock.h>
#include <kernel/klibc/string.h>

static uint8_t* vmm_bitmap = (uint8_t*)VMM_BITMAP_VIRT_START;
static uint64_t last_allocated_index = 0;
static spinlock_t vmm_lock = SPINLOCK_INIT;

/* ---- bitmap ---- */

static inline void bitmap_set(uint64_t i) {
    vmm_bitmap[i >> 3] |= (1ULL << (i & 7));
}

static inline void bitmap_clear(uint64_t i) {
    vmm_bitmap[i >> 3] &= ~(1ULL << (i & 7));
}

static inline int bitmap_test(uint64_t i) {
    return vmm_bitmap[i >> 3] & (1ULL << (i & 7));
}

/* ---- init ---- */

void vmm_init() {
    uint64_t pages = (KERNEL_HEAP_BITMAP_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        /* Boot-critical: there is no meaningful way to continue without
         * the VMM's own bitmap, so this is a legitimate panic — unlike
         * the allocator itself, which must return failure. */
        if (!p) panic("VMM: bitmap alloc fail (out of physical memory)");

        kmap(VMM_BITMAP_VIRT_START + i * PAGE_SIZE,
             p,
             PAGE_PRESENT | PAGE_WRITE);
    }

    for (uint64_t i = 0; i < KERNEL_HEAP_BITMAP_SIZE; i++)
        vmm_bitmap[i] = 0;

    kprintf("VMM: Bitmap initialized (%llu bytes)\n", KERNEL_HEAP_BITMAP_SIZE);
}

/* ---- alloc ---- */

virt_addr_t vmm_alloc_pages(size_t n) {
    spin_lock(&vmm_lock);

    uint64_t total = KERNEL_HEAP_SIZE / PAGE_SIZE;
    uint64_t found = 0, start = 0;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t begin = (pass == 0) ? last_allocated_index : 0;
        uint64_t end   = (pass == 0) ? total : last_allocated_index;

        // FIX: Reset search counter if we wrap around to the beginning
        if (pass == 1) found = 0;

        for (uint64_t i = begin; i < end; i++) {
            if (!bitmap_test(i)) {
                if (found == 0) start = i;

                if (++found == n) {
                    /* Physical pages can genuinely run out now that
                     * pmm_alloc_page() returns NULL instead of panicking,
                     * so a partial allocation has to be undone rather
                     * than left mapped and leaked. */
                    uint64_t done = 0;
                    for (; done < n; done++) {
                        uint64_t idx = start + done;

                        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
                        if (!p)
                            break;

                        if (kmap(KERNEL_HEAP_START + idx * PAGE_SIZE,
                                 p,
                                 PAGE_PRESENT | PAGE_WRITE) != 0) {
                            pmm_free_page((void*)p);
                            break;
                        }

                        bitmap_set(idx);
                    }

                    if (done < n) {
                        for (uint64_t j = 0; j < done; j++) {
                            uint64_t idx = start + j;
                            virt_addr_t va = KERNEL_HEAP_START + idx * PAGE_SIZE;
                            phys_addr_t p = kvirt_to_phys(va);
                            kunmap(va);
                            if (p) pmm_free_page((void*)p);
                            bitmap_clear(idx);
                        }
                        spin_unlock(&vmm_lock);
                        return 0;
                    }

                    last_allocated_index = start + n;
                    virt_addr_t result = KERNEL_HEAP_START + start * PAGE_SIZE;
                    spin_unlock(&vmm_lock);
                    return result;
                }
            } else {
                found = 0;
            }
        }
    }

    /* 0, not panic: kmalloc's caller can cope with a failed allocation,
     * and a userspace-driven request must never be able to halt the
     * kernel. liballoc already handles a NULL from liballoc_alloc(). */
    spin_unlock(&vmm_lock);
    return 0;
}


virt_addr_t vmm_alloc_page() {
    return vmm_alloc_pages(1);
}

/* ---- free ---- */

/* Holds vmm_lock across the whole teardown, which it previously did not
 * while vmm_alloc_pages() did. Lock order is vmm_lock -> pmm_lock, the
 * same direction the allocation path takes, so the two can't deadlock
 * against each other. */
int vmm_free_pages(virt_addr_t v, size_t n) {
    // Included the bounds check fix from earlier!
    if (v < KERNEL_HEAP_START ||
        v + (n * PAGE_SIZE) > KERNEL_HEAP_START + KERNEL_HEAP_SIZE)
        return -1;

    uint64_t start = (v - KERNEL_HEAP_START) / PAGE_SIZE;

    spin_lock(&vmm_lock);

    for (uint64_t i = 0; i < n; i++) {
        virt_addr_t addr = v + i * PAGE_SIZE;

        // Use kvirt_to_phys to find the address before unmapping
        phys_addr_t p = kvirt_to_phys(addr);
        
        if (p) {
            kunmap(addr);
            pmm_free_page((void*)p);
        }
        else {
            spin_unlock(&vmm_lock);
            panic("VMM: Tried to free virtual address that was never mapped: %p\n", (void*)addr);
        }

        bitmap_clear(start + i);
    }

    spin_unlock(&vmm_lock);
    return 0;
}

int vmm_free_page(virt_addr_t v) {
    return vmm_free_pages(v, 1);
}

/* ---- stack ---- */

virt_addr_t vmm_setup_stack() {
    virt_addr_t base = KERNEL_STACK_RANGE_START + KERNEL_STACK_GUARD;

    for (uint64_t i = 0; i < KERNEL_STACK_SIZE; i += PAGE_SIZE) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        if (!p) panic("VMM: stack alloc fail (out of physical memory)");

        if (kmap(base + i, p, PAGE_PRESENT | PAGE_WRITE) != 0) {
            panic("VMM: stack map fail");
        }
    }

    virt_addr_t top = base + KERNEL_STACK_SIZE;

    kprintf("VMM: stack at %p (top %p)\n",
            (void*)base, (void*)top);

    return top;
}

