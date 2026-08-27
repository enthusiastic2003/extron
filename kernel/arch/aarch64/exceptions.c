#include <arch/exceptions.h>
#include <arch/gic.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/proc/syscall.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/resource.h>
#include <stdbool.h>

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
 * The IRQ path mirrors kernel/arch/x86_64/irq.c's shape (an
 * irq_handlers[] table + register_irq_handler()) but acknowledges
 * through the GIC-400 (gic_ack_irq()/gic_eoi_irq()) instead of PIC port
 * writes — x86's irq.c sends EOI *before* dispatching (its comment
 * explains why: a handler might reschedule away mid-stack, and an
 * un-ACKed PIC would starve further IRQs on that line until whatever
 * comes back around); GIC's EOI is independent of the ACK read (IAR) and
 * is read here after the handler returns, since nothing reschedules yet.
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
}

void exceptions_enable_irqs(void) {
    /* Unmask IRQ (DAIF.I) AND FIQ (DAIF.F) — SError/Debug stay masked.
     * FIQ is unmasked defensively: GIC-400's Group 0 (the reset default
     * for any interrupt our EL3 armstub hasn't explicitly reassigned)
     * signals via FIQ rather than IRQ, and exception_dispatch() handles
     * both lines through the same GIC ack/EOI path — so nothing currently
     * routed through Group 0 gets silently dropped just because it didn't
     * arrive on the line we happened to be listening to. */
    __asm__ volatile ("msr daifclr, #3");
    __asm__ volatile ("isb");
}

static const char *ec_name(uint32_t ec) {
    switch (ec) {
        case 0x00: return "Unknown reason";
        case 0x0E: return "Illegal Execution state";
        case 0x15: return "SVC instruction (AArch64)";
        case 0x18: return "Trapped MSR/MRS/System instruction";
        case 0x20: return "Instruction Abort (lower EL)";
        case 0x21: return "Instruction Abort (same EL)";
        case 0x22: return "PC alignment fault";
        case 0x24: return "Data Abort (lower EL)";
        case 0x25: return "Data Abort (same EL)";
        case 0x26: return "SP alignment fault";
        case 0x2C: return "Floating-point exception";
        case 0x3C: return "BRK instruction";
        default:   return "Unhandled exception class";
    }
}

static int fault_signal(uint32_t ec) {
    switch (ec) {
        case 0x20: /* instruction abort */
        case 0x22: /* PC alignment */
        case 0x24: /* data abort */
        case 0x26: /* SP alignment */
            return 11; /* SIGSEGV */
        case 0x2C:
            return 8;  /* SIGFPE */
        case 0x3C:
            return 5;  /* SIGTRAP */
        default:
            return 4;  /* SIGILL */
    }
}

void exception_dispatch(struct aarch64_frame *f, int type) {
    resource_account_exception_enter(f);

    if (type == AARCH64_EXC_IRQ || type == AARCH64_EXC_FIQ) {
        /* GICC_IAR/GICC_EOIR are the same registers regardless of which
         * line (IRQ or FIQ) signaled the core — see
         * exceptions_enable_irqs()'s comment on why real hardware
         * delivers Group 0 interrupts (anything gic_init() failed to
         * move to Group 1) via FIQ instead. */
        unsigned id = gic_ack_irq();

        if (id == 1023) {
            /* Spurious — GIC's "no pending interrupt" sentinel. Not a
             * real interrupt; must NOT be EOI'd (there's nothing to
             * acknowledge completion of). */
            resource_account_exception_leave(f);
            return;
        }

        /* EOI BEFORE dispatch, not after: a handler that reschedules
         * (kernel/proc/sched.c's schedule() -> context_switch())
         * can permanently divert into another proc's trampoline instead
         * of ever returning back up through this function — so anything
         * placed after the handler call is not guaranteed to run. GICv2
         * allows EOI independent of whether processing is actually
         * finished (it just tells the distributor this priority level
         * is done, unrelated to our own bookkeeping), so this is safe:
         * re-arming already happens at the very start of
         * timer_irq_handler() regardless. (Previously EOI'd after —
         * fine when nothing ever rescheduled; now something does.) */
        gic_eoi_irq(id);

        if (id < IRQ_HANDLER_MAX && irq_handlers[id]) {
            irq_handlers[id](f);
        } else {
            panic("Unhandled %s %u (no registered handler)\nELR=%p\n",
                  type == AARCH64_EXC_FIQ ? "FIQ" : "IRQ", id, (void *)f->elr_el1);
        }

        signal_deliver_pending(f);

        resource_account_exception_leave(f);

        return;
    }

    if (type == AARCH64_EXC_SERROR) {
        panic("Unexpected SError\nELR=%p\nESR=%p\nFAR=%p\n",
              (void *)f->elr_el1, (void *)f->esr_el1, (void *)f->far_el1);
        return;
    }

    /* Synchronous exception — decode ESR_EL1.EC (bits [31:26]). SPSR's
     * low 4 bits (M[3:0]) name which EL/SP the exception came FROM —
     * 0x0 = EL0t, 0x5 = EL1h — the direct proof an EL0 eret really
     * landed and trapped back, not just that *something* faulted. */
    uint32_t ec = (uint32_t)((f->esr_el1 >> 26) & 0x3F);

    bool from_el0 = (f->spsr_el1 & 0xF) == 0x0;

    if (ec == 0x15 && from_el0) {
        /* SVC (AArch64) — a real syscall, not a fault. Dispatch and
         * return normally: ELR_EL1 already points past the svc
         * instruction (set by hardware on exception entry, no manual
         * adjustment needed), so falling through to the vector table's
         * own RESTORE_CONTEXT+eret resumes userland exactly where it
         * left off, with the result sitting in x0 per AAPCS64's
         * return-value convention. No separate return path needed —
         * unlike x86's SYSCALL, which clobbers RSP and needs a
         * dedicated syscall_return landing pad to rebuild an IRETQ
         * frame, SP_EL0 here is a banked register eret never touches,
         * so the user stack pointer survives this round trip for free. */
        f->x[0] = syscall_dispatch(f);
        signal_deliver_pending(f);
        resource_account_exception_leave(f);
        return;
    }

    /* Which process, and whether it is standing on its own kernel
     * stack. `frame` is the stack pointer at the moment of the fault
     * (SAVE_CONTEXT builds the frame at SP), so a frame outside
     * [kernel_stack_base, kernel_stack_top) says the proc is running on
     * somebody else's stack — which is a completely different bug from
     * whatever the faulting instruction appears to be doing, and is
     * invisible without printing both.
     *
     * Added after exactly that: a Data Abort inside RESTORE_CONTEXT
     * that read as memory corruption, and was really the first process
     * having been handed the boot stack by the race
     * install_and_switch() now masks against. The ELR/ESR/FAR triple
     * alone pointed at the wrong file. */
    struct proc *cur = my_proc();
    struct thread *thread = my_thread();
    if (from_el0 && cur && thread) {
        int signal = fault_signal(ec);
        if (signal_deliver_sync(f, signal)) {
            resource_account_exception_leave(f);
            return;
        }
        kprintf("\n[USER FAULT] pid=%lu tid=%lu signal=%d (%s)\n"
                "ELR=%p ESR=%p FAR=%p\n",
                (unsigned long)cur->pid, (unsigned long)thread->tid,
                signal, ec_name(ec), (void *)f->elr_el1,
                (void *)f->esr_el1, (void *)f->far_el1);
        proc_exit_current(-signal);
    }

    panic("SYNCHRONOUS EXCEPTION\n"
          "pid=%ld frame=%p kstack=[%p,%p)\n"
          "class=%s\n"
          "ELR=%p\n"
          "SPSR=%p (mode=%s)\n"
          "ESR=%p\n"
          "FAR=%p\n",
          cur ? (long)cur->pid : -1, (void *)f,
          thread ? (void *)thread->kernel_stack_base : 0,
          thread ? (void *)thread->kernel_stack_top : 0,
          ec_name(ec), (void *)f->elr_el1, (void *)f->spsr_el1,
          (f->spsr_el1 & 0xF) == 0x0 ? "EL0t" : (f->spsr_el1 & 0xF) == 0x5 ? "EL1h" : "other",
          (void *)f->esr_el1, (void *)f->far_el1);
}
