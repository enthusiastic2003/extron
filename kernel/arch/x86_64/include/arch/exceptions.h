#ifndef ARCH_EXCEPTIONS_H
#define ARCH_EXCEPTIONS_H

#include <stdint.h>

/*
 * Trap frame, built by isr.S's common entry for every interrupt,
 * exception and syscall (the `syscall` instruction path fabricates the
 * same hardware-style iretq frame, so everything returns via iretq).
 * Lowest address first; the field order is what the push sequence in
 * isr.S produces. 24 qwords keeps the frame 16-byte aligned.
 */
struct arch_frame {
    uint64_t pad;
    uint64_t cr2;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    /* pushed by the CPU (or fabricated by syscall_entry) */
    uint64_t rip, cs, rflags, rsp, ss;
};

#define ARCH_FRAME_SIZE 192

#define X86_USER_CS 0x23
#define X86_USER_SS 0x1B

/* Syscall ABI (Linux-like): rax = number, rdi/rsi/rdx = args 1-3, result in rax. */
#define FRAME_SYSNR(f)       ((f)->rax)
#define FRAME_ARG(f, n)      (*((n) == 0 ? &(f)->rdi : (n) == 1 ? &(f)->rsi : &(f)->rdx))
#define FRAME_RET(f)         ((f)->rax)
#define FRAME_FROM_USER(f)   (((f)->cs & 3) == 3)
#define FRAME_USER_SP(f)     ((f)->rsp)
#define FRAME_FAULT_ADDR(f)  ((f)->cr2)

/* Rewrite `f` so the next return-to-user starts a fresh program image. */
static inline void arch_frame_init_user(struct arch_frame *f, uint64_t entry,
                                        uint64_t sp, uint64_t arg0,
                                        uint64_t arg1) {
    f->r15 = f->r14 = f->r13 = f->r12 = f->r11 = f->r10 = f->r9 = f->r8 = 0;
    f->rbp = f->rbx = f->rcx = f->rdx = f->rax = 0;
    f->rdi = arg0;
    f->rsi = arg1;
    f->rip = entry;
    f->cs = X86_USER_CS;
    f->ss = X86_USER_SS;
    f->rflags = 0x202;              /* IF set, everything else clear */
    f->rsp = sp;
}

typedef void (*arch_irq_handler_fn)(struct arch_frame *f);

/* Legacy-PIC IRQ line numbers (0-15), not IDT vectors. */
void register_irq_handler(unsigned irq, arch_irq_handler_fn handler);

/* Installs the GDT/TSS/IDT and enables the syscall instruction. */
void exceptions_init(void);
void exceptions_enable_irqs(void);

#endif
