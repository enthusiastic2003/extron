#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/drivers/serial.h> /* SERIAL_MMIO_*, serial_remap_to_hhdm */
#include <boot/multiboot2.h>

/*
 * aarch64's implementation of kernel/include/kernel/mm/paging.h — the
 * exact same public contract kernel/mm/paging.c (x86) implements, which
 * is what lets kernel/mm/vmm.c be reused completely unmodified: it only
 * ever calls kmap/kunmap/kvirt_to_phys, never touches a raw PTE bit
 * itself. Only the descriptor encoding underneath differs from x86 —
 * see the plan's original note that map_page/virt_to_phys/
 * create_user_pml4/arch_translate_vm_flags are directly portable in
 * *shape* (4-level, 512 entries/table, 9 bits/level — VMSAv8-64 with a
 * 4KB granule matches x86's PML4/PDPT/PD/PT exactly), just re-encoded.
 *
 * PAGE_PRESENT/PAGE_WRITE/PAGE_USER/PAGE_NX/PAGE_HUGE (from paging.h)
 * are treated here purely as semantic flags, not literal hardware bit
 * positions — vmm.c passes them in exactly as x86 does, and this file
 * decides what they mean in AArch64 descriptor terms.
 *
 * Kernel lives entirely under TTBR1_EL1 (matches memory_map_decisions.md
 * — every address there has the top bits set). TTBR0_EL1 stays pointed
 * at boot.S's early bootstrap table (1GB identity + UART) for now, so
 * PMM/DTB access by physical address keeps working; create_user_pml4()
 * will give it a real per-process table once there's a process to make
 * one for.
 */

#define PTE_VALID           (1ULL << 0)
#define PTE_TABLE_OR_PAGE   (1ULL << 1) /* table (L0-L2) / page (L3); 0 = block (L1-L2) */
#define PTE_AF              (1ULL << 10) /* access flag: unset -> fault on first access */
#define PTE_SH_INNER        (3ULL << 8)
#define PTE_AP_RO           (1ULL << 7)  /* AP[2]: 1 = read-only, 0 = read-write */
#define PTE_AP_EL0          (1ULL << 6)  /* AP[1]: 1 = EL0 (user) accessible */
#define PTE_ATTR_IDX(n)     ((uint64_t)(n) << 2)
#define PTE_UXN             (1ULL << 54)
#define PTE_PXN             (1ULL << 53)

#define MAIR_IDX_NORMAL     0
#define MAIR_IDX_DEVICE     1
#define MAIR_IDX_NORMAL_NC  2

static uint64_t g_hhdm_offset = 0;
static phys_addr_t kernel_l0; /* TTBR1_EL1: the kernel's own top-level table */

uint64_t *phys_to_virt_hhdm(phys_addr_t p) {
    return (uint64_t *)(p + g_hhdm_offset);
}

uint64_t *virt_to_phys_hhdm(virt_addr_t v) {
    return (uint64_t *)(v - g_hhdm_offset);
}

void flush_tlb(void) {
    __asm__ volatile ("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

/* Semantically "activate this kernel table" (x86: load into cr3). The
 * kernel always lives under TTBR1_EL1 here, so that's what gets reloaded
 * — TTBR0_EL1 is left alone (still boot.S's bootstrap identity table). */
void load_cr3(uint64_t phys_addr) {
    __asm__ volatile ("msr ttbr1_el1, %0" :: "r"(phys_addr) : "memory");
    flush_tlb();
}

static uint64_t *table_ptr(phys_addr_t table_phys) {
    return phys_to_virt_hhdm(table_phys);
}

/* 0 on failure, not a panic. map_page() is reachable from user-driven
 * paths (SYS_ANON_ALLOC via kernel/mm/uvm.c, and exec), so a process
 * that exhausts physical memory must get a failed mapping rather than
 * halt the machine. Physical address 0 is never a valid table here — it
 * sits inside the first 1MB, which init_pmm() reserves unconditionally
 * — so it is safe to use as the sentinel. */
static phys_addr_t alloc_table_zeroed(void) {
    phys_addr_t p = (phys_addr_t)pmm_alloc_page();
    if (!p) return 0;
    uint64_t *v = table_ptr(p);
    for (int i = 0; i < 512; i++) v[i] = 0;
    return p;
}

/* Propagates alloc_table_zeroed()'s 0-on-failure. Crucially, the parent
 * entry is only written once the child table actually exists — writing
 * `0 | PTE_VALID` would install a table descriptor pointing at physical
 * 0, and the next walk would happily follow it. */
static phys_addr_t get_next_table(phys_addr_t table, uint64_t index) {
    uint64_t *t = table_ptr(table);
    if (t[index] & PTE_VALID) {
        return t[index] & PAGE_ADDR_MASK;
    }
    phys_addr_t new_table = alloc_table_zeroed();
    if (!new_table)
        return 0;
    t[index] = new_table | PTE_TABLE_OR_PAGE | PTE_VALID;
    return new_table;
}

static phys_addr_t get_table_if_present(phys_addr_t table, uint64_t index) {
    uint64_t *t = table_ptr(table);
    if (t[index] & PTE_VALID) {
        return t[index] & PAGE_ADDR_MASK;
    }
    return 0;
}

/* Translates paging.h's arch-neutral semantic flags into AArch64
 * descriptor attribute bits (AF/SH/AttrIndx/AP/UXN/PXN). PAGE_CACHE_DISABLE
 * (an existing x86 flag, same semantic meaning here) selects Device
 * memory instead of Normal — required for any MMIO mapping (e.g.
 * gic.c): caching device registers can silently drop or reorder the
 * side effects that make them work at all. */
static uint64_t hw_attrs_from_flags(uint64_t flags) {
    uint64_t attrs = PTE_AF;
    if (flags & PAGE_CACHE_DISABLE) {
        attrs |= PTE_ATTR_IDX(MAIR_IDX_DEVICE);
    } else if (flags & PAGE_NORMAL_NC) {
        /* Normal but uncached: coherent with the GPU without cache
         * maintenance, while still allowing unaligned access and write
         * gathering — which Device memory does not. */
        attrs |= PTE_SH_INNER | PTE_ATTR_IDX(MAIR_IDX_NORMAL_NC);
    } else {
        attrs |= PTE_SH_INNER | PTE_ATTR_IDX(MAIR_IDX_NORMAL);
    }
    if (!(flags & PAGE_WRITE)) attrs |= PTE_AP_RO;
    if (flags & PAGE_USER) attrs |= PTE_AP_EL0;
    if (flags & PAGE_NX) attrs |= PTE_UXN | PTE_PXN;
    return attrs;
}

/* AArch64's I-cache and D-cache are NOT automatically coherent — unlike
 * x86, where hardware snoops this transparently. Code loaded at runtime
 * (parse_and_load_binary()'s memcpy, kernel/proc/elf_loader.c, shared
 * with x86 and thus unaware this even needs doing) is written through
 * the D-cache like any other data; without an explicit clean-D +
 * invalidate-I sequence, the CPU can fetch stale or non-coherent bytes
 * the first time it executes that page. boot.S already does this once
 * for the firmware-loaded kernel image itself (see its DminLine-based
 * clean_dcache_loop) — this is the same idea, applied per-page, for
 * anything mapped executable afterward. Silent on QEMU's TCG (which
 * doesn't model the hazard) — only surfaced booting on real hardware,
 * as an EC=0x00 "Unknown reason" fault on the very first EL0
 * instruction fetch of freshly-loaded ELF code. */
static void sync_icache_dcache(uint64_t va, uint64_t size) {
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ULL << ((ctr >> 16) & 0xF); /* CTR_EL0.DminLine */
    uint64_t iline = 4ULL << (ctr & 0xF);         /* CTR_EL0.IminLine */

    for (uint64_t a = va & ~(dline - 1); a < va + size; a += dline)
        __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish");

    for (uint64_t a = va & ~(iline - 1); a < va + size; a += iline)
        __asm__ volatile ("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

int map_page(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    /* -1 when an intermediate table can't be allocated. Any tables that
     * did get created on the way down stay: they're empty, correctly
     * linked, and the next mapping through this range will reuse them.
     * Unwinding them would mean tracking which levels this call created
     * versus found, for no benefit. */
    phys_addr_t l1 = get_next_table(pml4, PML4_IDX(virt));
    if (!l1) return -1;
    phys_addr_t l2 = get_next_table(l1, PDPT_IDX(virt));
    if (!l2) return -1;
    phys_addr_t l3 = get_next_table(l2, PD_IDX(virt));
    if (!l3) return -1;

    uint64_t *vl3 = table_ptr(l3);
    uint64_t current = vl3[PT_IDX(virt)];

    if (current & PTE_VALID) {
        if ((current & PAGE_ADDR_MASK) == (phys & PAGE_ADDR_MASK)) {
            return 0;
        }
        return -2;
    }

    vl3[PT_IDX(virt)] = (phys & PAGE_ADDR_MASK) | hw_attrs_from_flags(flags)
                       | PTE_TABLE_OR_PAGE | PTE_VALID;

    /* Only actual executable Normal memory needs this — Device mappings
     * (MMIO, PAGE_CACHE_DISABLE) don't participate in cache coherency
     * the same way, and nothing should ever be fetching instructions
     * from one anyway. */
    if (!(flags & PAGE_NX) && !(flags & PAGE_CACHE_DISABLE))
        sync_icache_dcache((uint64_t)phys_to_virt_hhdm(phys), PAGE_SIZE);

    return 0;
}

/* Internal-only, mirrors x86's map_page_2mb: not part of paging.h's
 * public contract, used by init_paging() for the HHDM/kernel mapping. */
static int map_block_2mb(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    phys_addr_t l1 = get_next_table(pml4, PML4_IDX(virt));
    phys_addr_t l2 = get_next_table(l1, PDPT_IDX(virt));

    uint64_t *vl2 = table_ptr(l2);
    uint64_t current = vl2[PD_IDX(virt)];

    if (current & PTE_VALID) {
        if ((current & PAGE_ADDR_MASK) == (phys & PAGE_ADDR_MASK)) {
            return 0;
        }
        return -2;
    }

    /* Block descriptor: PTE_TABLE_OR_PAGE must stay 0 here (that bit
     * means "block" at L1/L2, "page" only at L3). */
    vl2[PD_IDX(virt)] = (phys & PAGE_ADDR_MASK) | hw_attrs_from_flags(flags) | PTE_VALID;
    return 0;
}

phys_addr_t virt_to_phys(pml4_t pml4, virt_addr_t virt) {
    uint64_t *l0v = table_ptr(pml4);
    uint64_t l0e = l0v[PML4_IDX(virt)];
    if (!(l0e & PTE_VALID)) return 0;

    uint64_t *l1v = table_ptr(l0e & PAGE_ADDR_MASK);
    uint64_t l1e = l1v[PDPT_IDX(virt)];
    if (!(l1e & PTE_VALID)) return 0;

    /* 1GB block at L1 (TABLE_OR_PAGE bit clear = block, not a pointer). */
    if (!(l1e & PTE_TABLE_OR_PAGE))
        return (l1e & PAGE_ADDR_MASK) | (virt & 0x3FFFFFFF);

    uint64_t *l2v = table_ptr(l1e & PAGE_ADDR_MASK);
    uint64_t l2e = l2v[PD_IDX(virt)];
    if (!(l2e & PTE_VALID)) return 0;

    /* 2MB block at L2. */
    if (!(l2e & PTE_TABLE_OR_PAGE))
        return (l2e & PAGE_ADDR_MASK) | (virt & 0x1FFFFF);

    uint64_t *l3v = table_ptr(l2e & PAGE_ADDR_MASK);
    uint64_t l3e = l3v[PT_IDX(virt)];
    if (!(l3e & PTE_VALID)) return 0;

    /* 4KB page at L3 (TABLE_OR_PAGE must be set here — "page descriptor"). */
    return (l3e & PAGE_ADDR_MASK) | (virt & 0xFFF);
}

static inline void flush_tlb_single(uint64_t virt_addr) {
    __asm__ volatile ("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
                       :: "r"(virt_addr >> 12) : "memory");
}

int unmap_page(pml4_t pml4, virt_addr_t virt) {
    phys_addr_t l1 = get_table_if_present(pml4, PML4_IDX(virt));
    if (!l1) return -1;
    phys_addr_t l2 = get_table_if_present(l1, PDPT_IDX(virt));
    if (!l2) return -1;
    phys_addr_t l3 = get_table_if_present(l2, PD_IDX(virt));
    if (!l3) return -1;

    uint64_t *vl3 = table_ptr(l3);
    if (!(vl3[PT_IDX(virt)] & PTE_VALID)) return -1;

    vl3[PT_IDX(virt)] = 0;
    flush_tlb_single(virt);
    return 0;
}

int kmap(virt_addr_t v, phys_addr_t p, uint64_t flags) {
    return map_page(kernel_l0, v, p, flags);
}

int kunmap(virt_addr_t v) {
    return unmap_page(kernel_l0, v);
}

phys_addr_t kvirt_to_phys(virt_addr_t v) {
    return virt_to_phys(kernel_l0, v);
}

phys_addr_t create_user_pml4(void) {
    /* Unlike x86 (one combined table, user process copies the kernel's
     * top-level entries to share its mapping), the kernel here is
     * globally visible via the separate TTBR1_EL1 register — a fresh,
     * all-zero TTBR0_EL1 table for the user half needs no copying at
     * all to see it. */
    return alloc_table_zeroed();
}

uint64_t arch_translate_vm_flags(int vm_flags) {
    uint64_t hw_flags = 0;
    if (vm_flags & VM_WRITE) hw_flags |= PAGE_WRITE;
    if (vm_flags & VM_USER) hw_flags |= PAGE_USER;
    if (!(vm_flags & VM_EXEC)) hw_flags |= PAGE_NX;
    return hw_flags;
}

/* Maps [_kernel_start, _kernel_end) (linker symbols — no ELF-sections
 * multiboot tag to read here, per the plan's own recommendation this is
 * simpler on a platform with no bootloader providing one) as one
 * uniform RWX block. Per-section W^X permissions are deferred, same as
 * the minimal pass deferred them. */
static void map_kernel_range(phys_addr_t l0) {
    extern char _kernel_start[];
    extern char _kernel_end[];
    uint64_t vstart = align_down((uint64_t)_kernel_start, PAGE_SIZE);
    uint64_t vend   = align_up((uint64_t)_kernel_end, PAGE_SIZE);

    for (uint64_t v = vstart; v < vend; v += PAGE_SIZE) {
        uint64_t p = v - KERNEL_VMA;
        int res = map_page(l0, v, p, PAGE_PRESENT | PAGE_WRITE);
        if (res != 0) panic("aarch64 paging: kernel range mapping failed for %p (res %d)", (void *)v, res);
    }
}

void init_paging(uint64_t mb2_addr) {
    kernel_l0 = alloc_table_zeroed();

    map_kernel_range(kernel_l0);

    /* No separate UART mapping here: PL011_UART0_PAGE/BCM2711_GPIO_PAGE
     * are LOW physical addresses, but kernel_l0 only ever backs
     * TTBR1_EL1, which the CPU consults exclusively for HIGH addresses
     * (VA[63:48] all set) — a map_page() call here for a low address
     * would build page-table structure nothing ever actually walks. The
     * genuinely load-bearing mapping is boot.S's original TTBR0
     * identity table, built before this function even runs and never
     * touched again since; uart.c's serial_putc() keeps working through
     * that, not through anything init_paging() does. (An earlier
     * version of this function called map_uart_device(), which made
     * exactly this mistake — silently inert, papered over by boot.S's
     * mapping already covering the same need.)
     *
     * MMIO that DOES need a real TTBR1-side mapping (e.g. gic.c) should
     * map at NEW_HDDM + physaddr — a genuinely high address the kernel's
     * own table is actually consulted for — with PAGE_CACHE_DISABLE, not
     * a bare physical-looking address like this. */

    /* HHDM: every discovered RAM region, mapped at NEW_HDDM + physaddr,
     * 2MB blocks — matches x86's own init_paging() exactly (just walking
     * our synthesized MMAP tag instead of a real GRUB-provided one). */
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            struct multiboot_mmap_entry *entry;
            for (entry = mmap->entries;
                 (uint8_t *)entry < (uint8_t *)tag + tag->size;
                 entry = (struct multiboot_mmap_entry *)((uint64_t)entry + mmap->entry_size)) {
                if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    uint64_t start = align_down(entry->addr, 0x200000);
                    uint64_t end   = align_up(entry->addr + entry->len, 0x200000);
                    for (uint64_t p = start; p < end; p += 0x200000) {
                        if (map_block_2mb(kernel_l0, NEW_HDDM + p, p, PAGE_PRESENT | PAGE_WRITE | PAGE_NX) != 0)
                            panic("aarch64 paging: HHDM mapping failed at %p", (void *)p);
                    }
                }
            }
        }
        tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
    }

    load_cr3(kernel_l0);

    g_hhdm_offset = NEW_HDDM;

    /* GPIO + PL011 UART0, as Device memory, in the kernel's own table.
     * Deliberately not part of the HHDM loop above: that walks only
     * MULTIBOOT_MEMORY_AVAILABLE regions (real RAM, and these
     * peripherals sit ~2GB past the top of it), and it maps them Normal
     * cacheable. Cacheable is exactly wrong for device registers — the
     * CPU would be free to cache, merge, reorder and speculatively
     * re-read accesses whose side effects are the whole point, so a
     * write to the TX register could sit in a cache line and never
     * reach the chip. MMIO always needs its own PAGE_CACHE_DISABLE
     * mapping; same convention gic.c's mmio_map_device() uses.
     *
     * Must come after g_hhdm_offset is set: kmap() walks page tables
     * through phys_to_virt_hhdm(), which is only meaningful once the
     * offset is live and the HHDM covers the RAM those tables sit in. */
    for (uint64_t off = 0; off < SERIAL_MMIO_SIZE; off += PAGE_SIZE) {
        if (kmap(NEW_HDDM + SERIAL_MMIO_PHYS + off, SERIAL_MMIO_PHYS + off,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE) != 0)
            panic("aarch64 paging: UART/GPIO device mapping failed at %p",
                  (void *)(SERIAL_MMIO_PHYS + off));
    }
    serial_remap_to_hhdm();

    set_virtual_pmm_bitmap_location(get_virtual_pmm_bitmap_location() + NEW_HDDM);

    kprintf("aarch64: Paging initialized (higher-half + HHDM, TTBR1_EL1 live).\n");
}
