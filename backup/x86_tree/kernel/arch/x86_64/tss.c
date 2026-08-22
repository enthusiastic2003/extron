#include <arch/tss.h>
#include <arch/gdt.h>
#include <stdint.h>

static struct tss kernel_tss;

void tss_init(uint64_t kernel_stack_top) {
    kernel_tss.rsp0 = kernel_stack_top;
    // Placing iopb past the TSS limit denies all port I/O from Ring 3
    kernel_tss.iopb = sizeof(struct tss);

    uint64_t base  = (uint64_t)&kernel_tss;
    uint32_t limit = sizeof(struct tss) - 1;

    // TSS occupies a 16-byte system segment descriptor at GDT offset 0x28
    // (null + kcode + kdata + udata + ucode = 5 x 8-byte entries before it)
    uint64_t *desc = (uint64_t *)((uint8_t *)&gdt64 + 0x28);

    // Low qword encoding:
    //   [15:0]  = limit[15:0]
    //   [39:16] = base[23:0]
    //   [47:40] = 0x89 (Present=1, DPL=0, S=0, Type=0b1001 64-bit TSS available)
    //   [51:48] = limit[19:16]
    //   [63:56] = base[31:24]
    desc[0] = (uint64_t)(limit & 0xFFFF)
            | ((uint64_t)(base & 0xFFFFFF)       << 16)
            | ((uint64_t)0x89                    << 40)
            | ((uint64_t)((limit >> 16) & 0xF)  << 48)
            | ((uint64_t)((base >> 24) & 0xFF)  << 56);

    // High qword: base[63:32] in low 32 bits, upper 32 bits reserved
    desc[1] = base >> 32;

    // Reload GDT with updated limit that now covers the TSS descriptor
    struct gdt_ptr ptr = { .limit = 55, .base = (uint64_t)&gdt64 };
    __asm__ volatile ("lgdt %0" :: "m"(ptr));

    // Load the task register with the TSS selector (offset 0x28, RPL=0, TI=0)
    __asm__ volatile ("ltr %0" :: "r"((uint16_t)0x28));
}

void tss_set_rsp0(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}
