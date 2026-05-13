#include <kernel/proc/syscall.h>
#include <kernel/console.h>
#include <stdint.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/time.h>
#include <kernel/mm/paging.h>
#include <kernel/klibc/string.h>

// ----------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count);
static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count);
static uint64_t sys_sleep(uint64_t seconds, uint64_t arg2, uint64_t arg3);
static uint64_t sys_proc_dump(uint64_t arg1, uint64_t arg2, uint64_t arg3);
static uint64_t sys_anon_allocate(uint64_t size, uint64_t arg2, uint64_t arg3);
static uint64_t sys_anon_free(uint64_t addr, uint64_t size, uint64_t arg3);
static uint64_t sys_tcb_set(uint64_t addr, uint64_t arg2, uint64_t arg3);
static uint64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3);
static uint64_t sys_fork(struct syscall_frame *f);

// ----------------------------------------------------------------
// Syscall table
// ----------------------------------------------------------------

static const syscall_fn syscall_table[] = {
    [SYS_READ]       = sys_read,
    [SYS_WRITE]      = sys_write,
    [SYS_SLEEP]      = sys_sleep,
    [SYS_PROC_DUMP]  = sys_proc_dump,
    [SYS_ANON_ALLOC] = sys_anon_allocate,
    [SYS_ANON_FREE]  = sys_anon_free,
    [SYS_TCB_SET]    = sys_tcb_set,
    [SYS_EXIT]       = sys_exit,
};

static const char *syscall_names[] = {
    [SYS_READ]       = "read",
    [SYS_WRITE]      = "write",
    [SYS_SLEEP]      = "sleep",
    [SYS_PROC_DUMP]  = "proc_dump",
    [SYS_ANON_ALLOC] = "anon_alloc",
    [SYS_ANON_FREE]  = "anon_free",
    [SYS_TCB_SET]    = "tcb_set",
    [SYS_EXIT]       = "exit",
    [SYS_FORK]       = "fork",
};

#define SYSCALL_COUNT (sizeof(syscall_table) / sizeof(syscall_table[0]))

// ----------------------------------------------------------------
// Handlers
// ----------------------------------------------------------------

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 1 && fd != 2) {
        kprintf("  [write] unsupported fd=%d\n", (int)fd);
        return (uint64_t)-1;
    }

    const char *buf = (const char *)buf_addr;
    //kprintf("[SYS_WRITE] fd=%d buf=0x%lx count=%lu\n", (int)fd, buf_addr, count);
    for (uint64_t i = 0; i < count && i < 64; i++)
        console_putc(buf[i]);
    //kprintf("\"\n");

    return count;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    //kprintf("[SYS_READ] fd=%llu buf=0x%llx count=%llu\n",
            // fd, buf_addr, count);

    if (fd != 0) {
        kprintf("[SYS_READ] unsupported fd %llu\n", fd);
        return (uint64_t)-1;
    }

    uint64_t ret = kbd_read((char *)buf_addr, count);

    // kprintf("[SYS_READ] returned %llu\n", ret);

    return ret;
}

static uint64_t sys_sleep(uint64_t seconds,
                          uint64_t nanos,
                          uint64_t arg3)
{
    (void)arg3;

    struct proc *p = my_cpu();
    if (!p)
        return (uint64_t)-1;

    // 100 Hz timer -> 1 tick = 10ms = 10,000,000 nanos
    uint64_t ticks_to_sleep = (seconds * 100) + (nanos / 10000000);
    
    // Always sleep at least 1 tick if any time was requested, so we don't return instantly
    if (ticks_to_sleep == 0 && (seconds > 0 || nanos > 0)) {
        ticks_to_sleep = 1;
    }

    p->sleep_until = time_now() + ticks_to_sleep;
    p->chan = NULL;

    proc_set_sleeping(p);

    schedule();

    return 0;
}

static uint64_t sys_proc_dump(uint64_t arg1,
                              uint64_t arg2,
                              uint64_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    // kprintf("[SYS_PROC_DUMP]\n");

    proc_dump_table();

    return 0;
}

static uint64_t sys_anon_allocate(uint64_t size,
                                  uint64_t arg2,
                                  uint64_t arg3)
{
    (void)arg2;
    (void)arg3;

    // kprintf("[SYS_ANON_ALLOC] size=%llu\n", size);

    struct proc *p = my_cpu();

    if (!p || !p->mm)
        return (uint64_t)-1;

    virt_addr_t addr =
        vm_allocate_region(p->mm,
                           (size_t)size,
                           VM_READ | VM_WRITE | VM_USER);

    // kprintf("[SYS_ANON_ALLOC] returned 0x%llx\n", addr);

    return addr ? addr : (uint64_t)-1;
}

static uint64_t sys_anon_free(uint64_t addr,
                              uint64_t size,
                              uint64_t arg3)
{
    (void)arg3;

    // kprintf("[SYS_ANON_FREE] addr=0x%llx size=%llu\n",
    //         addr, size);

    struct proc *p = my_cpu();

    if (!p || !p->mm)
        return (uint64_t)-1;

    vm_free_region(p->mm,
                   (virt_addr_t)addr,
                   (size_t)size);

    return 0;
}

static uint64_t sys_tcb_set(uint64_t addr,
                            uint64_t arg2,
                            uint64_t arg3)
{
    (void)arg2;
    (void)arg3;

    // kprintf("[SYS_TCB_SET] fs_base=0x%llx\n", addr);

    struct proc *p = my_cpu();

    if (!p)
        return (uint64_t)-1;

    p->fs_base = addr;

    __asm__ volatile(
        "wrmsr"
        :
        : "c"(0xC0000100U),
          "a"((uint32_t)addr),
          "d"((uint32_t)(addr >> 32)));

    return 0;
}

static uint64_t sys_exit(uint64_t status,
                         uint64_t arg2,
                         uint64_t arg3)
{
    (void)arg2;
    (void)arg3;

    // kprintf("[SYS_EXIT] status=%llu\n", status);

    struct proc *p = my_cpu();

    if (!p)
        for (;;)
            __asm__ volatile("hlt");

    proc_set_zombie(p);

    sched_remove(p);

    schedule();

    __builtin_unreachable();
}

extern void syscall_return(void);

static uint64_t sys_fork(struct syscall_frame *f)
{
    struct proc *parent = my_cpu();
    if (!parent) return (uint64_t)-1;

    struct proc *child = proc_alloc(parent);
    if (!child) return (uint64_t)-1;

    // Clone memory space
    child->mm = vm_space_clone(parent->mm);
    if (!child->mm) {
        proc_free(child);
        return (uint64_t)-1;
    }
    child->cr3 = child->mm->cr3;
    child->fs_base = parent->fs_base;

    // Copy the exact syscall frame to the child's kernel stack
    struct syscall_frame *child_f = (struct syscall_frame *)(child->kernel_stack_top - sizeof(struct syscall_frame));
    memcpy(child_f, f, sizeof(struct syscall_frame));

    // The child should return 0 from fork()
    child_f->rax = 0;

    // Set up child's context to return via syscall_return
    extern void syscall_return(void);
    child->context.rip = (uint64_t)syscall_return;
    child->context.rsp = (uint64_t)child_f;

    child->user_rsp = child_f->user_rsp;

    child->state = PROC_RUNNABLE;
    sched_add(child);

    f->rax = child->pid;
    return child->pid;
}

// ----------------------------------------------------------------
// Dispatcher
// ----------------------------------------------------------------

uint64_t syscall_dispatch(struct syscall_frame *f)
{
    uint64_t nr = f->rax;
    uint64_t arg1 = f->rdi;
    uint64_t arg2 = f->rsi;
    uint64_t arg3 = f->rdx;
    const char *name =
        (nr < SYSCALL_COUNT && syscall_names[nr])
            ? syscall_names[nr]
            : "unknown";

    // kprintf("[SYSCALL #%d] %s\n", (int)nr, name);
    // kprintf("  arg1=0x%lx (%ld)\n", arg1, (int64_t)arg1);
    // kprintf("  arg2=0x%lx (%ld)\n", arg2, (int64_t)arg2);
    // kprintf("  arg3=0x%lx (%ld)\n", arg3, (int64_t)arg3);

    if (nr == SYS_FORK) {
        return sys_fork(f);
    }

    if (nr >= SYSCALL_COUNT || !syscall_table[nr]) {
        kprintf("  -> INVALID SYSCALL\n");
        return (uint64_t)-1;
    }

    uint64_t ret = syscall_table[nr](arg1, arg2, arg3);

    f->rax = ret;

    // kprintf("  -> ret=0x%lx (%ld)\n", ret, (int64_t)ret);
    return ret;
}
