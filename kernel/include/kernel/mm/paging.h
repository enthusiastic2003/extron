#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include <stdint.h>
#include <kernel/mm/vmm.h>

// --- Virtual Memory Offsets ---
#define KERNEL_VIRT_OFFSET 0xFFFFFFFF80000000ULL
#define DIRECT_MAP_OFFSET  0xFFFF800000000000ULL

// --- Page Table Entry Flags ---
#define PAGE_PRESENT       (1ULL << 0)
#define PAGE_WRITE         (1ULL << 1)
#define PAGE_USER          (1ULL << 2)
#define PAGE_WRITE_THROUGH (1ULL << 3)
#define PAGE_CACHE_DISABLE (1ULL << 4)
#define PAGE_ACCESSED      (1ULL << 5)
#define PAGE_DIRTY         (1ULL << 6)
#define PAGE_HUGE          (1ULL << 7)  // For 2MB or 1GB pages
#define PAGE_GLOBAL        (1ULL << 8)
/* Normal memory, non-cacheable — for buffers another bus master reads
 * or writes directly (the framebuffer; DMA later). NOT the same as
 * PAGE_CACHE_DISABLE, which selects Device memory: Device forbids
 * unaligned access unconditionally, so a memcpy() into it can fault
 * even with SCTLR_EL1.A clear, and it permits no write gathering. This
 * gives coherency with the other master while still behaving like
 * ordinary memory. Ignored if PAGE_CACHE_DISABLE is also set. */
#define PAGE_NORMAL_NC     (1ULL << 9)
#define PAGE_NX            (1ULL << 63) // No Execute (if supported)
/* Semantic input to map_page(), consumed by AArch64's descriptor encoder:
 * keep the page present for kernel bookkeeping but grant no EL0 access.
 * Bit 62 is outside the physical-address field and is never stored in the
 * final descriptor. */
#define PAGE_NO_ACCESS     (1ULL << 62)

// Mask to extract the physical address from an entry (drops the flags)
#define PAGE_ADDR_MASK     0x000FFFFFFFFFF000ULL

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

// --- Custom OS Software Flags (Bits 9-11) ---
// The CPU ignores these. We use them for VMM logic.
#define PAGE_CUSTOM_COW    (1ULL << 9)  // 1 = This is a Copy-On-Write page
#define PAGE_CUSTOM_SHARED (1ULL << 10) // 1 = Shared IPC memory
#define PAGE_CUSTOM_SWAP   (1ULL << 11) // 1 = Page is swapped to disk

// --- 1. The OS Generic Definitions (vmm.h) ---
#define VM_READ    (1 << 0)
#define VM_WRITE   (1 << 1)
#define VM_EXEC    (1 << 2)
#define VM_USER    (1 << 3)
/* PAGE_NORMAL_NC, not the default cacheable mapping every other VM_*
 * combination gets — for memory something OUTSIDE the CPU (the GPU
 * scanning out a framebuffer, any future MMIO) reads or writes
 * continuously, where a write sitting dirty in cache instead of
 * actually reaching memory is invisible to that watching until an
 * unrelated eviction happens to flush it. */
#define VM_NOCACHE (1 << 4)

#include <stdint.h>

typedef union PageEntry {
    uint64_t value;

    struct {
        uint64_t present        : 1; // PAGE_PRESENT
        uint64_t writable       : 1; // PAGE_WRITE
        uint64_t user           : 1; // PAGE_USER
        uint64_t write_through  : 1; // PAGE_WRITE_THROUGH
        uint64_t cache_disable  : 1; // PAGE_CACHE_DISABLE
        uint64_t accessed       : 1; // PAGE_ACCESSED
        uint64_t dirty          : 1; // PAGE_DIRTY
        uint64_t huge           : 1; // PAGE_HUGE
        uint64_t global         : 1; // PAGE_GLOBAL

        // Custom software bits
        uint64_t cow            : 1; // PAGE_CUSTOM_COW
        uint64_t shared         : 1; // PAGE_CUSTOM_SHARED
        uint64_t swapped        : 1; // PAGE_CUSTOM_SWAP

        // Physical address bits [51:12]
        uint64_t addr           : 40;

        // Available to software
        uint64_t available      : 11;

        uint64_t nx             : 1; // PAGE_NX
    };
} PageEntry;

typedef struct PageTable {
    PageEntry entries[512];
} __attribute__((aligned(4096))) PageTable;

void init_paging(uint64_t mb2_addr);
int map_page(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags);
int unmap_page(pml4_t pml4, virt_addr_t virt);

int kmap(virt_addr_t v, phys_addr_t p, uint64_t flags);
int kunmap(virt_addr_t v);
phys_addr_t kvirt_to_phys(virt_addr_t v);
phys_addr_t virt_to_phys(pml4_t pml4, virt_addr_t virt); /* walks an arbitrary process's own table, unlike kvirt_to_phys() (hardcoded to kernel_l0) */
uint64_t* phys_to_virt_hhdm(phys_addr_t p);
uint64_t* virt_to_phys_hhdm(virt_addr_t v);
phys_addr_t create_user_pml4(void);

/* Descriptor-level access, for fork()'s address-space clone. map_page()
 * is map_page_raw() with hw_attrs_from_flags() applied; pte_lookup()
 * hands back the exact bits so a copied page can be mapped with the
 * parent's real permissions and memory type rather than a guess
 * re-derived from the semantic PAGE_* flags. See map_page_raw(). */
uint64_t    pte_lookup(pml4_t pml4, virt_addr_t virt);   /* raw L3 descriptor, 0 if unmapped */
int         map_page_raw(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t attrs);

/* Frees a user address space's page TABLES (not the pages they map —
 * the caller disposes of those, since only it knows which were owned).
 * Never call on the kernel's TTBR1 table. */
void        free_user_page_tables(phys_addr_t pml4);
void load_cr3(uint64_t phys_addr);
void flush_tlb(void);


/* Syscall related processes */
uint64_t arch_translate_vm_flags(int vm_flags);
#endif
