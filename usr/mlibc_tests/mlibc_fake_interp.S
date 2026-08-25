/*
 * Stand-in "dynamic linker" for mlibc_ptinterp_victim.elf's PT_INTERP
 * regression test (see that file, tools/add_pt_interp.py, which is
 * what actually gives the victim a PT_INTERP segment pointing here,
 * and mlibc_ptinterp_test.c, which runs the whole thing end to end).
 *
 * This is hand-written freestanding AArch64, not an ordinary mlibc
 * program, on purpose: a first attempt built it as a plain static
 * mlibc test binary like every other file in this directory, and it
 * crashed on entry. The reason is fundamental, not a bug to route
 * around: this project's mlibc build's static-startup path
 * (__dlapi_enter -> interpreterMain(), see
 * third_party/mlibc/options/rtld/generic/main.cpp) reads the
 * process's own AT_PHDR/AT_PHNUM out of the auxiliary vector to find
 * its PT_TLS segment and set up thread-local storage. But per real
 * ld.so/kernel convention (see kernel/proc/exec.c's exec_image_build()
 * and its comment on AUXV(AT_PHDR, ...)), AT_PHDR always describes the
 * MAIN PROGRAM being interpreted, never the interpreter itself — a
 * real dynamic linker instead locates its own phdrs via a
 * self-relocation trick using its own program counter. A plain mlibc
 * binary has no such trick; it unconditionally trusts the auxv, so it
 * ends up reading the VICTIM's TLS layout and corrupts its own thread
 * pointer before main() ever runs.
 *
 * The fix is to sidestep mlibc's startup machinery entirely: this
 * file IS its own _start, calling this kernel's write/exit syscalls
 * directly (see third_party/mlibc/sysdeps/extron/generic/generic.cpp
 * for the x8=number/x0-x2=args/svc#0 convention this mirrors), with no
 * TLS segment, no libc, and nothing that reads the auxiliary vector at
 * all — arguably a more faithful stand-in for a real interpreter's
 * bootstrap than a full mlibc binary would have been anyway.
 */
#define SYS_WRITE 1
#define SYS_EXIT  7

.global _start
.text
_start:
    mov     x8, #SYS_WRITE
    mov     x0, #1              // fd 1 (stdout)
    adr     x1, msg
    mov     x2, #(msg_end - msg)
    svc     #0

    mov     x8, #SYS_EXIT
    mov     x0, #66             // distinct exit code the test checks for
    svc     #0

1:  b       1b                  // unreachable: SYS_EXIT never returns

.section .rodata
msg:
    .ascii "FAKE_INTERP_RAN\n"
msg_end:
