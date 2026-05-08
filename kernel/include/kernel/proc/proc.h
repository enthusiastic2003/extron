#pragma once
#include <stdint.h>

// Beginning of the per-process kernel state.
// Field offsets are used directly by syscall_entry.asm — do NOT reorder.
struct proc {
    uint64_t kernel_rsp;    // offset  0: kernel stack top (syscalls + IRQs)
    uint64_t user_rsp;      // offset  8: user RSP saved on SYSCALL entry

    // Future fields (pid, state, cr3, …) go here.
};

// Pointer to the currently-running process; set by exec() before entering ring 3.
extern struct proc *current_proc;
