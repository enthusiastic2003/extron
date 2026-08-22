#ifndef ELF_LOADER_H
#define ELF_LOADER_H
#include<kernel/mm/vmm.h>
#include<kernel/mm/pmm.h>
#include<stddef.h>

int parse_and_load_binary(virt_addr_t binary_mem_loc, size_t buffer_size, pml4_t user_pml4, virt_addr_t* out_entry_point);
#endif // !ELF_LOADER_H


