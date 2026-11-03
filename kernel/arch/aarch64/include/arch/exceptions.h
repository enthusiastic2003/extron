#ifndef ARCH_EXCEPTIONS_H
#define ARCH_EXCEPTIONS_H

#include <stdint.h>

/*
 * Matches the stack layout exceptions.S's SAVE_CONTEXT macro builds —
 * the aarch64 counterpart to x86's struct isr_frame (kernel/arch/x86_64/
 * include/arch/isr.h), but shaped around AArch64's actual exception
 * model: no CPU-pushed error code/CS/RFLAGS/SS (nothing is pushed
 * automatically at all — this struct exists purely because our own
 * vector stubs explicitly save every field), and ESR_EL1 (exception
 * syndrome, an "what/why" field) plus FAR_EL1 (fault address) stand in
 * for x86's error_code/cr2.
 */
struct arch_frame {
    uint64_t x[31]; /* x0-x30 (x30 = LR) */
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t esr_el1;
    uint64_t far_el1;
    /* SP_EL0 — the interrupted context's OWN stack pointer. Must be
     * saved and restored like any other per-process register.
     *
     * It is banked, so it survives an exception on its own and a
     * single-process system never notices it missing. Across a CONTEXT
     * SWITCH it is not preserved by anything: only
     * proc_bootstrap_trampoline() ever writes it, and only on first
     * launch, so every later resumption inherited whatever SP_EL0 the
     * previously-running process happened to leave behind. Stayed
     * invisible until the first processes that actually use a stack
     * (the C payloads) — every assembly payload pushes nothing.
     *
     * Also conveniently keeps the frame 16-byte aligned, which is what
     * this slot was previously padding for. */
    uint64_t sp_el0;
};

#define ARCH_FRAME_SIZE 288

/* Arch-neutral trap-frame accessors used by shared kernel code. */
#define FRAME_SYSNR(f)       ((f)->x[8])
#define FRAME_ARG(f, n)      ((f)->x[n])
#define FRAME_RET(f)         ((f)->x[0])
#define FRAME_FROM_USER(f)   (((f)->spsr_el1 & 0xf) == 0)
#define FRAME_USER_SP(f)     ((f)->sp_el0)
#define FRAME_FAULT_ADDR(f)  ((f)->far_el1)

/* Rewrite `f` so the next return-to-user starts a fresh program image. */
static inline void arch_frame_init_user(struct arch_frame *f, uint64_t entry,
                                        uint64_t sp, uint64_t arg0,
                                        uint64_t arg1) {
    __builtin_memset(f->x, 0, sizeof(f->x));
    f->x[0]     = arg0;
    f->x[1]     = arg1;
    f->elr_el1  = entry;
    f->spsr_el1 = 0;                  /* EL0t, all masks clear */
    f->sp_el0   = sp;
}

/* Exception type, as set by exceptions.S before branching into C —
 * distinguishes which of VBAR_EL1's 4 exception classes fired (only the
 * "Current EL, SP_ELx" group is reachable right now; no EL0/userspace
 * exists yet to trigger the "Lower EL" groups). */
enum aarch64_exception_type {
    AARCH64_EXC_SYNC = 0,
    AARCH64_EXC_IRQ = 1,
    AARCH64_EXC_FIQ = 2,
    AARCH64_EXC_SERROR = 3,
};

typedef void (*arch_irq_handler_fn)(struct arch_frame *f);

/* Points VBAR_EL1 at the vector table in exceptions.S. Call first, before
 * anything that could fault (including the GIC's own MMIO mapping) —
 * without this, any exception has nowhere valid to land. */
void exceptions_init(void);

/* Unmasks IRQ and FIQ at the CPU level (DAIF.I/DAIF.F) — both, since
 * GIC-400's Group 0 default routes to FIQ rather than IRQ, and
 * exception_dispatch() handles either line through the same GIC ack/EOI
 * path. Call last, only once the GIC and every interrupt source it's
 * expected to deliver are fully configured — deliberately separate from
 * exceptions_init() so nothing can be delivered before something exists
 * to handle it. */
void exceptions_enable_irqs(void);

/* Registers a handler for one GIC interrupt ID, dispatched from the IRQ
 * path once gic_init() + an actual enabled interrupt source exist. */
void register_irq_handler(unsigned irq, arch_irq_handler_fn handler);

#endif
