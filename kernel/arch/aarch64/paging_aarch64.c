#include "paging_aarch64.h"
#include <kernel/mm/pmm.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <boot/multiboot2.h>

/*
 * VMSAv8-64, 4KB granule, 4 levels (L0-L3) — structurally identical to
 * x86's PML4/PDPT/PD/PT: 512 entries/table, 9 bits/level, same index
 * formula (kernel/include/kernel/mm/paging.h's PML4_IDX/PDPT_IDX/PD_IDX/
 * PT_IDX). Only the descriptor bit encoding differs from x86, which is
 * why this is a separate implementation rather than another #ifdef in
 * the shared paging.c.
 *
 * Table memory is accessed by raw physical pointer while building it —
 * exactly what x86's init_paging() does too (its phys_to_virt_hhdm() is
 * an identity function until g_hhdm_offset is set, right after its own
 * cr3 load). The MMU is off on both arches during table construction.
 *
 * Bulk RAM is mapped with 2MB blocks at L2, not individual 4KB pages:
 * the PMM's pmm_alloc_page() does a linear scan from page 0 on every
 * call, so allocating ~524,000 4KB page-table leaves for 2GB of RAM
 * would be O(n^2) and effectively hang. 2MB blocks need ~1000 entries
 * instead. Only the small UART/GPIO MMIO region gets fine 4KB pages.
 */

#define PTE_VALID           (1ULL << 0)
#define PTE_TABLE_OR_PAGE   (1ULL << 1) /* table (L0-L2) / page (L3); 0 = block (L1-L2) */
#define PTE_AF              (1ULL << 10) /* access flag: unset -> fault on first access */
#define PTE_SH_INNER        (3ULL << 8)
#define PTE_ATTR_IDX(n)     ((uint64_t)(n) << 2)
#define PTE_UXN             (1ULL << 54)
#define PTE_PXN             (1ULL << 53)
#define PTE_ADDR_MASK       0x0000FFFFFFFFF000ULL

#define MAIR_IDX_NORMAL     0
#define MAIR_IDX_DEVICE     1

#define TCR_T0SZ(x)         ((uint64_t)(x) << 0)
#define TCR_IRGN0_WBWA      (1ULL << 8)
#define TCR_ORGN0_WBWA      (1ULL << 10)
#define TCR_SH0_INNER       (3ULL << 12)
#define TCR_TG0_4K          (0ULL << 14)
#define TCR_T1SZ(x)         ((uint64_t)(x) << 16)
#define TCR_EPD1            (1ULL << 23) /* disable TTBR1_EL1 walks: no higher-half yet */
#define TCR_IPS_40BIT       (2ULL << 32) /* generous margin over RPi4's max 8GB */

#define PL011_UART0_PAGE    0xFE201000ULL
#define BCM2711_GPIO_PAGE   0xFE200000ULL

static uint64_t *table_ptr(uint64_t table_phys) {
    return (uint64_t *)table_phys;
}

static uint64_t align_down_u64(uint64_t x, uint64_t a) {
    return x & ~(a - 1);
}

static uint64_t align_up_u64(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

static uint64_t alloc_table(void) {
    /* Nolock: the MMU isn't up yet, so spin_lock's exclusive-access
     * atomics don't work on real hardware (see pmm_alloc_page_nolock's
     * comment). Safe here — genuinely single-threaded, no interrupts. */
    void *p = pmm_alloc_page_nolock();
    if (!p) panic("aarch64 paging: out of memory for a page table");
    uint64_t *v = (uint64_t *)p;
    for (int i = 0; i < 512; i++) v[i] = 0;
    return (uint64_t)(uintptr_t)p;
}

static uint64_t get_or_alloc_next_table(uint64_t table_phys, unsigned index) {
    uint64_t *t = table_ptr(table_phys);
    if (t[index] & PTE_VALID) {
        return t[index] & PTE_ADDR_MASK;
    }
    uint64_t new_table = alloc_table();
    t[index] = new_table | PTE_TABLE_OR_PAGE | PTE_VALID;
    return new_table;
}

/* va/pa must be 2MB-aligned. */
static void map_block_2mb(uint64_t l0_phys, uint64_t va, uint64_t pa, uint64_t attrs) {
    uint64_t l1 = get_or_alloc_next_table(l0_phys, (va >> 39) & 0x1FF);
    uint64_t l2 = get_or_alloc_next_table(l1, (va >> 30) & 0x1FF);
    uint64_t *l2t = table_ptr(l2);
    l2t[(va >> 21) & 0x1FF] = (pa & PTE_ADDR_MASK) | attrs | PTE_VALID;
}

/* va/pa must be 4KB-aligned. */
static void map_page_4k(uint64_t l0_phys, uint64_t va, uint64_t pa, uint64_t attrs) {
    uint64_t l1 = get_or_alloc_next_table(l0_phys, (va >> 39) & 0x1FF);
    uint64_t l2 = get_or_alloc_next_table(l1, (va >> 30) & 0x1FF);
    uint64_t l3 = get_or_alloc_next_table(l2, (va >> 21) & 0x1FF);
    uint64_t *l3t = table_ptr(l3);
    l3t[(va >> 12) & 0x1FF] = (pa & PTE_ADDR_MASK) | attrs | PTE_TABLE_OR_PAGE | PTE_VALID;
}

static void identity_map_ram_region(uint64_t l0_phys, uint64_t base, uint64_t len, uint64_t attrs) {
    uint64_t start = align_down_u64(base, 0x200000);
    uint64_t end   = align_up_u64(base + len, 0x200000);
    for (uint64_t a = start; a < end; a += 0x200000) {
        map_block_2mb(l0_phys, a, a, attrs);
    }
}

void aarch64_paging_init(uint64_t mb2_addr) {
    uint64_t l0_phys = alloc_table();

    /* Normal memory, RW, EL1-only (AP[2:1] left at 0), executable — the
     * kernel's own code lives inside this identity-mapped RAM, so it must
     * NOT be UXN/PXN or the very next instruction fetch after enabling
     * the MMU would fault. Per-section W^X permissions are deferred. */
    uint64_t normal_attrs = PTE_AF | PTE_SH_INNER | PTE_ATTR_IDX(MAIR_IDX_NORMAL);

    /* Device memory: RW, EL1-only, never executable. */
    uint64_t device_attrs = PTE_AF | PTE_ATTR_IDX(MAIR_IDX_DEVICE) | PTE_UXN | PTE_PXN;

    /* Walk the multiboot2 MMAP tag mb2_addr already points at (built by
     * mb2_shim_build from FDT data) and identity-map every region. */
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            struct multiboot_mmap_entry *entry;
            for (entry = mmap->entries;
                 (uint8_t *)entry < (uint8_t *)tag + tag->size;
                 entry = (struct multiboot_mmap_entry *)((uint64_t)entry + mmap->entry_size)) {
                if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    identity_map_ram_region(l0_phys, entry->addr, entry->len, normal_attrs);
                }
            }
        }
        tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
    }

    /* UART + GPIO MMIO — init_serial() touches both, and we need UART to
     * keep working the instant the MMU turns on or output just vanishes. */
    map_page_4k(l0_phys, PL011_UART0_PAGE, PL011_UART0_PAGE, device_attrs);
    map_page_4k(l0_phys, BCM2711_GPIO_PAGE, BCM2711_GPIO_PAGE, device_attrs);

    /* --- MAIR_EL1: index 0 = Normal WB/WA cacheable, index 1 = Device-nGnRnE --- */
    uint64_t mair = (0xFFULL << (MAIR_IDX_NORMAL * 8))
                   | (0x00ULL << (MAIR_IDX_DEVICE * 8));
    __asm__ volatile ("msr mair_el1, %0" :: "r"(mair));

    /* --- TCR_EL1: 48-bit VA (T0SZ=16), 4KB granule, TTBR1 walks disabled --- */
    uint64_t tcr = TCR_T0SZ(16) | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | TCR_TG0_4K
                 | TCR_T1SZ(16) | TCR_EPD1 | TCR_IPS_40BIT;
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr));

    /* --- TTBR0_EL1: our identity-map table --- */
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(l0_phys));

    __asm__ volatile ("isb");

    /* --- enable the MMU + caches (SCTLR_EL1.M/C/I). Read-modify-write:
     * SCTLR_EL1 has RES1 bits we must not clear. C and I aren't optional
     * here despite MAIR describing Normal memory as cacheable: leaving
     * the actual cache administratively off while the page tables claim
     * cacheable memory is a documented bad combination — the exclusive
     * monitor ldxr/stxr (what spin_lock compiles to) relies on is often
     * implemented via cache coherency logic, and doesn't work reliably
     * in that mismatched state on real hardware (invisible under QEMU,
     * which doesn't model cache state at all). */
    uint64_t sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1ULL | (1ULL << 2) | (1ULL << 12); /* M | C | I */
    __asm__ volatile ("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile ("isb");

    kprintf("aarch64: MMU + caches enabled (identity map, TTBR0_EL1 only).\n");
}
