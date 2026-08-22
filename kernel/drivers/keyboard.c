#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/serial.h>
#include <arch/gic.h>
#include <arch/exceptions.h>
#include <arch/irq_spinlock.h>
#include <stdint.h>

/*
 * There's no physical keyboard on this setup — a headless RPi4 talked
 * to over the same USB-TTL serial cable already used for console
 * output. "Keyboard input" here just means bytes arriving on the UART
 * RX line (GPIO15/RXD0). Genuinely tied to this exact board's
 * PL011/GPIO wiring (kernel/drivers/uart.c) — not a portable
 * abstraction, no reason to pretend otherwise.
 *
 * Interrupt-driven: PL011's own hardware RX FIFO is only 16 bytes deep
 * and, unlike this buffer, can't grow — if the ISR doesn't drain it
 * promptly, further bytes are silently dropped (PL011 flags this as an
 * overrun, but can't prevent it). This ring buffer decouples "how much
 * can the silicon hold" (fixed, tiny) from "how much can the system
 * tolerate not being read yet" (this — much larger, software-defined).
 * Matches the shape of x86's own kernel/drivers/keyboard.c
 * (backup/x86_tree/): head/tail/lock ring buffer, ISR pushes, a getc()
 * pops — just backed by UART bytes instead of PS/2 scancodes.
 */

#define KBD_BUFFER_SIZE 256

struct kbd_buffer {
    char     buf[KBD_BUFFER_SIZE];
    uint32_t head; /* next write index */
    uint32_t tail; /* next read index */
    spinlock_t lock;
};

static struct kbd_buffer kbuf = { .lock = SPINLOCK_INIT };

static void kbd_irq_handler(struct aarch64_frame *f) {
    (void)f;
    /* No irq_spin_lock needed here: this ISR runs with DAIF fully
     * masked (automatic on any exception entry), so it can't be
     * preempted by anything, including a second UART interrupt. The
     * only other accessor, kbd_getc(), already masks DAIF itself
     * around its own critical section (that's what irq_spin_lock does
     * for it) — which means this ISR architecturally cannot even be
     * entered while kbd_getc() is mid-read, lock or no lock on this
     * side. On a single core (the only kind this kernel runs on —
     * boot.S parks the others permanently) there's no other way these
     * two could ever touch kbuf at the same time, so a lock here would
     * just be overhead with nothing left to exclude. Revisit if this
     * ever runs on more than one core. */
    int c;
    /* Drain the ENTIRE hardware FIFO in one go — the whole point is to
     * empty it before more bytes can possibly overflow it, not just
     * take one byte per interrupt. */
    while ((c = serial_try_getc()) != -1) {
        uint32_t next_head = (kbuf.head + 1) % KBD_BUFFER_SIZE;
        if (next_head != kbuf.tail) { /* drop the byte if the software buffer is full too */
            kbuf.buf[kbuf.head] = (char)c;
            kbuf.head = next_head;
        }
    }
}

void init_kbd(void) {
    register_irq_handler(GIC_SPI_UART0, kbd_irq_handler);
    gic_enable_irq(GIC_SPI_UART0);
    serial_enable_rx_irq();
}

char kbd_getc(void) {
    for (;;) {
        irq_spin_lock(&kbuf.lock);
        if (kbuf.head != kbuf.tail) {
            char c = kbuf.buf[kbuf.tail];
            kbuf.tail = (kbuf.tail + 1) % KBD_BUFFER_SIZE;
            irq_spin_unlock(&kbuf.lock);
            return c;
        }
        irq_spin_unlock(&kbuf.lock);

        /* Idle instead of hot-spinning: DAIF is unmasked here (set once
         * in kernel_aarch64_stage2()), so the next UART IRQ — the exact
         * event this loop is waiting for — is taken normally and wakes
         * this wfe on its way to running kbd_irq_handler(); no
         * explicit sev needed. */
        __asm__ volatile ("wfe");
    }
}
