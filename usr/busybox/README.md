# BusyBox on Extron

Extron boots a static BusyBox 1.38.0 binary as `/sh`. The current seed enables
`ash` plus `cat`, `echo`, `false`, `ls`, `mkdir`, `printf`, `pwd`, `sleep`,
`test`, `true`, and `wc`. Applets are executed through BusyBox's standalone
shell support, so the initrd does not need a directory full of symlinks.

## Build

Download and extract the official BusyBox 1.38.0 release as
`third_party/busybox`, then run:

- Source: `https://busybox.net/downloads/busybox-1.38.0.tar.bz2`
- SHA-256: `34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2`

```sh
tools/configure_busybox.sh
CCACHE_DISABLE=1 make -j4
```

The configuration script applies `busybox-1.38.0-extron.patch`, starts from
BusyBox's `allnoconfig`, and applies `extron.config`. The normal Extron build
then cross-compiles BusyBox against `usr/mlibc-sysroot`, copies it to the
initrd as `sh`, and builds the kernel image. Set `BUSYBOX_DIR` when the source
tree lives elsewhere.

## Supported shell baseline

The QEMU smoke test covers shell startup, external applet re-exec,
`ls`, `cat hello.txt`, `mkdir`, `cd`, and `pwd`. This is backed by ramfs path
resolution, directory iteration, `stat`/`fstat`, cwd inheritance, and the
existing fork/exec/wait implementation.

The console is now a kernel TTY rather than raw UART reads with hard-coded
echo. It implements the termios and winsize ioctls used by BusyBox, canonical
and noncanonical reads, CR/LF translations, erase/kill/EOF handling, output
post-processing, and an interrupt-or-timeout `poll` path. BusyBox's line
editor therefore owns interactive echo and provides cursor editing, a
50-entry history, tab completion, and fancy prompts. UART input remains
interrupt-driven; finite `poll` waits use the scheduler timer and do not scan
the UART in a loop. Run `/mlibc_tty_test.elf` to check the userspace ABI.

This is not yet a complete POSIX shell environment. The TTY does not yet
deliver `ISIG` control characters to process groups, and noncanonical `VTIME`
combinations beyond the BusyBox path (`VMIN=1`, `VTIME=0`) are not complete.
Pipelines and redirection need `pipe`, `dup2`, and more descriptor semantics;
job control needs userspace signals, process groups, sessions, and controlling
TTY semantics. The filesystem also still lacks `unlink`, `rename`, symlinks,
permissions enforcement, and persistent storage. Those are the next
compatibility layers, rather than BusyBox-specific libc workarounds.
