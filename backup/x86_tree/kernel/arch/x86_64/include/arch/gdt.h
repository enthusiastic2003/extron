#ifndef GDT_H
#define GDT_H

#include<stdint.h>

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint64_t gdt64;

void gdt_reload(void);

#endif