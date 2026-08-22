#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <arch/irq_spinlock.h>
#include <kernel/klibc/string.h>
#include <stdbool.h>

/* Range managed by vm_allocate_region()/vm_free_region() — clear of the
 * fixed test VAs used elsewhere (ELF load at 0x400000, stack at
 * 0x500000). 256MB is generous for a hobby kernel and easy to raise. */
#define USER_HEAP_START 0x10000000UL
#define USER_HEAP_SIZE  0x10000000UL /* 256MB */

struct vm_space *vm_space_create(phys_addr_t ttbr0) {
    struct vm_space *mm = kmalloc(sizeof(struct vm_space));
    if (!mm)
        return NULL;

    mm->lock       = (spinlock_t)SPINLOCK_INIT;
    mm->ttbr0      = ttbr0;
    mm->heap_start = USER_HEAP_START;
    mm->heap_end   = USER_HEAP_START + USER_HEAP_SIZE;
    mm->vmas       = NULL;
    return mm;
}

void vm_space_destroy(struct vm_space *mm) {
    if (!mm)
        return;

    struct vma *v = mm->vmas;
    while (v) {
        struct vma *next = v->next;
        for (size_t off = 0; off < v->size; off += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, v->base + off);
            if (phys) {
                unmap_page(mm->ttbr0, v->base + off);
                /* Only pages this process actually owns go back to the
                 * PMM — see struct vma's owns_pages comment. */
                if (v->owns_pages)
                    pmm_free_page((void *)phys);
            }
        }
        kfree(v);
        v = next;
    }
    kfree(mm);
}

/* First-fit scan through the gaps between consecutive VMAs (plus the
 * gap before the first one and after the last one, up to heap_end).
 * Free space is never itself a tracked object, so there's no separate
 * free-list to keep in sync — the gap a removed VMA leaves behind is
 * just automatically visible here on the next call. */
virt_addr_t vm_allocate_region(struct vm_space *mm, size_t size, int flags) {
    if (!mm || size == 0)
        return 0;
    /* align_up() would wrap a near-SIZE_MAX request to 0, and a 0-sized
     * region then "succeeds": the mapping loop body never runs, the
     * failure check `mapped < size` is 0 < 0, and the caller gets a VMA
     * of length zero back as if it were a real allocation. Reject before
     * the arithmetic can wrap rather than after. */
    if (size > (size_t)-1 - (PAGE_SIZE - 1))
        return 0;

    size = align_up(size, PAGE_SIZE);
    irq_spin_lock(&mm->lock);

    virt_addr_t cursor = mm->heap_start;
    struct vma *prev = NULL;
    struct vma *cur = mm->vmas;

    while (cur) {
        if (cur->base - cursor >= size)
            break;
        cursor = cur->base + cur->size;
        prev = cur;
        cur = cur->next;
    }

    if (mm->heap_end - cursor < size) {
        irq_spin_unlock(&mm->lock);
        return 0; /* no gap big enough */
    }

    uint64_t pflags = arch_translate_vm_flags(flags);
    size_t mapped = 0;
    for (; mapped < size; mapped += PAGE_SIZE) {
        void *page = pmm_alloc_page();
        if (!page)
            break;
        /* pmm_alloc_page() returns a physical address disguised as
         * void* (same convention exec.c relies on) — not something
         * safe to dereference directly; only phys_to_virt_hhdm() gives
         * a real pointer. */
        memset(phys_to_virt_hhdm((phys_addr_t)page), 0, PAGE_SIZE);
        if (map_page(mm->ttbr0, cursor + mapped, (phys_addr_t)page, pflags) != 0) {
            pmm_free_page(page);
            break;
        }
    }

    if (mapped < size) {
        /* OOM partway through — unwind what was mapped. */
        for (size_t off = 0; off < mapped; off += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, cursor + off);
            unmap_page(mm->ttbr0, cursor + off);
            if (phys)
                pmm_free_page((void *)phys);
        }
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    struct vma *node = kmalloc(sizeof(struct vma));
    if (!node) {
        for (size_t off = 0; off < size; off += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, cursor + off);
            unmap_page(mm->ttbr0, cursor + off);
            if (phys)
                pmm_free_page((void *)phys);
        }
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    node->base       = cursor;
    node->size       = size;
    node->owns_pages = true;   /* freshly allocated here, ours to free */
    node->next       = cur;
    if (prev)
        prev->next = node;
    else
        mm->vmas = node;

    irq_spin_unlock(&mm->lock);
    return cursor;
}

/* Frees the region whose base == addr. `size` is not trusted beyond
 * this being a real request — the VMA's own recorded size is what
 * actually gets unmapped/freed, so a mismatched caller can't corrupt
 * more or less than was really allocated. No-op if nothing starts at
 * exactly `addr`. */
void vm_free_region(struct vm_space *mm, virt_addr_t addr, size_t size) {
    (void)size;
    if (!mm)
        return;

    irq_spin_lock(&mm->lock);

    struct vma *prev = NULL;
    struct vma *cur = mm->vmas;
    while (cur && cur->base != addr) {
        prev = cur;
        cur = cur->next;
    }
    if (!cur) {
        irq_spin_unlock(&mm->lock);
        return;
    }

    for (size_t off = 0; off < cur->size; off += PAGE_SIZE) {
        phys_addr_t phys = virt_to_phys(mm->ttbr0, cur->base + off);
        if (phys) {
            unmap_page(mm->ttbr0, cur->base + off);
            if (cur->owns_pages)
                pmm_free_page((void *)phys);
        }
    }

    if (prev)
        prev->next = cur->next;
    else
        mm->vmas = cur->next;

    irq_spin_unlock(&mm->lock);
    kfree(cur);
}

/*
 * Map memory that already exists somewhere physical into this process,
 * instead of allocating fresh pages for it. Same first-fit VA scan as
 * vm_allocate_region(); everything else differs.
 *
 * The point is to hand a process a window onto something the kernel
 * already holds — a file sitting in the initrd, and later the
 * framebuffer — without copying it. For a multi-megabyte DOOM WAD
 * that's the difference between a view and a duplicate: the initrd is
 * already resident, so mapping it costs page-table entries and nothing
 * else, and it's the reason the stdio layer (fopen/fread/fseek) can be
 * skipped entirely.
 *
 * phys need not be page-aligned. The containing pages are mapped and
 * the return value carries the same intra-page offset, so the caller
 * gets a pointer to the exact byte phys named.
 */
virt_addr_t vm_map_region(struct vm_space *mm, phys_addr_t phys, size_t size, int flags) {
    if (!mm || size == 0)
        return 0;

    size_t      page_off  = (size_t)(phys & (PAGE_SIZE - 1));
    phys_addr_t phys_base = phys - page_off;
    /* Same wrap guard as vm_allocate_region(), with page_off folded in
     * since it is added before the rounding. */
    if (size > (size_t)-1 - page_off - (PAGE_SIZE - 1))
        return 0;
    size_t      span      = align_up(page_off + size, PAGE_SIZE);

    irq_spin_lock(&mm->lock);

    virt_addr_t cursor = mm->heap_start;
    struct vma *prev = NULL;
    struct vma *cur = mm->vmas;

    while (cur) {
        if (cur->base - cursor >= span)
            break;
        cursor = cur->base + cur->size;
        prev = cur;
        cur = cur->next;
    }

    if (mm->heap_end - cursor < span) {
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    uint64_t pflags = arch_translate_vm_flags(flags);
    size_t mapped = 0;
    for (; mapped < span; mapped += PAGE_SIZE) {
        if (map_page(mm->ttbr0, cursor + mapped, phys_base + mapped, pflags) != 0)
            break;
    }

    if (mapped < span) {
        /* Unmap only — these pages were never ours to free. */
        for (size_t off = 0; off < mapped; off += PAGE_SIZE)
            unmap_page(mm->ttbr0, cursor + off);
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    struct vma *node = kmalloc(sizeof(struct vma));
    if (!node) {
        for (size_t off = 0; off < span; off += PAGE_SIZE)
            unmap_page(mm->ttbr0, cursor + off);
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    node->base       = cursor;
    node->size       = span;
    node->owns_pages = false;  /* a view, not an allocation */
    node->next       = cur;
    if (prev) prev->next = node; else mm->vmas = node;

    irq_spin_unlock(&mm->lock);
    return cursor + page_off;
}
