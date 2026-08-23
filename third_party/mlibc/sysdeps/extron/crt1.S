// aarch64 port of extron's mlibc crt1.S, matching mlibc's own documented
// entry-point pattern: grab the raw stack pointer and hand it to
// __mlibc_start_main(), which calls __dlapi_enter() with it before
// touching anything else.
//
// kernel/proc/exec.c's build_arg_stack() now lays out a genuine SysV
// AArch64 entry stack here — argc at [sp], argv[] right after it, a
// NULL, then an (empty) envp[] terminated by another NULL — because
// mlibc's own options/elf/generic/startup.cpp registers a real
// [[gnu::constructor]] (init_libc()) that parses exactly that shape
// automatically, as part of running this executable's .init_array
// from inside __dlapi_enter(). That constructor runs unconditionally;
// there's no opting out of it, so the stack has to be real.
.file "crt1.S"
.section .text
.global _start
.type _start, %function
_start:
    mov  x29, #0            // terminate the frame-pointer chain so a
    mov  x30, #0            // backtrace stops here rather than walking
                             // into whatever the stack page held

    mov  x0, sp              // pass the raw stack pointer, per mlibc's docs
    adrp x1, main
    add  x1, x1, :lo12:main  // make crt1 reference main so archive members
                             // containing it are selected before libc.a
    bl   __mlibc_start_main

    // __mlibc_start_main calls exit() and never returns; trap rather
    // than fall through if it somehow did.
1:  brk  #0
    b    1b
.size _start, . - _start
