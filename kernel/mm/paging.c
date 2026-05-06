#include <boot/multiboot2.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/elf.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <stddef.h>

static uint64_t g_hhdm_offset = 0;
static phys_addr_t kernel_pml4;

static inline uint64_t* phys_to_virt(phys_addr_t p) {
    return (uint64_t*)(p + g_hhdm_offset);
}

static inline void load_cr3(uint64_t phys_addr) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_addr) : "memory");
}

static inline void flush_tlb(void) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
}

static inline void flush_tlb_single(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

struct multiboot_tag_elf_sections* find_elf_sections(uint64_t mb2_addr) {
    struct multiboot_tag* tag = (struct multiboot_tag*)(mb2_addr + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_ELF_SECTIONS)
            return (struct multiboot_tag_elf_sections*)tag;

        tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7));
    }
    return NULL;
}

/* -------- FIXED: USER BIT HANDLING -------- */
static phys_addr_t get_next_table(phys_addr_t table, uint64_t index, uint64_t flags) {
    virt_addr_t* t = phys_to_virt(table);

    if (t[index] & PAGE_PRESENT) {
        if (flags & PAGE_USER)
            t[index] |= PAGE_USER;  // upgrade existing entry if needed
        return t[index] & PAGE_ADDR_MASK;
    }

    phys_addr_t new_phys = (phys_addr_t)pmm_alloc_page();
    if (!new_phys) return 0;

    virt_addr_t* v = phys_to_virt(new_phys);
    for (int i = 0; i < 512; i++) v[i] = 0;

    t[index] = new_phys | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
    return new_phys;
}

static phys_addr_t get_table_if_present(phys_addr_t table, uint64_t index) {
    virt_addr_t* t = phys_to_virt(table);
    if (t[index] & PAGE_PRESENT)
        return t[index] & PAGE_ADDR_MASK;
    return 0;
}

int map_page_2mb(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    phys_addr_t pdpt = get_next_table(pml4, PML4_IDX(virt), flags);
    if (!pdpt) return -1;

    phys_addr_t pd = get_next_table(pdpt, PDPT_IDX(virt), flags);
    if (!pd) return -1;

    virt_addr_t* vpd = phys_to_virt(pd);
    uint64_t current_entry = vpd[PD_IDX(virt)];

    if (current_entry & PAGE_PRESENT) {
        if ((current_entry & PAGE_ADDR_MASK) == (phys & PAGE_ADDR_MASK)) {
            return 0; 
        }
        return -2;
    }

    vpd[PD_IDX(virt)] = (phys & PAGE_ADDR_MASK) | flags | PAGE_HUGE;
    return 0;
}

int map_page(pml4_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    phys_addr_t pdpt = get_next_table(pml4, PML4_IDX(virt), flags);
    if (!pdpt) return -1;

    phys_addr_t pd = get_next_table(pdpt, PDPT_IDX(virt), flags);
    if (!pd) return -1;

    phys_addr_t pt = get_next_table(pd, PD_IDX(virt), flags);
    if (!pt) return -1;

    virt_addr_t* vpt = phys_to_virt(pt);
    uint64_t current_entry = vpt[PT_IDX(virt)];

    if (current_entry & PAGE_PRESENT) {
        if ((current_entry & PAGE_ADDR_MASK) == (phys & PAGE_ADDR_MASK)) {
            return 0; 
        }
        return -2;
    }

    vpt[PT_IDX(virt)] = (phys & PAGE_ADDR_MASK) | flags;
    return 0;
}

phys_addr_t virt_to_phys(pml4_t pml4, virt_addr_t virt) {
    phys_addr_t pdpt = get_table_if_present(pml4, PML4_IDX(virt));
    if (!pdpt) return 0;

    phys_addr_t pd = get_table_if_present(pdpt, PDPT_IDX(virt));
    if (!pd) return 0;

    phys_addr_t pt = get_table_if_present(pd, PD_IDX(virt));
    if (!pt) return 0;

    virt_addr_t* vpt = phys_to_virt(pt);
    uint64_t entry = vpt[PT_IDX(virt)];

    if (!(entry & PAGE_PRESENT))
        return 0;

    return entry & PAGE_ADDR_MASK;
}

int unmap_page(pml4_t pml4, virt_addr_t virt) {
    phys_addr_t pdpt = get_table_if_present(pml4, PML4_IDX(virt));
    if (!pdpt) return -1;

    phys_addr_t pd = get_table_if_present(pdpt, PDPT_IDX(virt));
    if (!pd) return -1;

    phys_addr_t pt = get_table_if_present(pd, PD_IDX(virt));
    if (!pt) return -1;

    virt_addr_t* vpt = phys_to_virt(pt);

    if (!(vpt[PT_IDX(virt)] & PAGE_PRESENT))
        return -1;

    vpt[PT_IDX(virt)] = 0;
    flush_tlb_single(virt);
    
    return 0;
}

int kmap(virt_addr_t v, phys_addr_t p, uint64_t flags) {
    return map_page(kernel_pml4, v, p, flags);
}

int kunmap(virt_addr_t v) {
    return unmap_page(kernel_pml4, v);
}

phys_addr_t kvirt_to_phys(virt_addr_t v) {
    return virt_to_phys(kernel_pml4, v);
}

void map_kernel_elf_sections(uint64_t pml4, uint64_t mb2_addr) {
    struct multiboot_tag_elf_sections* elf_tag = find_elf_sections(mb2_addr);
    if (!elf_tag) panic("Paging: ELF sections not found");

    Elf64_Shdr* sections = (Elf64_Shdr*)elf_tag->sections;

    for (uint32_t i = 0; i < elf_tag->num; i++) {
        Elf64_Shdr* sh = &sections[i];
        if (!(sh->sh_flags & SHF_ALLOC)) continue;

        uint64_t virt_start = sh->sh_addr;
        uint64_t end = virt_start + sh->sh_size;

        uint64_t flags = PAGE_PRESENT;
        if (sh->sh_flags & SHF_WRITE) flags |= PAGE_WRITE;
        if (!(sh->sh_flags & SHF_EXECINSTR)) flags |= PAGE_NX;

        for (uint64_t v = align_down(virt_start, PAGE_SIZE); v < end; v += PAGE_SIZE) {
            uint64_t p = v - KERNEL_VMA;
            int res = map_page(pml4, v, p, flags);
            if (res != 0)
                panic("Kernel ELF mapping failed for virt %p (res: %d)", (void*)v, res);
        }
    }
}

phys_addr_t allocate_table_zeroed() {
    phys_addr_t p = (phys_addr_t)pmm_alloc_page();
    if (!p) panic("Page table alloc failed");

    uint64_t* v = phys_to_virt(p);
    for (int i = 0; i < 512; i++) v[i] = 0;

    return p;
}

void init_paging(uint64_t mb2_addr) {
    kernel_pml4 = allocate_table_zeroed();

    map_kernel_elf_sections(kernel_pml4, mb2_addr);

    uint64_t total = align_up(get_pmm_total_manage(), 0x200000);

    for (uint64_t i = 0; i < total; i += 0x200000) {
        if (map_page_2mb(kernel_pml4,
                     NEW_HDDM + i,
                     i,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_NX) != 0)
            panic("HHDM mapping failed");
    }

    load_cr3((uint64_t)kernel_pml4);

    set_console_write_loc(NEW_HDDM);

    g_hhdm_offset = NEW_HDDM;

    flush_tlb();

    set_virtual_pmm_bitmap_location(
        get_virtual_pmm_bitmap_location() + NEW_HDDM);

    kprintf("Paging initialized.\n");
}