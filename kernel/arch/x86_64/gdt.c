// Define the exact same pointer structure for the GDT
#include <stdint.h>
#include <arch/gdt.h>


void gdt_reload(void) {
    struct gdt_ptr gdt_reg;
    
    // Size of your GDT in bytes, minus 1. 
    // If you have 3 entries (Null, Code, Data) * 8 bytes = 24. Limit = 23.
    gdt_reg.limit = 23; 
    
    // &gdt64 evaluates to the 0xFFFFFFFF80... virtual address
    gdt_reg.base = (uint64_t)&gdt64; 

    __asm__ volatile ("lgdt %0" : : "m"(gdt_reg));
}