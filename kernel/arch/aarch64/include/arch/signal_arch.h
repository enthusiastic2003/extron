#ifndef ARCH_SIGNAL_ARCH_H
#define ARCH_SIGNAL_ARCH_H

/* AArch64 user-visible signal frame layout and register marshalling.
 * Included by kernel/proc/signal.c after struct user_siginfo/user_sigset/
 * user_stack are defined. */
#include <stddef.h>
#include <stdint.h>
#include <arch/exceptions.h>
#include <arch/vma.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/proc.h>

#define FPSIMD_MAGIC 0x46508001U

struct user_aarch64_ctx { uint32_t magic, size; };
struct user_fpsimd_context {
    struct user_aarch64_ctx head;
    uint32_t fpsr;
    uint32_t fpcr;
    uint64_t vregs[64];
};
struct user_mcontext {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint8_t reserved[4096];
};
struct user_ucontext {
    uint64_t flags;
    uint64_t link;
    struct user_stack stack;
    struct user_sigset sigmask;
    struct user_mcontext mcontext;
};
struct user_signal_frame {
    uint64_t magic;
    uint64_t saved_tpidr_el0;
    struct user_siginfo info;
    struct user_ucontext ucontext;
};
_Static_assert(sizeof(struct user_fpsimd_context) == 528,
               "fpsimd_context ABI size");
_Static_assert(offsetof(struct user_ucontext, mcontext) == 168,
               "ucontext_t mcontext ABI offset");
_Static_assert((sizeof(struct user_signal_frame) & 15) == 0,
               "signal frame preserves AAPCS64 stack alignment");



static inline uint64_t arch_signal_frame_addr(const struct arch_frame *f) {
    return (f->sp_el0 - sizeof(struct user_signal_frame)) & ~0xFULL;
}

static inline void arch_signal_fill_ucontext(struct user_signal_frame *frame,
                                             const struct arch_frame *saved,
                                             uint64_t old_mask) {
    struct user_ucontext *uc = &frame->ucontext;
    memset(uc, 0, sizeof(*uc));
    uc->sigmask.words[0] = old_mask;
    uc->mcontext.fault_address = saved->far_el1;
    memcpy(uc->mcontext.regs, saved->x, sizeof(saved->x));
    uc->mcontext.sp = saved->sp_el0;
    uc->mcontext.pc = saved->elr_el1;
    uc->mcontext.pstate = saved->spsr_el1;
    struct cpu_context state = {0};
    cpu_context_save_fpsimd(&state);
    frame->saved_tpidr_el0 = state.tpidr_el0;
    struct user_fpsimd_context *fp =
        (struct user_fpsimd_context *)uc->mcontext.reserved;
    fp->head.magic = FPSIMD_MAGIC;
    fp->head.size = sizeof(*fp);
    fp->fpsr = (uint32_t)state.fpsr;
    fp->fpcr = (uint32_t)state.fpcr;
    memcpy(fp->vregs, state.v, sizeof(fp->vregs));
}

/* Redirect the trap frame into the user's signal handler. */
static inline void arch_signal_enter(struct arch_frame *f,
                                     struct user_signal_frame *frame,
                                     uint64_t sp, int signo,
                                     const struct signal_action *action) {
    f->x[0] = (uint64_t)signo;
    f->x[1] = (uint64_t)&frame->info;
    f->x[2] = (uint64_t)&frame->ucontext;
    f->x[30] = action->restorer;
    f->elr_el1 = action->handler;
    f->spsr_el1 = 0;
    f->sp_el0 = sp;
}

/* Reload registers saved by arch_signal_fill_ucontext. false = corrupt frame. */
static inline bool arch_signal_restore(struct arch_frame *f,
                                       struct user_signal_frame *frame) {
    struct user_mcontext *mc = &frame->ucontext.mcontext;
    if (mc->pc >= ARCH_USER_VA_LIMIT || mc->sp >= ARCH_USER_VA_LIMIT)
        return false;
    struct user_fpsimd_context *fp =
        (struct user_fpsimd_context *)mc->reserved;
    if (fp->head.magic != FPSIMD_MAGIC || fp->head.size != sizeof(*fp))
        return false;
    memcpy(f->x, mc->regs, sizeof(f->x));
    f->elr_el1 = mc->pc;
    f->sp_el0 = mc->sp;
    f->spsr_el1 = 0;
    struct cpu_context state = {0};
    state.tpidr_el0 = frame->saved_tpidr_el0;
    state.fpsr = fp->fpsr;
    state.fpcr = fp->fpcr;
    memcpy(state.v, fp->vregs, sizeof(state.v));
    cpu_context_restore_fpsimd(&state);
    return true;
}

#endif
