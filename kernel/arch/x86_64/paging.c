#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/drivers/serial.h>
#include <boot/multiboot2.h>
#include <arch/cpu.h>

/*
 * x86-64 implementation of kernel/include/kernel/mm/paging.h — the same
 * public contract kernel/mm/paging.c implements for AArch64, so vmm.c,
 * uvm.c, exec and fork run unmodified. The PAGE_* flags in paging.h are
 * the literal x86 PTE bits, plus two semantic extras (PAGE_NORMAL_NC,
 * PAGE_NO_ACCESS) consumed and stripped here.
 *
 * Unlike AArch64's split TTBR0/TTBR1, x86 has one root. The kernel owns
 * PML4 entries 256..511; every one is pre-populated at init so each user
 * PML4 can simply copy them (create_user_pml4) and share the kernel
 * mapping for ever, including mappings made after the copy.
 */

#define PTE_P   (1ULL << 0)
#define PTE_W   (1ULL << 1)
#define PTE_U   (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_PS  (1ULL << 7)
#define PTE_NX  (1ULL << 63)

static uint64_t g_hhdm_offset = 0;
static phys_addr_t kernel_pml4;

uint64_t *phys_to_virt_hhdm(phys_addr_t p) { return (uint64_t *)(p + g_hhdm_offset); }
uint64_t *virt_to_phys_hhdm(virt_addr_t v) { return (uint64_t *)(v - g_hhdm_offset); }

void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
}

void load_cr3(uint64_t phys_addr) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(phys_addr) : "memory");
}

static uint64_t *table_ptr(phys_addr_t t) { return phys_to_virt_hhdm(t); }

#ifdef KHEAP_TRACK
static phys_addr_t dbg_root;                       /* root being mapped into (set by map_*) */
unsigned long dbg_tbl_user, dbg_tbl_kernel, dbg_tbl_freed;
void paging_track_dump(void (*out)(const char *fmt, ...)) {
    out("[PGTRK] page-table pages: user-allocated %lu, kernel-allocated %lu, freed-by-teardown %lu\n",
        dbg_tbl_user, dbg_tbl_kernel, dbg_tbl_freed);
}
#endif

static phys_addr_t alloc_table_zeroed(void) {
    phys_addr_t p = (phys_addr_t)pmm_alloc_page();
    if (!p) return 0;
#ifdef KHEAP_TRACK
    if (dbg_root == kernel_pml4) dbg_tbl_kernel++; else dbg_tbl_user++;
#endif
    uint64_t *v = table_ptr(p);
    for (int i = 0; i < 512; i++) v[i] = 0;
    return p;
}

/* Intermediate entries are always P|W|U; the leaf decides real access. */
static phys_addr_t get_next_table(phys_addr_t table, uint64_t index) {
    uint64_t *t = table_ptr(table);
    if (t[index] & PTE_P)
        return t[index] & PAGE_ADDR_MASK;
    phys_addr_t n = alloc_table_zeroed();
    if (!n) return 0;
    t[index] = n | PTE_P | PTE_W | PTE_U;
    return n;
}

static phys_addr_t get_table_if_present(phys_addr_t table, uint64_t index) {
    uint64_t *t = table_ptr(table);
    return (t[index] & PTE_P) ? (t[index] & PAGE_ADDR_MASK) : 0;
}

static uint64_t hw_attrs_from_flags(uint64_t flags) {
    uint64_t a = PTE_P;
    if (flags & PAGE_WRITE) a |= PTE_W;
    if ((flags & PAGE_USER) && !(flags & PAGE_NO_ACCESS)) a |= PTE_U;
    if (flags & PAGE_CACHE_DISABLE) a |= PTE_PCD;
    else if (flags & PAGE_NORMAL_NC) a |= PTE_PWT;
    if (flags & PAGE_NX) a |= PTE_NX;
    if (flags & PAGE_GLOBAL) a |= PAGE_GLOBAL;
    return a;
}

int map_page_raw(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t attrs) {
#ifdef KHEAP_TRACK
    dbg_root = pml4;
#endif
    phys_addr_t pdpt = get_next_table(pml4, PML4_IDX(virt));
    if (!pdpt) return -1;
    phys_addr_t pd = get_next_table(pdpt, PDPT_IDX(virt));
    if (!pd) return -1;
    phys_addr_t pt = get_next_table(pd, PD_IDX(virt));
    if (!pt) return -1;

    uint64_t *vpt = table_ptr(pt);
    uint64_t cur = vpt[PT_IDX(virt)];
    if (cur & PTE_P) {
        if ((cur & PAGE_ADDR_MASK) == (phys & PAGE_ADDR_MASK))
            return 0;
        return -2;
    }
    vpt[PT_IDX(virt)] = (phys & PAGE_ADDR_MASK)
                      | (attrs & ~PAGE_ADDR_MASK & ~PAGE_NO_ACCESS) | PTE_P;
    return 0;
}

int map_page(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    return map_page_raw(pml4, virt, phys, hw_attrs_from_flags(flags));
}

uint64_t pte_lookup(pml4_t pml4, virt_addr_t virt) {
    uint64_t e = table_ptr(pml4)[PML4_IDX(virt)];
    if (!(e & PTE_P)) return 0;
    e = table_ptr(e & PAGE_ADDR_MASK)[PDPT_IDX(virt)];
    if (!(e & PTE_P) || (e & PTE_PS)) return 0;
    e = table_ptr(e & PAGE_ADDR_MASK)[PD_IDX(virt)];
    if (!(e & PTE_P) || (e & PTE_PS)) return 0;
    e = table_ptr(e & PAGE_ADDR_MASK)[PT_IDX(virt)];
    return (e & PTE_P) ? e : 0;
}

static int map_block_2mb(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    phys_addr_t pdpt = get_next_table(pml4, PML4_IDX(virt));
    if (!pdpt) return -1;
    phys_addr_t pd = get_next_table(pdpt, PDPT_IDX(virt));
    if (!pd) return -1;
    uint64_t *vpd = table_ptr(pd);
    uint64_t cur = vpd[PD_IDX(virt)];
    if (cur & PTE_P)
        return ((cur & PAGE_ADDR_MASK) == (phys & PAGE_ADDR_MASK)) ? 0 : -2;
    vpd[PD_IDX(virt)] = (phys & PAGE_ADDR_MASK) | hw_attrs_from_flags(flags) | PTE_PS;
    return 0;
}

phys_addr_t virt_to_phys(pml4_t pml4, virt_addr_t virt) {
    uint64_t e = table_ptr(pml4)[PML4_IDX(virt)];
    if (!(e & PTE_P)) return 0;
    e = table_ptr(e & PAGE_ADDR_MASK)[PDPT_IDX(virt)];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFF);
    e = table_ptr(e & PAGE_ADDR_MASK)[PD_IDX(virt)];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) return (e & 0x000FFFFFFFE00000ULL) | (virt & 0x1FFFFF);
    e = table_ptr(e & PAGE_ADDR_MASK)[PT_IDX(virt)];
    if (!(e & PTE_P)) return 0;
    return (e & PAGE_ADDR_MASK) | (virt & 0xFFF);
}

static inline void flush_tlb_single(uint64_t v) {
    __asm__ volatile ("invlpg (%0)" :: "r"(v) : "memory");
}

int unmap_page(pml4_t pml4, virt_addr_t virt) {
    phys_addr_t pdpt = get_table_if_present(pml4, PML4_IDX(virt));
    if (!pdpt) return -1;
    phys_addr_t pd = get_table_if_present(pdpt, PDPT_IDX(virt));
    if (!pd) return -1;
    phys_addr_t pt = get_table_if_present(pd, PD_IDX(virt));
    if (!pt) return -1;
    uint64_t *vpt = table_ptr(pt);
    if (!(vpt[PT_IDX(virt)] & PTE_P)) return -1;
    vpt[PT_IDX(virt)] = 0;
    flush_tlb_single(virt);
    return 0;
}

int kmap(virt_addr_t v, phys_addr_t p, uint64_t flags) { return map_page(kernel_pml4, v, p, flags); }
int kunmap(virt_addr_t v) { return unmap_page(kernel_pml4, v); }
phys_addr_t kvirt_to_phys(virt_addr_t v) { return virt_to_phys(kernel_pml4, v); }

phys_addr_t create_user_pml4(void) {
#ifdef KHEAP_TRACK
    dbg_root = 0;
#endif
    phys_addr_t p = alloc_table_zeroed();
    if (!p) return 0;
    uint64_t *dst = table_ptr(p);
    uint64_t *src = table_ptr(kernel_pml4);
    for (int i = 256; i < 512; i++) dst[i] = src[i];
    return p;
}

/* Frees the lower-half (user) tables only. The upper half is shared with
 * the kernel's own PML4 and must never be touched. */
void free_user_page_tables(phys_addr_t pml4) {
    if (!pml4) return;
    uint64_t *l0 = table_ptr(pml4);
    for (int i = 0; i < 256; i++) {
        if (!(l0[i] & PTE_P)) continue;
        uint64_t *l1 = table_ptr(l0[i] & PAGE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(l1[j] & PTE_P) || (l1[j] & PTE_PS)) continue;
            uint64_t *l2 = table_ptr(l1[j] & PAGE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(l2[k] & PTE_P) || (l2[k] & PTE_PS)) continue;
                pmm_free_page((void *)(l2[k] & PAGE_ADDR_MASK));
#ifdef KHEAP_TRACK
                dbg_tbl_freed++;
#endif
            }
            pmm_free_page((void *)(l1[j] & PAGE_ADDR_MASK));
#ifdef KHEAP_TRACK
            dbg_tbl_freed++;
#endif
        }
        pmm_free_page((void *)(l0[i] & PAGE_ADDR_MASK));
#ifdef KHEAP_TRACK
        dbg_tbl_freed++;
#endif
    }
    pmm_free_page((void *)pml4);
#ifdef KHEAP_TRACK
    dbg_tbl_freed++;
#endif
}

uint64_t arch_translate_vm_flags(int vm_flags) {
    uint64_t hw = 0;
    if (vm_flags & VM_WRITE) hw |= PAGE_WRITE;
    if (vm_flags & VM_USER) hw |= PAGE_USER;
    if ((vm_flags & VM_USER) && !(vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
        hw |= PAGE_NO_ACCESS;
    if (!(vm_flags & VM_EXEC)) hw |= PAGE_NX;
    if (vm_flags & VM_NOCACHE) hw |= PAGE_NORMAL_NC;
    return hw;
}

static void map_kernel_range(phys_addr_t l0) {
    extern char _kernel_start[];
    extern char _kernel_end[];
    uint64_t vstart = align_down((uint64_t)_kernel_start, PAGE_SIZE);
    uint64_t vend   = align_up((uint64_t)_kernel_end, PAGE_SIZE);
    for (uint64_t v = vstart; v < vend; v += PAGE_SIZE) {
        if (map_page(l0, v, v - KERNEL_VMA, PAGE_PRESENT | PAGE_WRITE) != 0)
            panic("x86_64 paging: kernel range mapping failed for %p", (void *)v);
    }
}

void init_paging(uint64_t mb2_addr) {
    kernel_pml4 = alloc_table_zeroed();
    if (!kernel_pml4) panic("x86_64 paging: out of memory for the kernel PML4");

    /* Pre-populate every kernel-half PML4 slot so user PML4 copies stay valid. */
    uint64_t *k = table_ptr(kernel_pml4);
    for (int i = 256; i < 512; i++) {
        phys_addr_t t = alloc_table_zeroed();
        if (!t) panic("x86_64 paging: out of memory for kernel PDPTs");
        k[i] = t | PTE_P | PTE_W;   /* no U: kernel-only half */
    }

    map_kernel_range(kernel_pml4);

    /* HHDM over every usable RAM region, 2 MiB pages (NX, kernel-only). */
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            struct multiboot_mmap_entry *e;
            for (e = mmap->entries; (uint8_t *)e < (uint8_t *)tag + tag->size;
                 e = (struct multiboot_mmap_entry *)((uint64_t)e + mmap->entry_size)) {
                if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
                uint64_t start = align_down(e->addr, 0x200000);
                uint64_t end   = align_up(e->addr + e->len, 0x200000);
                for (uint64_t p = start; p < end; p += 0x200000) {
                    if (map_block_2mb(kernel_pml4, NEW_HDDM + p, p,
                                      PAGE_PRESENT | PAGE_WRITE | PAGE_NX) != 0)
                        panic("x86_64 paging: HHDM mapping failed at %p", (void *)p);
                }
            }
        }
        tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
    }

    load_cr3(kernel_pml4);
    g_hhdm_offset = NEW_HDDM;

    set_virtual_pmm_bitmap_location(get_virtual_pmm_bitmap_location() + NEW_HDDM);

    kprintf("x86_64: paging initialized (higher-half + HHDM, kernel PML4 live).\n");
}
