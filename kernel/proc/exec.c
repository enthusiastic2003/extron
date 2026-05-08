#include<kernel/proc/elf_loader.h>
#include<kernel/fs/tar.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/console.h>
#include <kernel/proc/exec.h>
#include<stdbool.h>
#include<kernel/klibc/string.h>

int exec(const char* binary_path){
    struct tar_file binary_file;
    
    bool err_code = tar_open(binary_path, &binary_file);

    if(err_code == false){
        kprintf("Couldn't Load the binary file\n");
        return -1;
    }

    phys_addr_t user_pml4 =  create_user_pml4();
    virt_addr_t entry_pt;

    int err_parse_code = parse_and_load_binary((virt_addr_t)binary_file.data, binary_file.size, user_pml4, &entry_pt);
    
    if(err_parse_code!=0){
        kprintf("Couldn't parse the binary file\n");
    }

    // Allocate pages, map them R+W+USER+NX into user_pml4
    for (uint64_t i = 0; i < USER_STACK_SIZE; i += PAGE_SIZE) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        memset(phys_to_virt_hhdm(p), 0, PAGE_SIZE);
        map_page(user_pml4, USER_STACK_TOP - USER_STACK_SIZE + i, p,
                PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
    }

    load_cr3(user_pml4);
    flush_tlb();
    enter_userspace(entry_pt, USER_STACK_TOP);
    return err_parse_code;
}