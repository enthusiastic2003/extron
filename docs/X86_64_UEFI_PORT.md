# x86-64 / UEFI port

The kernel now builds and boots on x86-64 under UEFI (tested with QEMU + OVMF,
TCG and KVM) alongside the original AArch64/Raspberry Pi 4 target. Every
arch-neutral source is shared; `kernel/arch/x86_64/` supplies the rest.

```
make -f Makefile.x86_64          # kernel.elf, BOOTX64.EFI, ESP tree in build-x86_64/esp
make -f Makefile.x86_64 run      # boot under QEMU + OVMF (serial on stdio)
make -f Makefile.x86_64 check    # headless boot, pass/fail on the in-guest smoke test
```

Needs `qemu-system-x86_64`, OVMF (`/usr/share/OVMF/OVMF_{CODE,VARS}_4M.fd`;
override with `OVMF_CODE=`/`OVMF_VARS=`), `gcc`/`ld` with PE (`i386pep`) support
and `mkfs.ext2 -d`.

## Boot flow

1. OVMF runs `EFI/BOOT/BOOTX64.EFI` (`boot/uefi/loader.c`, a freestanding PE
   app, no gnu-efi dependency).
2. The loader reads `\extron\kernel.elf` and `\extron\initrd.ext2`, places the
   kernel's PT_LOAD segments at their physical addresses (`KERNEL_LMA` =
   64 MiB, see `linker.ld`), builds a multiboot2-shaped info block (MMAP +
   MODULE tags — the format `pmm.c`/`ramdisk.c` already consume), calls
   `ExitBootServices` and jumps to `_start` with `rdi` = info block.
3. `boot.S` builds bootstrap tables (identity 0-4 GiB + kernel at
   `0xFFFFFFFF80000000`), `gdt_early_init`, then `kernel_x86_64_main` →
   the shared `kernel_stage1` → `arch_kernel_jump_to_stage2` → timer/IRQs →
   shared `kernel_stage2` (VFS, initrd ext2 root, `/bin/sh`).

The loader requires kernel + initrd + 2 MiB slack to sit contiguously below
1 GiB; the PMM bitmap lands in that slack (it goes above the highest of
kernel/info/module). Use QEMU `-m 128M` or more.

## What is x86-specific

| area | file |
|---|---|
| entry, page-table bootstrap | `boot.S`, `linker.ld` |
| GDT/TSS/IDT, dispatch, `syscall` MSRs | `exceptions.c`, `isr.S` |
| context switch (callee-saved, FS base, FXSAVE) | `proc/switch.S`, `include/arch/context.h` |
| paging (4-level, kernel half shared by all PML4s) | `paging.c` |
| 8259 PIC, PIT tick + PIT-calibrated TSC clock, CMOS RTC | `pic.c`, `timer.c` |
| COM1 console / RX IRQ | `serial.c` |
| signal frames (Linux x86-64 mcontext order) | `include/arch/signal_arch.h` |
| stand-ins for Pi-only drivers (mailbox, fb, EMMC, PM) | `drivers_stub.c` |

Syscall ABI: `syscall` instruction (and `int 0x80`), `rax` = number, `rdi/rsi/rdx`
= args 1-3, result in `rax`; same numbers as the AArch64 build. User space is
the lower 47-bit half (`ARCH_USER_VA_LIMIT`).

## Shared-code refactor

To keep one tree, arch-specific bits that used to be inline AArch64 asm were
moved behind small per-arch headers (`arch/cpu.h`, `arch/context.h`,
`arch/exceptions.h` frame accessors, `arch/irq.h`, `arch/vma.h`,
`arch/signal_arch.h`); `struct aarch64_frame` became `struct arch_frame`.
The AArch64 kernel image is byte-for-byte the same size as before the refactor.

## Userland status

The checked-in mlibc sysroot, BusyBox, DOOM etc. are AArch64 binaries. For
x86-64 the initrd carries `usr/x86_64/init.c` as `/bin/sh`: a libc-free
payload that exercises fork/wait/execve/pipe/mmap/signals/SIGSEGV/sleep/clock
and FP/SSE isolation under timer preemption (`make -f Makefile.x86_64 check`).
Porting mlibc (`third_party/mlibc/sysdeps/extron`), BusyBox and the rest to
x86-64 is the remaining step before the full userland runs.
