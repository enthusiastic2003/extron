/* Kernels without a network stack (aarch64 today): AF_INET is unsupported. The real
 * implementations are in kernel/net/inet_sock.c, which overrides these weak ones. */
#include <kernel/errno.h>
#include <kernel/net.h>
#include <kernel/net_sock.h>

#define W __attribute__((weak))
W void *inet_sock_new(int t, int p, long *err) { (void)t; (void)p; *err = -EAFNOSUPPORT; return 0; }
W void  inet_sock_free(void *is) { (void)is; }
W int   inet_sock_type(void *is) { (void)is; return 0; }
W long  inet_sock_bind(void *is, const void *a, size_t l) { (void)is; (void)a; (void)l; return -EAFNOSUPPORT; }
W long  inet_sock_listen(void *is, int b) { (void)is; (void)b; return -EAFNOSUPPORT; }
W long  inet_sock_connect(void *is, int f, const void *a, size_t l) { (void)is; (void)f; (void)a; (void)l; return -EAFNOSUPPORT; }
W long  inet_sock_accept(void *is, int f, void **n, void *a, uint32_t *l) { (void)is; (void)f; (void)n; (void)a; (void)l; return -EAFNOSUPPORT; }
W long  inet_sock_getname(void *is, void *a, uint32_t *l, int p) { (void)is; (void)a; (void)l; (void)p; return -EAFNOSUPPORT; }
W long  inet_sock_shutdown(void *is, int h) { (void)is; (void)h; return -EAFNOSUPPORT; }
W long  inet_sock_sendmsg(struct proc *p, void *is, int f, const void *n, size_t nl,
                          const struct sock_iov *i, int c, int fl) {
    (void)p; (void)is; (void)f; (void)n; (void)nl; (void)i; (void)c; (void)fl; return -EAFNOSUPPORT; }
W long  inet_sock_recvmsg(struct proc *p, void *is, int f, void *n, uint32_t *nl,
                          const struct sock_iov *i, int c, int fl, int *mf) {
    (void)p; (void)is; (void)f; (void)n; (void)nl; (void)i; (void)c; (void)fl; (void)mf; return -EAFNOSUPPORT; }
W void  inet_sock_poll(void *is, int e, short *r) { (void)is; (void)e; *r = 0; }
W long  inet_sock_opt(void *is, int s, int l, int n, void *v, uint32_t *len) {
    (void)is; (void)s; (void)l; (void)n; (void)v; (void)len; return -ENOPROTOOPT; }
W long  inet_sock_bytes_available(void *is) { (void)is; return 0; }

/* /proc/net without a network stack: no interface. */
W bool net_get_config(struct net_config *out) { (void)out; return false; }
