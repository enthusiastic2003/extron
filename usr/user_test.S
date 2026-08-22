// Test userland payload — not part of the kernel image. Built as a
// real static ELF64/AARCH64 executable (no aarch64-extron cross
// toolchain exists yet, so this is plain aarch64-linux-gnu-gcc/ld
// freestanding, the same toolchain the kernel itself already uses —
// see the Makefile's $(BUILD)/initrd/%.elf rule) and packaged into
// initrd.tar, giving kernel/proc/elf_loader.c's parse_and_load_binary()
// something real to load.
//
// A tight counter-increment loop — no svc, no privileged instructions,
// deliberately never yields. Used by the (currently parked — see
// kernel/kernel.c's kernel_stage2(), commented out in favor of the
// keyboard-echo test) 2-process scheduler round-robin proof: loaded
// twice into two separate address spaces, each backed by its own
// physical counter page mapped at this same VA (0x600000) — safe
// because each process's TTBR0 is independent. The kernel reads both
// pages' contents directly via phys_to_virt_hhdm() to prove both
// processes are actually running, without either one making a syscall.
.section ".text"
.global _start
_start:
    ldr     x0, =0x600000
loop:
    ldr     x1, [x0]
    add     x1, x1, #1
    str     x1, [x0]
    b       loop
