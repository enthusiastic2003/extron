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

## Deliberately deferred

- access-control lists, capabilities, file flags, and a user/group database;
- complete identity APIs such as `setresuid()`/`setresgid()` and filesystem-ID
  variants;
- devfs and VFS-backed console/TTY device nodes;
- VFS-based executable loading (exec still reads an initrd image directly);
- persistent/block-backed storage and an unmount protocol.

`usr/mlibc_tests/mlibc_file_test.c` exercises the namespace stages end to end:
nested lookup and creation, `.`/`..`, cwd reconstruction and fork inheritance,
long paths, creation modes, seeded-file COW behavior, shared open-file offsets,
open-after-unlink lifetime, detached cwd recovery, atomic rename replacement,
subtree moves, hard-link lifetimes, relative and absolute symlink expansion,
loop rejection, final-component following rules, real directory-fd `*at()`
operations, and distinct POSIX failures through mlibc.
