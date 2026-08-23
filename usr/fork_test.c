/*
 * fork / execve / wait.
 *
 * Six claims, each with a way to be wrong that a casual "it printed
 * something" run would not catch:
 *
 *  1. fork() returns twice, with different values.
 *  2. The child's memory is a COPY, not a share. Both processes write
 *     to the same address and each must still see its own value —
 *     vm_space_clone() sharing a page instead of copying it would look
 *     completely fine until exactly this test.
 *  3. FP/SIMD state is inherited. Checked by leaving a value in d8
 *     across the `svc` itself, because that state is not in the trap
 *     frame and not in the parent's saved context — it is live in the
 *     hardware, and only cpu_context_save_fpsimd() puts it anywhere the
 *     child can get it from. This is the fourth time per-process
 *     register state has had to be chased down on this port; a child
 *     that inherits only integer registers works perfectly for any
 *     program that reloads its FP before using it.
 *  4. The child's ELF image and stack really are its own — it runs code
 *     and makes calls after the fork.
 *  5. execve() replaces the program, delivers argv, and the exit status
 *     comes back through wait().
 *  6. None of it leaks. SYS_PROC_DUMP prints the PMM's free page count;
 *     across repeated fork/exec/wait cycles that number has to stop
 *     moving. It is the only check here that would fail on a kernel
 *     that is otherwise completely correct, and the entire reason the
 *     VMA list had to start describing the ELF image and the stack
 *     rather than just the heap.
 */
#include <stdio.h>
#include <string.h>
#include <extron/syscall.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[fork_test] %-38s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/*
 * fork(), with known values parked in two FP registers across the
 * syscall, reported back as raw bit patterns.
 *
 * Written as one asm block, and compared as INTEGERS, both on purpose.
 *
 * Calling sys_fork() and checking a double around it would only prove
 * the C compiler honoured AAPCS64's callee-saved rule for d8 — the
 * value would be spilled to the stack and reloaded, and the kernel
 * could lose every FP register without the test noticing. Keeping the
 * value in the register across the `svc` is what makes the question
 * being asked the kernel's.
 *
 * Comparing bit patterns in general-purpose registers matters just as
 * much, and is the subtler half. The first version of this compared two
 * doubles, and GCC parked the expected value in d31 — so in a child
 * that had lost its FP state, the observed value and the expected value
 * were BOTH zero and the check passed. A test for "did these registers
 * survive" cannot keep its answer key in one of them. Moving the
 * comparison into integer registers puts the answer key somewhere the
 * failure mode can't reach, since general-purpose registers come from
 * the copied trap frame rather than from the FP save.
 *
 * The patterns are arbitrary 64-bit values, not meaningful doubles;
 * nothing performs arithmetic on them, so what they would denote as
 * floating point is irrelevant.
 */
#define PARKED_D8  0x0123456789ABCDEFULL
#define PARKED_D20 0xFEDCBA9876543210ULL

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

int main(void) {
    printf("\n[fork_test] === fork / execve / wait ===\n");

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
        printf("[fork_test] fork failed outright\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* ---------------- child ---------------- */
        shared_slot = 222;
        strcpy(deep, "child");

        /* Reported, not asserted, so the parent stays the single voice
         * deciding PASS/FAIL — but printed from inside the child, which
         * is itself the proof that it has working code and a stack. */
        printf("[fork_test]   child: fork()=0, slot=%ld, buf=\"%s\"\n",
               shared_slot, deep);

        /* The child is the only place the inherited FP state can be
         * observed, and it is about to stop existing — so the verdict
         * is handed to exec_child through argv, which turns it into an
         * exit status the parent can fold into its own count. A FAIL
         * printed here and nowhere else would sit above a "0 failures"
         * summary and be believed by nobody. */
        int fp_ok = (d8_after == PARKED_D8 && d20_after == PARKED_D20);
        if (!fp_ok) {
            printf("[fork_test]   child: FP/SIMD LOST across fork\n");
            printf("[fork_test]     d8  = 0x%lx (expected 0x%lx)\n",
                   (unsigned long)d8_after, (unsigned long)PARKED_D8);
            printf("[fork_test]     d20 = 0x%lx (expected 0x%lx)\n",
                   (unsigned long)d20_after, (unsigned long)PARKED_D20);
        }

        /* --- 5: replace this program entirely --- */
        static char a0[] = "exec_child.elf";
        static char a1[] = "from-fork";
        static char ok[]   = "fp-ok";
        static char lost[] = "fp-lost";
        char *args[] = { a0, a1, fp_ok ? ok : lost, 0 };

        sys_execve(a0, args, 0);

        /* Only reachable if execve failed — on success this address
         * space no longer exists. */
        printf("[fork_test]   child: execve returned, so it FAILED\n");
        sys_exit(99);
    }

    /* ---------------- parent ---------------- */
    check("fork() returned a child pid", pid > 0);
    check("parent's copy of the slot is untouched", shared_slot == 111);
    check("parent's copy of the buffer is untouched", strcmp(deep, "parent") == 0);
    check("FP/SIMD survives the fork syscall (parent)",
          d8_after == PARKED_D8 && d20_after == PARKED_D20);

    int  status = -1;
    long reaped = sys_wait(&status);
    check("wait() returned the child's pid", reaped == pid);
    /* exec_child exits 7 only when it was told "fp-ok", so this single
     * status covers three things at once: execve loaded and ran a new
     * program, argv reached it intact, and the FP/SIMD state the child
     * inherited across the fork was correct. */
    check("execve ran, argv arrived, child FP inherited", status == 7);

    long none = sys_wait(&status);
    check("wait() with no children returns -1", none == -1);

    /* --- 6: the leak check --- */
    printf("\n[fork_test] --- teardown: PMM free pages across cycles ---\n");
    printf("[fork_test] The count below must STOP CHANGING. A steady drop\n"
           "[fork_test] means an address space is not being fully freed.\n");

    for (int i = 0; i < 5; i++) {
        printf("[fork_test] cycle %d:\n", i);
        __syscall0(SYS_PROC_DUMP);

        long c = sys_fork();
        if (c == 0) {
            static char e0[] = "exec_child.elf";
            static char *eargs[] = { e0, 0 };
            sys_execve(e0, eargs, 0);
            sys_exit(98);
        }
        if (c < 0) {
            printf("[fork_test] cycle %d: fork failed\n", i);
            failures++;
            break;
        }
        int st = -1;
        if (sys_wait(&st) != c || st != 7) {
            printf("[fork_test] cycle %d: wait mismatch (pid/status)\n", i);
            failures++;
        }
    }
    printf("[fork_test] final:\n");
    __syscall0(SYS_PROC_DUMP);

    printf("\n[fork_test] === %d failure(s) ===\n", failures);
    sys_exit(failures);
    return 0;
}
