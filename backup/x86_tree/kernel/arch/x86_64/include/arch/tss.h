#ifndef TSS_H
#define TSS_H

#include <stdint.h>

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;       // kernel stack for Ring 3 -> Ring 0 transitions
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];     // interrupt stack table (unused for now)
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;       // I/O permission bitmap offset
} __attribute__((packed));

void tss_init(uint64_t kernel_stack_top);
void tss_set_rsp0(uint64_t rsp0);

#endif
