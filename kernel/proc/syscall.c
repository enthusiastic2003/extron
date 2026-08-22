#include <kernel/proc/syscall.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/console.h>

typedef uint64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t);

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    if (fd != 1 && fd != 2) {
        kprintf("[SYSCALL write] unsupported fd=%lu\n", (unsigned long)fd);
        return (uint64_t)-1;
    }
    const char *buf = (const char *)buf_addr;
    for (uint64_t i = 0; i < count; i++) {
        console_putc(buf[i]);
    }
    return count;
}

/* x86 needs this because FS_BASE (its TLS base) historically requires
 * ring-0 help to set. AArch64's equivalent, TPIDR_EL0, is architecturally
 * designed to be directly read/write from EL0 (`msr tpidr_el0, x0`) —
 * mlibc's own generic aarch64 sysdeps almost certainly just do that
 * directly rather than trapping here at all. Kept as a working syscall
 * anyway, for ABI-number symmetry with x86 and as a fallback, even
 * though it may never actually get called. */
static uint64_t sys_tcb_set(uint64_t addr, uint64_t arg2, uint64_t arg3) {
    (void)arg2;
    (void)arg3;
    __asm__ volatile ("msr tpidr_el0, %0" :: "r"(addr) : "memory");
    return 0;
}

/* Minimal: no process table yet (nothing to report exit status to), no
 * zombie state, nothing ever frees this proc's resources — matches
 * "buildable now" scope, not full x86 parity (proc_destroy/proc_free,
 * parent wait, are explicitly deferred future work). Setting state away
 * from PROC_RUNNING is what actually matters here: schedule() only
 * re-enqueues `old` if it's still PROC_RUNNING when called, so this is
 * enough to keep the exiting proc out of the rotation permanently
 * without needing a separate sched_policy_remove() — it was never in
 * the run queue while running in the first place. */
static uint64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3) {
    (void)arg2;
    (void)arg3;
    struct proc *p = my_proc();
    kprintf("[SYSCALL exit] pid=%lu status=%lu\n",
            p ? (unsigned long)p->pid : 0, (unsigned long)status);
    if (p) {
        p->state = PROC_UNUSED;
    }
    schedule();
    /* Only reachable if schedule() found nothing else runnable — this
     * milestone's tests always keep >=1 other proc alive, matching
     * schedule()'s own existing assumption (see its comment on why
     * there's no idle loop yet). A real idle path is future work. */
    __builtin_unreachable();
}

static uint64_t sys_not_implemented(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    kprintf("[SYSCALL] not implemented yet\n");
    return (uint64_t)-1;
}

static const syscall_fn syscall_table[] = {
    [SYS_READ]       = sys_not_implemented, /* needs sleep/wake */
    [SYS_WRITE]      = sys_write,
    [SYS_SLEEP]      = sys_not_implemented, /* needs sleep/wake */
    [SYS_PROC_DUMP]  = sys_not_implemented, /* needs a process table */
    [SYS_ANON_ALLOC] = sys_not_implemented, /* needs a bump allocator */
    [SYS_ANON_FREE]  = sys_not_implemented, /* needs a bump allocator */
    [SYS_TCB_SET]    = sys_tcb_set,
    [SYS_EXIT]       = sys_exit,
    [SYS_FORK]       = sys_not_implemented, /* needs VMA + process table */
    [SYS_EXECVE]     = sys_not_implemented, /* needs argv/envp-aware exec */
};

#define SYSCALL_COUNT (sizeof(syscall_table) / sizeof(syscall_table[0]))

uint64_t syscall_dispatch(struct aarch64_frame *f) {
    uint64_t nr   = f->x[8];
    uint64_t arg1 = f->x[0];
    uint64_t arg2 = f->x[1];
    uint64_t arg3 = f->x[2];

    if (nr >= SYSCALL_COUNT || !syscall_table[nr]) {
        kprintf("[SYSCALL] invalid syscall number %lu\n", (unsigned long)nr);
        return (uint64_t)-1;
    }

    return syscall_table[nr](arg1, arg2, arg3);
}
