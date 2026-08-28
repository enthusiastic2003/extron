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

/* `require_fixed_base`: true for a normal, non-PIE executable at its own
 * fixed address. Reject mappings near the null page, but do not require one
 * magic link address: valid AArch64 linker scripts can put file headers one
 * maximum-page below their nominal 0x400000 text address. False means the
 * caller applies a load bias to an ET_DYN image, whose p_vaddr is relative. */
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
    uint64_t phdr_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    if (ehdr->e_phoff == 0 ||
        ehdr->e_phnum == 0 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phoff > size || phdr_bytes > size - ehdr->e_phoff)
        return ELF_ERR_PHOFF;

    /* 9. Program header entry size must match what we expect */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr))
        return ELF_ERR_PHENTSIZE;
    
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)((const uint8_t *)buffer + ehdr->e_phoff);
    if (require_fixed_base) {
        for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD) {
                if (phdr[i].p_vaddr < ELF_USER_MIN_LOAD_ADDR)
                    return ELF_ERR_BASE;
                break;
            }
        }
    }

    /* 10. PT_INTERP, if present, must name a path this loader can
     * actually copy out and later resolve through the VFS: its bytes
     * must lie within the buffer, and fit (with room for a NUL) in
     * the fixed-size buffer struct elf_aux_info offers for it. */
    bool entry_is_executable = false;
    bool phdr_is_loaded = false;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_filesz > phdr[i].p_memsz ||
                phdr[i].p_offset > size ||
                phdr[i].p_filesz > size - phdr[i].p_offset ||
                phdr[i].p_vaddr > UINT64_MAX - phdr[i].p_memsz)
                return ELF_ERR_SEGMENT;
            /* GNU ld can emit an empty placeholder LOAD whose offset and
             * virtual address are not congruent. It maps no bytes, so its
             * alignment has no runtime meaning. */
            if (!phdr[i].p_memsz)
                continue;
            if (phdr[i].p_align > 1 &&
                ((phdr[i].p_align & (phdr[i].p_align - 1)) ||
                 (phdr[i].p_vaddr & (phdr[i].p_align - 1)) !=
                 (phdr[i].p_offset & (phdr[i].p_align - 1))))
                return ELF_ERR_SEGMENT;
            if ((phdr[i].p_flags & PF_X) &&
                ehdr->e_entry >= phdr[i].p_vaddr &&
                ehdr->e_entry < phdr[i].p_vaddr + phdr[i].p_memsz)
                entry_is_executable = true;
            if (ehdr->e_phoff >= phdr[i].p_offset &&
                ehdr->e_phoff + phdr_bytes <= phdr[i].p_offset + phdr[i].p_filesz)
                phdr_is_loaded = true;
        }
        if (phdr[i].p_type != PT_INTERP)
            continue;
        if (phdr[i].p_filesz == 0 ||
            phdr[i].p_filesz > (uint64_t)ELF_INTERP_PATH_MAX - 1 ||
            phdr[i].p_offset > size ||
            phdr[i].p_filesz > size - phdr[i].p_offset ||
            ((const char *)buffer)[phdr[i].p_offset + phdr[i].p_filesz - 1] != '\0')
            return ELF_ERR_INTERP;
    }

    if (!entry_is_executable || !phdr_is_loaded)
        return ELF_ERR_SEGMENT;

    return ELF_OK;
}

static uint64_t load_page_flags(const Elf64_Phdr *phdr, Elf64_Half phnum,
                                virt_addr_t bias, virt_addr_t page_va) {
    bool writable = false;
    bool executable = false;
    for (Elf64_Half i = 0; i < phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || !phdr[i].p_memsz)
            continue;
        virt_addr_t start = phdr[i].p_vaddr + bias;
        virt_addr_t end = start + phdr[i].p_memsz;
        if (page_va + PAGE_SIZE <= start || page_va >= end)
            continue;
        writable |= (phdr[i].p_flags & PF_W) != 0;
        executable |= (phdr[i].p_flags & PF_X) != 0;
    }
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (writable) flags |= PAGE_WRITE;
    if (!executable) flags |= PAGE_NX;
    return flags;
}

static bool earlier_load_covers_page(const Elf64_Phdr *phdr, Elf64_Half limit,
                                     virt_addr_t bias, virt_addr_t page_va) {
    for (Elf64_Half i = 0; i < limit; i++) {
        if (phdr[i].p_type != PT_LOAD || !phdr[i].p_memsz)
            continue;
        virt_addr_t start = phdr[i].p_vaddr + bias;
        virt_addr_t end = start + phdr[i].p_memsz;
        if (page_va < end && page_va + PAGE_SIZE > start)
            return true;
    }
    return false;
}

int parse_and_load_binary(virt_addr_t binary_mem_loc,
                          size_t buffer_size,
                          pml4_t user_pml4,
                          virt_addr_t bias,
                          struct elf_aux_info *out_aux,
                          struct vm_space *mm) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)binary_mem_loc;

    /* Validate ELF. bias == 0 is a fixed-address executable and therefore
     * receives the minimum-user-address check. A nonzero bias is an ET_DYN
     * image whose p_vaddr values are relative to that bias. */
    Elf64_ValidationResult result = elf64_validate(ehdr, buffer_size, bias == 0);
    if(result != ELF_OK) {
        kprintf("[LOADER] ELF validation failed: %d\n", result);
        return result;
    }

    const Elf64_Phdr *phdr =
        (const Elf64_Phdr *)((uint8_t *)ehdr + ehdr->e_phoff);

    /* Validate every bias-adjusted range before installing the first page.
     * load_page_flags() scans later segments too, so doing this lazily inside
     * the mapping loop would still expose wrapped future ranges. */
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || !phdr[i].p_memsz)
            continue;
        if (phdr[i].p_vaddr > UINT64_MAX - bias ||
            phdr[i].p_vaddr + bias > UINT64_MAX - phdr[i].p_memsz)
            return ELF_ERR_SEGMENT;
        uint64_t delta = (phdr[i].p_vaddr + bias) & (PAGE_SIZE - 1);
        if (phdr[i].p_memsz > UINT64_MAX - delta - (PAGE_SIZE - 1))
            return ELF_ERR_SEGMENT;
    }

    memset(out_aux, 0, sizeof(*out_aux));
    out_aux->image_start = UINT64_MAX;
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

    /* Locate the program headers in whichever PT_LOAD actually contains
     * them; p_offset is not required to be zero by the ELF ABI. */
    Elf64_Addr phdr_va = 0;
    uint64_t phdr_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && ehdr->e_phoff >= phdr[i].p_offset &&
            ehdr->e_phoff + phdr_bytes <= phdr[i].p_offset + phdr[i].p_filesz) {
            phdr_va = bias + phdr[i].p_vaddr + (ehdr->e_phoff - phdr[i].p_offset);
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



        /* ELF segments may not be page aligned */
        uint64_t aligned_vaddr  = vaddr  & ~(PAGE_SIZE - 1);

        uint64_t page_delta = vaddr - aligned_vaddr;

        uint64_t total_mem =
            page_delta + memsz;

        uint64_t total_pages =
            (total_mem + PAGE_SIZE - 1) / PAGE_SIZE;

        if (aligned_vaddr < out_aux->image_start)
            out_aux->image_start = aligned_vaddr;
        if (aligned_vaddr + total_pages * PAGE_SIZE > out_aux->image_end)
            out_aux->image_end = aligned_vaddr + total_pages * PAGE_SIZE;

        /* Record the complete segment before mapping pages. If allocation or
         * page-table construction fails, vm_space_destroy() can now unwind
         * every page from this partially loaded segment as well. */
        if (mm && vm_insert_region(mm, aligned_vaddr,
                                   total_pages * PAGE_SIZE, true) != 0) {
            kprintf("[LOADER] could not record segment at 0x%llx\n", aligned_vaddr);
            return -2;
        }

        for(uint64_t page = 0; page < total_pages; page++) {
            virt_addr_t page_va = aligned_vaddr + page * PAGE_SIZE;
            phys_addr_t phys = virt_to_phys(user_pml4, page_va);
            bool existing = phys != 0;

            /* Reuse only a boundary page belonging to an earlier PT_LOAD in
             * this same ELF. Anything else is a real main/interpreter/stack
             * collision and must abort exec rather than overwrite it. */
            if (existing && !earlier_load_covers_page(phdr, i, bias, page_va)) {
                kprintf("[LOADER] VA collision at 0x%llx\n", page_va);
                return -2;
            }
            if (!existing)
                phys = (phys_addr_t)pmm_alloc_page();

            /* pmm_alloc_page() returns NULL on exhaustion now instead of
             * panicking. Unchecked, phys_to_virt_hhdm(0) below would
             * memset the base of the HHDM — i.e. corrupt low physical
             * memory — which is a far worse failure than refusing to
             * load the binary. */
            if (!phys) {
                kprintf("[LOADER] out of physical memory loading segment\n");
                return -1;
            }

            uint8_t *dst = (uint8_t *)phys_to_virt_hhdm(phys);

            if (!existing)
                memset(dst, 0, PAGE_SIZE);

            virt_addr_t copy_start = page_va > vaddr ? page_va : vaddr;
            virt_addr_t file_end = vaddr + filesz;
            virt_addr_t copy_end = page_va + PAGE_SIZE < file_end
                                 ? page_va + PAGE_SIZE : file_end;
            if (copy_start < copy_end) {
                memcpy(dst + (copy_start - page_va),
                       (const uint8_t *)binary_mem_loc + offset + (copy_start - vaddr),
                       copy_end - copy_start);
            }

            if (!existing && map_page(user_pml4, page_va, phys,
                    load_page_flags(phdr, ehdr->e_phnum, bias, page_va)) != 0) {
                pmm_free_page((void *)phys);
                kprintf("[LOADER] could not map page at 0x%llx\n", page_va);
                return -2;
            }
        }
    }

    out_aux->entry     = ehdr->e_entry + bias;
    out_aux->phdr_va   = phdr_va;
    out_aux->phnum     = ehdr->e_phnum;
    out_aux->phentsize = ehdr->e_phentsize;



    return 0;
}
