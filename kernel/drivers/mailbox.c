#include <kernel/drivers/mailbox.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/vmm.h>
#include <kernel/console.h>
#include <kernel/panic.h>

/* Offsets from MAILBOX_MMIO_PHYS (0xFE00B000), NOT from the peripheral
 * base — the registers themselves live at 0xFE00B880, so these are
 * 0x880-relative. Mailbox 0 is the GPU->ARM direction and the one we
 * READ; mailbox 1 is ARM->GPU and the one we WRITE. Two separate FIFOs
 * 0x20 apart, each with its own STATUS. */
#define MBOX0_READ    0x880
#define MBOX0_STATUS  0x898
#define MBOX1_WRITE   0x8A0
#define MBOX1_STATUS  0x8B8

#define MBOX_FULL     0x80000000u
#define MBOX_EMPTY    0x40000000u

/* Channel 8: the property interface (tag-based). Lower-numbered
 * channels are legacy single-purpose ones we have no use for. */
#define MBOX_CH_PROP  8

#define RESP_SUCCESS  0x80000000u

static volatile uint8_t *mbox_base;

static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(mbox_base + off) = val;
}

static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(mbox_base + off);
}

/*
 * Clean and invalidate a range to the point of coherency.
 *
 * Needed because the GPU reads and writes the message buffer directly
 * out of RAM, while the ARM sees that same memory through its caches
 * (SCTLR_EL1.C has been on since boot.S). Without this the GPU can read
 * a stale message that is still sitting in our D-cache, and we can read
 * a stale reply after the GPU has already updated RAM. `dc civac` does
 * both directions in one op, which is what a buffer used for request
 * AND response wants.
 *
 * The line size comes from CTR_EL0.DminLine rather than being assumed —
 * same approach as paging.c's sync_icache_dcache().
 */
static void cache_flush_range(const void *addr, size_t size) {
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xF);

    uint64_t start = (uint64_t)addr & ~(line - 1);
    uint64_t end   = (uint64_t)addr + size;
    for (uint64_t a = start; a < end; a += line) {
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
}

void mailbox_init(void) {
    for (uint64_t off = 0; off < MAILBOX_MMIO_SIZE; off += PAGE_SIZE) {
        if (kmap(NEW_HDDM + MAILBOX_MMIO_PHYS + off, MAILBOX_MMIO_PHYS + off,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE) != 0) {
            panic("mailbox: failed to map MMIO at %p",
                  (void *)(MAILBOX_MMIO_PHYS + off));
        }
    }
    /* High-half alias, so this is reachable under any process's TTBR0 —
     * the same reason the UART moved in 268c962. Device memory, since
     * caching a mailbox register would drop the side effects that make
     * it work. */
    mbox_base = (volatile uint8_t *)(NEW_HDDM + MAILBOX_MMIO_PHYS);
    kprintf("aarch64: VideoCore mailbox mapped (0x%lx)\n",
            (unsigned long)MAILBOX_MMIO_PHYS);
}

int mailbox_property_call(volatile uint32_t *msg) {
    uint32_t len = msg[0];

    /* kvirt_to_phys, not virt_to_phys_hhdm: the caller's buffer is a
     * kernel .bss object at a high VMA, not an HHDM alias, so it needs a
     * real page-table walk to find its physical address. */
    phys_addr_t phys = kvirt_to_phys((virt_addr_t)msg);
    if (!phys) {
        kprintf("mailbox: message buffer %p is not mapped\n", (void *)msg);
        return -1;
    }
    if (phys & 0xF) {
        kprintf("mailbox: message buffer %p is not 16-byte aligned\n", (void *)msg);
        return -1;
    }

    /* Push the request out of the cache before the GPU goes looking for
     * it in RAM. */
    cache_flush_range((const void *)msg, len);

    uint32_t bus = PHYS_TO_BUS(phys);

    while (mmio_read32(MBOX1_STATUS) & MBOX_FULL) {
        __asm__ volatile ("nop");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    /* Low 4 bits carry the channel, which is why the buffer had to be
     * 16-byte aligned — they are not part of the address. */
    mmio_write32(MBOX1_WRITE, (bus & ~0xFu) | MBOX_CH_PROP);

    /* Drain until a reply on OUR channel appears. Other channels can
     * have traffic pending that isn't ours; taking it would both lose
     * someone else's message and misread it as our response. */
    for (;;) {
        while (mmio_read32(MBOX0_STATUS) & MBOX_EMPTY) {
            __asm__ volatile ("nop");
        }
        __asm__ volatile ("dsb sy" ::: "memory");
        uint32_t resp = mmio_read32(MBOX0_READ);
        if ((resp & 0xF) == MBOX_CH_PROP) {
            break;
        }
    }

    /* Drop our cached copy of the buffer so the GPU's edits are what we
     * read back. */
    cache_flush_range((const void *)msg, len);

    if (msg[1] != RESP_SUCCESS) {
        kprintf("mailbox: firmware returned 0x%x (not success)\n", msg[1]);
        return -1;
    }
    return 0;
}

/* 16-byte aligned, in .bss so it has a stable physical address the GPU
 * can be pointed at. Not on the stack: a stack buffer would work today
 * but its physical page changes with the kernel stack, and the alignment
 * would be the compiler's business rather than ours. */
static volatile uint32_t msg_buf[36] __attribute__((aligned(16)));

void mailbox_report(void) {
    /* One message, four tags. The firmware processes tags in order and
     * writes each answer back into the same slots, which is a better
     * transport test than four separate calls: it exercises the tag
     * walk, not just a single round trip. */
    int i = 0;
    msg_buf[i++] = 0;                       /* size, patched below */
    msg_buf[i++] = 0;                       /* request */

    msg_buf[i++] = TAG_GET_FIRMWARE_REV;
    msg_buf[i++] = 4;                       /* response buffer bytes */
    msg_buf[i++] = 0;                       /* request length */
    int fw_rev = i; msg_buf[i++] = 0;

    msg_buf[i++] = TAG_GET_BOARD_MODEL;
    msg_buf[i++] = 4;
    msg_buf[i++] = 0;
    int board = i; msg_buf[i++] = 0;

    msg_buf[i++] = TAG_GET_ARM_MEMORY;
    msg_buf[i++] = 8;
    msg_buf[i++] = 0;
    int arm_mem = i; msg_buf[i++] = 0; msg_buf[i++] = 0;

    msg_buf[i++] = TAG_GET_VC_MEMORY;
    msg_buf[i++] = 8;
    msg_buf[i++] = 0;
    int vc_mem = i; msg_buf[i++] = 0; msg_buf[i++] = 0;

    msg_buf[i++] = 0;                       /* end tag */
    msg_buf[0] = (uint32_t)(i * 4);

    if (mailbox_property_call(msg_buf) != 0) {
        kprintf("mailbox: property call FAILED\n");
        return;
    }

    kprintf("mailbox: firmware rev 0x%x, board model 0x%x\n",
            msg_buf[fw_rev], msg_buf[board]);
    kprintf("mailbox: ARM memory base 0x%x size 0x%x (%u MiB)\n",
            msg_buf[arm_mem], msg_buf[arm_mem + 1],
            msg_buf[arm_mem + 1] >> 20);
    /* The interesting one: this is where the framebuffer will be
     * allocated from, and it should match the hole the memory map
     * leaves between its two available regions. */
    kprintf("mailbox: VC  memory base 0x%x size 0x%x (%u MiB)\n",
            msg_buf[vc_mem], msg_buf[vc_mem + 1],
            msg_buf[vc_mem + 1] >> 20);
}
