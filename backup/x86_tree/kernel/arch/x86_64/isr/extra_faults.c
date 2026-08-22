// kernel/arch/x86_64/handlers.c

#include <stdint.h>
#include <kernel/panic.h>
#include <arch/isr.h>
#include <arch/irh.h>

/* --- handlers --- */

void handle_invalid_opcode(struct isr_frame* f) {
    panic("Invalid Opcode (#UD)\n"
          "RIP=%p\n",
          (void*)f->rip);
}

void handle_double_fault(struct isr_frame* f) {
    panic("Double Fault (#DF)\n"
          "RIP=%p\n",
          (void*)f->rip);
}

void handle_gpf(struct isr_frame* f) {
    panic("General Protection Fault (#GP)\n"
          "err=%x\n"
          "RIP=%p\n",
          (unsigned int)f->error_code,
          (void*)f->rip);
}