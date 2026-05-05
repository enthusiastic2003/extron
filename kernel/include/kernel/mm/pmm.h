// kernel/include/mm/pmm.h
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t* phys_mem_ptr; 
#define PAGE_SIZE 4096
#define ONE_GIB 0x40000000ULL

typedef uint64_t phys_addr_t;

typedef phys_addr_t pml4_t;
typedef phys_addr_t pdpt_t;
typedef phys_addr_t pd_t;
typedef phys_addr_t pt_t;


/* --- helpers --- */
static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}
static inline uint64_t align_down(uint64_t x, uint64_t a) {
    return x & ~(a - 1);
}


void scan_mb2_header(uint64_t );
void init_pmm(uint64_t);
void pmm_free_page(void*);
void* pmm_alloc_page(void);
void set_virtual_pmm_bitmap_location(uint64_t);
uint64_t get_virtual_pmm_bitmap_location();
uint64_t get_pmm_total_manage();

#endif