#include <kernel/proc/syscall.h>
#include <kernel/console.h>
#include <stdint.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>

// ----------------------------------------------------------------
// Syscall table
// ----------------------------------------------------------------


static const syscall_fn syscall_table[] = {
    [SYS_WRITE] = sys_write,
    [SYS_READ]  = sys_read,

};

#define SYSCALL_COUNT (sizeof(syscall_table) / sizeof(syscall_table[0]))

// ----------------------------------------------------------------
// Handlers
// ----------------------------------------------------------------

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 1)
        return (uint64_t)-1;

    const char *buf = (const char *)buf_addr;
    for (uint64_t i = 0; i < count; i++)
        console_putc(buf[i]);

    return count;
}


static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 0)
        return (uint64_t)-1;
    
    struct proc* p = my_cpu();
    proc_set_sleeping(p);

    return kbd_read((char *)buf_addr, count);
}



// ----------------------------------------------------------------
// Dispatcher
// ----------------------------------------------------------------

uint64_t syscall_dispatch(uint64_t nr,
                          uint64_t arg1,
                          uint64_t arg2,
                          uint64_t arg3)
{
    if (nr >= SYSCALL_COUNT || !syscall_table[nr])
        return (uint64_t)-1;

    return syscall_table[nr](arg1, arg2, arg3);
}