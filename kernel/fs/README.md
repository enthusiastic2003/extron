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
and directories retain their requested creation mode as metadata.

The current namespace is append-only: namespace references are deliberately
not removed because `unlink`, `rmdir`, and `rename` are not implemented yet.
That keeps lifetime rules explicit instead of pretending deletion works.

## Deliberately not in Stage 1

- unlink/rmdir, rename, hard links, symlinks, and no-follow lookup rules;
- timestamps, umask, ownership changes, and permission enforcement;
- devfs and VFS-backed console/TTY device nodes;
- VFS-based executable loading (exec still reads an initrd image directly);
- persistent/block-backed storage and an unmount protocol.

`usr/mlibc_tests/mlibc_file_test.c` exercises the Stage 1 contract end to end:
nested lookup and creation, `.`/`..`, cwd reconstruction and fork inheritance,
long paths, creation modes, seeded-file COW behavior, shared open-file offsets,
and distinct `ENOENT`, `ENOTDIR`, and `EEXIST` failures through mlibc.
