#include <arch/exceptions.h>
#include <kernel/console.h>
#include <kernel/panic.h>

/*
 * exception_dispatch() — the aarch64 counterpart to x86's isr_handler()
 * (kernel/arch/x86_64/isr_c.c), called from exceptions.S's
 * common_exception_entry after a full register save. Dispatch here is
 * by exception TYPE (Sync/IRQ/FIQ/SError, set by exceptions.S) and, for
 * synchronous exceptions, by ESR_EL1's exception class field — a
 * different axis than x86's per-vector-number IDT dispatch, since
 * AArch64 funnels every synchronous exception through one entry point
 * and expects software to read *why* out of a register instead.
 *
 * GIC bring-up (kernel/arch/aarch64/gic.c) hasn't landed yet, so IRQ/FIQ
 * have nothing legitimate to dispatch to — panic loudly rather than
 * silently drop them, same as x86's isr_handler falls back to panic for
 * anything without a registered handler.
 */

#define IRQ_HANDLER_MAX 256

static aarch64_irq_handler_fn irq_handlers[IRQ_HANDLER_MAX];

void register_irq_handler(unsigned irq, aarch64_irq_handler_fn handler) {
    if (irq < IRQ_HANDLER_MAX) {
        irq_handlers[irq] = handler;
    }
}

extern char vector_table[]; /* exceptions.S */

void exceptions_init(void) {
    __asm__ volatile ("msr vbar_el1, %0" :: "r"(vector_table) : "memory");
    __asm__ volatile ("isb");

    /* Unmask IRQs only (DAIF.I) — FIQ/SError/Debug stay masked for this
     * first pass, matching a conservative rollout (widen once there's
     * something real to handle). */
    __asm__ volatile ("msr daifclr, #2");
    __asm__ volatile ("isb");
}

static const char *ec_name(uint32_t ec) {
    switch (ec) {
        case 0x00: return "Unknown reason";
        case 0x0E: return "Illegal Execution state";
        case 0x15: return "SVC instruction (AArch64)";
        case 0x20: return "Instruction Abort (lower EL)";
        case 0x21: return "Instruction Abort (same EL)";
        case 0x22: return "PC alignment fault";
        case 0x24: return "Data Abort (lower EL)";
        case 0x25: return "Data Abort (same EL)";
        case 0x26: return "SP alignment fault";
        default:   return "Unhandled exception class";
    }
}

void exception_dispatch(struct aarch64_frame *f, int type) {
    if (type == AARCH64_EXC_IRQ) {
        /* GIC not wired up yet — nothing should be generating IRQs at
         * this point, but don't silently swallow one if it happens. */
        panic("Unexpected IRQ (no GIC driver yet)\nELR=%p\n", (void *)f->elr_el1);
        return;
    }

    if (type == AARCH64_EXC_FIQ || type == AARCH64_EXC_SERROR) {
        panic("Unexpected %s\nELR=%p\nESR=%p\nFAR=%p\n",
              type == AARCH64_EXC_FIQ ? "FIQ" : "SError",
              (void *)f->elr_el1, (void *)f->esr_el1, (void *)f->far_el1);
        return;
    }

    /* Synchronous exception — decode ESR_EL1.EC (bits [31:26]). */
    uint32_t ec = (uint32_t)((f->esr_el1 >> 26) & 0x3F);
    panic("SYNCHRONOUS EXCEPTION\n"
          "class=%s\n"
          "ELR=%p\n"
          "ESR=%p\n"
          "FAR=%p\n",
          ec_name(ec), (void *)f->elr_el1, (void *)f->esr_el1, (void *)f->far_el1);
}
