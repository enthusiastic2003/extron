#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// Syscall numbers
// ----------------------------------------------------------------

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_SLEEP       2
#define SYS_PROC_DUMP   3
#define SYS_ANON_ALLOC  4
#define SYS_ANON_FREE   5
#define SYS_TCB_SET     6   /* set FS base for TLS (mlibc sys_tcb_set) */
#define SYS_EXIT        7   /* terminate current process */

typedef uint64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t);

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

// Called once at boot to configure SYSCALL/SYSRET MSRs.
void syscall_init(void);

struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rsp;
} __attribute__((packed));

// Invoked by the assembly landing pad; returns the syscall result.
uint64_t syscall_dispatch(struct syscall_frame *f);