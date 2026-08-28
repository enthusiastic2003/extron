#ifndef ELF_LOADER_H
#define ELF_LOADER_H
#include<kernel/mm/vmm.h>
#include<kernel/mm/pmm.h>
#include<stddef.h>
#include<stdint.h>
#include<stdbool.h>

struct vm_space; /* kernel/mm/uvm.h */

/* Longest PT_INTERP path this loader accepts — generous for anything
 * this project would realistically ship (e.g. "/lib/ld.so"). Rejecting
 * an oversized one outright is simpler than a dynamically-sized copy
 * for a string that only ever needs to be one more VFS lookup. */
#define ELF_INTERP_PATH_MAX 128

/* Everything the initial-stack auxiliary vector (AT_PHDR/AT_PHENT/
 * AT_PHNUM/AT_ENTRY/AT_BASE — see build_arg_stack() in
 * kernel/proc/exec.c) needs from the ELF file, gathered in one place
 * since it all comes from parsing the same header. phdr_va/entry are
 * both already bias-adjusted (see parse_and_load_binary()'s `bias`
 * parameter) — phdr_va is e_phoff added onto `bias`, true only because
 * e_phoff always falls inside the first PT_LOAD's file range at its
 * very start, which is how every toolchain lays out an ELF, PIE or
 * not.
 *
 * has_interp/interp_path describe a PT_INTERP segment found in THIS
 * file, if any — only ever meaningful for the main executable, since
 * an interpreter loading another interpreter isn't something this
 * kernel supports or looks for on purpose (the caller simply never
 * checks the interpreter's own aux_info for it). */
struct elf_aux_info {
    virt_addr_t entry;
    virt_addr_t phdr_va;
    virt_addr_t image_start;
    virt_addr_t image_end;
    uint64_t    phnum;
    uint64_t    phentsize;
    bool        has_interp;
    char        interp_path[ELF_INTERP_PATH_MAX];
};

/* Loads every PT_LOAD into `user_pml4` and reports entry/phdr/interp
 * info for the auxiliary vector via `out_aux`.
 *
 * `bias` is added to every p_vaddr before it's used as a mapping
 * target or reported in `out_aux`, and to e_entry too. Pass 0 for a
 * normal, non-PIE executable at its own fixed address — in that case
 * (and ONLY that case) the first PT_LOAD must stay above the protected
 * low-address/null-page region.
 * Pass a nonzero bias to load a position-independent image (an ET_DYN
 * interpreter, in practice) at a chosen base instead: the fixed-base
 * check is skipped entirely, since a PIE's own p_vaddr values are
 * file-relative offsets from 0, not real addresses, and any value is
 * legal there.
 *
 * `mm` may be NULL, but should not be for a real process: each segment
 * is registered with vm_insert_region() so the address space knows the
 * image is there. Without that the ELF mapping is invisible to
 * vm_space_clone() and vm_space_destroy(), i.e. fork() produces a child
 * with no code and execve() leaks the image it replaced. */
int parse_and_load_binary(virt_addr_t binary_mem_loc, size_t buffer_size,
                          pml4_t user_pml4, virt_addr_t bias,
                          struct elf_aux_info *out_aux,
                          struct vm_space *mm);
#endif // !ELF_LOADER_H
