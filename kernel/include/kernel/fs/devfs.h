#ifndef KERNEL_FS_DEVFS_H
#define KERNEL_FS_DEVFS_H

#include <stdbool.h>
#include <kernel/fs/vfs.h>

/* Mounts a small, fixed-namespace device filesystem at /dev: console,
 * tty (an alias of console), null, and zero. Must run after ramfs_init()
 * has mounted root, and before any process is created (proc_init() opens
 * /dev/console for the console descriptors). */
void devfs_init(void);

/* True if `node` is the console/tty device — the one file_is_tty() and
 * sys_ioctl()'s termios/pgrp calls key off. */
bool devfs_is_console(struct vfs_node *node);

#endif
