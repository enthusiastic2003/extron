#pragma once
#include <stdint.h>

#define SYS_WRITE 1

void     syscall_init(void);

uint64_t syscall_dispatch(uint64_t nr,
                          uint64_t arg1,
                          uint64_t arg2,
                          uint64_t arg3);
