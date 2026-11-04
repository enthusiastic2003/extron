#ifndef KERNEL_NET_SOCK_H
#define KERNEL_NET_SOCK_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/fs/socket.h>

/*
 * AF_INET sockets (kernel/net/inet_sock.c). kernel/fs/socket.c forwards to these
 * for descriptors whose usock carries an inet socket; the return conventions are
 * the same (0/count or a negative errno). Weak stubs (kernel/fs/socket_inet_stub.c)
 * answer -EAFNOSUPPORT in kernels without networking.
 */
#define INET_AF 2

void *inet_sock_new(int type, int protocol, long *err);   /* NULL and *err on failure */
void  inet_sock_free(void *is);
int   inet_sock_type(void *is);
long  inet_sock_bind(void *is, const void *addr, size_t len);
long  inet_sock_listen(void *is, int backlog);
long  inet_sock_connect(void *is, int oflags, const void *addr, size_t len);
long  inet_sock_accept(void *is, int oflags, void **new_is, void *addr, uint32_t *addrlen);
long  inet_sock_getname(void *is, void *addr, uint32_t *addrlen, int peer);
long  inet_sock_shutdown(void *is, int how);
long  inet_sock_sendmsg(struct proc *p, void *is, int oflags, const void *name, size_t namelen,
                        const struct sock_iov *iov, int cnt, int flags);
long  inet_sock_recvmsg(struct proc *p, void *is, int oflags, void *name, uint32_t *namelen,
                        const struct sock_iov *iov, int cnt, int flags, int *msg_flags);
void  inet_sock_poll(void *is, int events, short *revents);
long  inet_sock_opt(void *is, int set, int level, int name, void *val, uint32_t *len);
long  inet_sock_bytes_available(void *is);

#endif
