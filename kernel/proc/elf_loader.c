#include<kernel/elf.h>
#include<kernel/mm/vmm.h>
#include<kernel/mm/pmm.h>
#include<stdint.h>
#include<kernel/mm/paging.h>
#include<stdbool.h>
#include<kernel/klibc/string.h>
#include<kernel/console.h>

Elf64_ValidationResult elf64_validate(const void *buffer, uint64_t size) {
    if (!buffer)
        return ELF_ERR_NULL;

    /* 1. Buffer must be large enough to hold the file header */
    if (size < sizeof(Elf64_Ehdr))
        return ELF_ERR_TOO_SMALL;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)buffer;

    /* 2. Magic bytes: 0x7F 'E' 'L' 'F' */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return ELF_ERR_MAGIC;

    /* 3. Must be 64-bit */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        return ELF_ERR_CLASS;

    /* 4. Must be little-endian (x86_64 is always LE) */
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return ELF_ERR_ENDIAN;

    /* 5. Must be x86_64 */
    if (ehdr->e_machine != EM_X86_64)
        return ELF_ERR_MACHINE;

    /* 6. Must be executable or shared object (for dynamic linking later) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return ELF_ERR_TYPE;

    /* 7. Entry point must exist */
    if (ehdr->e_entry == 0)
        return ELF_ERR_ENTRY;

    /* 8. Program header table must be within bounds */
    if (ehdr->e_phoff == 0 ||
        ehdr->e_phnum == 0 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > size)
        return ELF_ERR_PHOFF;

    /* 9. Program header entry size must match what we expect */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr))
        return ELF_ERR_PHENTSIZE;
    
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)((const uint8_t *)buffer + ehdr->e_phoff);
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_vaddr != ELF_USER_EXPECTED_BASE)
                return ELF_ERR_BASE;
            break;
        }
    }


    return ELF_OK;
}

int parse_and_load_binary(virt_addr_t binary_mem_loc, size_t buffer_size, pml4_t user_pml4, virt_addr_t* out_entry_point) {
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)binary_mem_loc;
    
    /* Validate ELF */
    Elf64_ValidationResult result = elf64_validate(ehdr, buffer_size);
    if (result != ELF_OK) {
        kprintf("[LOADER] ELF validation failed: %d\n", result);
        return result;
    }

    const Elf64_Phdr* phdr = (const Elf64_Phdr*)((uint8_t*)ehdr + ehdr->e_phoff);
    
    /* Verify first PT_LOAD is at expected base (0x400000) */
    bool found_first_load = false;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && !found_first_load) {
            if (phdr[i].p_vaddr != ELF_USER_EXPECTED_BASE) {
                kprintf("[LOADER] First PT_LOAD at 0x%llx, expected 0x%llx\n",
                        phdr[i].p_vaddr, (uint64_t)ELF_USER_EXPECTED_BASE);
                return -10; /* ELF_ERR_BASE from validator */
            }
            found_first_load = true;
            break;
        }
    }

    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        Elf64_Addr  vaddr  = phdr[i].p_vaddr;
        Elf64_Off   offset = phdr[i].p_offset;
        Elf64_Xword filesz = phdr[i].p_filesz;
        Elf64_Xword memsz  = phdr[i].p_memsz;
        Elf64_Word  pflags = phdr[i].p_flags;

        kprintf("[LOADER] PT_LOAD: vaddr 0x%llx | filesz %llu | memsz %llu | flags %c%c%c\n",
                vaddr, filesz, memsz,
                (pflags & PF_R) ? 'R' : '-',
                (pflags & PF_W) ? 'W' : '-',
                (pflags & PF_X) ? 'X' : '-');

        uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
        if (pflags & PF_W)    page_flags |= PAGE_WRITE;
        if (!(pflags & PF_X)) page_flags |= PAGE_NX;

        uint64_t total_pages = (memsz + PAGE_SIZE - 1) / PAGE_SIZE;
        const uint8_t* file_data = (const uint8_t*)binary_mem_loc + offset;

        /* Treat all segments uniformly: allocate fresh pages, copy data */
        for (uint64_t page = 0; page < total_pages; page++) {
            phys_addr_t phys = (phys_addr_t)pmm_alloc_page();
            uint8_t* dst = (uint8_t*)phys_to_virt_hhdm(phys);

            uint64_t page_start = page * PAGE_SIZE;
            uint64_t file_bytes = (page_start < filesz)
                ? ((filesz - page_start > PAGE_SIZE) ? PAGE_SIZE : filesz - page_start)
                : 0;

            if (file_bytes > 0)
                memcpy(dst, file_data + page_start, file_bytes);
            if (file_bytes < PAGE_SIZE)
                memset(dst + file_bytes, 0, PAGE_SIZE - file_bytes);

            map_page(user_pml4, vaddr + page_start, phys, page_flags);
        }
    }

    out_entry_point[0] = ehdr->e_entry;
    kprintf("[LOADER] Binary loaded, entry point: 0x%llx\n", ehdr->e_entry);
    return 0;
}


