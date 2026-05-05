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
#define PAGE_NX            (1ULL << 63) // No Execute (if supported)

// Mask to extract the physical address from an entry (drops the flags)
#define PAGE_ADDR_MASK     0x000FFFFFFFFFF000ULL

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

// Page Table Entry Flags
#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE   (1ULL << 1)
#define PAGE_USER    (1ULL << 2)
#define PAGE_NX      (1ULL << 63)

void init_paging(uint64_t mb2_addr);

int map_page(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags);
int unmap_page(pml4_t pml4, virt_addr_t virt);
phys_addr_t virt_to_phys(pml4_t pml4, virt_addr_t virt);

int kmap(virt_addr_t v, phys_addr_t p, uint64_t flags);
int kunmap(virt_addr_t v);
phys_addr_t kvirt_to_phys(virt_addr_t v);

#endif