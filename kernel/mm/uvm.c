#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <arch/irq_spinlock.h>
#include <kernel/klibc/string.h>
#include <stdbool.h>

/* Range managed by vm_allocate_region()/vm_free_region() — clear of the
 * fixed mappings used elsewhere (ELF image from 0x400000, stack at
 * 0x1000000). 256MB is generous for a hobby kernel and easy to raise. */
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

    /* The leaves are gone; the tables that held them are not. One page
     * per populated 512-entry span, which nothing reclaimed before
     * because nothing ever destroyed an address space. */
    free_user_page_tables(mm->ttbr0);
    kfree(mm);
}

/*
 * First-fit over the free space inside [heap_start, heap_end).
 *
 * The VMA list now describes the WHOLE user address space — the ELF
 * image at 0x400000, the stack at 0x500000, the framebuffer up at
 * 0x50000000 — and not merely the regions this allocator handed out,
 * because fork() has to clone what it can't otherwise see and execve()
 * has to free it. So the scan can no longer assume every node sits
 * inside the window, which the original could: a node below heap_start
 * made `cur->base - cursor` underflow to a ~16EB "gap" that it happily
 * accepted, then linked the new node ahead of it and left the list
 * unsorted. Nodes behind the cursor are skipped, never subtracted.
 *
 * Free space is still never itself a tracked object — it's just
 * whatever falls between consecutive VMAs — so removing a VMA merges
 * its space back into the surrounding gap with no coalescing logic.
 *
 * Returns the base of a gap big enough for `span`, or 0. On success
 * *out_prev is the node the new one should be linked after (NULL =
 * list head). Caller holds mm->lock.
 */
static virt_addr_t find_gap_locked(struct vm_space *mm, size_t span,
                                   struct vma **out_prev) {
    virt_addr_t cursor = mm->heap_start;
    struct vma *prev = NULL;
    struct vma *cur  = mm->vmas;

    while (cur) {
        virt_addr_t end = cur->base + cur->size;
        if (end <= cursor) {              /* entirely behind the cursor */
            prev = cur;
            cur  = cur->next;
            continue;
        }
        if (cur->base >= cursor && cur->base - cursor >= span)
            break;                        /* the gap in front of it fits */
        if (end > cursor)
            cursor = end;                 /* overlaps the cursor: step past */
        prev = cur;
        cur  = cur->next;
    }

    if (cursor >= mm->heap_end || mm->heap_end - cursor < span)
        return 0;

    *out_prev = prev;
    return cursor;
}

/*
 * Like find_gap_locked(), but checks a caller-specified range instead
 * of searching for one: succeeds only if [base, base+size) doesn't
 * overlap any existing VMA (real MAP_FIXED silently discards what's
 * there instead — see vm_allocate_region_at()'s own comment for why
 * that's deferred). On success *out_prev is the node the new one
 * should be linked after (NULL = list head), matching
 * find_gap_locked()'s own convention so both can feed the same
 * insertion logic. Caller holds mm->lock.
 */
static bool check_fixed_locked(struct vm_space *mm, virt_addr_t base,
                               size_t size, struct vma **out_prev) {
    virt_addr_t end = base + size;
    struct vma *prev = NULL;
    struct vma *cur  = mm->vmas;

    while (cur) {
        if (cur->base + cur->size <= base) { prev = cur; cur = cur->next; continue; }
        if (cur->base >= end)
            break;
        return false;                     /* genuinely overlaps */
    }

    *out_prev = prev;
    return true;
}

/*
 * Record a mapping somebody else created, so the address space knows
 * about it.
 *
 * Everything outside this allocator's window is established directly
 * with map_page(): the ELF loader's PT_LOAD segments, exec.c's user
 * stack, the framebuffer and the keystroke ring. Until fork/execve
 * those were invisible by consequence rather than by design — nothing
 * cloned or tore down an address space, so nothing ever needed to
 * enumerate it. Both now do.
 */
int vm_insert_region(struct vm_space *mm, virt_addr_t base, size_t size,
                     bool owns_pages) {
    if (!mm || size == 0)
        return -1;
    if (base + size < base)
        return -1;                        /* wraps */

    virt_addr_t end = align_up(base + size, PAGE_SIZE);
    base = align_down(base, PAGE_SIZE);
    if (end <= base)
        return -1;

    irq_spin_lock(&mm->lock);

    /* Absorb every node this range genuinely OVERLAPS into a single
     * one. Two PT_LOAD segments routinely share a page — text ending
     * partway through the page data begins in — so the loader really
     * does hand over overlapping spans. Inserted as two nodes, that
     * shared page would be described twice, and fork() would try to
     * copy it twice: the second map_page_raw() returns -2 and the clone
     * fails for a reason that has nothing to do with the fork.
     *
     * Abutting is NOT overlapping, and the difference is load-bearing.
     * The framebuffer descriptor page ends exactly where the
     * framebuffer itself begins (USER_FB_INFO_VA + PAGE_SIZE ==
     * USER_FB_VA), and the two have opposite ownership — the
     * descriptor is a page allocated per process, the framebuffer is
     * VideoCore memory. Treating "ends where the next begins" as an
     * overlap made that pair collide on the ownership check below and
     * refused to map the display at all. */
    struct vma *prev = NULL;
    struct vma *cur  = mm->vmas;
    while (cur) {
        if (cur->base + cur->size <= base) { prev = cur; cur = cur->next; continue; }
        if (cur->base >= end)
            break;
        /* Merging only makes sense if both halves agree on ownership: a
         * node covering both a shared view and an owned allocation
         * could not be torn down correctly either way round. */
        if (cur->owns_pages != owns_pages) {
            irq_spin_unlock(&mm->lock);
            return -1;
        }
        if (cur->base < base) base = cur->base;
        if (cur->base + cur->size > end) end = cur->base + cur->size;

        struct vma *dead = cur;
        cur = cur->next;
        if (prev) prev->next = cur; else mm->vmas = cur;
        kfree(dead);
    }

    struct vma *node = kmalloc(sizeof(struct vma));
    if (!node) {
        irq_spin_unlock(&mm->lock);
        return -1;
    }
    node->base       = base;
    node->size       = end - base;
    node->owns_pages = owns_pages;
    node->next       = cur;
    if (prev) prev->next = node; else mm->vmas = node;

    irq_spin_unlock(&mm->lock);
    return 0;
}

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

    struct vma *prev = NULL;
    virt_addr_t cursor = find_gap_locked(mm, size, &prev);
    if (!cursor) {
        irq_spin_unlock(&mm->lock);
        return 0; /* no gap big enough */
    }
    struct vma *cur = prev ? prev->next : mm->vmas;

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

virt_addr_t vm_allocate_region_at(struct vm_space *mm, virt_addr_t addr,
                                  size_t size, int flags) {
    if (!mm || size == 0 || (addr & (PAGE_SIZE - 1)))
        return 0;
    if (size > (size_t)-1 - (PAGE_SIZE - 1))
        return 0;

    size = align_up(size, PAGE_SIZE);
    if (addr < mm->heap_start || addr + size < addr || addr + size > mm->heap_end)
        return 0;

    irq_spin_lock(&mm->lock);

    struct vma *prev;
    if (!check_fixed_locked(mm, addr, size, &prev)) {
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    struct vma *cur = prev ? prev->next : mm->vmas;

    uint64_t pflags = arch_translate_vm_flags(flags);
    size_t mapped = 0;
    for (; mapped < size; mapped += PAGE_SIZE) {
        void *page = pmm_alloc_page();
        if (!page)
            break;
        memset(phys_to_virt_hhdm((phys_addr_t)page), 0, PAGE_SIZE);
        if (map_page(mm->ttbr0, addr + mapped, (phys_addr_t)page, pflags) != 0) {
            pmm_free_page(page);
            break;
        }
    }

    if (mapped < size) {
        for (size_t off = 0; off < mapped; off += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, addr + off);
            unmap_page(mm->ttbr0, addr + off);
            if (phys)
                pmm_free_page((void *)phys);
        }
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    struct vma *node = kmalloc(sizeof(struct vma));
    if (!node) {
        for (size_t off = 0; off < size; off += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, addr + off);
            unmap_page(mm->ttbr0, addr + off);
            if (phys)
                pmm_free_page((void *)phys);
        }
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    node->base       = addr;
    node->size       = size;
    node->owns_pages = true;
    node->next       = cur;
    if (prev)
        prev->next = node;
    else
        mm->vmas = node;

    irq_spin_unlock(&mm->lock);
    return addr;
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

    struct vma *prev = NULL;
    virt_addr_t cursor = find_gap_locked(mm, span, &prev);
    if (!cursor) {
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    struct vma *cur = prev ? prev->next : mm->vmas;

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

virt_addr_t vm_map_region_at(struct vm_space *mm, virt_addr_t addr,
                             phys_addr_t phys, size_t size, int flags) {
    if (!mm || size == 0 || (addr & (PAGE_SIZE - 1)))
        return 0;

    size_t      page_off  = (size_t)(phys & (PAGE_SIZE - 1));
    phys_addr_t phys_base = phys - page_off;
    if (size > (size_t)-1 - page_off - (PAGE_SIZE - 1))
        return 0;
    size_t span = align_up(page_off + size, PAGE_SIZE);
    if (addr < mm->heap_start || addr + span < addr || addr + span > mm->heap_end)
        return 0;

    irq_spin_lock(&mm->lock);

    struct vma *prev;
    if (!check_fixed_locked(mm, addr, span, &prev)) {
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    struct vma *cur = prev ? prev->next : mm->vmas;

    uint64_t pflags = arch_translate_vm_flags(flags);
    size_t mapped = 0;
    for (; mapped < span; mapped += PAGE_SIZE) {
        if (map_page(mm->ttbr0, addr + mapped, phys_base + mapped, pflags) != 0)
            break;
    }

    if (mapped < span) {
        for (size_t off = 0; off < mapped; off += PAGE_SIZE)
            unmap_page(mm->ttbr0, addr + off);
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    struct vma *node = kmalloc(sizeof(struct vma));
    if (!node) {
        for (size_t off = 0; off < span; off += PAGE_SIZE)
            unmap_page(mm->ttbr0, addr + off);
        irq_spin_unlock(&mm->lock);
        return 0;
    }

    node->base       = addr;
    node->size       = span;
    node->owns_pages = false;
    node->next       = cur;
    if (prev) prev->next = node; else mm->vmas = node;

    irq_spin_unlock(&mm->lock);
    return addr + page_off;
}

/*
 * Duplicate an address space: fork()'s half of the work.
 *
 * Every VMA is reproduced in the child. What happens to the pages
 * underneath depends on owns_pages, and that distinction is the whole
 * reason this walks the VMA list instead of walking the parent's page
 * tables directly:
 *
 *  - owned pages (ELF image, stack, heap) are COPIED. The child gets
 *    its own frame with the same contents, so the two processes diverge
 *    from the instant they return.
 *  - views (an initrd file, the framebuffer, the keystroke ring) are
 *    SHARED — the same physical page is mapped again. Copying them
 *    would be wrong twice over: the child would draw into a private
 *    buffer nothing scans out, and a multi-megabyte WAD would be
 *    duplicated per fork for no reason.
 *
 * A page table has no idea which of those a given frame is. The VMA
 * list does.
 *
 * Permissions come from the parent's own descriptors via pte_lookup()
 * rather than being re-derived from flags, so read-only text stays
 * read-only text and the framebuffer's Normal-NC memory type survives —
 * see map_page_raw() in kernel/mm/paging.c.
 *
 * Not copy-on-write. Every private page is copied eagerly, which is
 * honest about what it costs: fork+exec pays for an image it is about
 * to discard. COW is a later refinement and the reason paging.h already
 * reserves PAGE_CUSTOM_COW.
 */
struct vm_space *vm_space_clone(struct vm_space *parent) {
    if (!parent)
        return NULL;

    phys_addr_t child_ttbr0 = create_user_pml4();
    if (!child_ttbr0)
        return NULL;

    struct vm_space *child = vm_space_create(child_ttbr0);
    if (!child) {
        free_user_page_tables(child_ttbr0);
        return NULL;
    }
    child->heap_start = parent->heap_start;
    child->heap_end   = parent->heap_end;

    irq_spin_lock(&parent->lock);

    /* Appended in order — the parent's list is already sorted, so the
     * child's is too, with no scanning. */
    struct vma *tail = NULL;

    for (struct vma *v = parent->vmas; v; v = v->next) {
        struct vma *node = kmalloc(sizeof(struct vma));
        if (!node)
            goto fail;
        node->base       = v->base;
        node->size       = v->size;
        node->owns_pages = v->owns_pages;
        node->next       = NULL;
        if (tail) tail->next = node; else child->vmas = node;
        tail = node;

        for (size_t off = 0; off < v->size; off += PAGE_SIZE) {
            virt_addr_t va  = v->base + off;
            uint64_t    pte = pte_lookup(parent->ttbr0, va);
            /* A VMA may legitimately contain unmapped pages: two
             * PT_LOADs merged into one node can leave a gap between
             * them. Skip rather than fabricating a mapping. */
            if (!pte)
                continue;

            phys_addr_t src = (phys_addr_t)(pte & PAGE_ADDR_MASK);
            phys_addr_t dst = src;

            if (v->owns_pages) {
                dst = (phys_addr_t)pmm_alloc_page();
                if (!dst)
                    goto fail;
                memcpy(phys_to_virt_hhdm(dst), phys_to_virt_hhdm(src), PAGE_SIZE);
            }

            if (map_page_raw(child->ttbr0, va, dst, pte) != 0) {
                if (v->owns_pages)
                    pmm_free_page((void *)dst);
                goto fail;
            }
        }
    }

    irq_spin_unlock(&parent->lock);
    return child;

fail:
    irq_spin_unlock(&parent->lock);
    /* The child is a well-formed but incomplete address space at this
     * point — some VMAs fully mapped, the last one partly. Its own
     * teardown already tolerates that: unmapped pages inside a VMA are
     * skipped, exactly as above. */
    vm_space_destroy(child);
    return NULL;
}
