/*
 * Port of the retired usr/fp_test_a.S + usr/fp_test_b.S pair, rewritten
 * against real mlibc and fork() instead of two separately-linked
 * assembly binaries.
 *
 * Proves FP/SIMD state and TPIDR_EL0 survive an ORDINARY scheduler
 * timeslice preemption between two independent, concurrently-running
 * processes — a genuinely different code path from FP-across-fork
 * (mlibc_fork_stress.c): this one exercises context_switch's regular
 * save/restore (kernel/arch/aarch64/proc/switch.S), not
 * cpu_context_save_fpsimd()'s standalone use inside proc_fork(). Both
 * have their own history of being wrong independently — this bit the
 * kernel three separate times (FP/SIMD, SP_EL0, TPIDR_EL0), each
 * invisible until a workload happened to touch that specific piece of
 * state — so both paths get their own regression coverage.
 *
 * fork() gives the "two independent processes" shape for free: each
 * side sets its OWN distinct pattern into registers immediately after
 * the fork (not relying on what fork() itself preserved — that's the
 * other test's job), then both loop, yielding via sleep() so the
 * scheduler is forced to switch between them repeatedly, re-checking
 * after every wake.
 *
 * Register choice is deliberate, same reasoning as the original: v8/v9
 * are AAPCS64 callee-saved, so a switch that only preserved d8-d15
 * would still pass on those alone. v0, v1 and v31 are caller-saved —
 * live across an arbitrary preemption point even though the compiler
 * is free to clobber them across an ordinary call — which is exactly
 * what catches a d8-d15-only save. Both the low and high 64 bits of a
 * register are checked (v0.d[0] vs v1.d[1] etc.) so a save that only
 * covered the low half is caught too.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

#define ITERATIONS 20

/*
 * Loads `pattern` into every register under test, then sleeps and
 * re-checks it ITERATIONS times. Returns 0 if every register (and
 * TPIDR_EL0) still holds `pattern` after every single wake, 1 on the
 * first mismatch.
 *
 * One asm block for the whole loop, not "set registers in C, call
 * sleep(), check in C": across a real C function call the compiler is
 * already free to spill/reload d8-d15 through the stack on its own,
 * which would make this test pass even if context_switch never saved
 * anything at all. Keeping the values live in registers across the
 * syscall itself is what makes the question being asked the kernel's.
 */
static int run_pattern(uint64_t pattern) {
    long result = 0;

    __asm__ volatile (
        /* mlibc's own TLS (errno, stdio's internals) lives off the REAL
         * TPIDR_EL0, so trashing it with the test pattern below and
         * never putting it back would take out the very first libc
         * call made after this asm block returns (found the hard way:
         * a Data Abort at FAR matching the pattern, from printf()
         * dereferencing through a corrupted thread pointer). Save it
         * here, restore it right before falling out of this block —
         * the clobber is still real and still lives in the register
         * across the syscall/preemption, which is the actual thing
         * under test; it just doesn't outlive this function. */
        "mrs     x11, tpidr_el0      \n\t"
        "dup     v0.2d,  %[pat]      \n\t"
        "dup     v1.2d,  %[pat]      \n\t"
        "dup     v8.2d,  %[pat]      \n\t"
        "dup     v9.2d,  %[pat]      \n\t"
        "dup     v31.2d, %[pat]      \n\t"
        "msr     tpidr_el0, %[pat]   \n\t"

        "mov     x9, %[iters]        \n"
        "1:                          \n\t"
        "mov     x8, #2              \n\t" /* SYS_SLEEP */
        "mov     x0, #0              \n\t" /* 0 seconds */
        /* 50ms in nanoseconds (50000000) doesn't fit a single movz —
         * long enough a sleep that the other process definitely runs
         * in between. */
        "movz    x1, #0xf080         \n\t"
        "movk    x1, #0x2fa, lsl #16 \n\t"
        "svc     #0                  \n\t"

        "umov    x10, v0.d[0]        \n\t"
        "cmp     x10, %[pat]         \n\t"
        "b.ne    9f                  \n\t"
        "umov    x10, v1.d[1]        \n\t"
        "cmp     x10, %[pat]         \n\t"
        "b.ne    9f                  \n\t"
        "umov    x10, v8.d[0]        \n\t"
        "cmp     x10, %[pat]         \n\t"
        "b.ne    9f                  \n\t"
        "umov    x10, v9.d[1]        \n\t"
        "cmp     x10, %[pat]         \n\t"
        "b.ne    9f                  \n\t"
        "umov    x10, v31.d[0]       \n\t"
        "cmp     x10, %[pat]         \n\t"
        "b.ne    9f                  \n\t"
        "mrs     x10, tpidr_el0      \n\t"
        "cmp     x10, %[pat]         \n\t"
        "b.ne    9f                  \n\t"

        "subs    x9, x9, #1          \n\t"
        "b.ne    1b                  \n\t"
        "mov     %[result], #0       \n\t"
        "b       2f                  \n"
        "9:                          \n\t"
        "mov     %[result], #1       \n"
        "2:                          \n\t"
        "msr     tpidr_el0, x11      \n\t" /* restore the real TCB pointer */
        : [result] "=r"(result)
        : [pat] "r"(pattern), [iters] "i"(ITERATIONS)
        : "x0", "x1", "x8", "x9", "x10", "x11", "cc",
          "v0", "v1", "v8", "v9", "v31", "memory"
    );

    return (int)result;
}

int main(void) {
    printf("\n[fp_test] === FP/SIMD + TPIDR_EL0 across scheduler preemption ===\n");

    pid_t pid = fork();
    if (pid < 0) {
        printf("[fp_test] fork() failed outright\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: pattern B, distinct in every byte from parent's
         * pattern A, so any cross-contamination between the two
         * processes' saved contexts shows up as a mismatch rather
         * than coincidentally matching. */
        int failed = run_pattern(0xB1B1B2B2B3B3B4B4ULL);
        printf("[fp_test]   B: %s (%d switches)\n",
               failed ? "FAIL (FP/SIMD or tpidr_el0 clobbered)" : "PASS",
               ITERATIONS);
        _exit(failed);
    }

    /* Parent: pattern A. */
    int failed_a = run_pattern(0xA1A1A2A2A3A3A4A4ULL);
    printf("[fp_test]   A: %s (%d switches)\n",
           failed_a ? "FAIL (FP/SIMD or tpidr_el0 clobbered)" : "PASS",
           ITERATIONS);

    /* wait(), not waitpid(pid, ...): the kernel's SYS_WAIT only knows
     * how to block for "any child" (mlibc::sys_waitpid() in generic.cpp
     * refuses anything but pid==-1 with ENOSYS) — there's exactly one
     * child here, so the two are equivalent in practice. */
    int status = -1;
    pid_t reaped = wait(&status);
    int failed_b = reaped != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;

    int failures = failed_a + failed_b;
    printf("[fp_test] === %d failure(s) ===\n", failures);
    return failures;
}
