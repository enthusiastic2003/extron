// kernel/proc/syscall.c

#include <kernel/proc/syscall.h>
#include <kernel/console.h>
#include <stdint.h>

static uint64_t sys_write(uint64_t fd,
                          const char* buf,
                          uint64_t count)
{
    if (fd != 1)
        return (uint64_t)-1;

    for (uint64_t i = 0; i < count; i++) {
        console_putc(buf[i]);
    }
    kprintf("Kernel Here: Completed user's request!!\n");

    return count;
}

uint64_t syscall_dispatch(uint64_t syscall_no,
                          uint64_t arg1,
                          uint64_t arg2,
                          uint64_t arg3)
{
    switch (syscall_no) {

        case SYS_WRITE:
            return sys_write(
                arg1,
                (const char*)arg2,
                arg3
            );

        default:
            return (uint64_t)-1;
    }
}