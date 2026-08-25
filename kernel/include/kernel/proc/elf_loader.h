#ifndef ELF_LOADER_H
#define ELF_LOADER_H
#include<kernel/mm/vmm.h>
#include<kernel/mm/pmm.h>
#include<stddef.h>
#include<stdint.h>

struct vm_space; /* kernel/mm/uvm.h */

/* Everything the initial-stack auxiliary vector (AT_PHDR/AT_PHENT/
 * AT_PHNUM/AT_ENTRY — see build_arg_stack() in kernel/proc/exec.c)
 * needs from the ELF file, gathered in one place since it all comes
 * from parsing the same header. phdr_va is the RUNTIME address of the
 * program header table, not its file offset: this loader never
 * applies a load bias (every PT_LOAD's p_vaddr is used as-is, and
 * elf64_validate() requires the first one to sit at
 * ELF_USER_EXPECTED_BASE), so it's just e_phoff added onto that base —
 * true only because e_phoff always falls inside the first PT_LOAD's
 * file range at its very start, which is how every toolchain lays out
 * a non-PIE ELF and is exactly what elf64_validate() enforces. */
struct elf_aux_info {
    virt_addr_t entry;
    virt_addr_t phdr_va;
    uint64_t    phnum;
    uint64_t    phentsize;
};

/* Loads every PT_LOAD into `user_pml4` and reports entry/phdr info for
 * the auxiliary vector via `out_aux`.
 *
 * `mm` may be NULL, but should not be for a real process: each segment
 * is registered with vm_insert_region() so the address space knows the
 * image is there. Without that the ELF mapping is invisible to
 * vm_space_clone() and vm_space_destroy(), i.e. fork() produces a child
 * with no code and execve() leaks the image it replaced. */
int parse_and_load_binary(virt_addr_t binary_mem_loc, size_t buffer_size,
                          pml4_t user_pml4, struct elf_aux_info *out_aux,
                          struct vm_space *mm);
#endif // !ELF_LOADER_H


