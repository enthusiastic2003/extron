# Extron VFS

`vfs.c` is the filesystem-neutral boundary between process-facing file code
and concrete filesystems. The first root mount is ramfs, but descriptors and
syscalls no longer store or inspect `struct ramfs_node`.

There are two operation layers:

- `vfs_fs_ops` owns pathname namespace operations for a mount: open, lookup,
  mkdir, and resolving a directory for `chdir`.
- `vfs_node_ops` owns operations on a resolved object: read, write, readdir,
  and getattr.

An `open_file` contains a `vfs_node`, shared offset, and status flags. The
node provides stable metadata (`ino`, type, mode, link count, uid/gid, and
size) independent of its backing filesystem. Pipe and console descriptions
remain non-vnode kernel objects for now; devfs will move console devices into
the VFS later.

## Current root filesystem

Ramfs remains a single in-memory namespace lazily seeded from the initrd.
Seeded file contents are immutable views until the first write, at which point
ramfs allocates owned storage. The VFS migration intentionally preserves this
behavior and the existing pathname limit.

The current mount router contains one root mount. Adding devfs should replace
that single slot with longest-prefix mount lookup while leaving descriptor and
syscall code unchanged.

## Next layers

The next filesystem work should proceed through VFS operations rather than
ramfs calls:

1. component-by-component lookup with explicit parent validation;
2. unlink/rmdir and rename;
3. symlink/readlink and no-follow lookup rules;
4. timestamps, link counts, ownership, umask, and permission enforcement;
5. a devfs mount for `/dev/console`, `/dev/tty`, `/dev/null`, and `/dev/zero`;
6. a block-device/cache layer and an existing persistent filesystem.

`usr/mlibc_tests/mlibc_file_test.c` checks VFS-visible metadata and verifies
that pathname `stat` and descriptor `fstat` identify the same inode. The
kernel also has compile-time assertions for mlibc's AArch64 `struct stat`
layout; this migration exposed and fixed the previous 32-bit `st_nlink`
assumption that shifted `st_size` in userspace.
