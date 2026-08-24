#include <kernel/drivers/emmc.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <arch/gic.h>
#include <arch/exceptions.h>


#define EMMC_MMIO_PHYS 0xFE340000ULL

/* SDHCI Registers */
#define SDHCI_ARG2          0x00
#define SDHCI_BLKSZ         0x04
#define SDHCI_BLKCNT        0x06
#define SDHCI_ARG           0x08
#define SDHCI_TRNMOD        0x0C
#define SDHCI_CMD           0x0E
#define SDHCI_RSP0          0x10
#define SDHCI_RSP1          0x14
#define SDHCI_RSP2          0x18
#define SDHCI_RSP3          0x1C
#define SDHCI_DATA          0x20
#define SDHCI_PRSTAT        0x24
#define SDHCI_HOSTCR        0x28
#define SDHCI_PWR           0x29
#define SDHCI_CLKCR         0x2C
#define SDHCI_TIMEOUT       0x2E
#define SDHCI_SRST          0x2F
#define SDHCI_INTSTAT       0x30
#define SDHCI_INTMASK       0x34
#define SDHCI_INTEN         0x38
#define SDHCI_CAPS0         0x40
#define SDHCI_CAPS1         0x44

/* Constants */
#define SRST_ALL            0x01
#define SRST_CMD            0x02
#define SRST_DATA           0x04

#define INT_CMD_DONE        (1 << 0)
#define INT_DATA_DONE       (1 << 1)
#define INT_BLOCK_GAP       (1 << 2)
#define INT_WRITE_RDY       (1 << 4)
#define INT_READ_RDY        (1 << 5)
#define INT_ERR             (1 << 15)


static volatile uint8_t *emmc_base;
static uint32_t rca;

static void delay_cycles(volatile int cycles);
static volatile uint32_t emmc_irq_status = 0;
static void *emmc_io_wait = (void *)&emmc_irq_status;

static void emmc_irq_handler(struct aarch64_frame *f) {
    (void)f;
    uint32_t stat = *(volatile uint32_t *)(emmc_base + 0x30 /* SDHCI_INTSTAT */);
    *(volatile uint32_t *)(emmc_base + 0x30) = stat; /* ack */
    emmc_irq_status |= stat;
    wakeup(emmc_io_wait);
}

static uint32_t emmc_wait_for_interrupt(uint32_t mask) {
    if (!my_thread()) {
        /* Early boot, scheduler not running, poll hardware directly */
        int retries = 1000000;
        while (!((emmc_irq_status) & (mask | INT_ERR)) && retries--) {
            uint32_t raw = *(volatile uint32_t *)(emmc_base + 0x30);
            if (raw & (mask | INT_ERR)) {
                *(volatile uint32_t *)(emmc_base + 0x30) = raw;
                emmc_irq_status |= raw;
            }
            delay_cycles(100);
        }
    } else {
        /* Scheduler running, sleep on our channel */
        while (!((emmc_irq_status) & (mask | INT_ERR))) {
            struct thread *t = my_thread();
            t->chan = emmc_io_wait;
            t->sleep_until = 0;
            thread_set_sleeping(t);
            schedule();
        }
    }
    uint32_t res = emmc_irq_status & (mask | INT_ERR);
    emmc_irq_status &= ~(mask | INT_ERR);
    return res;
}


static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(emmc_base + off) = val;
}

static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(emmc_base + off);
}

static inline void mmio_write16(uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(emmc_base + off) = val;
}

static inline uint16_t mmio_read16(uint32_t off) {
    return *(volatile uint16_t *)(emmc_base + off);
}

static inline void mmio_write8(uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(emmc_base + off) = val;
}

static inline uint8_t mmio_read8(uint32_t off) {
    return *(volatile uint8_t *)(emmc_base + off);
}

static void delay_cycles(volatile int cycles) {
    while (cycles--) {
        __asm__ volatile("nop");
    }
}

/* Program the SDCLK divisor (10-bit Divided Clock Mode: SDCLK =
 * base_clock / (2 * divisor)). Must gate the SD clock output off
 * before reprogramming the frequency select bits — leaving it enabled
 * across a frequency change is undefined per the SDHCI spec — then
 * re-enable the internal clock, wait for it to report stable, and
 * finally re-enable the SD clock output. */
static void sd_set_clock_divisor(uint16_t divisor) {
    mmio_write16(SDHCI_CLKCR, 0);
    uint16_t clk = (divisor << 8);
    mmio_write16(SDHCI_CLKCR, clk | 0x01); /* Internal clock enable */
    delay_cycles(10000);
    while (!(mmio_read16(SDHCI_CLKCR) & 0x02)); /* Wait for stable */
    mmio_write16(SDHCI_CLKCR, clk | 0x05); /* Enable SD clock */
}

/* Send a command to the SD card.
 * flags: 
 *  0x02: 136-bit response
 *  0x03: 48-bit response with busy
 *  0x01: 48-bit response
 *  0x08: CRC check
 *  0x10: Index check
 *  0x20: Data present
 */
/*
 * NOTE ON INTERRUPT HANDLING (The "0x00000000 Timeout" Bug):
 * Previously, sd_cmd() manually polled the SDHCI_INTSTAT hardware register. 
 * This worked perfectly during early boot (emmc_init) because CPU interrupts 
 * were masked, so the hardware register retained its values. However, once 
 * exceptions_enable_irqs() was called, the physical interrupt would fire 
 * immediately upon command completion. Our emmc_irq_handler would run, read 
 * INTSTAT, and clear the hardware register to 0. When sd_cmd() resumed its 
 * polling loop, it would read 0x00000000 from the hardware, miss the event 
 * completely, and eventually time out.
 * 
 * FIX: All command and data wait loops MUST use emmc_wait_for_interrupt(), 
 * which checks the software-maintained `emmc_irq_status` variable. This ensures 
 * events aren't lost when the IRQ handler clears the hardware register.
 */
static int sd_cmd(uint32_t cmd_idx, uint32_t arg, uint32_t trnmod, uint32_t flags) {
    mmio_write32(SDHCI_INTSTAT, 0xFFFFFFFF); /* Clear previous hardware interrupts */
    emmc_irq_status = 0;                     /* Clear previous software status */
    mmio_write32(SDHCI_ARG, arg);
    mmio_write16(SDHCI_TRNMOD, trnmod);
    mmio_write16(SDHCI_CMD, (cmd_idx << 8) | flags);

    uint32_t status = emmc_wait_for_interrupt(INT_CMD_DONE);
    if (status & INT_ERR) {
        kprintf("eMMC: Command %u error! INTSTAT = 0x%x\n", cmd_idx, status);
        return -1;
    }
    if (!(status & INT_CMD_DONE)) {
        kprintf("eMMC: Command %u timeout!\n", cmd_idx);
        return -1;
    }
    return 0;
}

static int sd_acmd(uint32_t cmd_idx, uint32_t arg, uint32_t flags) {
    /* Send CMD55 first (APP_CMD) */
    if (sd_cmd(55, rca << 16, 0, 0x1A) < 0) { // 48-bit response + CRC + Index
        return -1;
    }
    return sd_cmd(cmd_idx, arg, 0, flags);
}

void emmc_init(void) {
    kprintf("eMMC: Initializing SDHCI at 0x%lx\n", (unsigned long)EMMC_MMIO_PHYS);

    /* Map the MMIO region as device memory (uncached). */
    for (uint64_t off = 0; off < 0x1000; off += 4096) {
        int res = kmap(NEW_HDDM + EMMC_MMIO_PHYS + off, EMMC_MMIO_PHYS + off, 
                       PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE);
        if (res != 0) {
            kprintf("eMMC: kmap failed!\n");
            return;
        }
    }

    emmc_base = (volatile uint8_t *)(NEW_HDDM + EMMC_MMIO_PHYS);

    /* Register IRQ 158 (GIC SPI 126) for BCM2711 EMMC2 */
    register_irq_handler(158, emmc_irq_handler);
    gic_enable_irq(158);

    /* Software Reset */
    mmio_write8(SDHCI_SRST, SRST_ALL);
    int retries = 10000;
    while ((mmio_read8(SDHCI_SRST) & SRST_ALL) && retries--) {
        delay_cycles(100);
    }
    if (retries <= 0) {
        kprintf("eMMC: Reset timeout!\n");
        return;
    }

    /* Enable Power (3.3V) */
    mmio_write8(SDHCI_PWR, 0x0F); 

    /* Clock config - minimal speed for init */
    /* Pi4 EMMC2 base clock is usually 50MHz, 100MHz, or 200MHz. We divide it down heavily. */
    sd_set_clock_divisor(0x40);

    /* Set timeout to max */
    mmio_write8(SDHCI_TIMEOUT, 0x0E);

    /* Enable all interrupt status flags so we can see them */
    mmio_write32(SDHCI_INTEN, 0xFFFFFFFF);
    mmio_write32(SDHCI_INTMASK, 0xFFFFFFFF);

    /* Wait a bit for the card to power up */
    delay_cycles(100000);

    /* CMD0: GO_IDLE_STATE */
    if (sd_cmd(0, 0, 0, 0x00) < 0) {
        kprintf("eMMC: CMD0 failed.\n");
        return;
    }

    /* CMD8: SEND_IF_COND (VHS=1, check pattern=0xAA) */
    if (sd_cmd(8, 0x1AA, 0, 0x1A) < 0) {
        kprintf("eMMC: CMD8 failed (might be v1 card or no card).\n");
        return;
    }
    if (mmio_read32(SDHCI_RSP0) != 0x1AA) {
        kprintf("eMMC: CMD8 response mismatch: 0x%x\n", mmio_read32(SDHCI_RSP0));
        return;
    }

    /* ACMD41: APP_SEND_OP_COND */
    kprintf("eMMC: Sending ACMD41...\n");
    int ready = 0;
    for (int i = 0; i < 1000; i++) {
        if (sd_acmd(41, 0x40300000, 0x02) < 0) { // HCS bit set, 136-bit response
            kprintf("eMMC: ACMD41 failed.\n");
            return;
        }
        if (mmio_read32(SDHCI_RSP0) & 0x80000000) { // Busy bit cleared (it's active low essentially in the spec, meaning powered up = 1)
            ready = 1;
            break;
        }
        delay_cycles(10000);
    }
    if (!ready) {
        kprintf("eMMC: ACMD41 timeout!\n");
        return;
    }

    /* CMD2: ALL_SEND_CID */
    if (sd_cmd(2, 0, 0, 0x09) < 0) { // 136-bit resp, CRC
        kprintf("eMMC: CMD2 failed.\n");
        return;
    }

    /* CMD3: SEND_RELATIVE_ADDR */
    if (sd_cmd(3, 0, 0, 0x1A) < 0) {
        kprintf("eMMC: CMD3 failed.\n");
        return;
    }
    rca = mmio_read32(SDHCI_RSP0) >> 16;
    kprintf("eMMC: Card RCA = 0x%x\n", rca);

    /* CMD7: SELECT_CARD */
    if (sd_cmd(7, rca << 16, 0, 0x1B) < 0) { // 48-bit resp + busy
        kprintf("eMMC: CMD7 failed.\n");
        return;
    }

    /* Now that the card is selected and out of identification mode,
     * step up from the ultra-conservative ID-speed clock (~base/128,
     * required during CMD0..CMD3) to a real Default Speed transfer
     * clock. BCM2711's EMMC2 base clock is firmware-fixed at 100MHz
     * (set by the VideoCore firmware at boot, not something this driver
     * negotiates), so divisor 2 (~base/4 = 25MHz) lands exactly at the
     * Default Speed spec's ceiling — as fast as this driver can safely
     * go without also negotiating High Speed mode (CMD6 + HOSTCR's High
     * Speed Enable bit, target 50MHz), which isn't implemented. Every
     * sector read before this point — and, before this fix existed at
     * all, every sector read for the rest of the session — ran at ID
     * speed, which was the single largest cost in reading anything off
     * the card. */
    sd_set_clock_divisor(0x02);

    /* Set block size to 512 */
    mmio_write16(SDHCI_BLKSZ, 512);
    /* Block count */
    mmio_write16(SDHCI_BLKCNT, 1);

    kprintf("eMMC: Initialized successfully.\n");

    /* TEST: Read Sector 0 */
    kprintf("eMMC: Attempting to read Sector 0 (MBR)...\n");

    /* CMD17: READ_SINGLE_BLOCK */
    /* trnmod: Data read (0x10) */
    if (sd_cmd(17, 0, 0x10, 0x3A) < 0) { // Data present, index, crc, 48-bit
        kprintf("eMMC: CMD17 failed.\n");
        return;
    }

    /* Wait for data ready */
    retries = 100000;
    while (!(mmio_read32(SDHCI_INTSTAT) & INT_READ_RDY) && retries--) {
        delay_cycles(100);
    }
    if (retries <= 0) {
        kprintf("eMMC: Sector read data timeout! INTSTAT=0x%x\n", mmio_read32(SDHCI_INTSTAT));
        return;
    }
    mmio_write32(SDHCI_INTSTAT, INT_READ_RDY);

    /* Read the 512 bytes */
    uint32_t buf[128];
    for (int i = 0; i < 128; i++) {
        buf[i] = mmio_read32(SDHCI_DATA);
    }

    /* Wait for data done */
    retries = 100000;
    while (!(mmio_read32(SDHCI_INTSTAT) & INT_DATA_DONE) && retries--) {
        delay_cycles(100);
    }
    if (retries > 0) {
        mmio_write32(SDHCI_INTSTAT, INT_DATA_DONE);
    }

    uint8_t *byte_buf = (uint8_t *)buf;
    kprintf("eMMC: Sector 0 read complete.\n");
    kprintf("eMMC: MBR Signature: 0x%02X 0x%02X\n", byte_buf[510], byte_buf[511]);
}

int emmc_read_sectors(uint64_t lba, size_t count, void *buf) {
    if (count == 0)
        return 0;

    uint8_t *dst = (uint8_t *)buf;

    /* One command for the whole run instead of one CMD17 per sector:
     * ext2 block reads are block_size/512 sectors (8, for the common
     * 4K block size), and re-issuing a full command+response round
     * trip per sector was most of the per-block I/O cost. BLKCNT is
     * 16-bit; ext2 never asks for anywhere near 65535 sectors in one
     * call (one filesystem block at a time), so no chunking loop is
     * needed here. Falls back to plain CMD17 semantics for count==1
     * (no Block Count Enable/Auto CMD12 needed for a single block),
     * which also covers emmc_init()'s own sector-0 MBR read above. */
    mmio_write16(SDHCI_BLKSZ, 512);
    mmio_write16(SDHCI_BLKCNT, (uint16_t)count);

    uint32_t cmd_idx = (count == 1) ? 17 : 18; /* READ_SINGLE_/MULTIPLE_BLOCK */
    uint32_t trnmod  = (count == 1) ? 0x10     /* dir=read only */
                                     : 0x36;    /* BCE | Auto CMD12 | dir=read | multi */

    if (sd_cmd(cmd_idx, (uint32_t)lba, trnmod, 0x3A) < 0) {
        kprintf("eMMC: Read command failed at LBA %llu (count=%zu)\n",
                (unsigned long long)lba, count);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        /* Wait for this block's data to be ready */
        uint32_t stat = emmc_wait_for_interrupt(INT_READ_RDY);
        if (stat & INT_ERR) {
            kprintf("eMMC: Read error at LBA %llu\n", (unsigned long long)lba);
            return -1;
        }
        if (!(stat & INT_READ_RDY)) {
            kprintf("eMMC: Read ready timeout at LBA %llu (sector %zu/%zu)\n",
                    (unsigned long long)lba, i, count);
            return -1;
        }

        /* Read the 512 bytes */
        uint32_t *d32 = (uint32_t *)dst;
        for (int j = 0; j < 128; j++) {
            *d32++ = mmio_read32(SDHCI_DATA);
        }
        dst += 512;
    }

    /* Transfer Complete fires once for the whole command (the
     * controller sends Auto CMD12 itself for the multi-block case). */
    uint32_t stat = emmc_wait_for_interrupt(INT_DATA_DONE);
    if (stat & INT_ERR) {
        kprintf("eMMC: Data error at LBA %llu\n", (unsigned long long)lba);
        return -1;
    }
    if (!(stat & INT_DATA_DONE)) {
        kprintf("eMMC: Data-done timeout at LBA %llu (count=%zu)\n",
                (unsigned long long)lba, count);
        return -1;
    }

    return 0;
}

int emmc_write_sectors(uint64_t lba, size_t count, const void *buf) {
    if (count == 0) return 0;
    const uint8_t *src = (const uint8_t *)buf;

    mmio_write16(SDHCI_BLKSZ, 512);
    mmio_write16(SDHCI_BLKCNT, (uint16_t)count);

    uint32_t cmd_idx = (count == 1) ? 24 : 25; /* WRITE_BLOCK / WRITE_MULTIPLE_BLOCK */
    uint32_t trnmod  = (count == 1) ? 0x00     /* dir=write only */
                                    : 0x26;    /* BCE | Auto CMD12 | dir=write | multi */

    if (sd_cmd(cmd_idx, (uint32_t)lba, trnmod, 0x3A) < 0) {
        kprintf("eMMC: Write command failed at LBA %llu (count=%zu)\n",
                (unsigned long long)lba, count);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        uint32_t stat = emmc_wait_for_interrupt(INT_WRITE_RDY);
        if (stat & INT_ERR) {
            kprintf("eMMC: Write error at LBA %llu\n", (unsigned long long)lba);
            return -1;
        }
        if (!(stat & INT_WRITE_RDY)) {
            kprintf("eMMC: Write ready timeout at LBA %llu (sector %zu/%zu)\n",
                    (unsigned long long)lba, i, count);
            return -1;
        }

        /* Write the 512 bytes */
        const uint32_t *d32 = (const uint32_t *)src;
        for (int j = 0; j < 128; j++) {
            mmio_write32(SDHCI_DATA, *d32++);
        }
        src += 512;
    }

    uint32_t stat = emmc_wait_for_interrupt(INT_DATA_DONE);
    if (stat & INT_ERR) {
        kprintf("eMMC: Write data error at LBA %llu\n", (unsigned long long)lba);
        return -1;
    }
    if (!(stat & INT_DATA_DONE)) {
        kprintf("eMMC: Write data-done timeout at LBA %llu\n", (unsigned long long)lba);
        return -1;
    }
    return 0;
}

#include <kernel/fs/ext2.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/kheap.h>

struct mbr_partition {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t num_sectors;
} __attribute__((packed));

static int emmc_block_dev_read(struct ext2_block_dev *dev, uint64_t lba, size_t count, void *buf) {
    (void)dev;
    return emmc_read_sectors(lba, count, buf);
}

static int emmc_block_dev_write(struct ext2_block_dev *dev, uint64_t lba, size_t count, const void *buf) {
    (void)dev;
    return emmc_write_sectors(lba, count, buf);
}

void emmc_mount_ext2(void) {
    uint8_t sector0[512];
    if (emmc_read_sectors(0, 1, sector0) < 0) {
        kprintf("eMMC: Failed to read MBR for partition parsing.\n");
        return;
    }

    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        kprintf("eMMC: Invalid MBR signature!\n");
        return;
    }

    struct mbr_partition *part = (struct mbr_partition *)(sector0 + 0x1BE);
    uint32_t ext2_lba = 0;

    for (int i = 0; i < 4; i++) {
        if (part[i].type == 0x83) { // Linux ext2/ext3/ext4
            ext2_lba = part[i].lba_start;
            kprintf("eMMC: Found Linux ext2 partition at LBA %u (Size: %u sectors)\n", 
                    ext2_lba, part[i].num_sectors);
            break;
        }
    }

    if (ext2_lba == 0) {
        kprintf("eMMC: No ext2 partition (type 0x83) found in MBR.\n");
        return;
    }

    /* Initialize block dev interface */
    struct ext2_block_dev *dev = kmalloc(sizeof(*dev));
    if (!dev) return;
    dev->read_sectors = emmc_block_dev_read;
    dev->write_sectors = emmc_block_dev_write;
    dev->sector_size = 512;
    dev->sector_count = 0; /* Unused by ext2_read_data */

    extern const struct vfs_fs_ops ext2_fs_ops;

    /* Create the ext2 mount structure */
    struct ext2_mount *ext2_m = ext2_mount_create(dev, ext2_lba);
    if (!ext2_m) {
        kprintf("eMMC: Failed to mount ext2 filesystem at LBA %u.\n", ext2_lba);
        return;
    }

    /* Mount it into the VFS at /mnt/sd */
    struct vfs_path root;
    if (vfs_root_path(&root) == 0) {
        int res = vfs_mount_at(&root, "mnt/sd", &ext2_fs_ops, ext2_m);
        if (res == 0) {
            kprintf("eMMC: Successfully mounted ext2 at /mnt/sd\n");
        } else {
            kprintf("eMMC: VFS mount failed with error %d\n", res);
        }
        vfs_path_release(&root);
    } else {
        kprintf("eMMC: Failed to resolve VFS root for mounting.\n");
    }
}
