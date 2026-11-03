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

## Framebuffer console

The loader keeps the firmware's current GOP mode and passes a multiboot2
framebuffer tag; `arch/x86_64/fb.c` maps it (implementing the same `fb.h` API
as the Pi mailbox driver, so `/dev/fb0` works too) and `kernel/console/fbcon.c`
renders a text console with an 8x16 font (`font8x16.h`, converted from the
public-domain Lat15-VGA16 PSF), 16 colours, scrolling and the VT100 subset
shells need (cursor movement, erase, SGR, insert/delete line/char). It mirrors
`kprintf` and console-tty output (boot messages printed before the
framebuffer exists are replayed), drawing is deferred until `fbcon_flush()`.
`arch/x86_64/ps2kbd.c` adds a PS/2 keyboard (US layout, Shift/Caps/Ctrl, arrows,
Home/End/Del/PgUp/PgDn) feeding the same tty path as serial, so the window is
interactive. `make -f Makefile.x86_64 run` opens a QEMU window;
`tools/x86_screenshot.py out.png [secs] [qemu sendkey names...]` boots headless
and saves a screendump.

## Fast mem*

`klibc/string.c` uses `rep movsq/stosq` (+ byte tail), word-wise memcmp/strlen
and an 8-byte backward memmove on x86-64, ~7-20x the old byte loops
(printed at boot: `mem: memcpy ... MB/s`). Explicit SSE/AVX is intentionally
not used in the kernel: it is built `-mgeneral-regs-only` so user FP/SSE state
is never clobbered by syscalls/IRQs. AArch64 keeps the original loops (the
early boot code runs with Device-memory semantics where wide/unaligned
accesses can fault).

## Userland (mlibc + BusyBox)

mlibc runs on x86-64 Extron. The sysdeps were made arch-generic
(`sysdeps/extron/{aarch64,x86_64}/` for crt1/signal-restore/cancel-syscall/
thread entry, `generic/syscall_arch.hpp` for the syscall wrappers and the
TLS thread-pointer placement) in a fork branch of the mlibc repository
(`x86` branch of https://github.com/enthusiastic2003/mlibc, base `mlibc-port`
plus this repo's `patches/mlibc/`). It is built with the **host GCC** using a
meson cross file (`system = 'extron'`, `cpu_family = 'x86_64'`) and a thin
driver script that supplies the sysroot headers and crt1 and undefines
`__linux__`; no custom `x86_64-extron` compiler is needed. BusyBox 1.38.0 is
built the same way with `patches/busybox/` and `usr/busybox/extron.config`.

### Building it (reproducible)

```
make -f Makefile.x86_64 userland     # = tools/build_x86_userland.sh (needs network once)
tools/build_x86_initrd.sh            # ext2 root: ld.so, libc.so, BusyBox, tests
make -f Makefile.x86_64 run          # or: check, check-userland, soak
```

`build_x86_userland.sh` clones the mlibc fork (`MLIBC_REPO`/`MLIBC_REF`, default
branch `x86`), installs meson+ninja if missing, builds mlibc, downloads and
sha256-verifies BusyBox, applies `patches/busybox` + `usr/busybox/extron.config`,
and builds the test programs; everything lands in `build-x86_64/userland/`.
`tools/x86_64-extron-gcc` is the compiler driver it uses (host GCC + sysroot).

The initrd then has `/lib/ld.so`, `/lib/libc.so`, `/bin/busybox` (`/bin/sh` ->
busybox) and the mlibc regression programs in `/opt/tests`.

### Tests

| target | what it does |
|---|---|
| `check` | boots the libc-free smoke payload headless (no userland build needed) |
| `check-userland` | boots BusyBox image, runs all of `/opt/tests` in the guest, fails on any FAIL / nonzero exit / panic / hang / leak |
| `soak SOAK=N` | the same, N times in a row (default 10), plus a free-page leak check after each pass |

## Kernel robustness notes

* `timer_ticks()` is derived from the TSC, not counted from PIT interrupts, so
  interrupts swallowed by long (interrupt-masked) syscalls no longer make
  `sleep()` deadlines drift.
* SMEP is enabled when the CPU has it (ring 0 can't execute user pages), on top
  of NX and CR0.WP. SMAP is not: handlers dereference validated user pointers
  directly rather than through stac/clac copy helpers.
* Zombie members keep a process group alive (fixes ash pipelines).
* Known limitation: syscalls still run with interrupts masked (several
  handlers rely on that for atomicity, e.g. `sys_write`); making them
  preemptible needs an audit of every such handler first.

## Persistent storage (virtio-blk + ext2)

A raw ext2 image attached to QEMU as a virtio-blk disk is mounted at `/mnt`
and survives reboots.

* **Driver** — `kernel/arch/x86_64/virtio_blk.c` (+ `pci.c`): PCI bus walk,
  **legacy** virtio interface (I/O-port BAR0), polled, one request in flight,
  64 KiB bounce buffer and virtqueue in static `.bss` (the kernel image is
  physically contiguous, so no DMA allocator is needed). It exposes the disk as
  a `struct ext2_block_dev`, so the existing ext2 driver is used unchanged.
  QEMU needs `disable-modern=on` (PCIe slots default to modern-only, which this
  driver detects and reports but does not drive). Modern virtio (capability
  parsing, MMIO BAR, possibly above the 4 GiB identity map) and interrupts are
  future work.
* **Mounting** — `arch_mount_data_disk()` runs after the root fs and `/dev`
  are up, creates `/mnt` if needed and mounts the disk if it holds ext2. No
  disk, or no filesystem on it, is not an error.
* **Durability** — ext2 writes dirty cached sectors back after each mutating
  operation. `fsync(2)`/`sync(2)`/`reboot(2)` additionally call
  `ext2_sync_all()`, which writes back everything and issues a virtio flush.
  `fsync(-1)` is the kernel's "sync everything" entry point.
* **Disk image** — `make -f Makefile.x86_64 disk` creates
  `build-x86_64/disk.img` (64 MiB, `mke2fs -E revision=0 -b 1024`) once;
  `make run` attaches it automatically. `make disk-clean` removes it.
* **Tests** — `make check-persist` writes files (including a 1.4 MB one that
  needs double-indirect blocks), kills QEMU without a clean shutdown, runs
  `e2fsck -fn` on the image from the host, then boots again and verifies the
  contents. `mlibc_disk_test` (a regular `/opt/tests` program, run by
  `check-userland` and `soak` against a fresh disk) exercises big files,
  shrinking `ftruncate` through every indirect level, rename/link/symlink,
  directory growth and `d_type`.

Filesystem fixes made along the way: dirent type bytes are only written when the
FILETYPE feature is set (and `readdir` reads the inode otherwise); directory
iteration uses a stable byte-offset cookie so `rm -r` doesn't skip entries;
unlinked symlinks are now reclaimed (a missing reference drop leaked them) and
fast-symlink text is never freed as block numbers; inodes get
creation/modification/deletion times; `ftruncate` frees blocks at all
indirection levels.


## /proc

`kernel/fs/procfs.c` mounts a synthetic read-only filesystem at `/proc` at boot
(the mount point is created if the root lacks it). Nodes are created on lookup
and freed with their last reference; a `revalidate` hook on `struct vfs_fs_ops`
stops the dentry cache from handing out entries for processes or descriptors
that no longer exist.

* `/proc/self` (symlink), `/proc/<pid>/{stat,status,cmdline,comm,statm,environ,
  exe,cwd,root,fd/}` (`environ` is empty: the environment is not retained).
* `/proc/{uptime,meminfo,loadavg,stat,cpuinfo,mounts,filesystems,version,cmdline}`.
* `struct proc` now records `comm`, `exe`, `cmdline` and `start_ns` (set at exec,
  copied by fork).
* `sysinfo(2)` is `SYS_SYSINFO` (121); `free` and `uptime` use it.
* BusyBox `ps`, `top`, `free`, `uptime`, `pidof`, `pgrep`/`pkill`, `killall` and
  `watch` work. `df`, `mount` and `umount` are still off (no mount syscall, and
  BusyBox's `find_mount_point` does not build against mlibc's `<mntent.h>` yet).

Limits: load averages are always 0.00, CPU time in `/proc/stat` only covers live
processes, `tty_nr` is always 0, and `/proc/<pid>/task` and `maps` do not exist.

## execve limits

`EXEC_MAX_ARGS` 1024 arguments, `EXEC_ARG_BYTES` 20 KiB of argument strings,
`EXEC_MAX_ENVS` 256 variables, `EXEC_ENV_BYTES` 8 KiB of environment strings
(`sysconf(_SC_ARG_MAX)` reports the sum, 28 KiB). Beyond them execve returns
`E2BIG`. The block (strings, argv/envp arrays, auxv) is assembled in a heap
buffer of `EXEC_BLOCK_MAX` (40 KiB) and copied into the top of the new
stack, so a program always keeps at least 88 KiB of its 128 KiB stack. Raising the
limits further means growing the fixed, eagerly mapped stack (`EXEC_USER_STACK_BYTES`).
