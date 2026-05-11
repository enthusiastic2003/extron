#ifndef VMM_H
#define VMM_H
#

#include <stdint.h>
#include <kernel/mm/pmm.h>
#include <kernel/sync/spinlock.h>

typedef uint64_t virt_addr_t;

#define KERNEL_VMA 0xFFFFFFFF80000000ULL
#define NEW_HDDM 0xFFFF800000000000ULL

// --- Kernel Virtual Memory Layout ---
#define VMM_BITMAP_VIRT_START 0xFFFFBF0000000000ULL // 1TB below heap
#define KERNEL_HEAP_START     0xFFFFC00000000000ULL
#define KERNEL_HEAP_SIZE      (128ULL * 1024 * 1024 * 1024) // 128 GB
#define KERNEL_HEAP_BITMAP_SIZE (((KERNEL_HEAP_SIZE / PAGE_SIZE) + 7) / 8)


#define KERNEL_STACK_RANGE_START 0xFFFFD00000000000ULL
#define KERNEL_STACK_SIZE        (4 * 1024 * 1024) // 4 MB per stack
#define KERNEL_STACK_GUARD       (4096)      // 4 KB guard page

// --- User Virtual Memory Layout ---
// Userspace virtual addresses run 0x0 .. 0x00007FFFFFFFFFFF (128 TB).
// ELF segments load near the bottom; the stack is at the very top.
// The heap sits in PML4 entry 224 — upper quarter of userspace —
// leaving the low 112 TB free for future mmap regions and keeping a
// large gap below the stack (PML4[255] = 0x7F80...).
#define USER_HEAP_START  0x0000700000000000ULL   // PML4[224]
#define USER_HEAP_SIZE   ONE_GIB                  // 1 GB hard cap

// Forward declaration for the future VMA struct
struct vma_node; 

struct vm_space {
    spinlock_t lock;                // Protects this address space
    phys_addr_t cr3;                // The physical page table root

    // --- The Bump Allocator State (Current) ---
    virt_addr_t mmap_next;          

    // --- The VMA Tree State (Future Placeholder) ---
    // struct vma_node *vma_head;   
    // struct vma_node *vma_tree;   
};

void vmm_init();
virt_addr_t vmm_alloc_page();
virt_addr_t vmm_alloc_pages(size_t num_pages);
int vmm_free_page(virt_addr_t v);
int vmm_free_pages(virt_addr_t v, size_t num_pages);

virt_addr_t vmm_setup_stack();

/* Allocate and initialise a vm_space for a new user process. */
struct vm_space *vm_space_create(phys_addr_t cr3);

/* Free the vm_space (does not tear down the page tables themselves). */
void vm_space_destroy(struct vm_space *mm);

/*
 * Bump-allocate a region in the user heap [USER_HEAP_START, +USER_HEAP_SIZE).
 * Allocates physical frames, maps them with `flags`, and zeroes the memory.
 * Returns the virtual address, or 0 on failure.
 */
virt_addr_t vm_allocate_region(struct vm_space *mm, size_t size, int flags);

/*
 * Unmaps and frees the physical frames in the given range.
 */
void vm_free_region(struct vm_space *mm, virt_addr_t addr, size_t size);


#endif // VMM_H