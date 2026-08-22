#ifndef KERNEL_MM_UVM_H
#define KERNEL_MM_UVM_H

#include <stdint.h>
#include <stddef.h>
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
void              vm_space_destroy(struct vm_space *mm);

/* First-fit over the gaps between allocated regions. Maps and zeroes
 * `size` (rounded up to PAGE_SIZE) worth of anonymous memory with
 * `flags` (VM_READ/VM_WRITE/VM_EXEC/VM_USER, kernel/mm/paging.h),
 * returns the base VA, or 0 on failure (no gap big enough, or OOM). */
virt_addr_t vm_allocate_region(struct vm_space *mm, size_t size, int flags);

/* Frees the region whose base == addr — unmaps and frees its physical
 * pages, removes it from the list. `size` isn't trusted beyond a sanity
 * check; the VMA's own recorded size is what's actually freed, so a
 * mismatched caller can't corrupt more or less than was really
 * allocated. No-op if no VMA starts at exactly `addr`. */
void vm_free_region(struct vm_space *mm, virt_addr_t addr, size_t size);

#endif
