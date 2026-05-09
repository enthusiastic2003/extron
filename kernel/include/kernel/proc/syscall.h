#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// Syscall numbers
// ----------------------------------------------------------------

#define SYS_WRITE   1
#define SYS_READ    0

typedef uint64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t);

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

// Called once at boot to configure SYSCALL/SYSRET MSRs.
void syscall_init(void);

// Invoked by the assembly landing pad; returns the syscall result.
uint64_t syscall_dispatch(uint64_t nr,
                          uint64_t arg1,
                          uint64_t arg2,
                          uint64_t arg3);