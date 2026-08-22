#include <kernel/proc/syscall.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/console.h>
#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/drivers/timer.h>
#include <kernel/drivers/keyboard.h>
#include <stdbool.h>

typedef uint64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t);

/* boot.S sets TCR_EL1.T0SZ=16, so TTBR0 — the calling process's own
 * table — translates VA[0, 2^48). At or above this is either the
 * kernel's TTBR1 half or the non-canonical hole between them. */
#define USER_VA_LIMIT (1ULL << 48)

/*
 * Validate a user-supplied buffer before the kernel dereferences it.
 *
 * Syscalls run at EL1 with TTBR1 live, so a raw user pointer is not
 * merely untrusted, it is *powerful*: without this check a process
 * could pass a kernel address to SYS_WRITE and dump kernel memory to
 * the console, or pass one to SYS_READ and have the kernel write
 * attacker-chosen bytes into its own data structures. Neither is
 * exotic — they're a one-instruction change to any test payload.
 *
 * Two separate things are checked:
 *
 *  - the range lies entirely below USER_VA_LIMIT, and doesn't wrap.
 *    This is what keeps the kernel half unreachable.
 *  - every page is actually mapped in *this* process's table. Without
 *    it, a bad-but-user-range pointer faults at EL1, and a Data Abort
 *    there is a panic (kernel/arch/aarch64/exceptions.c) — i.e. any
 *    process could halt the machine with one bad pointer.
 *
 * Validate-then-use is race-free here only because the whole syscall
 * runs DAIF-masked: nothing can unmap these pages between the check
 * and the access, since nothing else runs at all. That is the same
 * invariant sys_write()'s atomicity rests on — see its comment. If
 * syscalls are ever made preemptible, this becomes a TOCTOU window
 * and has to move to a real copy_from_user()/copy_to_user() that
 * faults safely instead of checking up front.
 */
static bool user_buffer_ok(struct proc *p, uint64_t addr, uint64_t len) {
    if (len == 0)
        return true;
    if (!p || addr >= USER_VA_LIMIT || len > USER_VA_LIMIT - addr)
        return false;

    uint64_t start = addr & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t end   = (addr + len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (!virt_to_phys(p->ttbr0, v))
            return false;
    }
    return true;
}

/* Runs start-to-finish with DAIF masked — the SVC handler masks on
 * entry and nothing in this path unmasks, so the byte loop below cannot
 * be preempted and no second writer can interleave with it. That makes
 * sys_write atomic per call without a lock. The property is incidental
 * rather than designed, and load-bearing: unmask during syscalls (which
 * is worth doing eventually, since a long write currently blocks every
 * interrupt for its duration — at 115200 baud the PL011's 16-byte RX
 * FIFO overruns after ~1.4ms of unserviced input) and this needs a real
 * console lock, and user_buffer_ok() above needs to become a faulting
 * copy_to_user(). Both change together or neither does. */
static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    if (fd != 1 && fd != 2) {
        kprintf("[SYSCALL write] unsupported fd=%lu\n", (unsigned long)fd);
        return (uint64_t)-1;
    }
    if (!user_buffer_ok(my_proc(), buf_addr, count)) {
        kprintf("[SYSCALL write] rejected buffer %p (+%lu) from pid %lu\n",
                (void *)buf_addr, (unsigned long)count,
                my_proc() ? (unsigned long)my_proc()->pid : 0);
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

/* Matches x86 now: exited-but-unreaped processes stay visible in the
 * table as PROC_ZOMBIE rather than vanishing to PROC_UNUSED — no
 * proc_destroy/reaping exists on either tree yet, so nothing currently
 * removes them, but a future wait()/reaper has something real to find.
 * Setting state away from PROC_RUNNING is what actually matters for
 * scheduling: schedule() only re-enqueues `old` if it's still
 * PROC_RUNNING when called, so this is enough to keep the exiting proc
 * out of the rotation permanently without needing a separate
 * sched_policy_remove() — it was never in the run queue while running
 * in the first place. */
static uint64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3) {
    (void)arg2;
    (void)arg3;
    struct proc *p = my_proc();
    kprintf("[SYSCALL exit] pid=%lu status=%lu\n",
            p ? (unsigned long)p->pid : 0, (unsigned long)status);
    if (p) {
        proc_set_zombie(p);
    }
    schedule();
    /* schedule() (kernel/proc/sched.c) only returns here if nothing
     * else was runnable at this exact instant — its own documented
     * "no idle loop yet" case. That's a real, reachable outcome (e.g.
     * the last other proc exits while a third is legitimately
     * PROC_SLEEPING, not yet due to wake) — treating it as
     * __builtin_unreachable() was UB waiting to happen, not actually
     * unreachable. When it happens, this now-ZOMBIE proc's own EL0
     * code (every test payload ends in an infinite `b .`) just spins
     * harmlessly in place until the NEXT timer tick's own, separate
     * schedule() call (timer_irq_handler, kernel/drivers/timer.c) sees
     * this proc is ZOMBIE (not RUNNING) and sweeps it away once
     * something else — e.g. a sleeper waking via
     * proc_wakeup_expired() — becomes runnable. No new mechanism
     * needed: that preemption path already runs on every tick
     * regardless. */
    return status;
}

/* fd 0 only — one byte at a time via the real blocking kbd_getc()
 * (kernel/drivers/keyboard.c), which now sleep()s the calling proc
 * until a byte arrives instead of busy-spinning. A multi-byte-at-once
 * kbd_read() is a straightforward later refinement, not needed for
 * this milestone's scope. */
static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    if (fd != 0) {
        kprintf("[SYSCALL read] unsupported fd=%lu\n", (unsigned long)fd);
        return (uint64_t)-1;
    }
    /* Checked before blocking, not after: kbd_getc() sleeps, and coming
     * back from that only to discover the destination was never valid
     * would mean a keystroke consumed and thrown away. */
    if (!user_buffer_ok(my_proc(), buf_addr, count)) {
        kprintf("[SYSCALL read] rejected buffer %p (+%lu) from pid %lu\n",
                (void *)buf_addr, (unsigned long)count,
                my_proc() ? (unsigned long)my_proc()->pid : 0);
        return (uint64_t)-1;
    }
    char *buf = (char *)buf_addr;
    for (uint64_t i = 0; i < count; i++) {
        buf[i] = kbd_getc();
    }
    return count;
}

/* Ticks-from-time using the actually configured Hz (timer_ticks_per_
 * second()), not a hardcoded frequency baked in twice like x86's own
 * version. Runs fully DAIF-masked (this whole handler is inside the
 * SVC exception path, which masks DAIF on entry same as any other
 * exception) so setting sleep_until/chan/state here needs no lock —
 * proc_wakeup_expired() (kernel/drivers/timer.c's IRQ handler) simply
 * can't run concurrently with this. */
static uint64_t sys_sleep(uint64_t seconds, uint64_t nanos, uint64_t arg3) {
    (void)arg3;
    struct proc *p = my_proc();
    unsigned hz = timer_ticks_per_second();
    uint64_t ticks = seconds * hz + (nanos * hz) / 1000000000ULL;
    if (ticks == 0)
        ticks = 1;
    p->chan = NULL;
    p->sleep_until = timer_ticks() + ticks;
    proc_set_sleeping(p);
    schedule();
    return 0;
}

static uint64_t sys_proc_dump(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    proc_dump_table();
    return 0;
}

static uint64_t sys_anon_alloc(uint64_t size, uint64_t arg2, uint64_t arg3) {
    (void)arg2;
    (void)arg3;
    struct proc *p = my_proc();
    return (uint64_t)vm_allocate_region(p->mm, size, VM_READ | VM_WRITE | VM_USER);
}

static uint64_t sys_anon_free(uint64_t addr, uint64_t size, uint64_t arg3) {
    (void)arg3;
    struct proc *p = my_proc();
    vm_free_region(p->mm, addr, size);
    return 0;
}

static uint64_t sys_not_implemented(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    kprintf("[SYSCALL] not implemented yet\n");
    return (uint64_t)-1;
}

static const syscall_fn syscall_table[] = {
    [SYS_READ]       = sys_read,
    [SYS_WRITE]      = sys_write,
    [SYS_SLEEP]      = sys_sleep,
    [SYS_PROC_DUMP]  = sys_proc_dump,
    [SYS_ANON_ALLOC] = sys_anon_alloc,
    [SYS_ANON_FREE]  = sys_anon_free,
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
