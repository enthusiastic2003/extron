/*
 * x86-64 bring-up payload: a freestanding (no libc) static program that
 * exercises the kernel's process model through raw syscalls — the x86
 * counterpart of the aarch64 usr/mlibc_tests/ smoke tests, usable before a
 * full mlibc port exists. Installed as /bin/sh so kernel_stage2() launches
 * it; it re-execs itself ("/bin/sh child-exec") to test execve.
 *
 * Syscall ABI: rax = number, rdi/rsi/rdx = args, result in rax; numbers are
 * the kernel's own (usr/include/extron/syscall.h).
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_WRITE 1
#define SYS_SLEEP 2
#define SYS_EXIT 7
#define SYS_FORK 8
#define SYS_EXECVE 9
#define SYS_WAIT 12
#define SYS_GETPID 17
#define SYS_PIPE 25
#define SYS_READ 0
#define SYS_CLOSE 14
#define SYS_SIGACTION 35
#define SYS_KILL 36
#define SYS_SIGRETURN 38
#define SYS_CLOCK_GET 61
#define SYS_MMAP 69
#define SYS_MUNMAP 70
#define SYS_REBOOT 71
#define SYS_TCB_SET 6
#define SYS_OPEN 13

static inline long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, slen(s)); }

static void outnum(long v) {
    char buf[24]; int i = 23; buf[i] = 0;
    int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { buf[--i] = '0' + u % 10; u /= 10; } while (u);
    if (neg) buf[--i] = '-';
    out(buf + i);
}

static int failures;
static void check(const char *name, int ok) {
    out(ok ? "  [PASS] " : "  [FAIL] "); out(name); out("\n");
    if (!ok) failures++;
}

__attribute__((noreturn)) static void do_exit(long code) {
    for (;;) sc(SYS_EXIT, code, 0, 0);
}

/* ---- signals ---- */
struct ksigaction { unsigned long handler, flags, restorer, mask[16]; };
static volatile long sig_seen, sig_arg_ok;
static void sig_handler(int signo) {
    sig_seen = signo;
    /* clobber callee-visible registers to prove sigreturn restores them */
    __asm__ volatile ("mov $0x1111, %%rbx; mov $0x2222, %%r12; mov $0x3333, %%r13" ::: "rbx", "r12", "r13");
}
__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile ("mov $38, %eax; syscall");
}

/* ---- FP state ---- */
static double fp_work(double seed) {
    double acc = seed;
    for (int i = 1; i <= 1000; i++) acc = acc * 1.0000001 + 1.0 / i;
    return acc;
}

struct mmap_req { unsigned long hint, size; long prot, flags, fd, offset; };

static int run_child_exec(void) {
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        const char *argv[] = { "/bin/sh", "child-exec", 0 };
        const char *envp[] = { "FOO=bar", 0 };
        sc(SYS_EXECVE, (long)"/bin/sh", (long)argv, (long)envp);
        do_exit(99);
    }
    int status = -1;
    long w = sc(SYS_WAIT, (long)&status, -1, 0);
    return (w == pid && status == 42) ? 1 : (outnum(status), 0);
}

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void mini_shell(void) {
    char line[128];
    out("\nExtron mini shell on the console (type 'help').\n");
    for (;;) {
        out("\x1b[1;32mextron\x1b[0m:\x1b[1;34m/\x1b[0m# ");
        long n = sc(SYS_READ, 0, (long)line, sizeof(line) - 1);
        if (n <= 0) continue;
        if (line[n - 1] == '\n') n--;
        line[n] = 0;
        if (streq(line, "help")) out("commands: help colors fill echo <text> uname reboot\n");
        else if (streq(line, "colors")) {
            for (int bg = 40; bg < 48; bg++) {
                for (int fg = 30; fg < 38; fg++) {
                    out("\x1b["); outnum(bg); out(";"); outnum(fg); out("m Aa ");
                }
                out("\x1b[0m\n");
            }
            out("\x1b[1mbold\x1b[0m \x1b[7mreverse\x1b[0m \x1b[91mbright red\x1b[0m\n");
        } else if (streq(line, "fill")) {
            for (int i = 1; i <= 150; i++) { out("line "); outnum(i); out(": the quick brown fox jumps over the lazy dog\n"); }
        } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' && line[3] == 'o') { out(line + (n > 4 ? 5 : 4)); out("\n"); }
        else if (streq(line, "uname")) out("Extron x86_64 (UEFI)\n");
        else if (streq(line, "reboot") || streq(line, "exit")) { sc(SYS_REBOOT, 0, 0, 0); }
        else if (line[0]) {
            /* Not built in: run /bin/<word> with the remaining words as args. */
            char path[64] = "/bin/"; int pl = 5, i = 0;
            while (line[i] && line[i] != ' ' && pl < 62) path[pl++] = line[i++];
            path[pl] = 0;
            char *argv[16]; int ac = 0; argv[ac++] = path;
            while (line[i] && ac < 15) {
                while (line[i] == ' ') line[i++] = 0;
                if (line[i]) { argv[ac++] = &line[i]; while (line[i] && line[i] != ' ') i++; }
            }
            argv[ac] = 0;
            const char *envp[] = { "PATH=/bin", "HOME=/root", "TERM=vt100", 0 };
            long cpid = sc(SYS_FORK, 0, 0, 0);
            if (cpid == 0) {
                sc(SYS_EXECVE, (long)path, (long)argv, (long)envp);
                out("sh: cannot execute "); out(path); out("\n");
                do_exit(127);
            }
            int st = 0;
            sc(SYS_WAIT, (long)&st, -1, 0);
            if (st) { out("[exit status "); outnum(st); out("]\n"); }
        }
    }
}

static void run_tests(void) {
    out("\n=== Extron x86-64 userland smoke test ===\n");

    long pid = sc(SYS_GETPID, 0, 0, 0);
    check("getpid > 0", pid > 0);

    /* clock */
    unsigned long t0 = sc(SYS_CLOCK_GET, 1, 0, 0);
    sc(SYS_SLEEP, 0, 50000000, 0);                /* 50 ms */
    unsigned long t1 = sc(SYS_CLOCK_GET, 1, 0, 0);
    out("  slept ~"); outnum((t1 - t0) / 1000000); out(" ms\n");
    check("sleep(50ms) took 45..200 ms", (t1 - t0) >= 45000000UL && (t1 - t0) < 200000000UL);
    unsigned long wall = sc(SYS_CLOCK_GET, 0, 0, 0);
    check("realtime clock is after 2024", wall > 1704067200UL * 1000000000UL);

    /* floating point + SSE state */
    double r = fp_work(1.5);
    check("floating point computes", r > 8.9 && r < 9.1);

    /* mmap anonymous */
    struct mmap_req rq = { 0, 1 << 20, 3, 0x22, -1, 0 };
    unsigned char *m = (unsigned char *)sc(SYS_MMAP, (long)&rq, 0, 0);
    int mm_ok = ((long)m > 0);
    if (mm_ok) { for (int i = 0; i < (1 << 20); i += 4096) m[i] = (unsigned char)i; mm_ok = m[8192] == 0; }
    check("mmap 1 MiB anon + touch", mm_ok);

    /* fork + wait + exit status */
    long cpid = sc(SYS_FORK, 0, 0, 0);
    if (cpid == 0) {
        double c = fp_work(1.5);               /* child must see its own FP state */
        do_exit(c > 8.9 && c < 9.1 ? 7 : 8);
    }
    int st = -1;
    long w = sc(SYS_WAIT, (long)&st, -1, 0);
    check("fork/wait returns child pid", w == cpid);
    check("child exit status == 7", st == 7);

    /* pipe across fork */
    int fds[2];
    sc(SYS_PIPE, (long)fds, 0, 0);
    long ppid = sc(SYS_FORK, 0, 0, 0);
    if (ppid == 0) {
        sc(SYS_CLOSE, fds[0], 0, 0);
        const char *msg = "hello-through-pipe";
        sc(SYS_WRITE, fds[1], (long)msg, slen(msg));
        do_exit(0);
    }
    sc(SYS_CLOSE, fds[1], 0, 0);
    char buf[64] = {0};
    long n = sc(SYS_READ, fds[0], (long)buf, 63);
    sc(SYS_WAIT, 0, -1, 0);
    int same = n == 18; const char *exp = "hello-through-pipe";
    for (int i = 0; same && i < 18; i++) same = buf[i] == exp[i];
    check("pipe parent<-child", same);

    /* signals with register preservation */
    struct ksigaction sa = { (unsigned long)sig_handler, 0x04000000UL, (unsigned long)sig_restorer, {0} };
    long sr = sc(SYS_SIGACTION, 10, (long)&sa, 0);
    register long keep_b __asm__("rbx") = 0xAAAA;
    register long keep_12 __asm__("r12") = 0xBBBB;
    sc(SYS_KILL, pid, 10, 0);
    long kb = keep_b, k12 = keep_12;
    check("sigaction ok", sr == 0);
    check("signal handler ran (SIGUSR1)", sig_seen == 10);
    check("callee-saved regs survive sigreturn", kb == 0xAAAA && k12 == 0xBBBB);

    /* fault -> SIGSEGV kills child, parent sees signal status */
    long fpid = sc(SYS_FORK, 0, 0, 0);
    if (fpid == 0) { *(volatile int *)0 = 1; do_exit(0); }
    st = 0;
    sc(SYS_WAIT, (long)&st, -1, 0);
    out("  segv child wait status = "); outnum(st); out("\n");
    check("null deref kills child with a signal", st != 0);

    /* preemption + FP/SSE isolation: a child spins in user mode scribbling on
     * XMM registers while the parent does FP math; only timer-IRQ preemption
     * lets the parent run at all, and any FXSAVE/FXRSTOR mistake corrupts it. */
    double ref = fp_work(2.25);
    long spid = sc(SYS_FORK, 0, 0, 0);
    if (spid == 0) {
        for (;;) {
            __asm__ volatile (
                "movq $0x7ff8dead00000000, %%rax\n\t"
                "movq %%rax, %%xmm0\n\tmovq %%rax, %%xmm1\n\tmovq %%rax, %%xmm2\n\t"
                "movq %%rax, %%xmm3\n\tmovq %%rax, %%xmm7\n\t"
                ::: "rax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm7");
        }
    }
    unsigned long f0 = sc(SYS_CLOCK_GET, 1, 0, 0);
    int fp_ok = 1, rounds = 0;
    while (sc(SYS_CLOCK_GET, 1, 0, 0) - f0 < 120000000UL) {
        if (fp_work(2.25) != ref) fp_ok = 0;
        rounds++;
    }
    sc(SYS_SLEEP, 0, 20000000, 0);                 /* parent sleeps; child must be preempted for us to wake */
    sc(SYS_KILL, spid, 9, 0);
    st = 0;
    sc(SYS_WAIT, (long)&st, -1, 0);
    out("  fp rounds under preemption: "); outnum(rounds); out(", spinner status "); outnum(st); out("\n");
    check("FP results exact while a spinner runs (FXSAVE per switch)", fp_ok && rounds > 10);
    check("spinner preempted, then SIGKILLed (status -9)", st == -9);

    /* execve */
    check("fork+execve+wait (child exits 42)", run_child_exec());

    out("\n=== "); outnum(failures); out(" failure(s) ===\n");
    out(failures ? "SMOKE TEST FAILED\n" : "SMOKE TEST PASSED\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc > 1 && argv[1][0] == 'c') {          /* "child-exec" */
        out("  [child] exec'd image running, FOO=");
        char **e = envp; while (*e) { if ((*e)[0]=='F') out(*e + 4); e++; }
        out("\n");
        do_exit(42);
    }
    run_tests();
    /* CI images carry /etc/autoexit: reset the machine (QEMU -no-reboot turns
     * that into an exit). Otherwise fall into the tiny interactive shell. */
    long fd = sc(SYS_OPEN, (long)"/etc/autoexit", 0, 0);
    if (fd >= 0) {
        out("rebooting (QEMU -no-reboot turns this into an exit)\n");
        sc(SYS_REBOOT, 0, 0, 0);
        do_exit(0);
    }
    mini_shell();
    do_exit(0);
}

void c_start(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    do_exit(main(argc, argv, envp));
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile ("xor %ebp, %ebp\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall c_start");
}
