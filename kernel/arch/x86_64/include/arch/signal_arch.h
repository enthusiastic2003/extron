#ifndef ARCH_SIGNAL_ARCH_H
#define ARCH_SIGNAL_ARCH_H

/* x86-64 user-visible signal frame. Layout of mcontext follows the Linux
 * x86-64 ABI order (r8..r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip,
 * eflags, ...) so a libc using the same layout can use it unchanged.
 * Included by kernel/proc/signal.c after struct user_siginfo/user_sigset/
 * user_stack are defined. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <arch/exceptions.h>
#include <arch/vma.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/proc.h>

struct user_mcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;        /* unused: FP state lives in `fxsave` below */
    uint64_t reserved1[8];
};
struct user_ucontext {
    uint64_t flags;
    uint64_t link;
    struct user_stack stack;
    struct user_mcontext mcontext;
    struct user_sigset sigmask;
    uint8_t fxsave[512] __attribute__((aligned(16)));
};
struct user_signal_frame {
    uint64_t magic;
    uint64_t saved_fs_base;
    struct user_siginfo info;
    struct user_ucontext ucontext;
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct user_siginfo) == 128, "siginfo_t ABI size");

/* Frame lives below the redzone (128 bytes) of the interrupted code. The
 * resulting rsp at handler entry is (16n+8), as if `call` had pushed the
 * return address (the restorer), per the SysV ABI. */
static inline uint64_t arch_signal_frame_addr(const struct arch_frame *f) {
    uint64_t sp = f->rsp - 128 - sizeof(struct user_signal_frame);
    return sp & ~0xFULL;
}

static inline void arch_signal_fill_ucontext(struct user_signal_frame *frame,
                                             const struct arch_frame *s,
                                             uint64_t old_mask) {
    struct user_ucontext *uc = &frame->ucontext;
    memset(uc, 0, sizeof(*uc));
    uc->sigmask.words[0] = old_mask;
    struct user_mcontext *m = &uc->mcontext;
    m->r8 = s->r8; m->r9 = s->r9; m->r10 = s->r10; m->r11 = s->r11;
    m->r12 = s->r12; m->r13 = s->r13; m->r14 = s->r14; m->r15 = s->r15;
    m->rdi = s->rdi; m->rsi = s->rsi; m->rbp = s->rbp; m->rbx = s->rbx;
    m->rdx = s->rdx; m->rax = s->rax; m->rcx = s->rcx; m->rsp = s->rsp;
    m->rip = s->rip; m->eflags = s->rflags;
    m->cs = (uint16_t)s->cs; m->ss = (uint16_t)s->ss;
    m->err = s->error_code; m->trapno = s->vector; m->cr2 = s->cr2;
    struct cpu_context state;
    memset(&state, 0, sizeof(state));
    cpu_context_save_fpsimd(&state);
    frame->saved_fs_base = state.fs_base;
    memcpy(uc->fxsave, state.fxsave, sizeof(uc->fxsave));
}

/* Redirect the trap frame into the user's signal handler. The restorer
 * address is pushed as the handler's return address. */
static inline void arch_signal_enter(struct arch_frame *f,
                                     struct user_signal_frame *frame,
                                     uint64_t sp, int signo,
                                     const struct signal_action *action) {
    uint64_t new_sp = sp - 8;
    *(uint64_t *)new_sp = action->restorer;
    f->rdi = (uint64_t)signo;
    f->rsi = (uint64_t)&frame->info;
    f->rdx = (uint64_t)&frame->ucontext;
    f->rax = 0;
    f->rip = action->handler;
    f->rflags = 0x202;
    f->cs = X86_USER_CS;
    f->ss = X86_USER_SS;
    f->rsp = new_sp;
}

/* After the restorer's sigreturn syscall: rsp points just past the
 * handler's return address, i.e. at sp+8 of the original frame base. */
static inline bool arch_signal_restore(struct arch_frame *f,
                                       struct user_signal_frame *frame) {
    struct user_mcontext *m = &frame->ucontext.mcontext;
    if (m->rip >= ARCH_USER_VA_LIMIT || m->rsp >= ARCH_USER_VA_LIMIT)
        return false;
    f->r8 = m->r8; f->r9 = m->r9; f->r10 = m->r10; f->r11 = m->r11;
    f->r12 = m->r12; f->r13 = m->r13; f->r14 = m->r14; f->r15 = m->r15;
    f->rdi = m->rdi; f->rsi = m->rsi; f->rbp = m->rbp; f->rbx = m->rbx;
    f->rdx = m->rdx; f->rax = m->rax; f->rcx = m->rcx;
    f->rsp = m->rsp;
    f->rip = m->rip;
    f->rflags = (m->eflags & 0xCD5) | 0x202; /* user-modifiable flags only */
    f->cs = X86_USER_CS;
    f->ss = X86_USER_SS;
    struct cpu_context state;
    memset(&state, 0, sizeof(state));
    state.fs_base = frame->saved_fs_base;
    memcpy(state.fxsave, frame->ucontext.fxsave, sizeof(state.fxsave));
    cpu_context_restore_fpsimd(&state);
    return true;
}

#endif
