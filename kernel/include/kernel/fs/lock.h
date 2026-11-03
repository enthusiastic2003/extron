#ifndef KERNEL_FS_LOCK_H
#define KERNEL_FS_LOCK_H

#include <stdint.h>

struct proc;
struct open_file;
struct vfs_node;

/* BSD flock(2): LOCK_SH 1, LOCK_EX 2, LOCK_NB 4, LOCK_UN 8. The lock belongs
 * to the open file description, so dup()ed and inherited descriptors share
 * it and it goes away with the last close. */
long lock_flock(struct proc *p, int fd, int op);

/* POSIX record locks (fcntl F_GETLK/F_SETLK/F_SETLKW), owned by the process.
 * `flock_user` points at a Linux x86-64 struct flock in the caller's memory. */
long lock_fcntl(struct proc *p, int fd, int cmd, void *flock_user);

/* Lifetime hooks called from the descriptor layer. */
void lock_release_file(struct open_file *f);                 /* last close of a description */
void lock_release_proc_node(struct proc *p, struct vfs_node *node); /* any close by p of this file */

#endif
