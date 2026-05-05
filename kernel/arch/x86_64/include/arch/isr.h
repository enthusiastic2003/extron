#ifndef ARCH_ISR_H
#define ARCH_ISR_H

#include <stdint.h>

/* Matches the stack layout we create in the stub */
struct isr_frame {
    /* general registers (pushed by us) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* our pushes */
    uint64_t vector;
    uint64_t error_code;

    /* pushed by CPU on interrupt */
    uint64_t rip, cs, rflags, rsp, ss;
};

void isr_handler(struct isr_frame* f);

void enable_interrupt();

void disable_interrupt();

#endif