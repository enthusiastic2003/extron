#ifndef EXTRON_SYSCALL_H
#define EXTRON_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* Must match kernel/include/kernel/proc/syscall.h. Duplicated rather
 * than shared because userspace has no business on the kernel's include
 * path — this is the ABI boundary, and writing it out twice is what
 * makes it one. */
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_SLEEP       2
#define SYS_PROC_DUMP   3
#define SYS_ANON_ALLOC  4
#define SYS_ANON_FREE   5
#define SYS_TCB_SET     6
#define SYS_EXIT        7
#define SYS_FORK        8
#define SYS_EXECVE      9
#define SYS_UPTIME_MS   10
#define SYS_MAP_INITRD  11
#define SYS_WAIT        12
#define SYS_OPEN        13
#define SYS_CLOSE       14
#define SYS_LSEEK       15
#define SYS_MKDIR       16
#define SYS_GETPID      17
#define SYS_GETPPID     18
#define SYS_GETCWD      19
#define SYS_CHDIR       20
#define SYS_READDIR     21
#define SYS_STAT        22
#define SYS_IOCTL       23
#define SYS_POLL        24
#define SYS_PIPE        25
#define SYS_DUP         26
#define SYS_DUP2        27
#define SYS_FCNTL       28
#define SYS_GETTID      29
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT 31
#define SYS_THREAD_JOIN 32
#define SYS_FUTEX_WAIT  33
#define SYS_FUTEX_WAKE  34

struct extron_thread_create_args {
    uintptr_t entry;
    uintptr_t user_sp;
    uintptr_t tls;
    uintptr_t exit_word;
};

/* x8 = number, x0-x2 = args, result in x0 — the AAPCS64/Linux
 * convention the kernel's syscall.h documents. "memory" clobber so the
 * compiler can't move loads/stores across a call that may read or write
 * the caller's buffers. */
static inline long __syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2)
                      : "memory", "cc");
    return x0;
}
#define __syscall0(n)          __syscall3((n), 0, 0, 0)
#define __syscall1(n,a)        __syscall3((n), (long)(a), 0, 0)
#define __syscall2(n,a,b)      __syscall3((n), (long)(a), (long)(b), 0)

long     sys_write(int fd, const void *buf, size_t count);
long     sys_read(int fd, void *buf, size_t count);
void     sys_exit(int status) __attribute__((noreturn));
long     sys_sleep(long seconds, long nanos);
uint64_t sys_uptime_ms(void);
void    *sys_anon_alloc(size_t size);
/* Map an initrd file read-only into this process and return a pointer to
 * its first byte, or NULL. *out_size receives the file's length. The
 * mapping is a VIEW of memory the kernel already holds — no copy — which
 * is what lets a multi-megabyte WAD be opened without a stdio layer. */
const void *sys_map_initrd(const char *name, size_t *out_size);
long     sys_anon_free(void *addr, size_t size);

/* Returns 0 in the child and the child's pid in the parent — one call,
 * two returns, because the kernel hands the child a copy of this exact
 * trap frame with x0 zeroed. -1 on failure. */
long     sys_fork(void);
/* Replaces this program. Returns only on failure. envp is accepted for
 * ABI shape and currently ignored: there is no environment yet. */
long     sys_execve(const char *path, char *const argv[], char *const envp[]);
/* Blocks until a child exits, reaps it, returns its pid; -1 when there
 * are no children left. *status gets the exit status unless NULL. */
long     sys_wait(int *status);
long     sys_gettid(void);
long     sys_thread_create(const struct extron_thread_create_args *args);
void     sys_thread_exit(void) __attribute__((noreturn));
long     sys_thread_join(long tid);
long     sys_futex_wait(int *word, int expected, uint64_t timeout_ms);
long     sys_futex_wake(int *word);

#endif
