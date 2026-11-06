#ifndef KERNEL_FS_SOCKET_H
#define KERNEL_FS_SOCKET_H

#include <stddef.h>
#include <stdint.h>

struct proc;
struct usock;
struct open_file;
struct vfs_path;

/* Same layout as struct iovec. */
struct sock_iov { void *base; size_t len; };

/* AF_UNIX sockets (stream, datagram, seqpacket). Everything returns 0/fd or
 * a negative errno, and works on descriptors of the calling process. */
long sock_create(struct proc *p, int domain, int type, int protocol);
long sock_pair(struct proc *p, int domain, int type, int protocol, int sv[2]);
long sock_bind(struct proc *p, int fd, const void *addr, size_t len);
long sock_listen(struct proc *p, int fd, int backlog);
long sock_accept(struct proc *p, int fd, void *addr, uint32_t *addrlen, int flags);
long sock_connect(struct proc *p, int fd, const void *addr, size_t len);
long sock_getname(struct proc *p, int fd, void *addr, uint32_t *addrlen, int peer);
long sock_shutdown(struct proc *p, int fd, int how);
#define SCM_MAX_FDS 8
/* sendmsg: `files` (nfiles descriptions, references held by the caller) travel with the first
 * byte; they are consumed whether or not the call succeeds. recvmsg: up to SCM_MAX_FDS
 * descriptions come back through files/nfiles (references owned by the caller), or are closed
 * if files is NULL. */
long sock_sendmsg(struct proc *p, int fd, const void *name, size_t namelen,
                  const struct sock_iov *iov, int cnt, int flags,
                  struct open_file **files, int nfiles);
long sock_recvmsg(struct proc *p, int fd, void *name, uint32_t *namelen,
                  const struct sock_iov *iov, int cnt, int flags, int *msg_flags,
                  struct open_file **files, int *nfiles);
long sock_bytes_available(struct usock *s);
long sock_opt(struct proc *p, int fd, int set, int level, int name, void *val, uint32_t *len);

/* Copy helpers shared with the INET backend: iov entries are user memory, re-validated
 * at the moment of the copy. gather: iov -> kernel buffer (after skipping `skip` bytes);
 * scatter: kernel buffer -> iov. Return bytes copied or -EFAULT. */
long sock_gather(struct proc *p, const struct sock_iov *iov, int cnt, size_t skip,
                 void *dst, size_t max);
long sock_scatter(struct proc *p, const struct sock_iov *iov, int cnt, const void *src, size_t n);

/* Descriptor-layer hooks. */
long sock_read(struct open_file *f, void *buffer, size_t count);
long sock_write(struct open_file *f, const void *buffer, size_t count);
void sock_poll(struct open_file *f, int events, short *revents);
void sock_release(struct usock *s);          /* last close of the description */
int  sock_type(const struct usock *s);

#endif
