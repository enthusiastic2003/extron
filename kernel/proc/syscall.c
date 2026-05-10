#include <kernel/proc/syscall.h>
#include <kernel/console.h>
#include <stdint.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/time.h>

// ----------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------
static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count);
static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count);
static uint64_t sys_sleep(uint64_t seconds, uint64_t arg2, uint64_t arg3);
static uint64_t sys_proc_dump(uint64_t arg1, uint64_t arg2, uint64_t arg3);

// ----------------------------------------------------------------
// Syscall table
// ----------------------------------------------------------------

static const syscall_fn syscall_table[] = {
    [SYS_WRITE] = sys_write,
    [SYS_READ]  = sys_read,
    [SYS_SLEEP] = sys_sleep,
    [SYS_PROC_DUMP] = sys_proc_dump,
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
    
    return kbd_read((char *)buf_addr, count);
}


static uint64_t sys_sleep(uint64_t seconds, uint64_t arg2, uint64_t arg3)
{
    (void)arg2;
    (void)arg3;
    struct proc *p = my_cpu();
    if (!p) return (uint64_t)-1;

    uint64_t ticks_to_sleep = seconds * 100; // 100 Hz
    p->sleep_until = time_now() + ticks_to_sleep;
    p->chan = NULL;
    proc_set_sleeping(p);

    schedule();

    return 0;
}


static uint64_t sys_proc_dump(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    proc_dump_table();
    return 0;
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