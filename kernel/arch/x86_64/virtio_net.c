/*
 * virtio-net, legacy (transitional) interface over the I/O-port BAR, the same
 * way virtio_blk.c drives the disk. Two queues (0 = receive, 1 = transmit), no
 * offloads, no multi-buffer receive: every frame is one descriptor whose first
 * 10 bytes are the virtio-net header. Receive is interrupt-driven and hands
 * frames to the stack (kernel/net/net.c); transmit copies a frame into one of a
 * few fixed buffers and reclaims them lazily.
 *
 * QEMU: -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on
 */
#include <kernel/net.h>
#include <kernel/console.h>
#include <kernel/klibc/string.h>
#include <kernel/mm/paging.h>
#include <arch/irq.h>
#include "pci.h"
#include "io.h"

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_NET_LEGACY   0x1000
#define VIRTIO_NET_MODERN   0x1041

#define VIO_HOST_FEATURES   0x00
#define VIO_GUEST_FEATURES  0x04
#define VIO_QUEUE_PFN       0x08
#define VIO_QUEUE_SIZE      0x0C
#define VIO_QUEUE_SELECT    0x0E
#define VIO_QUEUE_NOTIFY    0x10
#define VIO_STATUS          0x12
#define VIO_ISR             0x13
#define VIO_NET_MAC         0x14        /* 6 bytes */

#define ST_ACK              1
#define ST_DRIVER           2
#define ST_DRIVER_OK        4
#define ST_FAILED           0x80

#define F_NET_MAC           (1u << 5)

#define DESC_WRITE          2           /* the device writes into this buffer */

#define MAX_QSZ             512
#define VQ_BYTES            (5 * 4096)
#define NET_HDR             10          /* struct virtio_net_hdr without mergeable buffers */
#define BUF_SIZE            2048
#define RX_BUFS             64
#define TX_BUFS             32
#define PCI_INT_LINE        0x3C

struct vring_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };

struct vq {
    uint16_t qsz, avail_idx, last_used;
    struct vring_desc *desc;
    volatile uint16_t *avail_idxp, *avail_ring;
    volatile uint16_t *used_idxp;
    volatile uint32_t *used_ring;       /* pairs of {id, len} */
};

static uint8_t rx_mem[VQ_BYTES] __attribute__((aligned(4096)));
static uint8_t tx_mem[VQ_BYTES] __attribute__((aligned(4096)));
static uint8_t rx_buf[RX_BUFS][BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t tx_buf[TX_BUFS][BUF_SIZE] __attribute__((aligned(4096)));

static uint16_t io_base;
static struct vq rxq, txq;
static uint32_t tx_free_mask;           /* bit i set: tx_buf[i] is free */
static struct net_nic nic;

static uint64_t phys_of(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return kvirt_to_phys(v & ~0xFFFULL) + (v & 0xFFF);
}

static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

/* Select queue `index`, read its size, lay the legacy split ring out in `mem`. */
static bool vq_setup(struct vq *q, int index, uint8_t *mem) {
    outw(io_base + VIO_QUEUE_SELECT, (uint16_t)index);
    q->qsz = inw(io_base + VIO_QUEUE_SIZE);
    if (q->qsz == 0 || q->qsz > MAX_QSZ) {
        kprintf("virtio-net: unsupported queue %d size %u\n", index, q->qsz);
        return false;
    }
    size_t avail_off = 16u * q->qsz;
    size_t used_off  = (avail_off + 6u + 2u * q->qsz + 4095) & ~4095u;
    memset(mem, 0, VQ_BYTES);
    q->desc       = (struct vring_desc *)mem;
    q->avail_idxp = (volatile uint16_t *)(mem + avail_off) + 1;
    q->avail_ring = (volatile uint16_t *)(mem + avail_off) + 2;
    q->used_idxp  = (volatile uint16_t *)(mem + used_off + 2);
    q->used_ring  = (volatile uint32_t *)(mem + used_off + 4);
    q->avail_idx = q->last_used = 0;
    outl(io_base + VIO_QUEUE_PFN, (uint32_t)(phys_of(mem) >> 12));
    return true;
}

static void rx_post(uint16_t id) {
    rxq.avail_ring[rxq.avail_idx % rxq.qsz] = id;
    rxq.avail_idx++;
}

static void irq(struct arch_frame *f) {
    (void)f;
    uint8_t isr = inb(io_base + VIO_ISR);         /* read-to-clear: drops the line */
    if (!(isr & 1))
        return;                                   /* not ours (the line is shared) */
    barrier();

    /* Frames received. */
    bool posted = false;
    while (rxq.last_used != *rxq.used_idxp) {
        uint16_t slot = rxq.last_used % rxq.qsz;
        uint32_t id = rxq.used_ring[2 * slot], len = rxq.used_ring[2 * slot + 1];
        rxq.last_used++;
        if (id < RX_BUFS && len > NET_HDR && len <= BUF_SIZE)
            net_nic_rx(&nic, rx_buf[id] + NET_HDR, len - NET_HDR);
        if (id < RX_BUFS) {
            rx_post((uint16_t)id);
            posted = true;
        }
    }
    if (posted) {
        barrier();
        *rxq.avail_idxp = rxq.avail_idx;
        outw(io_base + VIO_QUEUE_NOTIFY, 0);
    }

    /* Transmitted buffers are free again. */
    while (txq.last_used != *txq.used_idxp) {
        uint32_t id = txq.used_ring[2 * (txq.last_used % txq.qsz)];
        txq.last_used++;
        if (id < TX_BUFS)
            tx_free_mask |= 1u << id;
    }
}

/* Called by the stack with net_spinlock held. */
static int xmit(struct net_nic *n, const void *frame, size_t len) {
    (void)n;
    if (len == 0 || len + NET_HDR > BUF_SIZE)
        return -1;
    /* Reclaim anything the device has finished with (the interrupt does this too). */
    while (txq.last_used != *txq.used_idxp) {
        uint32_t id = txq.used_ring[2 * (txq.last_used % txq.qsz)];
        txq.last_used++;
        if (id < TX_BUFS)
            tx_free_mask |= 1u << id;
    }
    if (!tx_free_mask)
        return -1;                                /* ring full: drop, TCP retransmits */
    unsigned id = (unsigned)__builtin_ctz(tx_free_mask);
    tx_free_mask &= ~(1u << id);
    memset(tx_buf[id], 0, NET_HDR);
    memcpy(tx_buf[id] + NET_HDR, frame, len);
    txq.desc[id].addr = phys_of(tx_buf[id]);
    txq.desc[id].len = (uint32_t)(len + NET_HDR);
    txq.desc[id].flags = 0;
    txq.desc[id].next = 0;
    txq.avail_ring[txq.avail_idx % txq.qsz] = (uint16_t)id;
    txq.avail_idx++;
    barrier();
    *txq.avail_idxp = txq.avail_idx;
    outw(io_base + VIO_QUEUE_NOTIFY, 1);
    return 0;
}

static bool start_device(struct pci_loc loc) {
    uint32_t bar0 = pci_read32(loc, PCI_BAR0);
    if (!(bar0 & 1)) {
        kprintf("virtio-net: BAR0 is memory-mapped (modern-only device); "
                "start QEMU with disable-modern=on\n");
        return false;
    }
    io_base = (uint16_t)(bar0 & ~3u);
    pci_write16(loc, PCI_COMMAND, pci_read16(loc, PCI_COMMAND) | PCI_CMD_IO | PCI_CMD_BUSMASTER);

    unsigned line = pci_read32(loc, PCI_INT_LINE) & 0xFF;
    if (line >= 16) {
        kprintf("virtio-net: no usable interrupt line (%u)\n", line);
        return false;
    }

    outb(io_base + VIO_STATUS, 0);                              /* reset */
    outb(io_base + VIO_STATUS, ST_ACK);
    outb(io_base + VIO_STATUS, ST_ACK | ST_DRIVER);
    uint32_t host = inl(io_base + VIO_HOST_FEATURES);
    if (!(host & F_NET_MAC)) {
        kprintf("virtio-net: device has no MAC address\n");
        outb(io_base + VIO_STATUS, ST_FAILED);
        return false;
    }
    outl(io_base + VIO_GUEST_FEATURES, F_NET_MAC);
    for (int i = 0; i < 6; i++)
        nic.mac[i] = inb(io_base + VIO_NET_MAC + i);

    if (!vq_setup(&rxq, 0, rx_mem) || !vq_setup(&txq, 1, tx_mem)) {
        outb(io_base + VIO_STATUS, ST_FAILED);
        return false;
    }
    for (int i = 0; i < RX_BUFS && i < rxq.qsz; i++) {
        rxq.desc[i].addr = phys_of(rx_buf[i]);
        rxq.desc[i].len = BUF_SIZE;
        rxq.desc[i].flags = DESC_WRITE;
        rxq.desc[i].next = 0;
        rx_post((uint16_t)i);
    }
    tx_free_mask = TX_BUFS >= 32 ? 0xFFFFFFFFu : ((1u << TX_BUFS) - 1);
    barrier();
    *rxq.avail_idxp = rxq.avail_idx;

    register_irq_handler(line, irq);
    if (line >= 8)                        /* PCI INTx is level-triggered: set ELCR */
        outb(0x4D1, inb(0x4D1) | (1u << (line - 8)));
    else
        outb(0x4D0, inb(0x4D0) | (1u << line));
    arch_irq_enable_line(line);

    outb(io_base + VIO_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);
    outw(io_base + VIO_QUEUE_NOTIFY, 0);
    kprintf("virtio-net: io 0x%x, IRQ %u, queues %u/%u\n", io_base, line, rxq.qsz, txq.qsz);
    return true;
}

/* Find the virtio NIC and register it with the stack. Absent hardware is not an error:
 * the loopback interface still works. */
void arch_probe_network(void) {
    net_init();
    struct pci_loc loc;
    if (!pci_find(VIRTIO_VENDOR, VIRTIO_NET_LEGACY, VIRTIO_NET_LEGACY, &loc)) {
        if (pci_find(VIRTIO_VENDOR, VIRTIO_NET_MODERN, VIRTIO_NET_MODERN, &loc))
            kprintf("virtio-net: found a modern-only device, which this driver "
                    "does not support; use disable-modern=on\n");
        return;
    }
    if (!start_device(loc))
        return;
    nic.mtu = 1500;
    nic.xmit = xmit;
    net_nic_add(&nic);
}
