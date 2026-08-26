/*
 * Kernel-only PT_INTERP regression fixture. Built normally (a plain static
 * ET_EXEC, same as every other usr/mlibc_tests/*.c), then
 * tools/add_pt_interp.py splices a PT_INTERP segment onto the
 * compiled binary pointing at /opt/tests/mlibc_fake_interp.elf — see
 * that script and mlibc_fake_interp.c's own header comment.
 *
 * If the kernel's PT_INTERP handling (kernel/proc/exec.c's
 * exec_image_build()) works, this main() body never runs at all: the
 * kernel sees the PT_INTERP segment, loads mlibc_fake_interp.elf
 * instead, and starts execution there — this program's own entry
 * point is never reached. _exit(1) here exists purely so a bug that
 * makes the kernel ignore PT_INTERP and jump into the main image
 * directly (the pre-PT_INTERP-support behavior) is loud and
 * unmistakable rather than silently "happening to look okay".
 */
#include <unistd.h>

int main(void) {
    write(1, "VICTIM_RAN_THIS_SHOULD_NOT_HAPPEN\n", 35);
    _exit(1);
}
