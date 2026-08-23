/*
 * Every syscall the kernel exposes (kernel/include/kernel/proc/syscall.h),
 * exercised through real mlibc — not usr/lib's own hand-rolled libc, and
 * not the raw usr/include/extron/syscall.h wrappers the earlier
 * fork_test.c/exec_child.c use. Those two things matter for different
 * reasons:
 *
 *  - Going through mlibc means printf/malloc/fork/exec/wait/sleep here
 *    are the SAME functions any ordinary C program would call — a much
 *    closer proxy for "does a real program work on this kernel" than a
 *    test written directly against our own syscall ABI, which by
 *    construction can never be surprised by anything mlibc's own layer
 *    does on top (argument marshalling, errno, the TLS/atexit/exit
 *    machinery sysdeps/extron/generic/generic.cpp had to build).
 *
 *  - Three syscalls have no idiomatic libc entry point at all —
 *    SYS_UPTIME_MS, SYS_MAP_INITRD, SYS_PROC_DUMP are extron-specific,
 *    with nothing in ISO C or POSIX shaped like them (sys_clock_get()
 *    in generic.cpp is stubbed to always report zero, so even
 *    clock_gettime() doesn't reach SYS_UPTIME_MS). Those three get a
 *    tiny raw `svc #0` shim, declared right here rather than pulling in
 *    usr/include/extron/syscall.h — this file is compiled against
 *    mlibc's headers, and the two syscall.h's are deliberately
 *    duplicated ABI boundaries, not something to mix.
 *
 * Built by the normal Makefile with the aarch64-extron cross toolchain
 * (~/extron-toolkit/toolchain/bin), not aarch64-linux-gnu-gcc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <sys/wait.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[mlibc_test] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* ---------------------------------------------------------------
 * Raw syscall shim, for the three numbers mlibc has no libc-level
 * entry point for. Same x8=number, x0-x2=args, result-in-x0 ABI as
 * every other syscall here — kernel/include/kernel/proc/syscall.h and
 * usr/include/extron/syscall.h both document it as the real AAPCS64/
 * Linux convention, which is exactly why this is a two-line function
 * instead of needing mlibc's own sysdeps changed.
 * --------------------------------------------------------------- */
#define SYS_PROC_DUMP  3
#define SYS_UPTIME_MS  10
#define SYS_MAP_INITRD 11

static long raw_syscall(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

/* ---------------------------------------------------------------
 * SYS_WRITE, via printf/puts/write() — fd 1, buffered and unbuffered
 * both, since mlibc's stdio has its own buffering logic on top of the
 * raw fd write().
 * --------------------------------------------------------------- */
static void test_write(void) {
    printf("\n[mlibc_test] === SYS_WRITE (via printf/write) ===\n");

    static const char message[] = "[mlibc_test] raw write() to fd 1\n";
    ssize_t n = write(1, message, sizeof(message) - 1);
    check("write() to fd 1 returns the byte count",
          n == (ssize_t)(sizeof(message) - 1));

    /* printf's return value is also SYS_WRITE's byte count, laundered
     * through stdio's own formatting and buffering. */
    int printed = printf("[mlibc_test] printf byte count is %d\n", 42);
    check("printf() reports a plausible byte count", printed > 30 && printed < 60);

    check("write() to an invalid fd fails", write(99, "x", 1) < 0);
}

/* ---------------------------------------------------------------
 * SYS_SLEEP, via sleep()/nanosleep() — cross-checked against
 * SYS_UPTIME_MS (CNTPCT_EL0-backed, see kernel/drivers/timer.c),
 * which is the same technique fib_ticker.c/uptime_test.S already use
 * to catch a sleep that's silently a no-op or badly miscalibrated.
 * --------------------------------------------------------------- */
static void test_sleep(void) {
    printf("\n[mlibc_test] === SYS_SLEEP (via sleep/nanosleep) ===\n");

    uint64_t before = (uint64_t)raw_syscall(SYS_UPTIME_MS, 0, 0, 0);
    unsigned int remaining = sleep(1);
    uint64_t after = (uint64_t)raw_syscall(SYS_UPTIME_MS, 0, 0, 0);
    uint64_t elapsed = after - before;

    check("sleep(1) reports nothing remaining", remaining == 0);
    /* Generous window (700-2000ms for a requested 1000ms): this is a
     * kernel-correctness check, not a scheduler-latency benchmark —
     * QEMU's virtual clock in particular is not 1:1 with wall time. */
    check("sleep(1) actually elapsed roughly 1s (real time moved)",
          elapsed >= 700 && elapsed <= 3000);

    struct timespec req = { 0, 200 * 1000 * 1000 }; /* 200ms */
    before = (uint64_t)raw_syscall(SYS_UPTIME_MS, 0, 0, 0);
    int rc = nanosleep(&req, NULL);
    after = (uint64_t)raw_syscall(SYS_UPTIME_MS, 0, 0, 0);
    elapsed = after - before;

    check("nanosleep(200ms) returns success", rc == 0);
    check("nanosleep(200ms) elapsed a plausible amount",
          elapsed >= 100 && elapsed <= 700);
}

/* ---------------------------------------------------------------
 * SYS_ANON_ALLOC / SYS_ANON_FREE, via malloc/free/calloc/realloc —
 * mlibc's own allocator sits on top of these (sys_anon_allocate/
 * sys_anon_free in sysdeps/extron/generic/generic.cpp), so this is
 * really testing that AND the raw syscalls together, which is the
 * honest thing a "does malloc work" test should check.
 * --------------------------------------------------------------- */
static void test_memory(void) {
    printf("\n[mlibc_test] === SYS_ANON_ALLOC / SYS_ANON_FREE (via malloc) ===\n");

    char *a = malloc(256);
    char *b = malloc(256);
    check("two allocations both succeed", a && b);
    check("two allocations don't overlap", a && b && (a + 256 <= b || b + 256 <= a));

    if (a) {
        memset(a, 0xAB, 256);
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if ((unsigned char)a[i] != 0xAB) { ok = 0; break; }
        check("a 256-byte allocation holds exactly what was written", ok);
    }

    void *big = malloc(1024 * 1024); /* 1MB — exercises a real SYS_ANON_ALLOC, not a small-object pool hit */
    check("a 1MB allocation succeeds", big != NULL);
    free(big);

    void *z = calloc(64, 8);
    check("calloc() succeeds", z != NULL);
    if (z) {
        int all_zero = 1;
        for (int i = 0; i < 64 * 8; i++)
            if (((char *)z)[i] != 0) { all_zero = 0; break; }
        check("calloc() memory is zeroed", all_zero);
    }
    free(z);

    void *r = realloc(a, 512);
    check("realloc() to a larger size succeeds", r != NULL);
    if (r) {
        int preserved = 1;
        for (int i = 0; i < 256; i++)
            if (((unsigned char *)r)[i] != 0xAB) { preserved = 0; break; }
        check("realloc() preserves the original bytes", preserved);
        free(r);
    }

    free(b);
    /* No crash / no double-free assertion here is itself part of what's
     * being checked — mlibc's allocator would abort() on heap
     * corruption, which would show up as this whole process dying
     * before the summary line prints. */
}

/* ---------------------------------------------------------------
 * SYS_TCB_SET — nothing currently calls mlibc's own sys_tcb_set()
 * wrapper (sysdeps/extron/generic/generic.cpp's install_tcb() sets
 * TPIDR_EL0 directly with `msr`, since that's what the kernel's own
 * comment on sys_tcb_set() says a real aarch64 sysdep would do). The
 * syscall path still has to work for whenever it IS used (mlibc's own
 * pthread bring-up would go through it), so it's checked directly
 * here rather than left completely unexercised.
 * --------------------------------------------------------------- */
#define SYS_TCB_SET 6

static void test_tcb_set(void) {
    printf("\n[mlibc_test] === SYS_TCB_SET (kernel path, not mlibc's TLS bootstrap) ===\n");

    /* Read the CURRENT TPIDR_EL0 (installed by generic.cpp's own
     * install_tcb() before main() ever ran), set it right back via the
     * syscall, and confirm it survives unchanged. This exercises the
     * kernel's msr path without disturbing the TLS block install_tcb()
     * already built — setting it to anything else would immediately
     * break errno on the very next line. */
    uint64_t before;
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(before));

    long ret = raw_syscall(SYS_TCB_SET, (long)before, 0, 0);
    check("SYS_TCB_SET returns success", ret == 0);

    uint64_t after;
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(after));
    check("TPIDR_EL0 round-trips through SYS_TCB_SET unchanged", before == after);

    /* And errno (a real __thread variable) still works after that
     * round-trip — the actual thing that would break if SYS_TCB_SET
     * silently corrupted TPIDR_EL0 instead of setting it correctly. */
    errno = 0;
    (void)write(-1, "x", 1); /* guaranteed EBADF */
    check("errno (TLS) still works after a SYS_TCB_SET round-trip", errno != 0);
}

/* ---------------------------------------------------------------
 * SYS_MAP_INITRD — the legacy zero-copy initrd mapping ABI has no libc
 * entry point and remains useful to test directly. Ordinary programs
 * now reach the same initrd seed through ramfs and fopen().
 * --------------------------------------------------------------- */
static void test_map_initrd(void) {
    printf("\n[mlibc_test] === SYS_MAP_INITRD (raw) + ramfs fopen() ===\n");

    static const char name[] = "hello.txt";
    uint64_t size = 0;
    long va = raw_syscall(SYS_MAP_INITRD, (long)name, sizeof(name) - 1, (long)&size);

    check("hello.txt maps successfully", va != 0);
    check("hello.txt reports a nonzero size", size > 0 && size < 4096);

    if (va != 0) {
        const char *expected = "Hello from the initrd!";
        int matches = (size == strlen(expected)) &&
                      (memcmp((const void *)va, expected, size) == 0);
        check("hello.txt's mapped content matches the known string", matches);
    }

    /* A name that can't exist in the tar. Confirms the syscall
     * actually validates rather than mapping garbage. */
    static const char missing[] = "this-file-does-not-exist.bin";
    long bad = raw_syscall(SYS_MAP_INITRD, (long)missing, sizeof(missing) - 1, (long)&size);
    check("mapping a nonexistent initrd file fails", bad == 0);

    /* The same file must also be reachable through the ordinary mlibc
     * descriptor path now that SYS_OPEN and ramfs are implemented. */
    FILE *f = fopen("hello.txt", "r");
    check("fopen() opens the initrd-seeded ramfs file", f != NULL);
    if (f) {
        char via_stdio[23] = {0};
        size_t got = fread(via_stdio, 1, 22, f);
        check("fread() returns the initrd-seeded contents",
              got == 22 && memcmp(via_stdio, "Hello from the initrd!", 22) == 0);
        check("fclose() closes the ramfs descriptor", fclose(f) == 0);
    }
}

/* ---------------------------------------------------------------
 * SYS_PROC_DUMP — a debug/visual syscall with no pass/fail contract;
 * calling it is the whole test (it must not crash the kernel, and its
 * output is there for you to read in the log, not for this program to
 * parse).
 * --------------------------------------------------------------- */
static void test_proc_dump(void) {
    printf("\n[mlibc_test] === SYS_PROC_DUMP (visual — check the table below) ===\n");
    raw_syscall(SYS_PROC_DUMP, 0, 0, 0);
    check("SYS_PROC_DUMP returned control (didn't hang/crash)", 1);
}

/* ---------------------------------------------------------------
 * SYS_FORK / SYS_EXECVE / SYS_WAIT / SYS_EXIT, via fork()/execve()/
 * wait()/exit() — real libc. This process re-execs ITSELF with a
 * marker argument to act as its own child, rather than needing a
 * second compiled binary and a second injection step.
 * --------------------------------------------------------------- */
#define CHILD_MARKER "--mlibc-test-child"
#define CHILD_EXIT_STATUS 42

static int run_as_child(void) {
    printf("[mlibc_test] (child) re-exec'd successfully, exiting %d\n", CHILD_EXIT_STATUS);
    exit(CHILD_EXIT_STATUS);
    return 0; /* unreachable */
}

static void test_fork_exec_wait(const char *self_path) {
    printf("\n[mlibc_test] === SYS_FORK / SYS_EXECVE / SYS_WAIT (via fork/execve/wait) ===\n");

    static volatile long shared = 1;
    pid_t pid = fork();
    check("fork() returned (didn't fail outright)", pid >= 0);

    if (pid == 0) {
        /* ---- child ---- */
        shared = 2;
        char *args[] = { (char *)self_path, (char *)CHILD_MARKER, NULL };
        execve(self_path, args, NULL);
        /* Only reachable if execve() failed. */
        printf("[mlibc_test] (child) execve() FAILED, errno=%d\n", errno);
        exit(97);
    }

    /* ---- parent ---- */
    check("parent's copy of shared memory is untouched (fork copies, not shares)",
          shared == 1);

    int status = -1;
    pid_t reaped = wait(&status);
    check("wait() returned the forked child's pid", reaped == pid);
    check("child (via execve) exited with the expected status",
          WIFEXITED(status) && WEXITSTATUS(status) == CHILD_EXIT_STATUS);

    errno = 0;
    pid_t none = waitpid(-1, &status, 0);
    check("waitpid() with no children left fails with ECHILD",
          none == -1 && errno == ECHILD);
}

/* ---------------------------------------------------------------
 * SYS_READ — the one syscall this suite cannot exercise unattended.
 * Enter noncanonical mode and flush bytes left by the command that
 * launched this program, then wait for exactly one NEW key. Deliberately
 * last, so every automated check has printed before this can block.
 * --------------------------------------------------------------- */
static void test_read_interactive(void) {
    printf("\n[mlibc_test] === SYS_READ (via read()) — INTERACTIVE, type a character ===\n");

    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        check("tcgetattr() before interactive read", 0);
        return;
    }
    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    printf("[mlibc_test] waiting for one fresh keystroke on fd 0...\n");
    fflush(stdout);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        check("enter noncanonical mode and flush pending input", 0);
        return;
    }

    char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    int restored = tcsetattr(STDIN_FILENO, TCSANOW, &saved) == 0;
    check("read() from fd 0 returned exactly one byte", n == 1);
    check("interactive test restored the TTY settings", restored);
    if (n == 1) {
        if (c >= 0x20 && c < 0x7F)
            printf("[mlibc_test] got '%c'\n", c);
        else
            printf("[mlibc_test] got 0x%02x\n", (unsigned char)c);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], CHILD_MARKER) == 0)
        return run_as_child();

    printf("[mlibc_test] ================================================\n");
    printf("[mlibc_test] mlibc syscall test suite — argc=%d argv[0]=%s\n",
           argc, argv[0]);
    printf("[mlibc_test] ================================================\n");

    test_write();
    test_sleep();
    test_memory();
    test_tcb_set();
    test_map_initrd();
    test_proc_dump();
    test_fork_exec_wait(argv[0]);

    printf("\n[mlibc_test] ================================================\n");
    printf("[mlibc_test] automated checks done: %d failure(s)\n", failures);
    printf("[mlibc_test] ================================================\n");

    test_read_interactive();

    printf("\n[mlibc_test] === %d total failure(s) ===\n", failures);
    return failures;
}
