#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <kernel/fs/vfs.h>
#include <kernel/errno.h>
#include <arch/irq_spinlock.h>
#include <kernel/klibc/string.h>
#include <stdbool.h>

/* Range managed by vm_allocate_region()/vm_free_region() — clear of the
 * fixed mappings used elsewhere (ELF image from 0x400000, stack at
 * 0x1000000). 256MB is generous for a hobby kernel and easy to raise. */
#define USER_HEAP_START 0x10000000UL
#define USER_HEAP_SIZE  0x10000000UL /* 256MB */

enum vm_backing_type {
    VM_BACKING_ANON_SHARED,
    VM_BACKING_FILE_SHARED,
};

/* One cache entry per shared file page. Keeping this cache in the VM
 * layer is deliberate: filesystems only see their existing read/write
 * operations and do not need mmap-specific hooks or page ownership. */
struct vm_file_page {
    struct vfs_node *node;
    size_t offset;
    phys_addr_t phys;
    size_t refs;
    struct vm_file_page *next;
};

struct vm_backing {
    spinlock_t lock;
    size_t refs;
    enum vm_backing_type type;
    size_t size;
    size_t file_bytes;
    size_t file_offset;
    bool writable_allowed;
    bool may_be_dirty;
    struct vfs_node *node;
    size_t page_count;
    phys_addr_t *pages;
    struct vm_file_page **file_pages;
};

static spinlock_t file_page_lock = SPINLOCK_INIT;
static struct vm_file_page *file_pages;

static void backing_retain(struct vm_backing *backing) {
    if (!backing) return;
    irq_spin_lock(&backing->lock);
    backing->refs++;
    irq_spin_unlock(&backing->lock);
}

static int backing_sync(struct vm_backing *backing, size_t offset, size_t size) {
    if (!backing || backing->type != VM_BACKING_FILE_SHARED
            || !backing->may_be_dirty || !size)
        return 0;
    if (!backing->node || !backing->node->ops || !backing->node->ops->write)
        return -EIO;
    if (offset >= backing->file_bytes)
        return 0;
    if (size > backing->file_bytes - offset)
        size = backing->file_bytes - offset;

    size_t first = offset / PAGE_SIZE;
    size_t last = (offset + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = first; i < last; i++) {
        size_t page_start = i * PAGE_SIZE;
        size_t start = offset > page_start ? offset - page_start : 0;
        size_t end = PAGE_SIZE;
        if (page_start + end > offset + size)
            end = offset + size - page_start;
        if (page_start + end > backing->file_bytes)
            end = backing->file_bytes - page_start;
        if (end <= start)
            continue;
        long written = backing->node->ops->write(
            backing->node, backing->file_offset + page_start + start,
            (char *)phys_to_virt_hhdm(backing->pages[i]) + start,
            end - start);
        if (written < 0)
            return (int)written;
        if ((size_t)written != end - start)
            return -EIO;
    }
    return 0;
}

static void file_page_put(struct vm_file_page *page) {
    if (!page) return;
    irq_spin_lock(&file_page_lock);
    if (--page->refs) {
        irq_spin_unlock(&file_page_lock);
        return;
    }
    struct vm_file_page **slot = &file_pages;
    while (*slot && *slot != page)
        slot = &(*slot)->next;
    if (*slot == page)
        *slot = page->next;
    irq_spin_unlock(&file_page_lock);
    vfs_node_release(page->node);
    pmm_free_page((void *)page->phys);
    kfree(page);
}

static void backing_release(struct vm_backing *backing) {
    if (!backing) return;
    irq_spin_lock(&backing->lock);
    bool last = --backing->refs == 0;
    irq_spin_unlock(&backing->lock);
    if (!last) return;

    if (backing->type == VM_BACKING_FILE_SHARED) {
        /* Each VMA flushes precisely its live slice before dropping its
         * reference. Do not flush the entire backing here: after a partial
         * unmap, an ordinary write() may legitimately update the removed
         * slice before the final surviving slice is unmapped. Replaying the
         * stale removed page here would overwrite that newer file data. */
        if (backing->file_pages) {
            for (size_t i = 0; i < backing->page_count; i++)
                file_page_put(backing->file_pages[i]);
            kfree(backing->file_pages);
        }
        if (backing->node)
            vfs_node_release(backing->node);
    } else {
        for (size_t i = 0; i < backing->page_count; i++)
            if (backing->pages[i]) pmm_free_page((void *)backing->pages[i]);
    }
    kfree(backing->pages);
    kfree(backing);
}

static void vma_release(struct vma *v) {
    if (v->backing)
        backing_release(v->backing);
    kfree(v);
}

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
        if (v->backing)
            (void)backing_sync(v->backing, v->backing_offset, v->size);
        for (size_t off = 0; off < v->size; off += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, v->base + off);
            if (phys) {
                unmap_page(mm->ttbr0, v->base + off);
                /* Only pages this process actually owns go back to the
                 * PMM — see struct vma's owns_pages comment. */
                if (v->owns_pages && !v->backing)
                    pmm_free_page((void *)phys);
            }
        }
        vma_release(v);
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
 * overlap any existing VMA. vm_allocate_region_at()/vm_map_region_at()
 * call carve_range_locked() first to guarantee that, so by the time
 * they call this it only ever hands back the insertion point (*out_prev,
 * matching find_gap_locked()'s own convention: NULL = list head) —
 * the `false` return remains reachable, just never taken in that
 * caller. Caller holds mm->lock.
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
 * Discard whatever already occupies [base, end) — real MAP_FIXED
 * semantics, and also what a real munmap(addr, len) needs for an
 * arbitrary sub-range instead of only ever matching a VMA's exact base.
 * base/end must already be page-aligned. Caller holds mm->lock.
 *
 * Splitting a VMA also retains its shared backing and advances the tail's
 * backing offset, so later writeback still targets the correct file pages.
 * Each VMA the range genuinely overlaps falls into one of
 * four cases: entirely inside the range (delete outright), the range
 * eats its head (shrink forward), the range eats its tail (shrink
 * back), or the range lands in the middle (split into two). Physical
 * pages are only unmapped/freed for the overlapped sub-range, never the
 * whole VMA, so a shrink or split leaves the surviving portion's
 * mapping untouched.
 */
static void carve_range_locked(struct vm_space *mm, virt_addr_t base,
                               virt_addr_t end) {
    struct vma *prev = NULL;
    struct vma *cur  = mm->vmas;

    while (cur) {
        virt_addr_t cur_end = cur->base + cur->size;
        if (cur_end <= base) { prev = cur; cur = cur->next; continue; }
        if (cur->base >= end)
            break;                         /* sorted list: nothing further overlaps */

        virt_addr_t ov_start = cur->base > base ? cur->base : base;
        virt_addr_t ov_end   = cur_end < end ? cur_end : end;

        if (cur->backing)
            (void)backing_sync(cur->backing,
                cur->backing_offset + (ov_start - cur->base),
                ov_end - ov_start);

        for (virt_addr_t va = ov_start; va < ov_end; va += PAGE_SIZE) {
            phys_addr_t phys = virt_to_phys(mm->ttbr0, va);
            if (phys) {
                unmap_page(mm->ttbr0, va);
                if (cur->owns_pages && !cur->backing)
                    pmm_free_page((void *)phys);
            }
        }

        bool head_hit = (ov_start == cur->base);
        bool tail_hit = (ov_end == cur_end);

        if (head_hit && tail_hit) {
            struct vma *dead = cur;
            cur = cur->next;
            if (prev) prev->next = cur; else mm->vmas = cur;
            vma_release(dead);
            continue;
        }

        if (head_hit) {
            cur->backing_offset += ov_end - cur->base;
            cur->base = ov_end;
            cur->size = cur_end - ov_end;
            prev = cur;
            cur  = cur->next;
            continue;
        }

        if (tail_hit) {
            cur->size = ov_start - cur->base;
            prev = cur;
            cur  = cur->next;
            continue;
        }

        /* Middle hit: split into [cur->base, ov_start) and
         * [ov_end, cur_end). */
        struct vma *tail = kmalloc(sizeof(struct vma));
        if (!tail) {
            /* Can't represent both halves as separate nodes. Rather than
             * leave [ov_end, cur_end) mapped but untracked by any VMA
             * (a real leak — fork()/vm_space_destroy() would never see
             * those pages again), unmap/free that remainder too and
             * drop it: an already-degenerate OOM-during-carve corner
             * case loses that trailing sub-range of address space, but
             * nothing physical leaks. */
            if (cur->backing)
                (void)backing_sync(cur->backing,
                    cur->backing_offset + (ov_end - cur->base),
                    cur_end - ov_end);
            for (virt_addr_t va = ov_end; va < cur_end; va += PAGE_SIZE) {
                phys_addr_t phys = virt_to_phys(mm->ttbr0, va);
                if (phys) {
                    unmap_page(mm->ttbr0, va);
                    if (cur->owns_pages && !cur->backing)
                        pmm_free_page((void *)phys);
                }
            }
            cur->size = ov_start - cur->base;
            prev = cur;
            cur  = cur->next;
            continue;
        }
        tail->base       = ov_end;
        tail->size       = cur_end - ov_end;
        tail->flags      = cur->flags;
        tail->owns_pages = cur->owns_pages;
        tail->backing    = cur->backing;
        tail->backing_offset = cur->backing_offset + (ov_end - cur->base);
        backing_retain(tail->backing);
        tail->next       = cur->next;

        cur->size = ov_start - cur->base;
        cur->next = tail;

        prev = tail;
        cur  = tail->next;
    }
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

    /* Allocate before modifying the list. Previously an allocation failure
     * after absorbing overlapping ELF VMAs lost the old bookkeeping nodes,
     * so exec failure could leak all pages they had described. */
    struct vma *node = kmalloc(sizeof(struct vma));
    if (!node)
        return -1;

    irq_spin_lock(&mm->lock);

    /* Validate the entire overlap set before removing any node. A range can
     * span more than one VMA; discovering an incompatible backing after an
     * earlier compatible node had already been removed would make failure
     * non-transactional. */
    virt_addr_t merged_base = base;
    virt_addr_t merged_end = end;
    for (struct vma *scan = mm->vmas; scan; scan = scan->next) {
        if (scan->base + scan->size <= merged_base)
            continue;
        if (scan->base >= merged_end)
            break;
        if (scan->owns_pages != owns_pages || scan->backing) {
            irq_spin_unlock(&mm->lock);
            kfree(node);
            return -1;
        }
        if (scan->base < merged_base)
            merged_base = scan->base;
        if (scan->base + scan->size > merged_end)
            merged_end = scan->base + scan->size;
    }
    base = merged_base;
    end = merged_end;

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
        if (cur->base < base) base = cur->base;
        if (cur->base + cur->size > end) end = cur->base + cur->size;

        struct vma *dead = cur;
        cur = cur->next;
        if (prev) prev->next = cur; else mm->vmas = cur;
        vma_release(dead);
    }

    node->base       = base;
    node->size       = end - base;
    node->flags      = VM_USER;
    node->owns_pages = owns_pages;
    node->backing    = NULL;
    node->backing_offset = 0;
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
    node->flags      = flags;
    node->owns_pages = true;   /* freshly allocated here, ours to free */
    node->backing    = NULL;
    node->backing_offset = 0;
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

    /* Real MAP_FIXED semantics: discard whatever already occupies
     * [addr, addr+size) rather than refusing — see carve_range_locked()
     * for how an arbitrary overlap gets trimmed/split/deleted down to
     * exactly this range. Nothing can still overlap afterwards, so
     * check_fixed_locked() below only exists to hand back the
     * insertion point (prev), not to fail. */
    carve_range_locked(mm, addr, addr + size);

    struct vma *prev = NULL;
    check_fixed_locked(mm, addr, size, &prev);
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
    node->flags      = flags;
    node->owns_pages = true;
    node->backing    = NULL;
    node->backing_offset = 0;
    node->next       = cur;
    if (prev)
        prev->next = node;
    else
        mm->vmas = node;

    irq_spin_unlock(&mm->lock);
    return addr;
}

/* Frees [addr, addr+size), which no longer needs to match any VMA's
 * exact base — carve_range_locked() trims/splits/deletes whatever
 * overlaps, same primitive MAP_FIXED's clobber path uses. `addr` must
 * be page-aligned (a misaligned request is simply not a valid unmap and
 * is a no-op, matching this function's existing "not really an error
 * path" convention); `size` is rounded up to a whole number of pages. */
void vm_free_region(struct vm_space *mm, virt_addr_t addr, size_t size) {
    if (!mm || size == 0 || (addr & (PAGE_SIZE - 1)))
        return;
    if (size > (size_t)-1 - (PAGE_SIZE - 1))
        return;

    virt_addr_t end = addr + align_up(size, PAGE_SIZE);
    if (end < addr)
        return; /* wrapped */

    irq_spin_lock(&mm->lock);
    carve_range_locked(mm, addr, end);
    irq_spin_unlock(&mm->lock);
}

static struct vma *split_vma_locked(struct vma *v, virt_addr_t cut) {
    if (cut <= v->base || cut >= v->base + v->size)
        return v;
    struct vma *tail = kmalloc(sizeof(*tail));
    if (!tail)
        return NULL;
    *tail = *v;
    tail->base = cut;
    tail->size = v->base + v->size - cut;
    tail->backing_offset += cut - v->base;
    backing_retain(tail->backing);
    v->size = cut - v->base;
    v->next = tail;
    return tail;
}

int vm_protect_region(struct vm_space *mm, virt_addr_t addr, size_t size,
                      int flags) {
    if (!mm || (addr & (PAGE_SIZE - 1)) || !size)
        return -EINVAL;
    if (size > (size_t)-1 - (PAGE_SIZE - 1))
        return -EINVAL;
    size = align_up(size, PAGE_SIZE);
    virt_addr_t end = addr + size;
    if (end < addr)
        return -EINVAL;

    irq_spin_lock(&mm->lock);
    virt_addr_t cursor = addr;
    struct vma *v = mm->vmas;
    while (v && v->base + v->size <= cursor) v = v->next;
    while (cursor < end) {
        if (!v || v->base > cursor) {
            irq_spin_unlock(&mm->lock);
            return -ENOMEM;
        }
        virt_addr_t covered = v->base + v->size;
        if (covered > end) covered = end;
        for (virt_addr_t va = cursor; va < covered; va += PAGE_SIZE) {
            if (!pte_lookup(mm->ttbr0, va)) {
                irq_spin_unlock(&mm->lock);
                return -ENOMEM;
            }
        }
        if ((flags & VM_WRITE) && v->backing
                && v->backing->type == VM_BACKING_FILE_SHARED
                && !v->backing->writable_allowed) {
            irq_spin_unlock(&mm->lock);
            return -EACCES;
        }
        cursor = covered;
        v = v->next;
    }

    v = mm->vmas;
    while (v && v->base + v->size <= addr) v = v->next;
    if (v && v->base < addr) {
        v = split_vma_locked(v, addr);
        if (!v) {
            irq_spin_unlock(&mm->lock);
            return -ENOMEM;
        }
    }
    for (struct vma *cur = v; cur && cur->base < end; cur = cur->next) {
        if (cur->base + cur->size > end && !split_vma_locked(cur, end)) {
            irq_spin_unlock(&mm->lock);
            return -ENOMEM;
        }
        cur->flags = (cur->flags & (VM_USER | VM_NOCACHE))
                   | (flags & (VM_READ | VM_WRITE | VM_EXEC));
        if ((cur->flags & VM_WRITE) && cur->backing
                && cur->backing->type == VM_BACKING_FILE_SHARED)
            cur->backing->may_be_dirty = true;
        uint64_t attrs = arch_translate_vm_flags(cur->flags);
        for (size_t off = 0; off < cur->size; off += PAGE_SIZE) {
            virt_addr_t va = cur->base + off;
            uint64_t pte = pte_lookup(mm->ttbr0, va);
            phys_addr_t phys = pte & PAGE_ADDR_MASK;
            unmap_page(mm->ttbr0, va);
            if (map_page(mm->ttbr0, va, phys, attrs) != 0) {
                irq_spin_unlock(&mm->lock);
                flush_tlb();
                return -EIO;
            }
        }
    }
    flush_tlb();
    irq_spin_unlock(&mm->lock);
    return 0;
}

static struct vm_backing *backing_alloc(enum vm_backing_type type,
                                        size_t size) {
    struct vm_backing *b = kmalloc(sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->lock = (spinlock_t)SPINLOCK_INIT;
    b->refs = 1;
    b->type = type;
    b->size = size;
    b->page_count = size / PAGE_SIZE;
    b->pages = kmalloc(b->page_count * sizeof(*b->pages));
    if (!b->pages) {
        kfree(b);
        return NULL;
    }
    memset(b->pages, 0, b->page_count * sizeof(*b->pages));
    return b;
}

static virt_addr_t map_backing(struct vm_space *mm, virt_addr_t addr,
                               struct vm_backing *backing, int flags,
                               bool fixed) {
    irq_spin_lock(&mm->lock);
    struct vma *prev = NULL;
    virt_addr_t base;
    if (fixed) {
        if ((addr & (PAGE_SIZE - 1)) || addr < mm->heap_start
                || addr + backing->size < addr
                || addr + backing->size > mm->heap_end) {
            irq_spin_unlock(&mm->lock);
            return 0;
        }
        carve_range_locked(mm, addr, addr + backing->size);
        if (!check_fixed_locked(mm, addr, backing->size, &prev)) {
            irq_spin_unlock(&mm->lock);
            return 0;
        }
        base = addr;
    } else {
        base = find_gap_locked(mm, backing->size, &prev);
        if (!base) {
            irq_spin_unlock(&mm->lock);
            return 0;
        }
    }
    struct vma *next = prev ? prev->next : mm->vmas;
    uint64_t attrs = arch_translate_vm_flags(flags);
    size_t mapped = 0;
    for (; mapped < backing->size; mapped += PAGE_SIZE) {
        if (map_page(mm->ttbr0, base + mapped,
                     backing->pages[mapped / PAGE_SIZE], attrs) != 0)
            break;
    }
    if (mapped != backing->size) {
        for (size_t off = 0; off < mapped; off += PAGE_SIZE)
            unmap_page(mm->ttbr0, base + off);
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    struct vma *node = kmalloc(sizeof(*node));
    if (!node) {
        for (size_t off = 0; off < mapped; off += PAGE_SIZE)
            unmap_page(mm->ttbr0, base + off);
        irq_spin_unlock(&mm->lock);
        return 0;
    }
    node->base = base;
    node->size = backing->size;
    node->flags = flags;
    node->owns_pages = true;
    node->backing = backing;
    node->backing_offset = 0;
    node->next = next;
    if (prev) prev->next = node; else mm->vmas = node;
    irq_spin_unlock(&mm->lock);
    return base;
}

virt_addr_t vm_allocate_shared_region(struct vm_space *mm, virt_addr_t addr,
                                      size_t size, int flags, bool fixed) {
    if (!mm || !size || size > (size_t)-1 - (PAGE_SIZE - 1))
        return 0;
    size = align_up(size, PAGE_SIZE);
    struct vm_backing *b = backing_alloc(VM_BACKING_ANON_SHARED, size);
    if (!b) return 0;
    for (size_t i = 0; i < b->page_count; i++) {
        b->pages[i] = (phys_addr_t)pmm_alloc_page();
        if (!b->pages[i]) {
            backing_release(b);
            return 0;
        }
        memset(phys_to_virt_hhdm(b->pages[i]), 0, PAGE_SIZE);
    }
    virt_addr_t result = map_backing(mm, addr, b, flags, fixed);
    if (!result) backing_release(b);
    return result;
}

static struct vm_file_page *file_page_get(struct vfs_node *node,
                                          size_t offset, int *error) {
    irq_spin_lock(&file_page_lock);
    for (struct vm_file_page *p = file_pages; p; p = p->next) {
        if (p->node == node && p->offset == offset) {
            p->refs++;
            irq_spin_unlock(&file_page_lock);
            return p;
        }
    }
    irq_spin_unlock(&file_page_lock);

    struct vm_file_page *fresh = kmalloc(sizeof(*fresh));
    phys_addr_t phys = (phys_addr_t)pmm_alloc_page();
    if (!fresh || !phys) {
        if (fresh) kfree(fresh);
        if (phys) pmm_free_page((void *)phys);
        *error = -ENOMEM;
        return NULL;
    }
    memset(phys_to_virt_hhdm(phys), 0, PAGE_SIZE);
    long got = node->ops->read(node, offset, phys_to_virt_hhdm(phys), PAGE_SIZE);
    if (got < 0) {
        pmm_free_page((void *)phys);
        kfree(fresh);
        *error = (int)got;
        return NULL;
    }
    fresh->node = node;
    fresh->offset = offset;
    fresh->phys = phys;
    fresh->refs = 1;
    vfs_node_retain(node);

    irq_spin_lock(&file_page_lock);
    /* Another CPU could have populated it while the VFS read ran. */
    for (struct vm_file_page *p = file_pages; p; p = p->next) {
        if (p->node == node && p->offset == offset) {
            p->refs++;
            irq_spin_unlock(&file_page_lock);
            vfs_node_release(node);
            pmm_free_page((void *)phys);
            kfree(fresh);
            return p;
        }
    }
    fresh->next = file_pages;
    file_pages = fresh;
    irq_spin_unlock(&file_page_lock);
    return fresh;
}

static void shared_file_copy(struct vfs_node *node, size_t offset,
                             void *buffer, size_t size, bool into_cache) {
    if (!node || !buffer || !size || offset + size < offset)
        return;
    irq_spin_lock(&file_page_lock);
    for (struct vm_file_page *p = file_pages; p; p = p->next) {
        if (p->node != node || p->offset + PAGE_SIZE <= offset
                || p->offset >= offset + size)
            continue;
        size_t start = p->offset > offset ? p->offset : offset;
        size_t end = p->offset + PAGE_SIZE < offset + size
                   ? p->offset + PAGE_SIZE : offset + size;
        char *cache = (char *)phys_to_virt_hhdm(p->phys) + (start - p->offset);
        char *caller = (char *)buffer + (start - offset);
        if (into_cache)
            memmove(cache, caller, end - start);
        else
            memmove(caller, cache, end - start);
    }
    irq_spin_unlock(&file_page_lock);
}

void vm_shared_file_read_overlay(struct vfs_node *node, size_t offset,
                                 void *buffer, size_t size) {
    shared_file_copy(node, offset, buffer, size, false);
}

void vm_shared_file_written(struct vfs_node *node, size_t offset,
                            const void *buffer, size_t size) {
    shared_file_copy(node, offset, (void *)buffer, size, true);
}

virt_addr_t vm_map_file_shared(struct vm_space *mm, virt_addr_t addr,
                               size_t size, int flags, bool fixed,
                               struct vfs_node *node, size_t file_offset,
                               size_t file_bytes, bool writable_allowed,
                               int *error) {
    if (error) *error = -ENOMEM;
    if (!mm || !node || !node->ops || !node->ops->read || !size
            || size > (size_t)-1 - (PAGE_SIZE - 1)) {
        if (error) *error = -EINVAL;
        return 0;
    }
    if ((flags & VM_WRITE) && !writable_allowed) {
        if (error) *error = -EACCES;
        return 0;
    }
    size = align_up(size, PAGE_SIZE);
    struct vm_backing *b = backing_alloc(VM_BACKING_FILE_SHARED, size);
    if (!b) return 0;
    b->file_pages = kmalloc(b->page_count * sizeof(*b->file_pages));
    if (!b->file_pages) {
        backing_release(b);
        return 0;
    }
    memset(b->file_pages, 0, b->page_count * sizeof(*b->file_pages));
    b->node = node;
    vfs_node_retain(node);
    b->file_offset = file_offset;
    b->file_bytes = file_bytes < size ? file_bytes : size;
    b->writable_allowed = writable_allowed;
    b->may_be_dirty = (flags & VM_WRITE) != 0;
    for (size_t i = 0; i < b->page_count; i++) {
        int page_error = -ENOMEM;
        struct vm_file_page *page = file_page_get(
            node, file_offset + i * PAGE_SIZE, &page_error);
        if (!page) {
            if (error) *error = page_error;
            backing_release(b);
            return 0;
        }
        b->file_pages[i] = page;
        b->pages[i] = page->phys;
    }
    virt_addr_t result = map_backing(mm, addr, b, flags, fixed);
    if (!result) backing_release(b);
    else if (error) *error = 0;
    return result;
}

int vm_sync_region(struct vm_space *mm, virt_addr_t addr, size_t size) {
    if (!mm || (addr & (PAGE_SIZE - 1)) || !size
            || size > (size_t)-1 - (PAGE_SIZE - 1))
        return -EINVAL;
    size = align_up(size, PAGE_SIZE);
    virt_addr_t end = addr + size;
    if (end < addr) return -EINVAL;

    irq_spin_lock(&mm->lock);
    virt_addr_t cursor = addr;
    for (struct vma *v = mm->vmas; v && cursor < end; v = v->next) {
        if (v->base + v->size <= cursor) continue;
        if (v->base > cursor) {
            irq_spin_unlock(&mm->lock);
            return -ENOMEM;
        }
        virt_addr_t stop = v->base + v->size < end ? v->base + v->size : end;
        int result = backing_sync(v->backing,
            v->backing_offset + (cursor - v->base), stop - cursor);
        if (result < 0) {
            irq_spin_unlock(&mm->lock);
            return result;
        }
        cursor = stop;
    }
    irq_spin_unlock(&mm->lock);
    return cursor == end ? 0 : -ENOMEM;
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
    node->flags      = flags;
    node->owns_pages = false;  /* a view, not an allocation */
    node->backing    = NULL;
    node->backing_offset = 0;
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

    /* Same clobber-instead-of-refuse handling as vm_allocate_region_at()
     * above. */
    carve_range_locked(mm, addr, addr + span);

    struct vma *prev = NULL;
    check_fixed_locked(mm, addr, span, &prev);
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
    node->flags      = flags;
    node->owns_pages = false;
    node->backing    = NULL;
    node->backing_offset = 0;
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
        node->flags      = v->flags;
        node->owns_pages = v->owns_pages;
        node->backing    = v->backing;
        node->backing_offset = v->backing_offset;
        backing_retain(node->backing);
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

            if (v->owns_pages && !v->backing) {
                dst = (phys_addr_t)pmm_alloc_page();
                if (!dst)
                    goto fail;
                memcpy(phys_to_virt_hhdm(dst), phys_to_virt_hhdm(src), PAGE_SIZE);
            }

            if (map_page_raw(child->ttbr0, va, dst, pte) != 0) {
                if (v->owns_pages && !v->backing)
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
