/*
 * Port of the retired usr/fork_test.c (+ usr/exec_child.c), rewritten
 * against real mlibc's fork()/execve()/wait()/waitpid().
 *
 * Six claims, each with a way to be wrong that a casual "it printed
 * something" run would not catch:
 *
 *  1. fork() returns twice, with different values.
 *  2. The child's memory is a COPY, not a share. Both processes write
 *     to the same address and each must still see its own value —
 *     vm_space_clone() sharing a page instead of copying it would look
 *     completely fine until exactly this test.
 *  3. FP/SIMD state is inherited across fork(). Checked by leaving a
 *     value in d8/d20 across the `svc` itself, because that state is
 *     not in the trap frame and not in the parent's saved context — it
 *     is live in the hardware, and only cpu_context_save_fpsimd() puts
 *     it anywhere the child can get it from. This is a DIFFERENT code
 *     path from mlibc_fp_test.c's scheduler-preemption check (that one
 *     exercises context_switch's ordinary save/restore; this one
 *     exercises the standalone FP copy inside proc_fork()) — both have
 *     their own history of being wrong independently, so both keep
 *     their own regression coverage.
 *  4. The child's ELF image and stack really are its own — it runs
 *     code and makes calls after the fork.
 *  5. execve() replaces the program, delivers argv, and the exit status
 *     comes back through wait()/waitpid(). This binary re-execs ITSELF
 *     with a marker argument to act as its own child, the same trick
 *     mlibc_syscall_test.c already established, rather than needing a
 *     separate exec_child.elf and a second injection step.
 *  6. None of it leaks. SYS_PROC_DUMP prints the PMM's free page count;
 *     across repeated fork/exec/wait cycles that number has to stop
 *     moving. Visual only (no pass/fail contract of its own — same as
 *     mlibc_syscall_test.c's test_proc_dump()): read it in the boot
 *     log. It is the check that would fail on a kernel that is
 *     otherwise completely correct, and the entire reason the VMA list
 *     had to start describing the ELF image and the stack rather than
 *     just the heap.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[fork_stress] %-38s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

#define SYS_PROC_DUMP 3

static long raw_syscall0(long n) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile ("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

/*
 * fork(), with known values parked in two FP registers across the
 * syscall, reported back as raw bit patterns.
 *
 * Compared as INTEGERS, on purpose: the first version of this compared
 * two doubles, and GCC parked the expected value in another FP
 * register — so in a child that had lost its FP state, the observed
 * value and the expected value were BOTH zero and the check passed. A
 * test for "did these registers survive" cannot keep its answer key in
 * one of them. Moving the comparison into integer registers puts the
 * answer key somewhere the failure mode can't reach, since
 * general-purpose registers come from the copied trap frame rather
 * than from the FP save.
 */
#define PARKED_D8  0x0123456789ABCDEFULL
#define PARKED_D20 0xFEDCBA9876543210ULL
#define SYS_FORK   8

static long fork_parking_fp(uint64_t *out_d8, uint64_t *out_d20) {
    register long x0 __asm__("x0") = 0;
    register long x8 __asm__("x8") = SYS_FORK;
    uint64_t in8 = PARKED_D8, in20 = PARKED_D20;
    uint64_t back8, back20;

    __asm__ volatile (
        "fmov   d8,  %[i8]\n\t"
        "fmov   d20, %[i20]\n\t"
        "svc    #0\n\t"
        "fmov   %[o8],  d8\n\t"
        "fmov   %[o20], d20\n\t"
        : "+r"(x0), [o8] "=r"(back8), [o20] "=r"(back20)
        : "r"(x8), [i8] "r"(in8), [i20] "r"(in20)
        : "memory", "cc", "d8", "d20"
    );

    *out_d8  = back8;
    *out_d20 = back20;
    return x0;
}

#define CHILD_MARKER "--fork-stress-child"

/*
 * Self-relaunch entry point. argv[2], when present, is the fp-inherit
 * verdict the forking side already decided ("fp-ok"/"fp-lost") — the
 * only place that verdict can still be observed is inside the child,
 * right before it stops existing via execve(), so it is handed through
 * argv and turned into an exit status the parent can fold into its own
 * count. A FAIL printed only here, right before the process disappears,
 * would sit above a "0 failures" summary and be believed by nobody.
 * The 5-cycle leak-check loop always passes "fp-ok" (it doesn't care
 * about FP inheritance, only about wait()/PMM bookkeeping), so its
 * children always exit 7 too.
 */
static int run_as_child(int argc, char **argv) {
    int fp_ok = !(argc >= 3 && strcmp(argv[2], "fp-lost") == 0);
    printf("[fork_stress]   (child) re-exec'd, fp=%s\n", fp_ok ? "ok" : "lost");
    _exit(fp_ok ? 7 : 8);
}

static void run_main_test(const char *self_path) {
    printf("\n[fork_stress] === fork / execve / wait (self-relaunch) ===\n");

    /* --- 1-4: fork, divergence, FP inheritance --- */

    /* Written before the fork, read after it by both sides. The child
     * gets a copy of this page; if it got the page itself, the two
     * writes below would land on the same memory. */
    static volatile long shared_slot = 111;
    char deep[64];
    strcpy(deep, "parent");

    uint64_t d8_after = 0, d20_after = 0;
    long     pid = fork_parking_fp(&d8_after, &d20_after);

    if (pid < 0) {
        printf("[fork_stress] fork failed outright\n");
        failures++;
        return;
    }

    if (pid == 0) {
        /* ---------------- child ---------------- */
        shared_slot = 222;
        strcpy(deep, "child");

        printf("[fork_stress]   child: fork()=0, slot=%ld, buf=\"%s\"\n",
               shared_slot, deep);

        int fp_ok = (d8_after == PARKED_D8 && d20_after == PARKED_D20);
        if (!fp_ok) {
            printf("[fork_stress]   child: FP/SIMD LOST across fork\n");
            printf("[fork_stress]     d8  = 0x%lx (expected 0x%lx)\n",
                   (unsigned long)d8_after, (unsigned long)PARKED_D8);
            printf("[fork_stress]     d20 = 0x%lx (expected 0x%lx)\n",
                   (unsigned long)d20_after, (unsigned long)PARKED_D20);
        }

        /* --- 5: replace this program entirely --- */
        char *args[] = {
            (char *)self_path, (char *)CHILD_MARKER,
            (char *)(fp_ok ? "fp-ok" : "fp-lost"), NULL
        };
        execve(self_path, args, NULL);

        /* Only reachable if execve failed. */
        printf("[fork_stress]   child: execve returned, so it FAILED (errno=%d)\n", errno);
        _exit(99);
    }

    /* ---------------- parent ---------------- */
    check("fork() returned a child pid", pid > 0);
    check("parent's copy of the slot is untouched", shared_slot == 111);
    check("parent's copy of the buffer is untouched", strcmp(deep, "parent") == 0);
    check("FP/SIMD survives the fork syscall (parent)",
          d8_after == PARKED_D8 && d20_after == PARKED_D20);

    int  status = -1;
    pid_t reaped = wait(&status);
    check("wait() returned the child's pid", reaped == pid);
    /* The re-exec'd child exits 7 only when it was told "fp-ok", so this
     * single status covers three things at once: execve loaded and ran
     * a new program, argv reached it intact, and the FP/SIMD state the
     * child inherited across the fork was correct. */
    check("execve ran, argv arrived, child FP inherited",
          WIFEXITED(status) && WEXITSTATUS(status) == 7);

    errno = 0;
    pid_t none = waitpid(-1, &status, 0);
    check("wait() with no children left fails with ECHILD",
          none == -1 && errno == ECHILD);
}

static void run_leak_check(const char *self_path) {
    printf("\n[fork_stress] --- teardown: PMM free pages across cycles ---\n");
    printf("[fork_stress] The count below must STOP CHANGING. A steady drop\n"
           "[fork_stress] means an address space is not being fully freed.\n");

    for (int i = 0; i < 5; i++) {
        printf("[fork_stress] cycle %d:\n", i);
        raw_syscall0(SYS_PROC_DUMP);

        pid_t c = fork();
        if (c == 0) {
            char *args[] = { (char *)self_path, (char *)CHILD_MARKER, (char *)"fp-ok", NULL };
            execve(self_path, args, NULL);
            _exit(98);
        }
        if (c < 0) {
            printf("[fork_stress] cycle %d: fork failed\n", i);
            failures++;
            break;
        }
        int st = -1;
        pid_t reaped = wait(&st);
        if (reaped != c || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
            printf("[fork_stress] cycle %d: wait mismatch (pid/status)\n", i);
            failures++;
        }
    }
    printf("[fork_stress] final:\n");
    raw_syscall0(SYS_PROC_DUMP);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], CHILD_MARKER) == 0)
        return run_as_child(argc, argv);

    run_main_test(argv[0]);
    run_leak_check(argv[0]);

    printf("\n[fork_stress] === %d failure(s) ===\n", failures);
    return failures;
}
