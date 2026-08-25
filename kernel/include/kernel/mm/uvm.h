#ifndef KERNEL_MM_UVM_H
#define KERNEL_MM_UVM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h> /* virt_addr_t */
#include <kernel/sync/spinlock.h>

/*
 * Per-process user address-space allocator (SYS_ANON_ALLOC/FREE) — a
 * genuinely different concern from kernel/mm/vmm.c, which manages the
 * KERNEL's own address space. x86 keeps these separate too
 * (kernel/mm/user_vmm.c vs vmm.c, in ~/extron-x86-backup/).
 *
 * Unlike x86's version (a one-directional bump cursor — vm_free_region()
 * there does free the physical frames, but the virtual address range
 * itself is never reclaimed, so the space leaks until the whole 1GB cap
 * is exhausted even though memory isn't), this tracks every allocated
 * region explicitly as a sorted linked list. Free space is never itself
 * a tracked object — it's just whatever falls between consecutive VMAs
 * (or before the first / after the last) — so removing a VMA
 * automatically merges its space back into the surrounding gap with no
 * separate coalescing logic: the next allocation's scan just sees one
 * bigger gap where there used to be two smaller ones.
 */

struct vma {
    virt_addr_t base;   /* page-aligned */
    size_t      size;   /* page-aligned */
    /* False when this region maps memory the process does NOT own —
     * vm_map_region() views of the initrd, and later the framebuffer.
     * Tearing such a region down must unmap it and stop there: handing
     * initrd pages back to the PMM would put live, shared, still-mapped
     * memory into the free pool to be handed out again. */
    bool        owns_pages;
    struct vma  *next;  /* sorted ascending by base */
};

struct vm_space {
    spinlock_t   lock;
    phys_addr_t  ttbr0;
    virt_addr_t  heap_start;
    virt_addr_t  heap_end;   /* exclusive: managed range is [heap_start, heap_end) */
    struct vma   *vmas;      /* allocated regions, sorted; gaps between = free space */
};

struct vm_space *vm_space_create(phys_addr_t ttbr0);

/* Unmaps and frees everything the process owned, frees the page tables
 * themselves, then the vm_space. Must not run on the address space
 * TTBR0_EL1 is currently pointed at. */
void              vm_space_destroy(struct vm_space *mm);

/* fork(): a new address space with its own page tables, every VMA
 * reproduced — owned pages copied, views re-mapped to the same physical
 * memory. Returns NULL on OOM, having freed whatever it built. */
struct vm_space *vm_space_clone(struct vm_space *parent);

/* Record a mapping established directly with map_page() — the ELF
 * loader's segments, the user stack, the framebuffer. Without this the
 * VMA list describes only what vm_allocate_region()/vm_map_region()
 * handed out, and fork() cannot clone (nor execve() free) the parts of
 * the address space it never hears about. Overlapping and abutting
 * ranges of the same ownership coalesce; overlap with the opposite
 * ownership is an error. Returns 0 on success, -1 otherwise. */
int               vm_insert_region(struct vm_space *mm, virt_addr_t base,
                                   size_t size, bool owns_pages);

/* First-fit over the gaps between allocated regions. Maps and zeroes
 * `size` (rounded up to PAGE_SIZE) worth of anonymous memory with
 * `flags` (VM_READ/VM_WRITE/VM_EXEC/VM_USER, kernel/mm/paging.h),
 * returns the base VA, or 0 on failure (no gap big enough, or OOM). */
virt_addr_t vm_allocate_region(struct vm_space *mm, size_t size, int flags);

/* Like vm_allocate_region(), but at a caller-chosen address instead of
 * first-fit — MAP_FIXED's anonymous case. `addr` must already be
 * page-aligned and fall entirely within [heap_start, heap_end). Real
 * MAP_FIXED semantics: whatever already occupies [addr, addr+size) is
 * silently discarded first (see carve_range_locked() in uvm.c — an
 * overlapping VMA is trimmed or split as needed, never just refused).
 * Returns `addr` on success, or 0 on failure (misaligned, out of range,
 * or OOM). */
virt_addr_t vm_allocate_region_at(struct vm_space *mm, virt_addr_t addr,
                                  size_t size, int flags);

/* Frees [addr, addr+size) — unmaps and frees the physical pages it
 * owns, trimming or splitting whatever VMA(s) the range overlaps rather
 * than requiring an exact match against some VMA's own base (see
 * carve_range_locked() in uvm.c). `addr` must be page-aligned; `size`
 * is rounded up to a whole number of pages. No-op if either is
 * malformed (this predates real error returns from the mmap ABI and
 * still doesn't have one). */
void vm_free_region(struct vm_space *mm, virt_addr_t addr, size_t size);

/* Map EXISTING physical memory into the process at a free VA, rather
 * than allocating fresh pages for it — a read-only window onto the
 * initrd today, the framebuffer later. `phys` and `size` need not be
 * page-aligned; the containing pages are mapped and the returned VA
 * points at the same byte offset `phys` did. The pages are marked
 * not-owned, so freeing the region unmaps without returning them to the
 * PMM. Returns 0 if no gap is big enough or a mapping fails. */
virt_addr_t vm_map_region(struct vm_space *mm, phys_addr_t phys, size_t size, int flags);

/* Like vm_map_region(), but at a caller-chosen address instead of
 * first-fit — MAP_FIXED's file/device-backed case. Same placement
 * rules as vm_allocate_region_at(): `addr` page-aligned and in range;
 * whatever already overlaps [addr, addr+size) is discarded first, not
 * refused. Returns `addr + (phys's own intra-page offset)` on success
 * (matching vm_map_region()'s own convention when `phys` isn't
 * page-aligned), or 0 on failure. */
virt_addr_t vm_map_region_at(struct vm_space *mm, virt_addr_t addr,
                             phys_addr_t phys, size_t size, int flags);

#endif
