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
struct aarch64_frame {
    uint64_t x[31]; /* x0-x30 (x30 = LR) */
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t esr_el1;
    uint64_t far_el1;
    uint64_t _pad; /* keeps the frame 16-byte aligned (AAPCS64 requirement) */
};

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

typedef void (*aarch64_irq_handler_fn)(struct aarch64_frame *f);

/* Points VBAR_EL1 at the vector table in exceptions.S and unmasks IRQs
 * (DAIF.I) — call once, after the GIC (kernel/arch/aarch64/gic.c) is
 * initialized so unmasking doesn't immediately take an IRQ with nothing
 * configured to handle it. */
void exceptions_init(void);

/* Registers a handler for one GIC interrupt ID, dispatched from the IRQ
 * path once gic_init() + an actual enabled interrupt source exist. */
void register_irq_handler(unsigned irq, aarch64_irq_handler_fn handler);

#endif
