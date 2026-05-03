#include <boot/multiboot2.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/elf.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <stddef.h>

// Start at 0 so init_paging can build the first tables
static uint64_t g_hhdm_offset = 0;


/**
 * @brief Loads the physical address of a PML4 into the CR3 register.
 */
static inline void load_cr3(uint64_t phys_addr) {
    // We use %%cr3 to tell the assembler this is a hardware register
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_addr) : "memory");
}

/**
 * @brief Flushes the entire Translation Lookaside Buffer (TLB).
 */
static inline void flush_tlb(void) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
}

/**
 * @brief Invalidates the TLB entry for a specific virtual address.
 */
static inline void flush_tlb_single(uint64_t virt_addr) {
    // AT&T syntax for memory operands uses parentheses () instead of brackets []
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

struct multiboot_tag_elf_sections* find_elf_sections(uint64_t mb2_addr) {
    struct multiboot_tag* tag = (struct multiboot_tag*)(mb2_addr + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_ELF_SECTIONS) {
            return (struct multiboot_tag_elf_sections*)tag;
        }
        tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7));
    }
    return NULL;
}


// Helper to get or create a sub-table
static uint64_t* get_next_table(uint64_t* table, uint64_t index) {
    if (table[index] & PAGE_PRESENT) {
        // Clear flags to get the physical address, then cast to virtual pointer
        // (Works currently because we are in the 1GB identity map)
        uint64_t phys = table[index] & ~0xFFFULL;
        return (uint64_t*)(phys + g_hhdm_offset);
    }

    // Table not present, allocate a new one
    void* new_table_phys = pmm_alloc_page();
    if (!new_table_phys) return NULL;

    // Zero the new table (Critical: old garbage = fake mappings!)
    uint64_t* new_table_virt = (uint64_t*)((uint64_t)new_table_phys + g_hhdm_offset);
    for (int i = 0; i < 512; i++) {
        new_table_virt[i] = 0;
    }

    // Link the new table into the current level
    // We set Present + Write + User so the leaf entry can control final permissions
    table[index] = (uint64_t)new_table_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

    return new_table_virt;
}

void map_page(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    // 1. Walk down the 4 levels
    uint64_t* pdpt = get_next_table(pml4, PML4_IDX(virt));
    uint64_t* pd   = get_next_table(pdpt, PDPT_IDX(virt));
    uint64_t* pt   = get_next_table(pd,   PD_IDX(virt));

    // 2. Set the entry in the Level 1 Page Table
    // We mask the physical address to ensure it's page-aligned
    pt[PT_IDX(virt)] = (phys & ~0xFFFULL) | flags;
}


void map_kernel_elf_sections(uint64_t* pml4, uint64_t mb2_addr) {
    struct multiboot_tag_elf_sections* elf_tag = find_elf_sections(mb2_addr);
    if (!elf_tag) {
        panic("Paging: Could not find ELF sections tag!");
    }

    Elf64_Shdr* sections = (Elf64_Shdr*)elf_tag->sections;

    kprintf("Found %d sections.\n", elf_tag->num);


    for (uint32_t i = 0; i < elf_tag->num; i++) {
        Elf64_Shdr* sh = &sections[i];

        // Only map sections that actually occupy memory
        if (!(sh->sh_flags & SHF_ALLOC)) continue;

        // Start at the virtual address defined in the ELF
        uint64_t virt_start = sh->sh_addr;
        uint64_t size = sh->sh_size;

        // Determine Page Table Flags based on ELF flags
        uint64_t pte_flags = PAGE_PRESENT;

        if (sh->sh_flags & SHF_WRITE) {
            pte_flags |= PAGE_WRITE;
        }

        // If it's NOT executable, set the NX (No-Execute) bit
        // Note: This assumes your CPU supports NX and you've enabled it in EFER
        if (!(sh->sh_flags & SHF_EXECINSTR)) {
            pte_flags |= PAGE_NX;
        }

        // Map every page in this section
        uint64_t end_addr = virt_start + size;
        for (uint64_t v = align_down(virt_start, PAGE_SIZE); v < end_addr; v += PAGE_SIZE) {
            uint64_t p = v - KERNEL_VMA;
            map_page(pml4, v, p, pte_flags);
        }

    }

}

uint64_t* allocate_table_zeroed(){
    uint64_t new_phys_page = (uint64_t)pmm_alloc_page(); // Cast to uint64_t
    if(new_phys_page == 0){
        panic("CANNOT ALLOCATE NEW PAGE TABLE!!");
    }
    // CRITICAL: You must zero this! 
    // If there is garbage in this RAM, the CPU will try to "walk" into 
    // non-existent tables and crash.
    uint64_t* virt = (uint64_t*)(new_phys_page + g_hhdm_offset);
    for(int i = 0; i < 512; i++) virt[i] = 0;
    
    return virt;
}

void  dono(){
    volatile int x = 1;
    volatile int y = 1;
    volatile int z = x+y;
    // for(;;);

}

void init_paging(uint64_t mb2_addr) {
    // 1. Create the new PML4 (g_hhdm_offset is 0, so this returns a phys pointer)
    uint64_t* new_pml4 = allocate_table_zeroed();

    // 2. Map the Kernel via ELF Sections
    map_kernel_elf_sections(new_pml4, mb2_addr);

    // 4. THE HHDM: Map all physical RAM to the high offset
    for (uint64_t i = 0; i < ONE_GIB; i += PAGE_SIZE) {
        map_page(new_pml4, NEW_HDDM + i, i, PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    }

    // 5. Switch to the new world
    // new_pml4 is already physical because g_hhdm_offset was 0

    load_cr3((uint64_t)new_pml4);

    set_console_write_loc(NEW_HDDM);

    // 6. Update the "Lens"
    g_hhdm_offset = NEW_HDDM;

    // 7. BURN THE BRIDGE: Unmap the identity map
    // We access it via its NEW virtual address in the HHDM
    uint64_t* high_pml4 = (uint64_t*)((uint64_t)new_pml4 + NEW_HDDM);
    
    flush_tlb();

    // 8. Update PMM to use the new virtual HHDM address for the bitmap
    set_virtual_pmm_bitmap_location(get_virtual_pmm_bitmap_location() + NEW_HDDM);
    
    kprintf("New PMLR4 Active: Kernel & HHDM mapped. Identity map purged.\n");
}