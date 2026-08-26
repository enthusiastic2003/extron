#ifndef KERNEL_PROC_SYSCALL_H
#define KERNEL_PROC_SYSCALL_H

#include <stdint.h>
#include <arch/exceptions.h>

/*
 * Same numbers as x86's kernel/proc/syscall.h (backup/x86_tree, now
 * ~/extron-x86-backup/) — one shared ABI concept across both trees, not
 * a coincidence. Convention here: x8 = number, x0/x1/x2 = args 1-3,
 * return value written into f->x[0] — the real AAPCS64/Linux syscall
 * convention, not just an Extron-specific choice, which matters for
 * mlibc's own generic aarch64 sysdeps eventually landing on this target.
 */
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_SLEEP       2
#define SYS_PROC_DUMP   3
#define SYS_ANON_ALLOC  4
#define SYS_ANON_FREE   5
#define SYS_TCB_SET     6   /* likely unneeded here — see syscall.c */
#define SYS_EXIT        7
#define SYS_FORK        8
#define SYS_EXECVE      9
/* Beyond x86's shared set — no counterpart there to stay aligned with.
 * Backs doomgeneric's DG_GetTicksMs; see sys_uptime_ms() in syscall.c
 * for why this can't just report tick_count. */
#define SYS_UPTIME_MS   10
#define SYS_MAP_INITRD  11  /* map an initrd file read-only into the caller */
#define SYS_WAIT        12  /* block until a child exits, then reap it */
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
#define SYS_SIGACTION   35
#define SYS_KILL        36
#define SYS_SIGPROCMASK 37
#define SYS_SIGRETURN   38
#define SYS_TGKILL      39
#define SYS_GETPGID     40
#define SYS_SETPGID     41
#define SYS_SETSID      42
#define SYS_UNLINK      43
#define SYS_RMDIR       44
#define SYS_RENAME      45
#define SYS_LINK        46
#define SYS_SYMLINK     47
#define SYS_READLINK    48
#define SYS_PATH_AT     49
#define SYS_GETUID      50
#define SYS_GETEUID     51
#define SYS_GETGID      52
#define SYS_GETEGID     53
#define SYS_SETUID      54
#define SYS_SETEUID     55
#define SYS_SETGID      56
#define SYS_SETEGID     57
#define SYS_GETGROUPS   58
#define SYS_SETGROUPS   59
#define SYS_UMASK       60
#define SYS_CLOCK_GET   61
#define SYS_CLOCK_SET   62
#define SYS_FCHMOD      63
#define SYS_FCHOWN      64
#define SYS_FUTIMENS    65
#define SYS_FCHDIR      66
#define SYS_SETRESUID   67
#define SYS_SETRESGID   68
#define SYS_MMAP        69
#define SYS_MUNMAP      70
#define SYS_REBOOT      71
#define SYS_PAUSE       72
#define SYS_MPROTECT      73

/* Called from exceptions.c's SVC path (ESR_EL1.EC == 0x15). Returns the
 * syscall's result; the caller writes it into f->x[0] and falls through
 * to the normal RESTORE_CONTEXT+eret every other exception already
 * uses — no separate return path needed. */
uint64_t syscall_dispatch(struct aarch64_frame *f);

#endif
