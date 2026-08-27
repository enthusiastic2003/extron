#ifndef KERNEL_ERRNO_H
#define KERNEL_ERRNO_H

/* Linux/mlibc-compatible errno values. Kernel interfaces return their
 * negative form; Extron's mlibc sysdeps translate -result to errno. */
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define EBADF           9
#define ECHILD         10
#define EAGAIN         11
#define ENOMEM         12
#define EACCES         13
#define EFAULT         14
#define EBUSY          16
#define EEXIST         17
#define EXDEV          18
#define ENODEV         19
#define ENOTDIR        20
#define EISDIR         21
#define EINVAL         22
#define ENOTTY         25
#define EMFILE         24
#define ENOSPC         28
#define ESPIPE         29
#define EROFS          30
#define EPIPE          32
#define ERANGE         34
#define ENAMETOOLONG   36
#define ENOSYS         38
#define ENOTEMPTY      39
#define ELOOP          40
#define EOPNOTSUPP     95

#endif
