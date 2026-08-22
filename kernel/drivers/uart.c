#include <kernel/drivers/serial.h>
#include <kernel/mm/vmm.h> /* NEW_HDDM */
#include <stdint.h>

// BCM2711 (RPi4) peripheral base + PL011 UART0 / GPIO offsets.
// Matches both real hardware and QEMU's raspi4b machine.
#define PERIPHERAL_PHYS 0xFE000000ULL
#define GPIO_OFFSET     0x200000ULL
#define UART0_OFFSET    0x201000ULL

/* A runtime value rather than a #define, because this driver has to
 * work on both sides of paging bring-up: init_serial() is the very
 * first thing kernel_stage1() calls, long before init_paging() exists,
 * so the only way to reach the UART then is the raw physical address
 * through boot.S's identity table. serial_remap_to_hhdm() switches it
 * over once the kernel's own table has a real mapping — see that
 * function's comment for why that switch matters. */
static uint64_t periph_base = PERIPHERAL_PHYS;

#define GPIO_BASE       (periph_base + GPIO_OFFSET)
#define UART0_BASE      (periph_base + UART0_OFFSET)

#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPIO_PUP_PDN0   (GPIO_BASE + 0xE4)

#define UART0_DR        (UART0_BASE + 0x00)
#define UART0_FR        (UART0_BASE + 0x18)
#define UART0_IBRD      (UART0_BASE + 0x24)
#define UART0_FBRD      (UART0_BASE + 0x28)
#define UART0_LCRH      (UART0_BASE + 0x2C)
#define UART0_CR        (UART0_BASE + 0x30)
#define UART0_IMSC      (UART0_BASE + 0x38)
#define UART0_ICR       (UART0_BASE + 0x44)

#define UART0_IMSC_RXIM (1u << 4)
#define UART0_IMSC_RTIM (1u << 6)

static inline void mmio_write(unsigned long addr, unsigned int val) {
    *(volatile unsigned int *)addr = val;
}

static inline unsigned int mmio_read(unsigned long addr) {
    return *(volatile unsigned int *)addr;
}

static inline void delay(volatile int count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

void init_serial(void) {
    // Disable UART0 while configuring it.
    mmio_write(UART0_CR, 0);

    // GPIO14/15 -> ALT0 (TXD0/RXD0).
    unsigned int sel = mmio_read(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (4u << 12) | (4u << 15); // ALT0 = 0b100
    mmio_write(GPFSEL1, sel);

    // No pull up/down on GPIO14/15 (BCM2711 pull scheme: 2 bits/pin, 00 = none).
    unsigned int pup = mmio_read(GPIO_PUP_PDN0);
    pup &= ~((3u << 28) | (3u << 30));
    mmio_write(GPIO_PUP_PDN0, pup);
    delay(150);

    // Clear pending interrupts.
    mmio_write(UART0_ICR, 0x7FF);

    // Baud rate divisor for 115200 8N1 assuming a 48MHz UART reference clock
    // (the common default set by the GPU firmware). Re-check this against
    // the real board once the USB-TTL cable is in.
    mmio_write(UART0_IBRD, 26);
    mmio_write(UART0_FBRD, 3);

    // Enable FIFOs, 8 bits, no parity, 1 stop bit.
    mmio_write(UART0_LCRH, (1u << 4) | (3u << 5));

    // Enable UART, TX, RX.
    mmio_write(UART0_CR, (1u << 0) | (1u << 8) | (1u << 9));
}

/* Move every register access from the raw physical address to the
 * kernel's own high-half Device mapping (NEW_HDDM + phys, established
 * by init_paging()).
 *
 * Not cosmetic. On AArch64 the CPU picks a translation table by
 * ADDRESS, not by privilege level: low addresses resolve through
 * TTBR0, high through TTBR1, and both are live at once. So kernel code
 * touching a low address like 0xFE201000 walks whatever *process*
 * table happens to be loaded — being at EL1 doesn't change that. That
 * is the entire reason proc_create_from_binary() (kernel/proc/exec.c)
 * used to identity-map this page into every single process: without
 * it, every kprintf() went silently into the void the moment a real
 * TTBR0 was installed.
 *
 * Reaching the UART through the high alias instead makes it
 * TTBR1-resident, so it's available under any process — or none — and
 * user page tables stop carrying an MMIO mapping they have no business
 * holding. Same physical registers either way; only the path the MMU
 * takes to them changes. Matches what gic.c already does for the
 * GIC-400, which is why that driver never needed the same crutch. */
void serial_remap_to_hhdm(void) {
    periph_base = NEW_HDDM + PERIPHERAL_PHYS;
}

void serial_putc(char c) {
    while (mmio_read(UART0_FR) & (1u << 5)) {
        // wait while TX FIFO full
    }
    mmio_write(UART0_DR, (unsigned int)c);
}

char serial_getc(void) {
    while (mmio_read(UART0_FR) & (1u << 4)) {
        // wait while RX FIFO empty (UART0_FR.RXFE)
    }
    return (char)(mmio_read(UART0_DR) & 0xFF);
}

int serial_try_getc(void) {
    if (mmio_read(UART0_FR) & (1u << 4)) {
        return -1; // RX FIFO empty right now
    }
    return (int)(mmio_read(UART0_DR) & 0xFF);
}

void serial_enable_rx_irq(void) {
    /* RXIM alone only fires once the FIFO reaches UARTIFLS's RX trigger
     * level — reset default is half-full (8 of 16 bytes), so single
     * keystrokes just sit there, unread, until enough accumulate. RTIM
     * (receive timeout) is PL011's fix for exactly this: fires after a
     * short quiet period even if the FIFO never reaches that threshold,
     * so interactive typing still gets delivered promptly while fast
     * bulk input still benefits from the FIFO-threshold interrupt. The
     * ISR (kernel/drivers/keyboard.c) doesn't need to know which of the
     * two fired — it already drains whatever's actually present. */
    mmio_write(UART0_IMSC, mmio_read(UART0_IMSC) | UART0_IMSC_RXIM | UART0_IMSC_RTIM);
}
