#ifndef KERNEL_DRIVERS_PTY_H
#define KERNEL_DRIVERS_PTY_H

#include <kernel/fs/vfs.h>
#include <kernel/drivers/tty.h>

void pty_init(void);
struct vfs_node *pty_allocate_master(void);
struct tty *pty_get_slave(int index);
int pty_get_index(struct vfs_node *master);

#endif
