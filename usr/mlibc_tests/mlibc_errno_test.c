/*
 * Regression coverage for a class of bug every other test in this suite
 * was blind to: a syscall reporting the WRONG errno for a real failure,
 * as opposed to failing outright. kernel/proc/syscall.c used to return a
 * bare -1 for most of these (which happens to equal -EPERM, since EPERM
 * is 1 in kernel/include/kernel/errno.h) or generic.cpp guessed one fixed
 * errno for every failure a given syscall could have, discarding whatever
 * real reason the kernel actually had. Every check below deliberately
 * triggers one specific failure and asserts the SPECIFIC errno POSIX
 * expects for it, not just "the call failed".
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

/* SYS_FUTEX_WAIT/WAKE, via the same raw-syscall shim mlibc_mem_stress.c
 * and mlibc_fork_stress.c already established: usr/include/extron/
 * syscall.h isn't on the mlibc sysroot's include path (it's for the
 * retired raw-syscall .S/.c binaries, compiled a different way), so an
 * mlibc-linked test can't just #include it. */
#define SYS_FUTEX_WAIT 33
#define SYS_FUTEX_WAKE 34

static long raw_syscall(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[errno_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void test_sigaction_sigprocmask(void) {
    /* 0x1000 (below the ELF's own load base, always around 0x400000
     * per the LOADER's own boot log) is unmapped, but > SIGNAL_IGN
     * (kernel/include/kernel/proc/signal.h defines that as 1) — a
     * handler of exactly 1 IS a legal SIG_IGN, not a bad pointer at
     * all, which the first version of this check got wrong. */
    struct sigaction act = {0};
    act.sa_handler = (void (*)(int))0x1000;
    errno = 0;
    check("sigaction() with an unmapped handler reports EFAULT",
          sigaction(SIGUSR1, &act, NULL) == -1 && errno == EFAULT);

    sigset_t set;
    sigemptyset(&set);
    errno = 0;
    check("sigprocmask() with an invalid 'how' reports EINVAL",
          sigprocmask(999, &set, NULL) == -1 && errno == EINVAL);

    errno = 0;
    check("sigprocmask() with an unmapped set pointer reports EFAULT",
          sigprocmask(SIG_BLOCK, (sigset_t *)1, NULL) == -1
          && errno == EFAULT);
}

static void test_ioctl(void) {
    errno = 0;
    check("ioctl() with an unrecognized tty request reports ENOTTY",
          ioctl(STDIN_FILENO, 0x99999999UL, NULL) == -1 && errno == ENOTTY);

    int fd = open("/opt/tests/hello.txt", O_RDONLY);
    check("open a plain file for the non-tty ioctl check", fd >= 0);
    struct termios term;
    errno = 0;
    check("tcgetattr() on a non-tty fd reports ENOTTY",
          tcgetattr(fd, &term) == -1 && errno == ENOTTY);
    close(fd);

    int bogus_pgid = 999999;
    errno = 0;
    check("ioctl(TIOCSPGRP) to a nonexistent group reports EPERM",
          ioctl(STDIN_FILENO, TIOCSPGRP, &bogus_pgid) == -1
          && errno == EPERM);
}

static void test_pipe(void) {
    errno = 0;
    check("pipe() with an unmapped fd array reports EFAULT",
          pipe((int *)1) == -1 && errno == EFAULT);
}

static void test_dup_family(void) {
    errno = 0;
    check("dup2() with an invalid oldfd reports EBADF",
          dup2(999, 5) == -1 && errno == EBADF);

    errno = 0;
    check("dup2() with an out-of-range newfd reports EBADF",
          dup2(STDOUT_FILENO, 99999) == -1 && errno == EBADF);

    /* PROC_MAX_FDS is 32 (kernel/include/kernel/fs/file.h); opening well
     * past that guarantees the table is genuinely full, not just low. */
    int extra[40];
    int count = 0;
    while (count < 40) {
        int fd = open("/opt/tests/hello.txt", O_RDONLY);
        if (fd < 0) break;
        extra[count++] = fd;
    }
    errno = 0;
    check("dup() reports EMFILE once the descriptor table is full",
          dup(STDIN_FILENO) == -1 && errno == EMFILE);
    errno = 0;
    check("fcntl(F_DUPFD) reports EMFILE once the descriptor table is full",
          fcntl(STDIN_FILENO, F_DUPFD, 0) == -1 && errno == EMFILE);
    for (int i = 0; i < count; i++) close(extra[i]);
}

static void test_process_group(void) {
    errno = 0;
    check("getpgid() on a nonexistent pid reports ESRCH",
          getpgid(999999) == -1 && errno == ESRCH);

    errno = 0;
    check("setpgid() on a nonexistent pid reports ESRCH",
          setpgid(999999, 0) == -1 && errno == ESRCH);

    /* getppid() is the shell: neither us nor our child, so this must be
     * refused as a permission violation, not "no such process". */
    errno = 0;
    check("setpgid() targeting an unrelated process reports EPERM",
          setpgid(getppid(), 0) == -1 && errno == EPERM);

    pid_t pid = fork();
    check("fork() before the setsid() check", pid >= 0);
    if (pid == 0) {
        setpgid(0, 0); /* become a process group leader of a new group */
        errno = 0;
        _exit(setsid() == -1 && errno == EPERM ? 0 : 1);
    }
    int status = -1;
    pid_t reaped = wait(&status);
    check("wait() returned the child's pid", reaped == pid);
    check("setsid() on an existing group leader reports EPERM",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_futex(void) {
    /* Through the raw syscall directly, not a POSIX entry point:
     * pthread's own mutex/condvar code is the only mlibc-side caller of
     * SYS_FUTEX_WAIT/WAKE, and it never hands futex a misaligned
     * pointer, so there's no POSIX-level way to trigger this specific
     * check. The kernel-side fix is still real — this exercises it
     * directly, the same way mlibc_mem_stress.c reaches SYS_ANON_ALLOC. */
    int word = 0;
    long misaligned = (long)&word + 1;
    check("raw SYS_FUTEX_WAIT on a misaligned pointer reports -EINVAL",
          raw_syscall(SYS_FUTEX_WAIT, misaligned, 0, 1) == -EINVAL);
    check("raw SYS_FUTEX_WAKE on a misaligned pointer reports -EINVAL",
          raw_syscall(SYS_FUTEX_WAKE, misaligned, 0, 0) == -EINVAL);
}

int main(void) {
    printf("\n[errno_test] === specific errno values, not just pass/fail ===\n");

    test_sigaction_sigprocmask();
    test_ioctl();
    test_pipe();
    test_dup_family();
    test_process_group();
    test_futex();

    printf("[errno_test] === %d failure(s) ===\n", failures);
    return failures;
}
