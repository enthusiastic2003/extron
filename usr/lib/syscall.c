#include <extron/syscall.h>

long sys_write(int fd, const void *buf, size_t count) {
    return __syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

long sys_read(int fd, void *buf, size_t count) {
    return __syscall3(SYS_READ, fd, (long)buf, (long)count);
}

void sys_exit(int status) {
    __syscall1(SYS_EXIT, status);
    /* The kernel marks us ZOMBIE and schedules away, but sys_exit()
     * does return if nothing else was runnable at that instant — see
     * its comment in kernel/proc/syscall.c. Spin rather than fall off
     * the end of whatever called us. */
    for (;;) { }
}

long sys_sleep(long seconds, long nanos) {
    return __syscall2(SYS_SLEEP, seconds, nanos);
}

uint64_t sys_uptime_ms(void) {
    return (uint64_t)__syscall0(SYS_UPTIME_MS);
}

const void *sys_map_initrd(const char *name, size_t *out_size) {
    size_t n = 0;
    while (name[n]) n++;
    long r = __syscall3(SYS_MAP_INITRD, (long)name, (long)n, (long)out_size);
    return r ? (const void *)r : NULL;
}

void *sys_anon_alloc(size_t size) {
    long r = __syscall1(SYS_ANON_ALLOC, size);
    return r ? (void *)r : NULL;
}

long sys_anon_free(void *addr, size_t size) {
    return __syscall2(SYS_ANON_FREE, addr, size);
}

/*
 * fork() — returns 0 in the child, the child's pid in the parent, -1 on
 * failure. The single call site produces two returns because the kernel
 * duplicates the caller's trap frame and zeroes x0 in the copy; see
 * proc_fork() (kernel/proc/proc_fork.c).
 */
long sys_fork(void) {
    return __syscall0(SYS_FORK);
}

/*
 * execve() — replaces this program. Only returns on failure, which is
 * why there is no success value to check: reaching the next line at all
 * means it didn't work.
 *
 * envp is passed through for ABI shape and currently ignored by the
 * kernel; there is no environment yet.
 */
long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    return __syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

/*
 * wait() — blocks until one child exits, reaps it, and returns its pid.
 * -1 means there are no children left to wait for, which is how a
 * "reap them all" loop terminates. *status receives the exit status if
 * status is non-NULL.
 */
long sys_wait(int *status) {
    return __syscall1(SYS_WAIT, status);
}
