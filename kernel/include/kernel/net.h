#ifndef KERNEL_NET_H
#define KERNEL_NET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/sync/spinlock.h>

/*
 * Networking: lwIP (third_party/lwip) runs inside the kernel with no OS layer. One
 * lock, net_spinlock, guards every lwIP call and all socket state in
 * kernel/net/inet_sock.c. It is an IRQ-safe spinlock (interrupts off while held),
 * so the NIC's receive interrupt and the timer tick, which also enter lwIP, can
 * never run in the middle of a socket call. Blocking socket calls sleep on it.
 */
extern spinlock_t net_spinlock;

/* A network interface card as the stack sees it. The driver fills the first
 * five fields and calls net_nic_add(); `xmit` is called with net_spinlock held. */
struct net_nic {
    char     name[8];                  /* "eth0", set by net_nic_add() */
    uint8_t  mac[6];
    uint16_t mtu;                      /* usually 1500 */
    int    (*xmit)(struct net_nic *nic, const void *frame, size_t len);   /* 0 on success */
    void    *priv;                     /* driver's */
    void    *netif;                    /* lwIP's (opaque) */
};

void net_init(void);                                   /* idempotent */
int  net_nic_add(struct net_nic *nic);                 /* register + start DHCP */
void net_nic_rx(struct net_nic *nic, const void *frame, size_t len);   /* IRQ-safe */
void net_tick(void);                                   /* from the timer interrupt */
/* Push queued loopback frames through the stack now (net_spinlock held). */
void net_kick(void);

/* Configuration of the first NIC as DHCP (or nobody) set it, for /proc/net. */
struct net_config {
    bool     up, have_ip;
    uint8_t  mac[6];
    uint32_t ip, mask, gw, dns[2];     /* network byte order */
    char     name[8];
};
bool net_get_config(struct net_config *out);

#endif
