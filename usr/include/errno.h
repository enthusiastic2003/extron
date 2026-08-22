#ifndef ERRNO_H
#define ERRNO_H
/* A single global, not per-thread: this kernel has no threads. */
extern int errno;
#define EPERM   1
#define ENOENT  2
#define EIO     5
#define EBADF   9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define ENOSPC 28
#define EISDIR 21
#define EROFS  30
#define ERANGE 34
#endif
