#include<kernel/elf.h>
#include<kernel/mm/vmm.h>
#include<kernel/mm/pmm.h>
#include<stdint.h>
#include<kernel/mm/paging.h>
#include<stdbool.h>
#include<kernel/klibc/string.h>
#include<kernel/console.h>
#include<kernel/mm/uvm.h>
#include<kernel/proc/elf_loader.h>

/* `require_fixed_base`: true for a normal, non-PIE executable at its
 * own address (the first PT_LOAD's p_vaddr must equal
 * ELF_USER_EXPECTED_BASE, as always); false when the caller is about
 * to apply its own load bias (a PIE interpreter) — in that case
 * p_vaddr is a file-relative offset from 0, not a real address, and
 * any value is legal. */
Elf64_ValidationResult elf64_validate(const void *buffer, uint64_t size,
                                      bool require_fixed_base) {
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

    /* 5. Must match this build's target architecture */
    if (ehdr->e_machine != ELF_EXPECTED_MACHINE)
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
    if (require_fixed_base) {
        for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD) {
                if (phdr[i].p_vaddr != ELF_USER_EXPECTED_BASE)
                    return ELF_ERR_BASE;
                break;
            }
        }
    }

    /* 10. PT_INTERP, if present, must name a path this loader can
     * actually copy out and later resolve through the VFS: its bytes
     * must lie within the buffer, and fit (with room for a NUL) in
     * the fixed-size buffer struct elf_aux_info offers for it. */
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_INTERP)
            continue;
        if (phdr[i].p_filesz == 0 ||
            phdr[i].p_filesz > (uint64_t)ELF_INTERP_PATH_MAX - 1 ||
            phdr[i].p_offset + phdr[i].p_filesz > size)
            return ELF_ERR_INTERP;
        break;
    }

    return ELF_OK;
}

int parse_and_load_binary(virt_addr_t binary_mem_loc,
                          size_t buffer_size,
                          pml4_t user_pml4,
                          virt_addr_t bias,
                          struct elf_aux_info *out_aux,
                          struct vm_space *mm) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)binary_mem_loc;

    /* Validate ELF. bias == 0 means a normal fixed-base executable —
     * elf64_validate() enforces the first PT_LOAD sits at
     * ELF_USER_EXPECTED_BASE in that case, so there is no separate
     * re-check of that here; a nonzero bias skips the fixed-base
     * check entirely, since p_vaddr is file-relative in that case. */
    Elf64_ValidationResult result = elf64_validate(ehdr, buffer_size, bias == 0);
    if(result != ELF_OK) {
        kprintf("[LOADER] ELF validation failed: %d\n", result);
        return result;
    }

    const Elf64_Phdr *phdr =
        (const Elf64_Phdr *)((uint8_t *)ehdr + ehdr->e_phoff);

    memset(out_aux, 0, sizeof(*out_aux));
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_INTERP)
            continue;
        /* Bounds already checked by elf64_validate() above. */
        memcpy(out_aux->interp_path,
               (const uint8_t *)binary_mem_loc + phdr[i].p_offset,
               phdr[i].p_filesz);
        out_aux->interp_path[phdr[i].p_filesz] = '\0';
        out_aux->has_interp = true;
        break;
    }

    /* The program header table's runtime address for AT_PHDR: the
     * first PT_LOAD's own p_vaddr (ELF_USER_EXPECTED_BASE when
     * bias == 0, enforced above; typically 0 for a PIE interpreter,
     * but read rather than assumed) plus e_phoff plus bias — true only
     * because e_phoff always falls inside that first segment's file
     * range at its very start (p_offset == 0), which is how every
     * toolchain lays out an ELF, PIE or not. */
    Elf64_Addr first_load_vaddr = 0;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            first_load_vaddr = phdr[i].p_vaddr;
            break;
        }
    }

    for(Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if(phdr[i].p_type != PT_LOAD)
            continue;

        /* A zero-memsz PT_LOAD has nothing to map and nothing for
         * vm_insert_region() to record (it rejects size == 0 outright) —
         * some linker configurations emit one anyway (e.g. a freestanding
         * -nostdlib link with no .data/.bss at all still gets an empty
         * second LOAD segment, as seen with mlibc_fake_interp.elf). */
        if (phdr[i].p_memsz == 0)
            continue;

        Elf64_Addr  vaddr  = phdr[i].p_vaddr + bias;
        Elf64_Off   offset = phdr[i].p_offset;
        Elf64_Xword filesz = phdr[i].p_filesz;
        Elf64_Xword memsz  = phdr[i].p_memsz;
        Elf64_Word  pflags = phdr[i].p_flags;

        kprintf(
            "[LOADER] PT_LOAD: "
            "vaddr=0x%llx offset=0x%llx "
            "filesz=%llu memsz=%llu flags=%c%c%c\n",
            vaddr,
            offset,
            filesz,
            memsz,
            (pflags & PF_R) ? 'R' : '-',
            (pflags & PF_W) ? 'W' : '-',
            (pflags & PF_X) ? 'X' : '-'
        );

        uint64_t page_flags = PAGE_PRESENT | PAGE_USER;

        if(pflags & PF_W)
            page_flags |= PAGE_WRITE;

        if(!(pflags & PF_X))
            page_flags |= PAGE_NX;

        /* ELF segments may not be page aligned */
        uint64_t aligned_vaddr  = vaddr  & ~(PAGE_SIZE - 1);
        uint64_t aligned_offset = offset & ~(PAGE_SIZE - 1);

        uint64_t page_delta = vaddr - aligned_vaddr;

        uint64_t total_mem =
            page_delta + memsz;

        uint64_t total_pages =
            (total_mem + PAGE_SIZE - 1) / PAGE_SIZE;

        const uint8_t *file_base =
            (const uint8_t *)binary_mem_loc + aligned_offset;

        for(uint64_t page = 0; page < total_pages; page++) {
            phys_addr_t phys =
                (phys_addr_t)pmm_alloc_page();

            /* pmm_alloc_page() returns NULL on exhaustion now instead of
             * panicking. Unchecked, phys_to_virt_hhdm(0) below would
             * memset the base of the HHDM — i.e. corrupt low physical
             * memory — which is a far worse failure than refusing to
             * load the binary. */
            if (!phys) {
                kprintf("[LOADER] out of physical memory loading segment\n");
                return -1;
            }

            uint8_t *dst =
                (uint8_t *)phys_to_virt_hhdm(phys);

            memset(dst, 0, PAGE_SIZE);

            uint64_t page_file_offset =
                page * PAGE_SIZE;

            uint64_t segment_file_start =
                page_delta;

            if(page_file_offset + PAGE_SIZE > segment_file_start &&
               page_file_offset < segment_file_start + filesz) {

                uint64_t copy_start_in_page = 0;

                if(segment_file_start > page_file_offset)
                    copy_start_in_page =
                        segment_file_start - page_file_offset;

                uint64_t file_data_offset =
                    page_file_offset + copy_start_in_page;

                uint64_t remaining =
                    segment_file_start + filesz - file_data_offset;

                uint64_t copy_size =
                    PAGE_SIZE - copy_start_in_page;

                if(copy_size > remaining)
                    copy_size = remaining;

                memcpy(
                    dst + copy_start_in_page,
                    file_base + file_data_offset,
                    copy_size
                );
            }

            map_page(
                user_pml4,
                aligned_vaddr + page * PAGE_SIZE,
                phys,
                page_flags
            );
        }

        /* Tell the address space this segment exists. Segments commonly
         * share a page at their boundary (text ending partway into the
         * page data begins in), so these spans overlap — vm_insert_region()
         * coalesces rather than describing that page twice. */
        if (mm && vm_insert_region(mm, aligned_vaddr,
                                   total_pages * PAGE_SIZE, true) != 0) {
            kprintf("[LOADER] could not record segment at 0x%llx\n", aligned_vaddr);
            return -2;
        }
    }

    out_aux->entry     = ehdr->e_entry + bias;
    out_aux->phdr_va   = first_load_vaddr + ehdr->e_phoff + bias;
    out_aux->phnum     = ehdr->e_phnum;
    out_aux->phentsize = ehdr->e_phentsize;

    kprintf(
        "[LOADER] Binary loaded, entry point: 0x%llx\n",
        out_aux->entry
    );

    return 0;
}

