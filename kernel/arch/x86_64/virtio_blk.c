/*
 * virtio-blk driver (legacy / "transitional" PCI interface, I/O-port BAR0),
 * interrupt-driven, one request in flight. Exposes the disk as a struct ext2_block_dev
 * and mounts an ext2 filesystem found on it at /mnt.
 *
 * Legacy interface only: QEMU needs `-device virtio-blk-pci,...,disable-modern=on`
 * (PCIe slots default to modern-only). A modern-only device is detected and
 * reported, not driven.
 *
 * DMA memory is static .bss: the kernel image is loaded physically
 * contiguous, so a page-aligned static array is a contiguous DMA region and
 * needs no physical allocator support.
 *
 * Completion: the CPU halts (sti;hlt) until the device's INTx interrupt fires, with
 * preemption held off: no polling, but also no thread switch while the request is
 * in flight, because the filesystem above is not written to be re-entered by
 * another thread mid-operation. The IRQ handler reads the ISR register (which also
 * deasserts the level-triggered line) and retires the used-ring entry. A 5 s
 * timeout resets the device.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/console.h>
#include <kernel/drivers/timer.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <arch/exceptions.h>
#include <arch/irq.h>
#include <kernel/fs/ext2.h>
#include <kernel/blockdev.h>
#include <kernel/fs/vfs.h>
#include <kernel/klibc/string.h>
#include <kernel/mm/paging.h>
#include <arch/irq_spinlock.h>
#include "pci.h"
#include "io.h"

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_BLK_LEGACY   0x1001      /* transitional / legacy-only block device */
#define VIRTIO_BLK_MODERN   0x1042      /* non-transitional */

/* Legacy register offsets from the I/O BAR. */
#define VIO_HOST_FEATURES   0x00
#define VIO_GUEST_FEATURES  0x04
#define VIO_QUEUE_PFN       0x08
#define VIO_QUEUE_SIZE      0x0C
#define VIO_QUEUE_SELECT    0x0E
#define VIO_QUEUE_NOTIFY    0x10
#define VIO_STATUS          0x12
#define VIO_ISR             0x13
#define VIO_BLK_CAPACITY    0x14        /* u64, in 512-byte sectors */

#define ST_ACK              1
#define ST_DRIVER           2
#define ST_DRIVER_OK        4
#define ST_FAILED           0x80

#define F_BLK_RO            (1u << 5)
#define F_BLK_FLUSH         (1u << 9)

#define T_IN                0
#define T_OUT               1
#define T_FLUSH             4

#define DESC_NEXT           1
#define DESC_WRITE          2           /* device writes into this buffer */

#define MAX_QSZ             512
#define BOUNCE_SECTORS      128
#define PCI_INT_LINE        0x3C
#define REQ_TIMEOUT_MS      5000

struct vring_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };

static uint8_t  vq_mem[5 * 4096] __attribute__((aligned(4096)));
static uint8_t  bounce[BOUNCE_SECTORS * 512] __attribute__((aligned(4096)));
static struct { uint32_t type, reserved; uint64_t sector; }
                req_hdr __attribute__((aligned(16)));
static volatile uint8_t req_status;

static uint16_t io_base;
static uint16_t qsz, avail_idx, last_used;
static struct vring_desc *desc;
static volatile uint16_t *avail_flags, *avail_idxp, *avail_ring;
static volatile uint16_t *used_idxp;
static bool have_flush, read_only;
static uint32_t host_features;
static bool dev_dead;                /* a reset failed: fail every request fast */
static spinlock_t vblk_lock = SPINLOCK_INIT;
static volatile bool req_done;       /* set by the IRQ handler */
static bool dev_busy;                /* whole-operation mutex, guarded by vblk_lock */
static char busy_chan;
static struct ext2_block_dev vblk_dev;

static uint64_t phys_of(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return kvirt_to_phys(v & ~0xFFFULL) + (v & 0xFFF);
}

static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

static bool vblk_start_device(void);

/* The device stopped answering: reset it and rebuild the queue. After a reset
 * the device may no longer touch guest memory, so the buffers of the abandoned
 * request are safe to reuse. If the device does not come back it is marked dead. */
static void vblk_reset(void) {
    if (!vblk_start_device()) {
        dev_dead = true;
        kprintf("virtio-blk: device did not recover; failing all further I/O\n");
    } else {
        kprintf("virtio-blk: device reset, I/O resumed\n");
    }
}

static void vblk_irq(struct arch_frame *f) {
    (void)f;
    uint8_t isr = inb(io_base + VIO_ISR);      /* read-to-clear; drops the line */
    if ((isr & 1) && *used_idxp != last_used) {
        barrier();
        last_used++;
        req_done = true;
    }
}

static void vblk_acquire(void) {
    irq_spin_lock(&vblk_lock);
    while (dev_busy)
        sleep(&busy_chan, &vblk_lock);
    dev_busy = true;
    irq_spin_unlock(&vblk_lock);
}

static void vblk_release(void) {
    irq_spin_lock(&vblk_lock);
    dev_busy = false;
    irq_spin_unlock(&vblk_lock);
    wakeup(&busy_chan);
}

/* Issue one request and wait for its interrupt. `data` (len bytes) is a
 * bounce-buffer address or NULL for a flush. Caller holds the device. */
static int vblk_request(uint32_t type, uint64_t sector, void *data, uint32_t len,
                        bool to_device) {
    irq_spin_lock(&vblk_lock);
    if (dev_dead) {
        irq_spin_unlock(&vblk_lock);
        return -5;
    }
    req_hdr.type = type;
    req_hdr.reserved = 0;
    req_hdr.sector = sector;
    req_status = 0xFF;
    req_done = false;

    int n = 0;
    desc[n] = (struct vring_desc){ phys_of(&req_hdr), sizeof(req_hdr), DESC_NEXT, 1 };
    n++;
    if (data) {
        desc[n] = (struct vring_desc){ phys_of(data), len,
            (uint16_t)(DESC_NEXT | (to_device ? 0 : DESC_WRITE)), 2 };
        n++;
    }
    desc[n] = (struct vring_desc){ phys_of((const void *)&req_status), 1, DESC_WRITE, 0 };
    if (!data) desc[0].next = 1;     /* header -> status */

    avail_ring[avail_idx % qsz] = 0;                 /* chain head = descriptor 0 */
    barrier();
    avail_idx++;
    *avail_idxp = avail_idx;
    barrier();
    outw(io_base + VIO_QUEUE_NOTIFY, 0);

    /* Wait for the interrupt WITHOUT yielding the CPU. The filesystem layers above
     * (ext2's shared scratch buffer and sector cache, spinlocks held across
     * vfs_read) assume a syscall is never switched out half way, so a sleeping wait
     * would corrupt data as soon as two processes touch the disk. Instead the CPU
     * halts until the device interrupts; preemption stays off so the timer tick
     * (which keeps counting, for the timeout) cannot reschedule. */
    uint64_t hz = timer_ticks_per_second();
    uint64_t deadline = timer_ticks() + (REQ_TIMEOUT_MS * hz + 999) / 1000;
    bool timed_out = false;
    sched_preempt_disable();
    while (!req_done) {
        if (timer_ticks() >= deadline) {
            timed_out = true;
            break;
        }
        /* sti;hlt is atomic: the IRQ either already fired or wakes the halted CPU. */
        irq_spin_unlock(&vblk_lock);
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
        irq_spin_lock(&vblk_lock);
    }
    sched_preempt_enable();
    if (timed_out) {
        kprintf("virtio-blk: request timed out after %d ms (type %u, sector %lu); "
                "resetting the device\n", REQ_TIMEOUT_MS, type, (unsigned long)sector);
        vblk_reset();
        irq_spin_unlock(&vblk_lock);
        return -5;      /* EIO */
    }
    int ok = req_status == 0;
    irq_spin_unlock(&vblk_lock);
    return ok ? 0 : -5;
}

static int vblk_read(struct ext2_block_dev *dev, uint64_t lba, size_t count, void *buf) {
    if (lba + count > dev->sector_count) return -5;
    uint8_t *dst = buf;
    int rc = 0;
    vblk_acquire();
    while (count) {
        size_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        if (vblk_request(T_IN, lba, bounce, (uint32_t)(n * 512), false) < 0) {
            rc = -5;
            break;
        }
        memcpy(dst, bounce, n * 512);
        dst += n * 512; lba += n; count -= n;
    }
    vblk_release();
    return rc;
}

static int vblk_write(struct ext2_block_dev *dev, uint64_t lba, size_t count,
                      const void *buf) {
    if (read_only || lba + count > dev->sector_count) return -5;
    const uint8_t *src = buf;
    int rc = 0;
    vblk_acquire();
    while (count) {
        size_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        memcpy(bounce, src, n * 512);
        if (vblk_request(T_OUT, lba, bounce, (uint32_t)(n * 512), true) < 0) {
            rc = -5;
            break;
        }
        src += n * 512; lba += n; count -= n;
    }
    vblk_release();
    return rc;
}

static int vblk_flush(struct ext2_block_dev *dev) {
    (void)dev;
    if (!have_flush) return 0;
    vblk_acquire();
    int rc = vblk_request(T_FLUSH, 0, NULL, 0, true);
    vblk_release();
    return rc;
}

/* (Re)initialise the device and its virtqueue: reset, negotiate features,
 * program the ring, set DRIVER_OK. Used at probe time and to recover from a
 * hung request. The caller serialises. */
static bool vblk_start_device(void) {
    outb(io_base + VIO_STATUS, 0);                              /* reset */
    outb(io_base + VIO_STATUS, ST_ACK);
    outb(io_base + VIO_STATUS, ST_ACK | ST_DRIVER);

    uint32_t host = inl(io_base + VIO_HOST_FEATURES);
    host_features = host;
    have_flush = host & F_BLK_FLUSH;
    read_only  = host & F_BLK_RO;
    outl(io_base + VIO_GUEST_FEATURES, host & (F_BLK_FLUSH | F_BLK_RO));

    outw(io_base + VIO_QUEUE_SELECT, 0);
    qsz = inw(io_base + VIO_QUEUE_SIZE);
    if (qsz == 0 || qsz > MAX_QSZ) {
        kprintf("virtio-blk: unsupported queue size %u\n", qsz);
        outb(io_base + VIO_STATUS, ST_FAILED);
        return false;
    }

    /* Legacy split-ring layout: descriptors, then the avail ring, then the
     * used ring starting on the next 4 KiB boundary. */
    size_t avail_off = 16u * qsz;
    size_t used_off  = (avail_off + 6u + 2u * qsz + 4095) & ~4095u;
    memset(vq_mem, 0, sizeof(vq_mem));
    desc        = (struct vring_desc *)vq_mem;
    avail_flags = (volatile uint16_t *)(vq_mem + avail_off);
    avail_idxp  = avail_flags + 1;
    avail_ring  = avail_flags + 2;
    used_idxp   = (volatile uint16_t *)(vq_mem + used_off + 2);
    avail_idx = last_used = 0;
    outl(io_base + VIO_QUEUE_PFN, (uint32_t)(phys_of(vq_mem) >> 12));

    outb(io_base + VIO_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);
    return true;
}

static bool vblk_init(struct pci_loc loc) {
    uint32_t bar0 = pci_read32(loc, PCI_BAR0);
    if (!(bar0 & 1)) {
        kprintf("virtio-blk: BAR0 is memory-mapped (modern-only device); "
                "start QEMU with disable-modern=on\n");
        return false;
    }
    io_base = (uint16_t)(bar0 & ~3u);
    pci_write16(loc, PCI_COMMAND, pci_read16(loc, PCI_COMMAND)
                | PCI_CMD_IO | PCI_CMD_BUSMASTER);

    /* INTx line the firmware routed this device to (legacy PIC, IRQ 0-15). */
    unsigned irq = pci_read32(loc, PCI_INT_LINE) & 0xFF;
    if (irq >= 16) {
        kprintf("virtio-blk: no usable interrupt line (%u)\n", irq);
        outb(io_base + VIO_STATUS, ST_FAILED);
        return false;
    }
    register_irq_handler(irq, vblk_irq);
    if (irq >= 8) {                       /* PCI INTx is level-triggered: set ELCR */
        outb(0x4D1, inb(0x4D1) | (1u << (irq - 8)));
    } else {
        outb(0x4D0, inb(0x4D0) | (1u << irq));
    }
    arch_irq_enable_line(irq);

    if (!vblk_start_device())
        return false;

    uint64_t cap = (uint64_t)inl(io_base + VIO_BLK_CAPACITY)
                 | ((uint64_t)inl(io_base + VIO_BLK_CAPACITY + 4) << 32);
    vblk_dev.read_sectors  = vblk_read;
    vblk_dev.write_sectors = vblk_write;
    vblk_dev.flush         = vblk_flush;
    vblk_dev.sector_count  = cap;
    vblk_dev.sector_size   = 512;
    kprintf("virtio-blk: %lu sectors (%lu MiB)%s%s, queue %u, io 0x%x\n",
            (unsigned long)cap, (unsigned long)(cap / 2048),
            read_only ? ", read-only" : "", have_flush ? ", flush" : "",
            qsz, io_base);
    kprintf("virtio-blk: completion by IRQ %u\n", irq);
    return true;
}

/* Find the virtio disk and register it as "vda" (before the root filesystem
 * is chosen, so root=/dev/vda can name it). */
void arch_probe_block_devices(void) {
    struct pci_loc loc;
    if (!pci_find(VIRTIO_VENDOR, VIRTIO_BLK_LEGACY, VIRTIO_BLK_LEGACY, &loc)) {
        if (pci_find(VIRTIO_VENDOR, VIRTIO_BLK_MODERN, VIRTIO_BLK_MODERN, &loc))
            kprintf("virtio-blk: found a modern-only device, which this driver "
                    "does not support; use disable-modern=on\n");
        return;
    }
    if (!vblk_init(loc))
        return;
    blockdev_register("vda", &vblk_dev);
}
