/*
 * Port of the retired usr/badptr_test.S, rewritten against real mlibc.
 *
 * Proves read()/write() reject pointers the calling process doesn't
 * own. Before user_buffer_ok() (kernel/proc/syscall.c) existed, all
 * three cases below succeeded: syscalls run at EL1 with TTBR1 live, so
 * a raw user pointer handed straight to console_putc() could read the
 * kernel's own half, and read() could write attacker-chosen bytes into
 * it. This is a real isolation hole, not a hypothetical — kept as its
 * own file (not folded into mlibc_syscall_test.c) because it's a
 * security-boundary check, not a syscall-plumbing check, and deserves
 * to stay legible as exactly that.
 */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[badptr] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("\n[badptr] === kernel pointer isolation (user_buffer_ok) ===\n");

    /* 1. write() pointed at the kernel half (the HHDM base — real,
     * live kernel mappings, 0xFFFF800000000000). */
    ssize_t r1 = write(1, (const void *)0xFFFF800000000000ULL, 64);
    check("write() to the kernel half is rejected", r1 < 0);

    /* 2. write() pointed at an unmapped USER address. In range, so the
     * bare range check alone wouldn't catch it — this is what the
     * per-page "is it actually mapped" check is for. Without it the
     * kernel takes a Data Abort at EL1 servicing the syscall, which is
     * a panic: any process could halt the machine with one bad
     * pointer. */
    ssize_t r2 = write(1, (const void *)0x30000000UL, 16);
    check("write() to an unmapped user address is rejected", r2 < 0);

    /* 3. read() aimed INTO the kernel half — the dangerous direction:
     * this has the kernel writing attacker-chosen bytes into its own
     * memory. Must be rejected before ever blocking on a keystroke, so
     * this returns immediately instead of waiting for input. */
    ssize_t r3 = read(0, (void *)0xFFFF800000000000ULL, 1);
    check("read() into the kernel half is rejected", r3 < 0);

    printf("[badptr] === %d failure(s) ===\n", failures);
    return failures;
}
