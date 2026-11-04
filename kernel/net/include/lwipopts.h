/*
 * lwIP configuration for the Extron kernel: IPv4 only, no OS layer (NO_SYS),
 * raw callback API (kernel/net/inet_sock.c turns it into BSD sockets), DHCP in
 * the kernel, name resolution left to userspace (mlibc's resolver over UDP).
 * All lwIP calls happen under net_lock (kernel/net/net.c).
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
/* Every lwIP call is made with net_lock held: no per-allocation protection. */
#define SYS_LIGHTWEIGHT_PROT        0
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define LWIP_TIMERS                 1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_NETIF_API              0

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_ACD                    0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
/* Only to remember the DNS servers DHCP hands out (userspace does the lookups). */
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2
#define LWIP_DHCP_PROVIDE_DNS_SERVERS 2

#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_LOOPBACK_MAX_PBUFS     16
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

#define SO_REUSE                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
#define LWIP_CHECKSUM_ON_COPY       0

/* Memory: everything comes from static pools in the kernel's .bss. */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (512 * 1024)
#define MEMP_NUM_PBUF               128
#define PBUF_POOL_SIZE              256
#define PBUF_POOL_BUFSIZE           1536
#define MEMP_NUM_UDP_PCB            32
#define MEMP_NUM_TCP_PCB            64
#define MEMP_NUM_TCP_PCB_LISTEN     16
#define MEMP_NUM_TCP_SEG            256
#define MEMP_NUM_RAW_PCB            8
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

#define TCP_MSS                     1460
#define TCP_WND                     (24 * TCP_MSS)
#define TCP_SND_BUF                 (24 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_LISTEN_BACKLOG          0
#define LWIP_TCP_SACK_OUT           0

/* Hooks into the kernel (defined in kernel/net/net.c). */
unsigned long net_random(void);
#define LWIP_RAND()                 ((u32_t)net_random())
void net_assert_fail(const char *msg, const char *file, int line);
#define LWIP_PLATFORM_ASSERT(x)     net_assert_fail((x), __FILE__, __LINE__)
#define LWIP_PLATFORM_DIAG(x)       do {} while (0)

#endif
