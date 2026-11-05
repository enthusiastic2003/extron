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
2. The loader reads `\extron\kernel.elf`, the optional `\extron\initrd.ext2` and the
   optional `\extron\cmdline.txt`, places the kernel's PT_LOAD segments at their physical
   addresses (`KERNEL_LMA` = 64 MiB, see `linker.ld`), builds a multiboot2-shaped info
   block (MMAP, MODULE and CMDLINE tags), calls `ExitBootServices` and jumps to
   `_start` with `rdi` = info block.
3. `boot.S` builds bootstrap tables (identity 0-4 GiB + kernel at
   `0xFFFFFFFF80000000`), `gdt_early_init`, then `kernel_x86_64_main` →
   the shared `kernel_stage1` → `arch_kernel_jump_to_stage2` → timer/IRQs →
   shared `kernel_stage2`.
4. `kernel_stage2` parses the command line (`kernel/cmdline.c`), probes the disk
   (`arch_probe_block_devices()` registers `vda` in the block-device registry,
   `kernel/drivers/blockdev.c`) and picks the root filesystem: the initrd module if
   there is one, else `root=/dev/<disk>` mounted directly. It mounts `/dev` and `/proc`,
   then starts `init=` (default: the first of `/sbin/init`, `/init`, `/bin/sh`).
5. **Normal boot: initramfs, then the disk.** The initramfs (`tools/build_x86_initrd.sh`,
   8 MB image, a few hundred KB of content) holds only `/init`
   (`usr/x86_64/initramfs_init.c`, a static program), a BusyBox with libc and
   `ld.so` for the emergency shell, and empty mount points. `/init` reads `root=` from
   `/proc/cmdline`, calls `mount("/dev/vda", "/newroot", "ext2")`, then
   `switch_root("/newroot")`, mounts the new root's own `/proc` and `/dev`, and executes
   its `/sbin/init` (BusyBox init: `inittab`, `getty`, `login`). If the root cannot be
   mounted, the initramfs shell starts instead.
   With `USE_INITRD=0` there is no initramfs and the kernel mounts `root=` itself.
6. `make check` (the libc-free smoke test) still uses a bare initrd as root with no
   command line and no disk.

Kernel interface added for this: `mount(2)` (`SYS_MOUNT`, one request structure; ext2 from a
registered block device, `proc`, `devtmpfs`; root only; a device holds one filesystem at a
time), `SYS_SWITCH_ROOT` (the mount at a path becomes `/`; the old root and all mounted on
it are detached and drop out of `/proc/mounts`), `/proc/cmdline` showing the real command
line, and a `/dev/vda` node (raw byte-offset reads and writes) that appears when a disk is
found. There is no `umount` yet and no per-process root (`chroot`).

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
make -f Makefile.x86_64 libs python coreutils bash   # optional: libraries, Python, GNU tools, bash
make -f Makefile.x86_64 run          # builds the root disk and initramfs on first use
```

`build_x86_userland.sh` clones the mlibc fork (`MLIBC_REPO`/`MLIBC_REF`, default
branch `x86`), installs meson+ninja if missing, builds mlibc, downloads and
sha256-verifies BusyBox, applies `patches/busybox` + `usr/busybox/extron.config`,
and builds the test programs; everything lands in `build-x86_64/userland/`.
`tools/x86_64-extron-gcc` is the compiler driver it uses (host GCC + sysroot).

`tools/build_x86_rootfs.sh` then assembles the root disk (see "Root filesystem layout").

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

A raw ext2 image attached to QEMU as a virtio-blk disk is the root filesystem
(`/dev/vda`, `root=/dev/vda`) and survives reboots.

* **Driver** — `kernel/arch/x86_64/virtio_blk.c` (+ `pci.c`): PCI bus walk,
  **legacy** virtio interface (I/O-port BAR0), polled, one request in flight,
  64 KiB bounce buffer and virtqueue in static `.bss` (the kernel image is
  physically contiguous, so no DMA allocator is needed). It exposes the disk as
  a `struct ext2_block_dev`, so the existing ext2 driver is used unchanged.
  QEMU needs `disable-modern=on` (PCIe slots default to modern-only, which this
  driver detects and reports but does not drive). Modern virtio (capability
  parsing, MMIO BAR, possibly above the 4 GiB identity map) and interrupts are
  future work.
* **Mounting** — the driver only registers the disk (`arch_probe_block_devices()`);
  the filesystem is mounted as the root by the initramfs (or by the kernel for
  `USE_INITRD=0`), or elsewhere with `mount(2)`.
* **Durability** — ext2 writes dirty cached sectors back after each mutating
  operation. `fsync(2)`/`sync(2)`/`reboot(2)` additionally call
  `ext2_sync_all()`, which writes back everything and issues a virtio flush.
  `fsync(-1)` is the kernel's "sync everything" entry point.
* **Disk image** — `build-x86_64/rootfs.img` (512 MB sparse, `mke2fs -E revision=0
  -b 1024`) is built on first use by `tools/build_x86_rootfs.sh`; `make run` attaches it.
  `make rootfs` rebuilds the system part and keeps `/root`, `/home` and `/mnt`;
  `make disk-tests` refreshes only `/opt/tests`; `make disk-clean` removes the image.
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

## Disk timeout and the clock

* virtio-blk gives every request 5 s (`REQ_TIMEOUT_MS`). On expiry the driver logs
  it, resets the device and rebuilds the virtqueue (`vblk_start_device`), and the
  caller gets `EIO`; if the device does not come back, all further I/O fails fast.
  `make check-disk` hangs the disk through the QEMU monitor (I/O throttle) to test it.
* The CMOS RTC is read at boot (stable double-read, 12-hour mode, century register);
  `clock_settime`/`settimeofday`/`date -s` write the new time back to the RTC
  (`timer_sync_rtc`), so it survives a reboot. There is no timezone support: the
  RTC is assumed to hold UTC.

## Root filesystem layout

`tools/build_x86_rootfs.sh` builds `build-x86_64/rootfs.img` like a normal Linux system:

```
/bin -> usr/bin   /sbin -> usr/sbin   /lib -> usr/lib        (merged /usr)
/usr/bin      coreutils (one binary, GNU names), bash, BusyBox + its applet links,
              python3, ldd, memfree, runtests, ...
/usr/sbin     BusyBox's init, getty, reboot, ...
/usr/lib      ld.so, libc.so, libncursesw, libreadline, libz, libsqlite3, ... and
              python3.14/ (standard library, lib-dynload)
/etc          inittab, init.d/rcS, profile, passwd, group, shadow, fstab, inputrc, ...
/opt/tests    the regression programs and scripts;  /root /home/user /var /tmp /run
/dev /proc /sys   mount points;   /mnt   an ordinary empty directory for the user
```

GNU coreutils and bash take precedence over the BusyBox applet links of the same names
(the script installs BusyBox's links first, then overwrites those GNU provides).
Everything on this image was on the initrd or on the old `/mnt` data disk before:
Python moved from `/mnt/python` to the `/usr` prefix, and no library or tool lives in
the initramfs any more. `/etc/fstab` documents the layout; nothing parses it yet.

## Python

`make x86-python` (tools/build_x86_python.sh) cross-builds CPython 3.14.4 against the
mlibc sysroot with prefix `/usr` and stages a pruned tree in
`build-x86_64/python/rootfs/usr` (about 34 MB with bytecode), which `build_x86_rootfs.sh`
merges into the root disk: `/usr/bin/python3`, `/usr/lib/python3.14`. Its extension
modules find their shared libraries in `/usr/lib` like any other program.

* Built: every standard module except those needing outside libraries (zlib, bz2,
  lzma, ssl/hashlib's OpenSSL parts, ctypes/libffi, sqlite3, readline, curses, tkinter,
  uuid, dbm). `_socket` is built, but only AF_UNIX works; `multiprocessing`,
  `asyncio`, `subprocess`, `threading`, `select`/`epoll`, `mmap`, `fcntl` locks and
  `shared_memory` are exercised by `usr/x86_64/tests/python_test.py` (run by
  `runtests` when Python is installed; 128 checks).
* AVX is compiled out (`ax_cv_check_cflags__Werror__mavx2=no`): CPython selects its
  256-bit BLAKE2 path from the CPUID bit alone, but this kernel does not enable AVX.
* Kernel/libc work this needed: futexes keyed by physical address in shared mappings
  (process-shared semaphores), `FIONBIO`/`FIONREAD` on any descriptor, `/proc/<pid>/maps`,
  `clock_nanosleep` with `TIMER_ABSTIME`, timed/shared/named semaphores
  (`/tmp/.sem.*`), `shm_open` under `/tmp` (no `/dev/shm`), `wcsftime`, `sendfile`
  (userspace loop), xattr stubs (`ENOTSUP`), `C.UTF-8` locale names, `pthread_setname_np`.

## Disk I/O must not yield

virtio-blk waits for its interrupt with the CPU halted and preemption held off
(`sched_preempt_disable`); it must not sleep and reschedule. ext2 (shared scratch
buffer and sector cache) and the VFS (spinlocks held across `vfs_read`) assume a
syscall is never switched out half way, and an earlier version that slept in the
driver corrupted data as soon as two processes read the disk at once (found by
`multiprocessing`). Making the kernel preemptible would need ext2 and `struct
open_file` locking reworked first.

## Libraries for Python's optional modules

`tools/build_x86_libs.sh` (`make x86-libs`, run automatically by the Python build) builds
zlib, bzip2, xz (liblzma), zstd, libffi, ncurses (wide, terminal descriptions
compiled in with `--with-fallbacks`, so no terminfo database is needed), readline and
sqlite as shared objects in `build-x86_64/extlibs/prefix`. The root disk carries the `.so`
files in `/usr/lib` (the default ld.so search path, also reachable as `/lib`); Python's extension modules list them
as DT_NEEDED and mlibc's rtld loads them when the module is imported. With them Python
gains `zlib`, `gzip`, `bz2`, `lzma`, `compression.zstd`, `ctypes`, `curses`, `readline` and
`sqlite3` (`tarfile`/`zipfile` compression included). Still absent: `_ssl`/`_hashlib`
(OpenSSL), `_uuid` (libuuid; `uuid` falls back to pure Python), `_tkinter`, `dbm`.

Gotchas found on the way:
* The toolchain leaves `.gnu.version` index 0 on unversioned undefined symbols in
  modules that also need a versioned library (zlib's `ZLIB_1.2.0`); mlibc's rtld
  now treats that as "global" instead of failing with "Unresolved load-time symbol".
* The compiler wrapper undefines `__linux__`, which libffi tests to choose its
  closure-memory strategy; without `-DFFI_MMAP_EXEC_WRIT=1` closures (ctypes callbacks)
  land in non-executable malloc memory and crash on first call.
* libreadline.a does not carry its ncurses dependency: configure needs
  `LIBREADLINE_LIBS="-lreadline -lncursesw"`.
* Shared-object links get `-lgcc` (128-bit division in `_decimal`).

## GNU coreutils

`tools/build_x86_coreutils.sh` (`make -f Makefile.x86_64 coreutils`) cross-builds coreutils
9.12 as one multi-call binary (`--enable-single-binary=symlinks`, no NLS/ACL/xattr/GMP). The
root disk carries it in `/usr/bin` (103 applets as symlinks over BusyBox's links of the same
names). BusyBox provides everything GNU does not, and init. BusyBox's
`FEATURE_PREFER_APPLETS`/`SH_STANDALONE`/`SH_NOFORK` are off so its shell honours `PATH`
(otherwise `ls` would silently run the applet). `/etc/mtab` links to `/proc/mounts` for `df`.
`usr/x86_64/tests/coreutils_test.sh` (30 checks) calls the GNU tools by full path.

What porting it needed:
* gnulib's `getlocalename_l-unsafe.c` has no branch for mlibc: the script patches in one
  that reports the C locale.
* Main-thread stack growth: the stack was a fixed 128 KB; GNU tools probe bigger frames.
  A not-present user fault just below it now maps zeroed pages down to an 8 MiB cap
  (`vm_grow_stack`, `RLIMIT_STACK` reports 8 MiB).
* mlibc: `sys_prctl` (the multi-call binary names itself with `PR_SET_NAME`),
  `sched_getaffinity` (one CPU), `fwrite` now reports `errno`/`ferror` on write errors
  (`sort | head` printed a stale ENOSYS), `fstatat(fd, "", AT_EMPTY_PATH)` (`ls -R`),
  quiet `ENOSYS` for `vmsplice`/`splice`/`timer_create`, no `uselocale`/`fadvise` nags.
* Not in coreutils proper: `cmp`, `diff`, `find`, `grep`, `sed`, `awk` stay BusyBox's.

## GNU bash

`tools/build_x86_bash.sh` (`make -f Makefile.x86_64 bash`) cross-builds bash 5.3 with its own
readline and the shared `libncursesw` from `build_x86_libs.sh`; `/usr/bin/bash` (`/bin/bash`) goes onto the
root disk and becomes the login shell for root and `user` (`/etc/passwd`, `/etc/shells`).
BusyBox `sh` is still `/bin/sh` and runs init and the rc scripts. `/etc/profile` sets
`PATH`, exports `HOME/USER/LOGNAME/SHELL` (login leaves them to the shell), and for bash
sets `PS1='\w \$ '` (same shape as BusyBox's `/ # `, which the test harnesses wait for) and
`INPUTRC=/etc/inputrc` (bracketed paste off). `usr/x86_64/tests/bash_test.bash` has 76 checks
(arrays, expansions, `[[ ]]`, functions, redirections, process substitution, traps, job
control, coproc, `wait -n`, `read -t`, ...), run by `runtests` when `/bin/bash` exists.

Cross-build notes: bash's generator programs use `CC_FOR_BUILD=gcc`; run-time configure
probes are answered in the script (`bash_cv_*`); `bash_cv_termcap_lib=libncursesw` stops it
linking the host's `libtinfo`; `bash_cv_dev_fd=absent` makes process substitution use FIFOs
because Extron has no `/dev/fd` yet.

What bash found missing in mlibc/the port (all fixed):
* `main()` never received `envp` (`__mlibc_start_main` passed only argc/argv), so bash started
  with an empty environment; BusyBox reads `environ`, which hid it.
* `getresuid`/`getresgid` sysdeps (bash printed "I have no name!"); `mktemp()` (FIFO names).
* `strtoimax`/`strtoumax` were a hand-written stub that rejected `+`, mishandled `-` and
  panicked above base 10; they now share `strtol`'s conversion (`printf %d -1`).

## Display ownership, keyboard events and DOOM

**Any program can take the screen** by mapping `/dev/fb0` (`open`, `read` the
`struct extron_fb_geometry`, `mmap` the pixels; `usr/include/extron/fb.h`). Mapping is the
whole API, and there is no release call to forget:

* While a process holds a `VM_FBOWNER` mapping the kernel text console stops painting
  (`fbcon_suspend()`; the cell grid keeps tracking output, so `fbcon_resume()` repaints
  everything, history included) and the keyboard is diverted from the console tty to the
  program's input rings. Ctrl-C and Ctrl-\ still reach the tty, so SIGINT/SIGQUIT stay an
  escape hatch. Serial output carries on as usual.
* Only one live process owns the display: a second `mmap` of `/dev/fb0` fails with `EBUSY`.
  The same process may map it again.
* Ownership ends when the owner exits, is killed, `munmap()`s every mapping or `exec`s. The
  kernel checks lazily (`kernel/drivers/fbown.c`: on exit, `munmap` and every console write),
  so the console returns as soon as anything touches it. `mlibc_fbown_test` covers
  EBUSY, munmap, exit and kill.

**Key events.** `/dev/input` is an mmap'd page with two single-producer/single-consumer
rings (`struct extron_input_ring`): the original byte ring (serial and PS/2 keyboard bytes,
as a terminal would send them) and a new event ring with **press and release** edges,
`(pressed << 8) | keycode`, keycode being the PS/2 set-1 make code with bit 7 set for
E0-prefixed keys. The PS/2 driver feeds both; serial feeds only bytes. A program that needs
both edges (a game) reads events, and falls back to synthesising releases from bytes when
there is no keyboard (`extron_input_getkey()` / `extron_input_getc()`).

**DOOM** (`make doom`, `tools/build_x86_doom.sh`): doomgeneric at a pinned commit plus
`usr/doom/doomgeneric_extron.c`, the platform layer (POSIX time and sleep, integer-scaled
output centred on the display, 2x on a 1280x800 GOP framebuffer, key events with a serial
fallback). The shareware WAD (`doom1.wad`, sha256 pinned) is fetched once and installed as
`/usr/share/games/doom/doom1.wad`; the program is `/usr/bin/doom` and takes doomgeneric's
usual options. Controls: arrows move/turn, Ctrl fire, Space use, Shift run, Alt strafe,
`,` `.` strafe, Esc menu. `make check-doom` boots, runs `doom`, checks through the QEMU
monitor that the game owns the screen, animates and reacts to `sendkey`, that Ctrl-C stops
it and that the console comes back repainted. The platform layer builds for aarch64 too (it
uses only POSIX calls) but only x86-64 is verified here.

## Networking

IPv4 over a virtio-net NIC, with the stack inside the kernel.

* **Driver** - `kernel/arch/x86_64/virtio_net.c`: legacy virtio over the I/O-port BAR like the disk
  driver (`disable-modern=on`), two queues, interrupt-driven receive into 64 fixed buffers, transmit
  through 32. PCI INTx lines are shared (the disk and the NIC often land on the same IRQ), so
  `register_irq_handler()` now chains up to four handlers per line.
* **Stack** - lwIP 2.2.0, vendored unmodified in `third_party/lwip` (BSD) and configured by
  `kernel/net/include/lwipopts.h`: IPv4, TCP, UDP, ICMP, ARP, DHCP, no OS layer, raw callback API,
  all memory from static pools. `kernel/net/net.c` is the glue (NIC registration, DHCP, the
  1 ms timer tick); `kernel/include/kernel/net.h` documents the contract. **One lock,
  `net_spinlock`, guards every lwIP call and all socket state.** It is an IRQ-safe spinlock, so
  the receive interrupt and the timer tick, which also enter lwIP, never run in the middle of a
  socket call; blocking socket calls sleep on it. lwIP callbacks therefore never sleep or
  allocate (sockets and receive nodes come from static pools).
* **Sockets** - `kernel/net/inet_sock.c`: `AF_INET` `SOCK_STREAM`, `SOCK_DGRAM` and
  `SOCK_RAW`/`IPPROTO_ICMP` (what `ping` uses), behind the same descriptor layer as `AF_UNIX`
  (`kernel/fs/socket.c` forwards when a socket carries an inet backend; kernels without a stack
  get weak stubs). `bind/listen/accept/connect`, `sendmsg/recvmsg`, `poll`/`epoll`,
  non-blocking mode, `shutdown`, `getsockname/getpeername`, `FIONREAD`, and the common options
  (`SO_ERROR`, `SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`, `SO_RCVTIMEO`/`SO_SNDTIMEO`
  honoured, `TCP_NODELAY`, `TCP_KEEP*`). Loopback (127.0.0.1) works with no NIC at all.
* **Configuration** - lwIP's DHCP client runs in the kernel; QEMU's user-mode network gives
  10.0.2.15/24, gateway 10.0.2.2 (the host), DNS 10.0.2.3. `/proc/net/ipconfig` summarises it and
  `/proc/net/resolv.conf` is the live nameserver list; `/etc/resolv.conf` is a symlink to it, so
  mlibc's resolver works without any boot script. `/etc/hosts`, `/etc/services` and
  `/etc/protocols` are static.
* **Running** - `make x86-run` attaches the NIC by default (`NET_ARGS`). From the shell:
  `wget https://host/file`, `nslookup name`, `ping -c 3 10.0.2.2`, `nc host port`, Python's
  `urllib`/`socket`. BusyBox `wget` does HTTPS with its built-in TLS 1.2 client, which does
  **not** verify certificates, until a real TLS library (OpenSSL) is ported.
* **Tests** - `mlibc_net_test` (in `/opt/tests`, so in `check-all`): loopback TCP echo of 512 KiB,
  half-close, `poll`, non-blocking connect, refused connections, UDP, `MSG_TRUNC`, eight parallel
  connections, writing to a closed peer, and `fread` on a non-blocking socket. `make check-net`
  (`tools/x86_net_test.py`) boots with a NIC, runs HTTP and HTTPS servers on the host and checks
  DHCP, `resolv.conf`, `wget` (6 MiB, SHA-256), a server that trickles its body, Python `urllib`
  and raw sockets, `nc`, `ping`, the guest listening (host connects through QEMU port
  forwarding), and, with internet, DNS, HTTPS to a public site, and real source tarballs (GNU
  make over HTTP, zlib from GitHub over HTTPS with a redirect).

What bringing it up found elsewhere (all fixed):
* `/dev/urandom` did not exist, so TLS could not even start (it and `/dev/random` now read the
  kernel's `getrandom` source).
* mlibc's `fread()` returned **0** when a read failed partway, dropping the bytes it had already
  copied. BusyBox `wget` reads a non-blocking socket with `fread` and expects a short count with
  `EAGAIN`; on a distant server (reads run dry mid-call) it silently lost chunks of every download.
  It never showed on fast local links. A packet capture proved the TCP layer had delivered every
  byte (`EXTRON_PCAP=file` on `tools/x86_serial_session.py` writes one).
* `<netinet/ip_icmp.h>` lacked `ICMP_MINLEN` (BusyBox `ping`).
* A listening socket's lwIP pcb must not have its `recv`/`sent`/`err` callbacks cleared (lwIP
  asserts), found by the first loopback test run.

Not done: IPv6, `ifconfig`/`route`/`netstat` (they need `SIOCGIF*` ioctls and `/proc/net/dev`),
certificate validation, `AF_PACKET`, forwarding, more than one NIC, modern (non-legacy)
virtio-net. mlibc's `res_init` is a stub, so BusyBox `nslookup` without a server argument
prints `(null)` for the server line (the lookup itself works).

## A C toolchain on Extron: make, tcc, ar

`make` (GNU make 4.4.1) and `tcc` (the Tiny C Compiler, a pinned snapshot of its development
branch) run on Extron, so source code can be downloaded and built on the machine
(`wget`, `tar`, `./configure`, `make`). `cc` is `tcc`; `ar` is `tcc -ar`; `ranlib` is a no-op
(`tcc -ar` writes the index). The root disk also carries the development files: the C headers
(mlibc's plus the Linux UAPI ones, `/usr/include`), `crt1.o` and the link libraries in
`/usr/lib`, and tcc's own `/usr/lib/tcc` (`libtcc1.a` and its freestanding headers).

* **Build** - `make -f Makefile.x86_64 gnumake tcc` (`tools/build_x86_make.sh`,
  `tools/build_x86_tcc.sh`), then `make rootfs`. tcc's own `./configure` is not run (it would
  execute the cross-compiled compiler on the build machine): the script writes the two files it
  generates, builds the host-side helper with the host compiler, and builds `libtcc1.a` with the
  cross compiler. The compiler's source, configured for a native rebuild, is installed as
  `/usr/src/tcc-extron-src.tar.gz`, so the OS can rebuild its own compiler:
  `tar xzf /usr/src/tcc-extron-src.tar.gz && cd tinycc-* && make tcc libtcc1.a` (about 4 s).
* **What links to what** - tcc links dynamically against the same `libc.so`/`ld.so` as every
  other program (the ones gcc built), or statically against `libc.a` with `-static`. Mixing the
  two compilers is safe because both follow the System V x86-64 ABI; `data/abi.c` proves it by
  printing identical output when built by gcc+glibc on the host and by tcc on Extron (structs
  by value of every ABI class, 18-argument calls, varargs built by tcc and read by libc's
  `vsnprintf`, callbacks from `qsort`, `setjmp`, `long double`, threads with per-thread errno).
* **Making tcc understand mlibc's headers** (all in the build scripts or in mlibc, none needs
  libc changes): tcc is told Extron is not Linux (`__extron__` instead of `__linux__`); GCC's
  predefined type and limit macros (`__UINT8_TYPE__`, `__INTMAX_MAX__`, ...) are generated from
  GCC's table and added to tcc's predefines, minus the 128-bit and extended-float ones tcc cannot
  honour; `errno` goes through `__errno_location()` for tcc because its linker cannot import a
  thread-local variable from a shared library; `__MLIBC_INLINE_DEFINITION` is `static inline`
  for tcc (it ignores `gnu_inline` and would emit duplicate globals); `endian.h` accepts
  `__TINYC__`; the linker defines `__ehdr_start` for static programs; mlibc is built with
  `-fno-gnu-unique` (tcc's linker rejects GCC's `STB_GNU_UNIQUE` C++ template statics in
  `libc.a`).
* **Tests** - `toolchain_test.sh` (in `/opt/tests`, so in `check-all`, 16 checks, ~40 s):
  dynamic and static programs, a multi-file project with `make` and `ar`, the ABI cross-check,
  the 40-check socket test built by tcc, zlib's `./configure && make && make test` from its
  tarball, the compiler rebuilding itself and compiling with the result, and all 152 system
  headers compiling on their own.
* **Limits** - tcc has no C++, no `_Complex` (`complex.h`), no `__int128`, and only part of
  GCC's extensions (SIMD, many builtins and attributes), and `lastlog.h` needs a `utmp.h` mlibc
  lacks. The kernel, mlibc (C++, meson), and larger programs therefore still need the host's GCC:
  this is native C development and a self-rebuilding compiler, not yet a fully self-hosting
  system (that needs GCC and binutils).
