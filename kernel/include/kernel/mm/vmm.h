#ifndef VMM_H
#define VMM_H
#

#include <stdint.h>
#include <kernel/mm/pmm.h>
#include <kernel/sync/spinlock.h>
#include <arch/vma.h>

typedef uint64_t virt_addr_t;

#define NEW_HDDM 0xFFFF800000000000ULL

// --- Kernel Virtual Memory Layout ---
#define VMM_BITMAP_VIRT_START 0xFFFFBF0000000000ULL // 1TB below heap
#define KERNEL_HEAP_START     0xFFFFC00000000000ULL
#define KERNEL_HEAP_SIZE      (128ULL * 1024 * 1024 * 1024) // 128 GB
#define KERNEL_HEAP_BITMAP_SIZE (((KERNEL_HEAP_SIZE / PAGE_SIZE) + 7) / 8)


#define KERNEL_STACK_RANGE_START 0xFFFFD00000000000ULL
#define KERNEL_STACK_SIZE        (4 * 1024 * 1024) // 4 MB per stack
#define KERNEL_STACK_GUARD       (4096)      // 4 KB guard page

void vmm_init();
virt_addr_t vmm_alloc_page();
virt_addr_t vmm_alloc_pages(size_t num_pages);
int vmm_free_page(virt_addr_t v);
int vmm_free_pages(virt_addr_t v, size_t num_pages);

virt_addr_t vmm_setup_stack();

/* struct vm_space (per-process user address-space allocator) and its
 * vm_space_create/destroy/vm_allocate_region/vm_free_region API live in
 * kernel/include/kernel/mm/uvm.h — a genuinely different concern from
 * this file (the KERNEL's own address space). This header used to carry
 * dead x86-derived declarations for a bump allocator that was never
 * implemented on this tree; removed in favor of uvm.h's real one. */

#endif // VMM_H