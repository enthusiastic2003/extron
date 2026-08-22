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

void *sys_anon_alloc(size_t size) {
    long r = __syscall1(SYS_ANON_ALLOC, size);
    return r ? (void *)r : NULL;
}

long sys_anon_free(void *addr, size_t size) {
    return __syscall2(SYS_ANON_FREE, addr, size);
}
