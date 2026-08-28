# BusyBox on Extron

Extron boots a static BusyBox 1.38.0 binary as `/sh`. The current seed enables
`ash` plus the core navigation, file mutation, metadata, process, and text
applets enabled in `extron.config`. Applets are executed through BusyBox's standalone
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

The configuration script applies `patches/busybox/busybox-1.38.0-extron.patch`, starts from
BusyBox's `allnoconfig`, and applies `extron.config`. The normal Extron build
then cross-compiles BusyBox against `usr/mlibc-sysroot`, copies it to the
initrd as `sh`, and builds the kernel image. Set `BUSYBOX_DIR` when the source
tree lives elsewhere.

## Supported shell baseline

The QEMU smoke test covers shell startup, external applet re-exec,
`ls`, `cat hello.txt`, `mkdir`, `cd`, `pwd`, input/output/append redirection,
and multi-process pipelines. This is backed by ramfs path resolution,
directory iteration, `stat`/`fstat`, cwd inheritance, fork/exec/wait, and a
real descriptor table with shared open-file descriptions.

Anonymous pipes use a 4 KiB kernel circular buffer. Readers and writers sleep
on scheduler wait channels when the buffer is empty or full, so pipelines do
not poll the UART or spin the CPU. `dup`, `dup2`, the `fcntl` operations used
by ash (`F_DUPFD_CLOEXEC`, descriptor flags, and status flags), and close on
exec are implemented. Closing or exiting the final writer delivers EOF to
readers. Run `/tests/mlibc_pipe_test.elf` to exercise these primitives directly.

The console is now a kernel TTY rather than raw UART reads with hard-coded
echo. It implements the termios and winsize ioctls used by BusyBox, canonical
and noncanonical reads, CR/LF translations, erase/kill/EOF handling, output
post-processing, and an interrupt-or-timeout `poll` path. BusyBox's line
editor therefore owns interactive echo and provides cursor editing, a
50-entry history, tab completion, and fancy prompts. UART input remains
interrupt-driven; finite `poll` waits use the scheduler timer and do not scan
the UART in a loop. Run `/tests/mlibc_tty_test.elf` to check the userspace ABI.

The shell now has signals, process groups, foreground job control, namespace
mutation, links, and ramfs permission/ownership/timestamp enforcement. It is
still not a complete POSIX environment: there is no persistent filesystem,
devfs, user/group database, access-control lists, or VFS-backed executable
loading. The TTY and poll implementations also retain narrower semantics than
a mature Unix. These are kernel compatibility boundaries rather than
BusyBox-specific libc workarounds.
