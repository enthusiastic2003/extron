#ifndef ELF_LOADER_H
#define ELF_LOADER_H
#include<kernel/mm/vmm.h>
#include<kernel/mm/pmm.h>
#include<stddef.h>

struct vm_space; /* kernel/mm/uvm.h */

/* Loads every PT_LOAD into `user_pml4` and reports the entry point.
 *
 * `mm` may be NULL, but should not be for a real process: each segment
 * is registered with vm_insert_region() so the address space knows the
 * image is there. Without that the ELF mapping is invisible to
 * vm_space_clone() and vm_space_destroy(), i.e. fork() produces a child
 * with no code and execve() leaks the image it replaced. */
int parse_and_load_binary(virt_addr_t binary_mem_loc, size_t buffer_size,
                          pml4_t user_pml4, virt_addr_t* out_entry_point,
                          struct vm_space *mm);
#endif // !ELF_LOADER_H


