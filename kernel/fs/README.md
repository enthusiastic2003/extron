# Extron VFS

`vfs.c` is the filesystem-neutral boundary between process-facing file code
and concrete filesystems. Ramfs supplies the root filesystem, but descriptors,
cwd state, and syscalls do not depend on ramfs internals.

## Stage 1 foundation

The namespace now has separate objects for the two roles POSIX filesystems
eventually need:

- a `vfs_dentry` is one name in one parent directory;
- a `vfs_node` is the inode-like object named by that dentry.

Both objects are reference counted. Namespace membership owns the baseline
dentry reference, open-file descriptions retain nodes, mounts retain their
roots and covered paths, and each process retains its cwd as a `vfs_path`.
Fork takes another cwd reference rather than copying a pathname. Cwd snapshots
are protected against concurrent `chdir()` by another thread in the process.

Pathnames are walked one component at a time. Absolute and relative paths,
repeated slashes, `.`, `..`, missing parents, non-directory intermediate
components, a 255-byte component limit, and a 1024-byte path limit are handled
at the common VFS layer. Filesystems only implement direct-child lookup and
creation. VFS and file operations return negative, Linux-compatible errno
values; the Extron mlibc sysdeps translate those into userspace `errno`.

The fixed-size mount table currently contains only the root ramfs. Its routing
and covered-path machinery is in place so a later devfs can be mounted without
changing pathname callers. There is no unmount operation yet.

## Ramfs

Ramfs is a real parent/child hierarchy of dentries and inodes. Initrd entries
seed that hierarchy at boot, creating intermediate directories when an archive
entry contains `/`. Seeded file contents remain immutable initrd views until
the first write or truncation, when ramfs creates owned storage. Runtime files
and directories retain POSIX mode, ownership, and timestamp metadata.
Creation modes are filtered through the calling process's umask.

## Stage 2 namespace mutation

`unlink`, `rmdir`, and `rename` now mutate the hierarchy through filesystem
operations under the ramfs tree lock. Removing a name drops its namespace
reference and link count, but an open-file description keeps the inode alive.
Likewise, a cwd retains a detached directory and its parent chain: `getcwd()`
reports `ENOENT` while detached, and `chdir("..")` can still escape it.

Rename moves the existing dentry rather than replacing it with a copy. Cwds
inside a renamed subtree therefore reconstruct the new path. Replacement is
atomic under the tree lock; replaced open inodes remain usable; directory/file
type mismatches, non-empty target directories, descendant cycles, mountpoints,
and cross-mount moves receive distinct errors. `rm`, `rmdir`, and `mv` are now
enabled in the BusyBox shell, including recursive removal.

## Stage 3 links and directory-relative operations

The common pathname walker now expands absolute and parent-relative symbolic
link targets, limits expansion to 40 links, and reports `ELOOP` for cycles.
Final-component no-follow lookup backs `lstat()`, `readlink()`, and
`AT_SYMLINK_NOFOLLOW`; ordinary lookup follows the final link. `open()` also
honors `O_NOFOLLOW`, while a trailing slash follows a link and requires the
result to be a directory.

Ramfs supports symbolic links and hard links to non-directory inodes. Hard
links share inode number, contents, and link count, and unlinking one name does
not disturb the others. Cross-mount links report `EXDEV`, and directory hard
links report `EPERM`.

Open vnode descriptions retain the exact `vfs_path` they opened. This makes a
directory descriptor a stable lookup base even if its pathname is subsequently
renamed or removed. The Extron syscall and mlibc layers use that base for
`unlinkat()`, `renameat()`, `linkat()`, `symlinkat()`, `readlinkat()`, and
`fstatat()`. Absolute paths correctly ignore the supplied descriptor; relative
paths distinguish `EBADF` from `ENOTDIR`. BusyBox `ln` and `readlink` are now
enabled.

## Stage 4 credentials, permissions, and metadata

Each process carries real, effective, and saved user and group IDs, up to 16
supplementary groups, and a per-process umask. Fork inherits this state.
The Extron mlibc sysdeps expose the corresponding basic identity, group, and
umask operations. Signal delivery now also checks the sender and target
credentials rather than allowing every process to signal every other process.

Path traversal requires directory search permission. Open, creation, removal,
rename, and link operations enforce owner/group/other mode bits and parent
directory write/search permissions. `access()` uses real IDs while
`faccessat(..., AT_EACCESS)` uses effective IDs. Root retains the usual bypass,
except that an execute access check still requires at least one execute bit.
Sticky directories protect entries from unrelated non-root users. New entries
inherit the group of a setgid directory, and child directories retain its
setgid bit. Writing or truncating a file as non-root clears its set-ID bits.

Ramfs records owner, group, mode, atime, mtime, and ctime. Initrd mode,
ownership, and modification time are imported from tar headers. `chmod`,
`chown`, and the `utimensat` family update metadata through filesystem-neutral
VFS operations. The realtime clock is currently uptime plus a settable offset;
the monotonic clock is backed by the architectural timer. There is not yet a
persistent wall-clock source.

Directory descriptors now also back `openat()`, `mkdirat()`, `faccessat()`,
`fchmodat()`, `fchownat()`, and `utimensat()`. Descriptor forms include
`fchdir()`, `fchmod()`, `fchown()`, and `futimens()`. BusyBox enables `chmod`,
`chown`, `id`, `stat`, and `touch`. `mlibc_perm_test.elf` exercises credential
transitions, supplementary groups, permission denial, sticky and setgid
semantics, timestamps, symlink no-follow metadata, and the new `*at()` calls.

## Stage 5 devfs

`/dev` is a second, real VFS mount (`kernel/fs/devfs.c`), covered over a plain
ramfs directory the same way any future mount will be — not a set of special
cases in ramfs. It is a fixed namespace: `console`, `tty` (an alias of the same
inode, `nlink` 2), `null`, and `zero`. Every mutating `vfs_fs_ops` entry
(`create`/`remove`/`rename`/`link`/`symlink`) returns `-EROFS` rather than
being left null, since the VFS layer calls them unconditionally.

`file_table_init()` now opens `/dev/console` through the VFS for fd 0/1/2,
sharing one open-file description across all three exactly like any other
process's stdio — there is no more a `FILE_CONSOLE_IN`/`FILE_CONSOLE_OUT`
kind, and `file_is_tty()` recognizes the console by node identity instead.
`mlibc_devfs_test.elf` checks the mount, the two device semantics, and that
stdin really is `/dev/console` now.

## Stage 6 VFS-backed program loading

`execve()` and `proc_create_from_binary()` read the target ELF through the VFS
(`kernel/proc/exec.c`'s `load_binary_bytes()`) — open, `fstat` for size, one
`vfs_read()` into a kernel buffer — rather than pulling it out of the initrd
tar directly. Path resolution runs as the calling process's own cwd and
credentials (root's for a kernel-initiated boot spawn), and a real
`VFS_ACCESS_EXEC` check now gates it, which the old tar-only path never
enforced. Since ramfs already serves initrd-seeded files as ordinary nodes,
this is also the first point at which a file created or mounted after boot —
never present in the initrd — is directly executable. `mlibc_vfsexec_test.elf`
proves exactly that: it copies an existing ELF's bytes into a brand-new file
under `/tmp` and `execve()`s the copy.

`/bin`, `/etc`, and `/tmp` now exist as real top-level directories, created
before devfs mounts over `/dev`. `/bin` is not yet populated: BusyBox's own
`CONFIG_BUSYBOX_EXEC_PATH` is compiled in as `/sh`, so relocating it needs a
BusyBox rebuild with an updated path (or a `/sh` -> `/bin/sh` symlink once
something else lives at the real location) — left for a follow-up rather than
risked alongside everything else in this stage.

## Real mmap()/munmap() and two more devfs devices

`vfs_node_ops` gained an `mmap` hook: given an offset/length into a node,
hand back an EXISTING physical range to map plus whether it should be
cacheable. `sys_mmap()`/`sys_munmap()` (kernel/proc/syscall.c) are real
now — `MAP_ANONYMOUS` still goes through `vm_allocate_region()` (the same
call `SYS_ANON_ALLOC` already made for malloc), and an fd-backed mapping
goes through the new hook into `vm_map_region()`, the same call the
framebuffer and initrd views already used. `mmap()`'s six arguments don't
fit three registers, so they're bundled into one struct passed by pointer —
the same shape `SYS_PATH_AT` already uses for the same reason. `MAP_FIXED`
and demand paging are refused/absent, not silently pretended to work (see
deferred list).

Two more devfs nodes exist purely to have something real to `mmap()`:
`/dev/fb0` (open, `read()` a `struct extron_fb_geometry`, `mmap()` the
pixels) and `/dev/input` (`mmap()` the same keystroke ring the kernel's UART
ISR writes). Both replace what used to be exec-time special cases —
`PROC_MAP_FRAMEBUFFER`, `map_framebuffer_into()`, and the fixed
`USER_FB_VA`/`USER_INPUT_VA` mappings are gone; `proc_create_from_binary()`
and `execve()` take no flags at all now. DOOM opens and `mmap()`s both
devices itself in `DG_Init()`, the same way any program would.

The framebuffer mapping needed `VM_NOCACHE` (new, `kernel/include/kernel/mm/
paging.h`) — the GPU scans that memory continuously, so a write sitting
dirty in cache would be invisible to it. The input ring stays ordinarily
cacheable: it's real RAM shared with the kernel's own ISR, ordered by the
existing `dmb ish` barriers, not a second bus master. `mlibc_mmap_test.elf`
covers both backings plus `MAP_FIXED` rejection and the `-ENODEV` case for a
node with no `mmap` op (an ordinary ramfs file).

## setresuid()/setresgid()

Each sets the real/effective/saved triple independently, `-1` in any slot
meaning "leave it alone" — the tool a careful privilege drop needs and
`setuid()`/`seteuid()` can't express, since neither lets an unprivileged
process choose its own real id apart from its effective one, or park a value
in the saved id to `setresuid()` back to later. An unprivileged process
(`euid != 0`) may only move each requested id to one already present
somewhere in its OLD real/effective/saved triple; root may set any of the
three to anything. All three requested values are checked against the
original triple before any of them is written — a partial apply would leave
a process with a set of credentials nobody asked for. `mlibc_setresid_test.c`
covers the root path, the unprivileged-boundary rejections, and the fact that
losing euid also revokes the equivalent bypass for `setresgid()` — this
kernel's only privilege signal is `euid == 0`, so a group triple set while
still root does not survive a later drop to a non-root uid.

## Deliberately deferred

- access-control lists, capabilities, file flags, and a user/group database;
- filesystem-ID variants (`setfsuid()`/`setfsgid()`) — a Linux-only extra used
  almost exclusively by NFS-style servers checking access as a specific user
  without becoming signalable/ptraceable as them; low value here;
- a hardware-backed realtime clock (currently uptime plus a settable offset);
- moving BusyBox and the other initrd binaries into `/bin` (see Stage 6);
- persistent/block-backed storage and an unmount protocol;
- `mprotect()` — no `vfs_node_ops`/page-table path changes a mapping's
  permissions after creation;
- `munmap()` splitting a mapping — only a whole `vm_map_region()`/
  `vm_allocate_region()` region matching an exact base can be freed;
- `MAP_FIXED` — refused outright, no placement control over
  `vm_allocate_region()`'s first-fit;
- demand paging — every mapping is eagerly backed (fresh zeroed pages for
  `MAP_ANONYMOUS`, already-resident device memory otherwise); a large,
  sparsely-touched mapping costs its full size up front;
- file-backed `mmap()` over an ordinary ramfs file — `vfs_node_ops.mmap` only
  has real users backed by fixed, already-existing physical memory
  (`/dev/fb0`, `/dev/input`) so far, not RAM a filesystem owns;
- `MAP_SHARED` between unrelated processes — needs its own reference-counted
  backing store independent of any one `vm_space`, closer to a `shm_open()`
  subsystem than an `mmap()` detail; `fork()` still eagerly copies every
  owned page rather than sharing or copy-on-writing any of them.

`usr/mlibc_tests/mlibc_file_test.c` exercises the namespace stages end to end:
nested lookup and creation, `.`/`..`, cwd reconstruction and fork inheritance,
long paths, creation modes, seeded-file COW behavior, shared open-file offsets,
open-after-unlink lifetime, detached cwd recovery, atomic rename replacement,
subtree moves, hard-link lifetimes, relative and absolute symlink expansion,
loop rejection, final-component following rules, real directory-fd `*at()`
operations, and distinct POSIX failures through mlibc.
