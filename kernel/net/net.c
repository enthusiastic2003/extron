/*
 * Kernel side of the network stack: lwIP initialisation, the glue between a NIC
 * driver and an lwIP netif, DHCP, the timer tick. Sockets are in inet_sock.c.
 */
#include <kernel/net.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <kernel/drivers/timer.h>
#include <arch/irq_spinlock.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

void kernel_random_fill(void *buf, size_t len);

spinlock_t net_spinlock = SPINLOCK_INIT;
static bool inited;
static struct net_nic *first_nic;

/* ---- lwIP platform hooks ------------------------------------------------- */
unsigned long net_random(void) {
    uint32_t v;
    kernel_random_fill(&v, sizeof(v));
    return v;
}

void net_assert_fail(const char *msg, const char *file, int line) {
    panic("lwIP assertion failed: %s (%s:%d)", msg, file, line);
}

u32_t sys_now(void) { return (u32_t)timer_uptime_ms(); }

int atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ---- interfaces ------------------------------------------------------------- */
static err_t nic_linkoutput(struct netif *ni, struct pbuf *p) {
    struct net_nic *nic = ni->state;
    static uint8_t frame[2048];
    if (p->tot_len > sizeof(frame))
        return ERR_IF;
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    return nic->xmit(nic, frame, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

static err_t nic_netif_init(struct netif *ni) {
    struct net_nic *nic = ni->state;
    ni->name[0] = 'e';
    ni->name[1] = 't';
    ni->output = etharp_output;
    ni->linkoutput = nic_linkoutput;
    ni->mtu = nic->mtu ? nic->mtu : 1500;
    ni->hwaddr_len = 6;
    memcpy(ni->hwaddr, nic->mac, 6);
    ni->hostname = "extron";
    ni->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
              | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void netif_status(struct netif *ni) {
    if (netif_is_up(ni) && !ip4_addr_isany_val(*netif_ip4_addr(ni))) {
        const ip_addr_t *dns = dns_getserver(0);
        char ip[16], mask[16], gw[16], dnsb[16];     /* ip4addr_ntoa() shares one buffer */
        ip4addr_ntoa_r(netif_ip4_addr(ni), ip, sizeof(ip));
        ip4addr_ntoa_r(netif_ip4_netmask(ni), mask, sizeof(mask));
        ip4addr_ntoa_r(netif_ip4_gw(ni), gw, sizeof(gw));
        kprintf("net: %s up, %s/%s gw %s", first_nic ? first_nic->name : "?", ip, mask, gw);
        if (dns && !ip_addr_isany(dns)) {
            ip4addr_ntoa_r(ip_2_ip4(dns), dnsb, sizeof(dnsb));
            kprintf(" dns %s", dnsb);
        }
        kprintf("\n");
    }
}

void net_init(void) {
    if (inited)
        return;
    irq_spin_lock(&net_spinlock);
    lwip_init();                     /* also brings up the loopback interface 127.0.0.1 */
    inited = true;
    irq_spin_unlock(&net_spinlock);
}

int net_nic_add(struct net_nic *nic) {
    net_init();
    struct netif *ni = kmalloc(sizeof(*ni));
    if (!ni)
        return -1;
    memset(ni, 0, sizeof(*ni));
    nic->netif = ni;
    irq_spin_lock(&net_spinlock);
    if (!netif_add(ni, NULL, NULL, NULL, nic, nic_netif_init, netif_input)) {
        irq_spin_unlock(&net_spinlock);
        kfree(ni);
        return -1;
    }
    /* "eth0", "eth1", ...: counted here (lwIP's own numbering starts at the loopback). */
    static int nics;
    nic->name[0] = 'e'; nic->name[1] = 't'; nic->name[2] = 'h';
    nic->name[3] = (char)('0' + (nics++ % 10));
    nic->name[4] = 0;
    if (!first_nic)
        first_nic = nic;
    kprintf("net: %s %02x:%02x:%02x:%02x:%02x:%02x, requesting an address (DHCP)\n", nic->name,
            nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3], nic->mac[4], nic->mac[5]);
    netif_set_default(ni);
    netif_set_status_callback(ni, netif_status);
    netif_set_up(ni);
    netif_set_link_up(ni);
    dhcp_start(ni);
    irq_spin_unlock(&net_spinlock);
    return 0;
}

void net_nic_rx(struct net_nic *nic, const void *frame, size_t len) {
    struct netif *ni = nic->netif;
    if (!ni || !len)
        return;
    irq_spin_lock(&net_spinlock);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (p) {
        pbuf_take(p, frame, (u16_t)len);
        if (ni->input(p, ni) != ERR_OK)
            pbuf_free(p);
    }
    irq_spin_unlock(&net_spinlock);
}

void net_kick(void) {
    netif_poll_all();
}

void net_tick(void) {
    if (!inited)
        return;
    irq_spin_lock(&net_spinlock);
    netif_poll_all();                /* loopback frames queued by senders */
    sys_check_timeouts();
    irq_spin_unlock(&net_spinlock);
}

bool net_get_config(struct net_config *out) {
    memset(out, 0, sizeof(*out));
    if (!first_nic || !first_nic->netif)
        return false;
    irq_spin_lock(&net_spinlock);
    struct netif *ni = first_nic->netif;
    memcpy(out->name, first_nic->name, sizeof(out->name));
    memcpy(out->mac, first_nic->mac, 6);
    out->up = netif_is_up(ni);
    out->ip = ip4_addr_get_u32(netif_ip4_addr(ni));
    out->mask = ip4_addr_get_u32(netif_ip4_netmask(ni));
    out->gw = ip4_addr_get_u32(netif_ip4_gw(ni));
    out->have_ip = out->ip != 0;
    for (int i = 0; i < 2; i++) {
        const ip_addr_t *d = dns_getserver((u8_t)i);
        out->dns[i] = d ? ip4_addr_get_u32(ip_2_ip4(d)) : 0;
    }
    irq_spin_unlock(&net_spinlock);
    return true;
}
