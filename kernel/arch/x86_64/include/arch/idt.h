#ifndef ARCH_IDT_H
#define ARCH_IDT_H

#include <stdint.h>

/* One IDT entry (16 bytes in long mode) */
struct idt_entry {
    uint16_t offset_low;     // bits 0–15 of handler address
    uint16_t selector;       // code segment selector (GDT)
    uint8_t  ist;            // Interrupt Stack Table (0 = unused)
    uint8_t  type_attr;      // type and flags
    uint16_t offset_mid;     // bits 16–31
    uint32_t offset_high;    // bits 32–63
    uint32_t zero;
} __attribute__((packed));

/* IDT pointer used by lidt */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// arch/idt.h
void idt_init(void);
void idt_set_entry(int n , void* handler);
#endif