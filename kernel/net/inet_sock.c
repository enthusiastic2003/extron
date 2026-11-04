/*
 * AF_INET sockets (TCP and UDP) on top of lwIP's raw callback API.
 *
 * Everything runs under net_spinlock (kernel/net/net.c). lwIP calls its callbacks
 * from the NIC interrupt, from the timer tick and from inside our own calls, always
 * with that lock held, so a callback never sleeps or allocates: sockets and receive
 * nodes come from static pools, and state changes just set flags and wakeup(is).
 * Blocking calls sleep(is, &net_spinlock) and re-test their condition.
 *
 * The AF_UNIX code in kernel/fs/socket.c dispatches here for descriptors whose
 * usock carries an inet socket.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/errno.h>
#include <kernel/fs/file.h>
#include <kernel/fs/socket.h>
#include <kernel/klibc/string.h>
#include <kernel/net.h>
#include <kernel/net_sock.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>
#include <kernel/proc/syscall.h>
#include <kernel/drivers/timer.h>
#include <arch/irq_spinlock.h>

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_ICMP    1
#define SOCK_RAW        3

#define MSG_PEEK        0x0002
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_WAITALL     0x0100
#define MSG_NOSIGNAL    0x4000

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30

#define TCP_NODELAY     1
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6

#define POLLIN          0x001
#define POLLOUT         0x004
#define POLLERR         0x008
#define POLLHUP         0x010
#define POLLRDHUP       0x2000

#define EINPROGRESS     115
#define EALREADY        114
#define ENOBUFS         105
#define ECONNRESET      104
#define ECONNABORTED    103
#define ENETUNREACH     101
#define ETIMEDOUT       110
#define EHOSTUNREACH    113
#define EDESTADDRREQ    89
#define EADDRNOTAVAIL   99

#define MAX_INET_SOCKS  256
#define MAX_RX_NODES    1024
#define BACKLOG_MAX     32
#define UDP_MAX_QUEUED  64

struct rxnode {
    struct rxnode *next;
    struct pbuf *p;
    uint32_t off;                 /* bytes of p already consumed (stream) */
    ip_addr_t from;               /* datagram source */
    uint16_t from_port;           /* host order */
    bool used;
};

enum st { ST_INIT, ST_LISTEN, ST_CONNECTING, ST_CONNECTED, ST_CLOSED };

struct inet_sock {
    bool in_use;
    int type;
    enum st st;
    struct tcp_pcb *tpcb;
    struct udp_pcb *upcb;
    struct raw_pcb *rpcb;         /* SOCK_RAW with IPPROTO_ICMP (ping) */
    ip_addr_t raw_dst;            /* connect() on a raw socket */
    bool bound;                   /* explicit bind() done */
    bool udp_connected;           /* connect() done on a datagram or raw socket */
    bool fin_rcvd;                /* the peer closed its sending side (TCP) */
    bool shut_rd, shut_wr;
    int err;                      /* pending asynchronous error, an errno (positive) */
    struct rxnode *rx_head, *rx_tail;
    size_t rx_bytes;
    unsigned rx_count;
    struct inet_sock *backlog[BACKLOG_MAX];
    int nbacklog, backlog_cap;
    bool accepted;                /* handed to a descriptor (else it is the listener's) */
    uint32_t rcv_to_ms, snd_to_ms; /* SO_RCVTIMEO / SO_SNDTIMEO; 0 = wait forever */
};

static inline bool is_dgram(const struct inet_sock *is) {
    return is->type == SOCK_DGRAM || is->type == SOCK_RAW;
}

static struct inet_sock socks[MAX_INET_SOCKS];
static struct rxnode nodes[MAX_RX_NODES];

/* ------------------------------------------------------------------------ */
static struct rxnode *node_alloc(void) {
    for (int i = 0; i < MAX_RX_NODES; i++)
        if (!nodes[i].used) {
            memset(&nodes[i], 0, sizeof(nodes[i]));
            nodes[i].used = true;
            return &nodes[i];
        }
    return NULL;
}

static void node_free(struct rxnode *n) {
    if (n->p)
        pbuf_free(n->p);
    n->p = NULL;
    n->used = false;
}

static void rx_push(struct inet_sock *is, struct rxnode *n, size_t bytes) {
    n->next = NULL;
    if (is->rx_tail) is->rx_tail->next = n; else is->rx_head = n;
    is->rx_tail = n;
    is->rx_bytes += bytes;
    is->rx_count++;
}

static void rx_pop(struct inet_sock *is) {
    struct rxnode *n = is->rx_head;
    is->rx_head = n->next;
    if (!is->rx_head) is->rx_tail = NULL;
    is->rx_count--;
    node_free(n);
}

static void rx_flush(struct inet_sock *is) {
    while (is->rx_head)
        rx_pop(is);
    is->rx_bytes = 0;
}

static void wake(struct inet_sock *is) {
    wakeup(is);
    file_poll_notify();
}

static int err_to_errno(err_t e) {
    switch (e) {
        case ERR_OK:         return 0;
        case ERR_MEM:
        case ERR_BUF:        return ENOBUFS;
        case ERR_TIMEOUT:    return ETIMEDOUT;
        case ERR_RTE:        return ENETUNREACH;
        case ERR_INPROGRESS: return EINPROGRESS;
        case ERR_WOULDBLOCK: return EAGAIN;
        case ERR_USE:        return EADDRINUSE;
        case ERR_ALREADY:    return EALREADY;
        case ERR_ISCONN:     return EISCONN;
        case ERR_CONN:       return ENOTCONN;
        case ERR_ABRT:       return ECONNABORTED;
        case ERR_RST:        return ECONNRESET;
        case ERR_CLSD:       return EPIPE;
        case ERR_IF:         return EIO;
        default:             return EINVAL;
    }
}

/* ------------------------------------------------------------------------ */
/* lwIP callbacks (net_spinlock held)                                       */
/* ------------------------------------------------------------------------ */
static void tcp_err_cb(void *arg, err_t e) {
    struct inet_sock *is = arg;
    if (!is)
        return;
    is->tpcb = NULL;                                  /* lwIP already freed it */
    is->err = (is->st == ST_CONNECTING && e == ERR_RST) ? ECONNREFUSED : err_to_errno(e);
    is->st = ST_CLOSED;
    is->fin_rcvd = true;
    wake(is);
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t e) {
    struct inet_sock *is = arg;
    (void)e;
    if (!is) {                                        /* socket gone: drop it */
        if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); }
        return ERR_OK;
    }
    if (!p) {                                         /* the peer sent FIN */
        is->fin_rcvd = true;
        wake(is);
        return ERR_OK;
    }
    struct rxnode *n = node_alloc();
    if (!n)
        return ERR_MEM;                               /* lwIP keeps the data and retries */
    n->p = p;
    rx_push(is, n, p->tot_len);
    wake(is);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb; (void)len;
    if (arg)
        wake(arg);
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t e) {
    struct inet_sock *is = arg;
    (void)pcb;
    if (!is)
        return ERR_OK;
    if (e == ERR_OK)
        is->st = ST_CONNECTED;
    else
        is->err = err_to_errno(e);
    wake(is);
    return ERR_OK;
}

static struct inet_sock *sock_alloc(int type) {
    for (int i = 0; i < MAX_INET_SOCKS; i++)
        if (!socks[i].in_use) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].in_use = true;
            socks[i].type = type;
            return &socks[i];
        }
    return NULL;
}

static void tcp_attach(struct inet_sock *is, struct tcp_pcb *pcb) {
    is->tpcb = pcb;
    tcp_arg(pcb, is);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_sent(pcb, tcp_sent_cb);
    tcp_err(pcb, tcp_err_cb);
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t e) {
    struct inet_sock *l = arg;
    if (e != ERR_OK || !l || l->nbacklog >= l->backlog_cap)
        return ERR_MEM;                               /* lwIP aborts the new connection */
    struct inet_sock *c = sock_alloc(SOCK_STREAM);
    if (!c)
        return ERR_MEM;
    c->st = ST_CONNECTED;
    tcp_attach(c, newpcb);
    l->backlog[l->nbacklog++] = c;
    wake(l);
    return ERR_OK;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
    struct inet_sock *is = arg;
    (void)pcb;
    if (!is || is->rx_count >= UDP_MAX_QUEUED) {
        pbuf_free(p);
        return;
    }
    struct rxnode *n = node_alloc();
    if (!n) {
        pbuf_free(p);
        return;
    }
    n->p = p;
    n->from = *addr;
    n->from_port = port;
    rx_push(is, n, p->tot_len);
    wake(is);
}

/* ICMP for a raw socket: a copy of the whole IP packet, header included, as on Linux.
 * Returns 0 so lwIP still handles the packet itself (it answers echo requests). */
static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    struct inet_sock *is = arg;
    (void)pcb;
    if (!is || is->rx_count >= UDP_MAX_QUEUED)
        return 0;
    struct rxnode *n = node_alloc();
    if (!n)
        return 0;
    struct pbuf *copy = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!copy) {
        n->used = false;
        return 0;
    }
    pbuf_copy(copy, p);
    n->p = copy;
    n->from = *addr;
    n->from_port = 0;
    rx_push(is, n, copy->tot_len);
    wake(is);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Creation and teardown                                                    */
/* ------------------------------------------------------------------------ */
void *inet_sock_new(int type, int protocol, long *err) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) {
        *err = -ESOCKTNOSUPPORT;
        return NULL;
    }
    if (type == SOCK_RAW ? protocol != IPPROTO_ICMP
        : (protocol != 0 && !(type == SOCK_STREAM && protocol == IPPROTO_TCP)
           && !(type == SOCK_DGRAM && protocol == IPPROTO_UDP))) {
        *err = -EPROTONOSUPPORT;
        return NULL;
    }
    net_init();
    irq_spin_lock(&net_spinlock);
    struct inet_sock *is = sock_alloc(type);
    if (!is) {
        irq_spin_unlock(&net_spinlock);
        *err = -ENOBUFS;
        return NULL;
    }
    if (type == SOCK_STREAM) {
        struct tcp_pcb *pcb = tcp_new();
        if (!pcb) {
            is->in_use = false;
            irq_spin_unlock(&net_spinlock);
            *err = -ENOBUFS;
            return NULL;
        }
        tcp_attach(is, pcb);
    } else if (type == SOCK_RAW) {
        struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
        if (!pcb) {
            is->in_use = false;
            irq_spin_unlock(&net_spinlock);
            *err = -ENOBUFS;
            return NULL;
        }
        is->rpcb = pcb;
        raw_recv(pcb, raw_recv_cb, is);
    } else {
        struct udp_pcb *pcb = udp_new();
        if (!pcb) {
            is->in_use = false;
            irq_spin_unlock(&net_spinlock);
            *err = -ENOBUFS;
            return NULL;
        }
        is->upcb = pcb;
        udp_recv(pcb, udp_recv_cb, is);
    }
    irq_spin_unlock(&net_spinlock);
    *err = 0;
    return is;
}

/* An unconnected, unlistened socket that a listener never handed out. */
static void sock_teardown(struct inet_sock *is) {
    if (is->tpcb) {
        struct tcp_pcb *pcb = is->tpcb;
        tcp_arg(pcb, NULL);
        if (pcb->state == LISTEN) {                   /* lwIP forbids the others on a listener */
            tcp_accept(pcb, NULL);
        } else {
            tcp_recv(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
        }
        if (tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);
        is->tpcb = NULL;
    }
    if (is->upcb) {
        udp_recv(is->upcb, NULL, NULL);
        udp_remove(is->upcb);
        is->upcb = NULL;
    }
    if (is->rpcb) {
        raw_recv(is->rpcb, NULL, NULL);
        raw_remove(is->rpcb);
        is->rpcb = NULL;
    }
    rx_flush(is);
    for (int i = 0; i < is->nbacklog; i++) {
        struct inet_sock *c = is->backlog[i];
        if (c->tpcb) {
            struct tcp_pcb *pcb = c->tpcb;
            tcp_arg(pcb, NULL);
            tcp_abort(pcb);
            c->tpcb = NULL;
        }
        rx_flush(c);
        c->in_use = false;
    }
    is->nbacklog = 0;
    is->in_use = false;
}

void inet_sock_free(void *p) {
    struct inet_sock *is = p;
    irq_spin_lock(&net_spinlock);
    sock_teardown(is);
    wake(is);
    irq_spin_unlock(&net_spinlock);
}

int inet_sock_type(void *p) { return ((struct inet_sock *)p)->type; }

/* ------------------------------------------------------------------------ */
/* Addresses                                                                */
/* ------------------------------------------------------------------------ */
struct sockaddr_in_k {
    uint16_t family;
    uint16_t port;                 /* network order */
    uint32_t addr;                 /* network order */
    uint8_t zero[8];
};

static long parse_in(const void *addr, size_t len, ip_addr_t *ip, uint16_t *port) {
    const struct sockaddr_in_k *sa = addr;
    if (!addr || len < sizeof(*sa))
        return -EINVAL;
    if (sa->family != AF_INET)
        return -EAFNOSUPPORT;
    ip_addr_set_ip4_u32(ip, sa->addr);
    *port = lwip_ntohs(sa->port);
    return 0;
}

static uint32_t fill_in(void *out, uint32_t cap, const ip_addr_t *ip, uint16_t port) {
    struct sockaddr_in_k sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_INET;
    sa.port = lwip_htons(port);
    sa.addr = ip ? ip4_addr_get_u32(ip_2_ip4(ip)) : 0;
    uint32_t n = cap < sizeof(sa) ? cap : sizeof(sa);
    memcpy(out, &sa, n);
    return sizeof(sa);
}

/* ------------------------------------------------------------------------ */
/* bind / listen / connect / accept                                         */
/* ------------------------------------------------------------------------ */
long inet_sock_bind(void *p, const void *addr, size_t len) {
    struct inet_sock *is = p;
    ip_addr_t ip; uint16_t port;
    long r = parse_in(addr, len, &ip, &port);
    if (r < 0)
        return r;
    irq_spin_lock(&net_spinlock);
    if (is->bound || is->st != ST_INIT) {
        irq_spin_unlock(&net_spinlock);
        return -EINVAL;
    }
    err_t e = is->type == SOCK_STREAM ? tcp_bind(is->tpcb, &ip, port)
            : is->type == SOCK_RAW    ? raw_bind(is->rpcb, &ip)
                                      : udp_bind(is->upcb, &ip, port);
    if (e == ERR_OK)
        is->bound = true;
    irq_spin_unlock(&net_spinlock);
    return e == ERR_OK ? 0 : -err_to_errno(e);
}

long inet_sock_listen(void *p, int backlog) {
    struct inet_sock *is = p;
    irq_spin_lock(&net_spinlock);
    if (is->type != SOCK_STREAM || is->st != ST_INIT || !is->tpcb) {
        irq_spin_unlock(&net_spinlock);
        return -EOPNOTSUPP;
    }
    struct tcp_pcb *l = tcp_listen_with_backlog(is->tpcb, 255);
    if (!l) {
        irq_spin_unlock(&net_spinlock);
        return -ENOBUFS;
    }
    is->tpcb = l;                                     /* the listening pcb replaced it */
    tcp_arg(l, is);
    tcp_accept(l, tcp_accept_cb);
    is->st = ST_LISTEN;
    is->backlog_cap = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog;
    irq_spin_unlock(&net_spinlock);
    return 0;
}

static bool interrupted(void) { return signal_pending_unblocked(my_thread()); }

/* Absolute timer tick at which a wait of `ms` ends (0 = no limit). */
static uint64_t deadline_in(uint32_t ms) {
    if (!ms)
        return 0;
    uint64_t ticks = ((uint64_t)ms * timer_ticks_per_second() + 999) / 1000;
    return timer_ticks() + (ticks ? ticks : 1);
}

/* Sleep on the socket until woken or `deadline`. False if the deadline had already
 * passed (the caller returns EAGAIN/ETIMEDOUT); the caller re-tests its condition. */
static bool wait_for(struct inet_sock *is, uint64_t deadline) {
    struct thread *t = my_thread();
    if (deadline) {
        if (timer_ticks() >= deadline)
            return false;
        t->sleep_until = deadline;
    }
    sleep(is, &net_spinlock);
    t->sleep_until = 0;
    return true;
}

long inet_sock_connect(void *p, int oflags, const void *addr, size_t len) {
    struct inet_sock *is = p;
    ip_addr_t ip; uint16_t port;
    long r = parse_in(addr, len, &ip, &port);
    if (r < 0)
        return r;
    irq_spin_lock(&net_spinlock);
    if (is->type == SOCK_RAW) {
        is->raw_dst = ip;
        is->udp_connected = true;
        irq_spin_unlock(&net_spinlock);
        return 0;
    }
    if (is->type == SOCK_DGRAM) {
        err_t e = udp_connect(is->upcb, &ip, port);
        if (e == ERR_OK)
            is->udp_connected = true;
        irq_spin_unlock(&net_spinlock);
        return e == ERR_OK ? 0 : -err_to_errno(e);
    }
    if (is->st == ST_CONNECTED) { irq_spin_unlock(&net_spinlock); return -EISCONN; }
    if (is->st == ST_LISTEN)    { irq_spin_unlock(&net_spinlock); return -EINVAL; }
    if (is->st == ST_CONNECTING) {
        /* A second connect() while one is in flight (a nonblocking caller polling). */
        r = -EALREADY;
    } else if (!is->tpcb) {
        irq_spin_unlock(&net_spinlock);
        return is->err ? -is->err : -ECONNABORTED;
    } else {
        is->err = 0;
        is->st = ST_CONNECTING;
        err_t e = tcp_connect(is->tpcb, &ip, port, tcp_connected_cb);
        if (e != ERR_OK) {
            is->st = ST_INIT;
            irq_spin_unlock(&net_spinlock);
            return -err_to_errno(e);
        }
        tcp_output(is->tpcb);
        net_kick();
        r = -EINPROGRESS;
    }
    if (oflags & 04000 /* O_NONBLOCK */) {
        irq_spin_unlock(&net_spinlock);
        return r;
    }
    uint64_t cdl = deadline_in(is->snd_to_ms);
    while (is->st == ST_CONNECTING) {
        if (!wait_for(is, cdl)) {
            irq_spin_unlock(&net_spinlock);
            return -ETIMEDOUT;
        }
        if (interrupted()) {
            irq_spin_unlock(&net_spinlock);
            return -EINTR;
        }
    }
    if (is->st == ST_CONNECTED) {
        irq_spin_unlock(&net_spinlock);
        return 0;
    }
    int ee = is->err ? is->err : ECONNREFUSED;
    is->err = 0;
    irq_spin_unlock(&net_spinlock);
    return -ee;
}

long inet_sock_accept(void *p, int oflags, void **new_is, void *addr, uint32_t *addrlen) {
    struct inet_sock *l = p;
    irq_spin_lock(&net_spinlock);
    if (l->st != ST_LISTEN) {
        irq_spin_unlock(&net_spinlock);
        return -EINVAL;
    }
    uint64_t adl = deadline_in(l->rcv_to_ms);
    while (!l->nbacklog) {
        if (oflags & 04000) {
            irq_spin_unlock(&net_spinlock);
            return -EAGAIN;
        }
        if (!wait_for(l, adl)) {
            irq_spin_unlock(&net_spinlock);
            return -EAGAIN;
        }
        if (interrupted()) {
            irq_spin_unlock(&net_spinlock);
            return -EINTR;
        }
        if (l->st != ST_LISTEN) {
            irq_spin_unlock(&net_spinlock);
            return -EINVAL;
        }
    }
    struct inet_sock *c = l->backlog[0];
    for (int i = 1; i < l->nbacklog; i++)
        l->backlog[i - 1] = l->backlog[i];
    l->nbacklog--;
    c->accepted = true;
    if (addr && addrlen && c->tpcb)
        *addrlen = fill_in(addr, *addrlen, &c->tpcb->remote_ip, c->tpcb->remote_port);
    irq_spin_unlock(&net_spinlock);
    *new_is = c;
    return 0;
}

long inet_sock_getname(void *p, void *addr, uint32_t *addrlen, int peer) {
    struct inet_sock *is = p;
    irq_spin_lock(&net_spinlock);
    const ip_addr_t *ip = NULL;
    uint16_t port = 0;
    if (is->type == SOCK_STREAM && is->tpcb) {
        if (peer) {
            if (is->st != ST_CONNECTED) {
                irq_spin_unlock(&net_spinlock);
                return -ENOTCONN;
            }
            ip = &is->tpcb->remote_ip; port = is->tpcb->remote_port;
        } else {
            ip = &is->tpcb->local_ip; port = is->tpcb->local_port;
        }
    } else if (is->type == SOCK_RAW && is->rpcb) {
        if (peer && !is->udp_connected) {
            irq_spin_unlock(&net_spinlock);
            return -ENOTCONN;
        }
        ip = peer ? &is->raw_dst : &is->rpcb->local_ip;
    } else if (is->type == SOCK_DGRAM && is->upcb) {
        if (peer) {
            if (!is->udp_connected) {
                irq_spin_unlock(&net_spinlock);
                return -ENOTCONN;
            }
            ip = &is->upcb->remote_ip; port = is->upcb->remote_port;
        } else {
            ip = &is->upcb->local_ip; port = is->upcb->local_port;
        }
    } else if (peer) {
        irq_spin_unlock(&net_spinlock);
        return -ENOTCONN;
    }
    *addrlen = fill_in(addr, *addrlen, ip, port);
    irq_spin_unlock(&net_spinlock);
    return 0;
}

long inet_sock_shutdown(void *p, int how) {
    struct inet_sock *is = p;
    if (how < 0 || how > 2)
        return -EINVAL;
    irq_spin_lock(&net_spinlock);
    if (is->type == SOCK_STREAM && is->st != ST_CONNECTED && !is->fin_rcvd) {
        irq_spin_unlock(&net_spinlock);
        return -ENOTCONN;
    }
    if (is->type == SOCK_STREAM && is->tpcb)
        tcp_shutdown(is->tpcb, how == 0 || how == 2, how == 1 || how == 2);
    if (how == 0 || how == 2) is->shut_rd = true;
    if (how == 1 || how == 2) is->shut_wr = true;
    net_kick();
    wake(is);
    irq_spin_unlock(&net_spinlock);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                  */
/* ------------------------------------------------------------------------ */
static void sigpipe(struct proc *pr, int flags) {
    if (!(flags & MSG_NOSIGNAL))
        signal_send(pr, 13 /* SIGPIPE */);
}

static size_t iov_total(const struct sock_iov *iov, int cnt) {
    size_t t = 0;
    for (int i = 0; i < cnt; i++) t += iov[i].len;
    return t;
}

static long send_stream(struct proc *pr, struct inet_sock *is, int oflags,
                        const struct sock_iov *iov, int cnt, int flags) {
    bool nonblock = (oflags & 04000) || (flags & MSG_DONTWAIT);
    size_t total = iov_total(iov, cnt), sent = 0;
    int seg = 0;
    size_t seg_off = 0;
    irq_spin_lock(&net_spinlock);
    while (sent < total) {
        if (is->err && !is->tpcb) {
            int e = is->err;
            is->err = 0;
            irq_spin_unlock(&net_spinlock);
            return sent ? (long)sent : -e;
        }
        if (is->st == ST_CONNECTING && !nonblock) {
            sleep(is, &net_spinlock);
            if (interrupted()) { irq_spin_unlock(&net_spinlock); return sent ? (long)sent : -EINTR; }
            continue;
        }
        if (is->st != ST_CONNECTED && is->st != ST_CLOSED && is->st != ST_CONNECTING) {
            irq_spin_unlock(&net_spinlock);
            return sent ? (long)sent : -EPIPE;
        }
        if (is->st == ST_CONNECTING) {
            irq_spin_unlock(&net_spinlock);
            return sent ? (long)sent : -EAGAIN;
        }
        if (is->st == ST_CLOSED || is->shut_wr || !is->tpcb) {
            irq_spin_unlock(&net_spinlock);
            sigpipe(pr, flags);
            return sent ? (long)sent : -EPIPE;
        }
        u16_t room = tcp_sndbuf(is->tpcb);
        if (room == 0) {
            tcp_output(is->tpcb);
            net_kick();
            if (nonblock) {
                irq_spin_unlock(&net_spinlock);
                return sent ? (long)sent : -EAGAIN;
            }
            sleep(is, &net_spinlock);
            if (interrupted()) { irq_spin_unlock(&net_spinlock); return sent ? (long)sent : -EINTR; }
            continue;
        }
        while (seg < cnt && seg_off >= iov[seg].len) { seg++; seg_off = 0; }
        if (seg >= cnt)
            break;
        size_t take = iov[seg].len - seg_off;
        if (take > room) take = room;
        if (take > 8192) take = 8192;
        if (!syscall_user_range_ok(pr, (uint64_t)iov[seg].base + seg_off, take)) {
            irq_spin_unlock(&net_spinlock);
            return sent ? (long)sent : -EFAULT;
        }
        err_t e = tcp_write(is->tpcb, (const uint8_t *)iov[seg].base + seg_off, (u16_t)take,
                            TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM) {                          /* out of segments or pbufs: wait */
            tcp_output(is->tpcb);
            net_kick();
            if (nonblock) {
                irq_spin_unlock(&net_spinlock);
                return sent ? (long)sent : -EAGAIN;
            }
            sleep(is, &net_spinlock);
            if (interrupted()) { irq_spin_unlock(&net_spinlock); return sent ? (long)sent : -EINTR; }
            continue;
        }
        if (e != ERR_OK) {
            irq_spin_unlock(&net_spinlock);
            return sent ? (long)sent : -err_to_errno(e);
        }
        seg_off += take;
        sent += take;
    }
    if (is->tpcb) {
        tcp_output(is->tpcb);
        net_kick();
    }
    irq_spin_unlock(&net_spinlock);
    return (long)sent;
}

static long send_dgram(struct proc *pr, struct inet_sock *is, int oflags, const void *name,
                       size_t namelen, const struct sock_iov *iov, int cnt, int flags) {
    (void)oflags;
    size_t total = iov_total(iov, cnt);
    if (total > 65507)
        return -EMSGSIZE;
    ip_addr_t ip; uint16_t port = 0;
    bool have_dest = false;
    if (name && namelen) {
        long r = parse_in(name, namelen, &ip, &port);
        if (r < 0)
            return r;
        have_dest = true;
    }
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)total, PBUF_RAM);
    if (!pb)
        return -ENOBUFS;
    long got = sock_gather(pr, iov, cnt, 0, pb->payload, total);
    if (got < 0) {
        pbuf_free(pb);
        return got;
    }
    irq_spin_lock(&net_spinlock);
    err_t e;
    if (is->type == SOCK_RAW) {
        if (!have_dest && !is->udp_connected) {
            irq_spin_unlock(&net_spinlock);
            pbuf_free(pb);
            return -EDESTADDRREQ;
        }
        e = raw_sendto(is->rpcb, pb, have_dest ? &ip : &is->raw_dst);
    } else if (have_dest) {
        e = udp_sendto(is->upcb, pb, &ip, port);
    } else if (is->udp_connected) {
        e = udp_send(is->upcb, pb);
    } else {
        irq_spin_unlock(&net_spinlock);
        pbuf_free(pb);
        return -EDESTADDRREQ;
    }
    net_kick();
    irq_spin_unlock(&net_spinlock);
    pbuf_free(pb);
    (void)flags;
    return e == ERR_OK ? (long)total : -err_to_errno(e);
}

long inet_sock_sendmsg(struct proc *pr, void *p, int oflags, const void *name, size_t namelen,
                       const struct sock_iov *iov, int cnt, int flags) {
    struct inet_sock *is = p;
    if (is->st == ST_LISTEN)
        return -EINVAL;
    if (is->type == SOCK_STREAM) {
        if (name && namelen)
            return -EISCONN;
        return send_stream(pr, is, oflags, iov, cnt, flags);
    }
    return send_dgram(pr, is, oflags, name, namelen, iov, cnt, flags);
}

/* ------------------------------------------------------------------------ */
/* Receiving                                                                */
/* ------------------------------------------------------------------------ */
/* Copy `n` bytes starting at `off` of a pbuf chain into the iov, from slot `skip`. */
static long copy_to_iov(struct proc *pr, const struct sock_iov *iov, int cnt, size_t skip,
                        struct pbuf *p, size_t off, size_t n) {
    size_t done = 0;
    for (int i = 0; i < cnt && done < n; i++) {
        size_t len = iov[i].len;
        if (skip >= len) { skip -= len; continue; }
        size_t take = len - skip;
        if (take > n - done) take = n - done;
        if (!syscall_user_range_ok(pr, (uint64_t)iov[i].base + skip, take))
            return -EFAULT;
        pbuf_copy_partial(p, (uint8_t *)iov[i].base + skip, (u16_t)take, (u16_t)(off + done));
        done += take;
        skip = 0;
    }
    return (long)done;
}

static long recv_stream(struct proc *pr, struct inet_sock *is, int oflags,
                        const struct sock_iov *iov, int cnt, int flags) {
    size_t want = iov_total(iov, cnt), got = 0;
    bool nonblock = (oflags & 04000) || (flags & MSG_DONTWAIT);
    bool peek = flags & MSG_PEEK;
    irq_spin_lock(&net_spinlock);
    uint64_t rdl = deadline_in(is->rcv_to_ms);
    if (is->st == ST_INIT || is->st == ST_LISTEN) {
        irq_spin_unlock(&net_spinlock);
        return -ENOTCONN;
    }
    for (;;) {
        while (!is->rx_head && !is->fin_rcvd && !is->shut_rd) {
            if ((got && !(flags & MSG_WAITALL)) || nonblock || !wait_for(is, rdl)) {
                irq_spin_unlock(&net_spinlock);
                return got ? (long)got : -EAGAIN;
            }
            if (interrupted()) {
                irq_spin_unlock(&net_spinlock);
                return got ? (long)got : -EINTR;
            }
        }
        if (!is->rx_head || want == 0) {              /* EOF, or an error behind the data */
            if (!got && is->err) {
                int e = is->err;
                is->err = 0;
                irq_spin_unlock(&net_spinlock);
                return -e;
            }
            break;
        }
        struct rxnode *n = is->rx_head;
        size_t avail = n->p->tot_len - n->off;
        size_t take = avail < want - got ? avail : want - got;
        long w = copy_to_iov(pr, iov, cnt, got, n->p, n->off, take);
        if (w < 0) {
            irq_spin_unlock(&net_spinlock);
            return got ? (long)got : w;
        }
        if (!peek) {
            n->off += (uint32_t)w;
            is->rx_bytes -= (size_t)w;
            if (is->tpcb)
                tcp_recved(is->tpcb, (u16_t)w);       /* reopen the receive window */
            if (n->off >= n->p->tot_len)
                rx_pop(is);
        }
        got += (size_t)w;
        if (peek || got >= want)
            break;
        if (!is->rx_head && !(flags & MSG_WAITALL))
            break;
    }
    if (is->tpcb)
        net_kick();                                   /* a window update may be queued */
    irq_spin_unlock(&net_spinlock);
    return (long)got;
}

static long recv_dgram(struct proc *pr, struct inet_sock *is, int oflags, void *name,
                       uint32_t *namelen, const struct sock_iov *iov, int cnt, int flags,
                       int *msg_flags) {
    bool nonblock = (oflags & 04000) || (flags & MSG_DONTWAIT);
    irq_spin_lock(&net_spinlock);
    uint64_t rdl = deadline_in(is->rcv_to_ms);
    while (!is->rx_head) {
        if (is->err) {
            int e = is->err;
            is->err = 0;
            irq_spin_unlock(&net_spinlock);
            return -e;
        }
        if (nonblock || !wait_for(is, rdl)) {
            irq_spin_unlock(&net_spinlock);
            return -EAGAIN;
        }
        if (interrupted()) {
            irq_spin_unlock(&net_spinlock);
            return -EINTR;
        }
    }
    struct rxnode *n = is->rx_head;
    size_t want = iov_total(iov, cnt), len = n->p->tot_len;
    size_t take = len < want ? len : want;
    long w = copy_to_iov(pr, iov, cnt, 0, n->p, 0, take);
    if (w < 0) {
        irq_spin_unlock(&net_spinlock);
        return w;
    }
    if (name && namelen)
        *namelen = fill_in(name, *namelen, &n->from, n->from_port);
    if (len > want && msg_flags)
        *msg_flags |= MSG_TRUNC;
    if (!(flags & MSG_PEEK)) {
        is->rx_bytes -= len;
        rx_pop(is);
    }
    irq_spin_unlock(&net_spinlock);
    return (flags & MSG_TRUNC) ? (long)len : w;
}

long inet_sock_recvmsg(struct proc *pr, void *p, int oflags, void *name, uint32_t *namelen,
                       const struct sock_iov *iov, int cnt, int flags, int *msg_flags) {
    struct inet_sock *is = p;
    if (is->st == ST_LISTEN)
        return -EINVAL;
    if (is->type == SOCK_STREAM) {
        if (namelen)
            *namelen = 0;
        return recv_stream(pr, is, oflags, iov, cnt, flags);
    }
    return recv_dgram(pr, is, oflags, name, namelen, iov, cnt, flags, msg_flags);
}

/* ------------------------------------------------------------------------ */
/* poll, options, FIONREAD                                                  */
/* ------------------------------------------------------------------------ */
void inet_sock_poll(void *p, int events, short *revents) {
    struct inet_sock *is = p;
    irq_spin_lock(&net_spinlock);
    short r = 0;
    if (is_dgram(is)) {
        if (is->rx_head) r |= POLLIN;
        r |= POLLOUT;
        if (is->err) r |= POLLERR;
    } else if (is->st == ST_LISTEN) {
        if (is->nbacklog) r |= POLLIN;
    } else {
        bool eof = is->fin_rcvd || is->shut_rd;
        if (is->rx_head || eof) r |= POLLIN;
        if (is->st == ST_CONNECTED && is->tpcb && !is->shut_wr && tcp_sndbuf(is->tpcb) > 0)
            r |= POLLOUT;
        if (is->st == ST_INIT || (is->st == ST_CLOSED && !is->err))
            r |= POLLOUT;                             /* a failed connect() reports writable too */
        if (is->st == ST_CONNECTING)
            r &= (short)~POLLOUT;
        if (is->err) r |= POLLERR | POLLOUT;
        if (eof && (events & POLLRDHUP)) r |= POLLRDHUP;
        if (is->st == ST_CLOSED) r |= POLLHUP;
    }
    irq_spin_unlock(&net_spinlock);
    *revents = (short)(r & (events | POLLERR | POLLHUP));
}

long inet_sock_bytes_available(void *p) {
    struct inet_sock *is = p;
    irq_spin_lock(&net_spinlock);
    long n = 0;
    if (is->type == SOCK_STREAM) {
        n = (long)is->rx_bytes;
    } else if (is->rx_head) {
        n = (long)is->rx_head->p->tot_len;
    }
    irq_spin_unlock(&net_spinlock);
    return n;
}

long inet_sock_opt(void *p, int set, int level, int name, void *val, uint32_t *len) {
    struct inet_sock *is = p;
    int ival = 0;
    if (set && *len >= sizeof(int))
        memcpy(&ival, val, sizeof(int));
    irq_spin_lock(&net_spinlock);
    long r = 0;
    if (level == SOL_SOCKET) {
        if (set) {
            struct ip_pcb *ip = is->tpcb ? (struct ip_pcb *)is->tpcb
                              : is->rpcb ? (struct ip_pcb *)is->rpcb : (struct ip_pcb *)is->upcb;
            switch (name) {
                case SO_REUSEADDR: case SO_REUSEPORT:
                    if (ip) { if (ival) ip_set_option(ip, SOF_REUSEADDR); else ip_reset_option(ip, SOF_REUSEADDR); }
                    break;
                case SO_KEEPALIVE:
                    if (ip) { if (ival) ip_set_option(ip, SOF_KEEPALIVE); else ip_reset_option(ip, SOF_KEEPALIVE); }
                    break;
                case SO_BROADCAST:
                    if (ip) { if (ival) ip_set_option(ip, SOF_BROADCAST); else ip_reset_option(ip, SOF_BROADCAST); }
                    break;
                case SO_RCVTIMEO: case SO_SNDTIMEO: {
                    /* struct timeval { long sec; long usec; } */
                    if (*len >= 16) {
                        long tv[2];
                        memcpy(tv, val, sizeof(tv));
                        uint64_t ms = (uint64_t)tv[0] * 1000 + (uint64_t)tv[1] / 1000;
                        if (ms > 0xFFFFFFFFu) ms = 0xFFFFFFFFu;
                        if (name == SO_RCVTIMEO) is->rcv_to_ms = (uint32_t)ms;
                        else is->snd_to_ms = (uint32_t)ms;
                    }
                    break;
                }
                case SO_SNDBUF: case SO_RCVBUF: case SO_LINGER:
                    break;                            /* accepted, no effect */
                default:
                    r = -ENOPROTOOPT;
            }
        } else {
            switch (name) {
                case SO_TYPE:       ival = is->type; break;
                case SO_ERROR:      ival = is->err; is->err = 0; break;
                case SO_ACCEPTCONN: ival = is->st == ST_LISTEN; break;
                case SO_SNDBUF:     ival = TCP_SND_BUF; break;
                case SO_RCVBUF:     ival = TCP_WND; break;
                case SO_REUSEADDR: case SO_REUSEPORT:
                    ival = is->tpcb ? ip_get_option(is->tpcb, SOF_REUSEADDR) != 0 : 0; break;
                case SO_KEEPALIVE:
                    ival = is->tpcb ? ip_get_option(is->tpcb, SOF_KEEPALIVE) != 0 : 0; break;
                case SO_BROADCAST:  ival = 0; break;
                case SO_LINGER:
                    ival = 0; break;
                case SO_RCVTIMEO: case SO_SNDTIMEO: {
                    uint32_t ms = name == SO_RCVTIMEO ? is->rcv_to_ms : is->snd_to_ms;
                    if (*len >= 16) {
                        long tv[2] = { (long)(ms / 1000), (long)(ms % 1000) * 1000 };
                        memcpy(val, tv, sizeof(tv));
                        *len = 16;
                        irq_spin_unlock(&net_spinlock);
                        return 0;
                    }
                    ival = 0;
                    break;
                }
                default:            r = -ENOPROTOOPT;
            }
        }
    } else if (level == IPPROTO_TCP && is->type == SOCK_STREAM) {
        if (set) {
            switch (name) {
                case TCP_NODELAY:
                    if (is->tpcb) { if (ival) tcp_nagle_disable(is->tpcb); else tcp_nagle_enable(is->tpcb); }
                    break;
                case TCP_KEEPIDLE:
                    if (is->tpcb) is->tpcb->keep_idle = (u32_t)ival * 1000;
                    break;
                case TCP_KEEPINTVL:
                    if (is->tpcb) is->tpcb->keep_intvl = (u32_t)ival * 1000;
                    break;
                case TCP_KEEPCNT:
                    if (is->tpcb) is->tpcb->keep_cnt = (u32_t)ival;
                    break;
                default:
                    r = -ENOPROTOOPT;
            }
        } else {
            switch (name) {
                case TCP_NODELAY: ival = is->tpcb ? tcp_nagle_disabled(is->tpcb) != 0 : 0; break;
                case TCP_KEEPIDLE:  ival = is->tpcb ? (int)(is->tpcb->keep_idle / 1000) : 0; break;
                case TCP_KEEPINTVL: ival = is->tpcb ? (int)(is->tpcb->keep_intvl / 1000) : 0; break;
                case TCP_KEEPCNT:   ival = is->tpcb ? (int)is->tpcb->keep_cnt : 0; break;
                default:            r = -ENOPROTOOPT;
            }
        }
    } else if (level == IPPROTO_IP) {
        if (!set) ival = 0;                           /* IP_TOS/IP_TTL etc.: accepted, no effect */
    } else {
        r = -ENOPROTOOPT;
    }
    irq_spin_unlock(&net_spinlock);
    if (r == 0 && !set && *len >= sizeof(int)) {
        memcpy(val, &ival, sizeof(int));
        *len = sizeof(int);
    }
    return r;
}
