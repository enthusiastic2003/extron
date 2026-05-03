#include <stdint.h>
#include <arch/idt.h>

/* 256 entries (required by x86) */
static struct idt_entry idt[256];

/* pointer used by lidt */
static struct idt_ptr idt_reg;

/* load IDT into CPU */
static inline void idt_load(struct idt_ptr* ptr) {
    __asm__ volatile ("lidt %0" : : "m"(*ptr));
}

extern void isr0();
extern void isr14();
extern void isr0();
extern void isr6();
extern void isr8();
extern void isr13();
extern void isr14();

/* initialize IDT (empty for now) */
void idt_init(void) {
    idt_reg.limit = sizeof(idt) - 1;
    idt_reg.base  = (uint64_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].zero        = 0;
    }

    /* register core exceptions */
    idt_set_entry(0,  isr0);
    idt_set_entry(6,  isr6);
    idt_set_entry(8,  isr8);
    idt_set_entry(13, isr13);
    idt_set_entry(14, isr14);

    idt_load(&idt_reg);
}

void idt_set_entry(int n , void* handler){
    uint64_t addr = (uint64_t)handler;

    // Extract and shift bits to fit the struct members
    idt[n].offset_low  = (uint16_t)(addr & 0xFFFF);        // Bits 0..15
    idt[n].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF); // Bits 16..31
    idt[n].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF); // Bits 32..63
    idt[n].selector    = 0x08;   // your 64-bit code segment
    idt[n].ist         = 0;      // no alternate stack (for now)
    idt[n].type_attr   = 0x8E;   // present, ring 0, interrupt gate
    idt[n].zero        = 0;
}