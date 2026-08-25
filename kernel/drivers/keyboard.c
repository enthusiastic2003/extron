#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/serial.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <arch/gic.h>
#include <arch/exceptions.h>
#include <arch/irq_spinlock.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/mm/paging.h>
#include <kernel/klibc/string.h>
#include <kernel/console.h>
#include <kernel/drivers/timer.h>

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

/* The shared ring (kernel/include/kernel/drivers/keyboard.h). Kernel
 * accesses it through the HHDM; a process gets the same page mapped
 * into its own address space by exec. */
static struct kbd_ring *ring;
static phys_addr_t      ring_phys;

phys_addr_t kbd_ring_phys(void) {
    return ring_phys;
}

/* Push one byte to whatever process has the ring mapped. Separate from
 * kbuf above, not a replacement for it: kbuf still backs the blocking
 * SYS_READ path, and a byte goes to both so a console reader and a
 * polling game can coexist. Drops the byte if the consumer has fallen a
 * whole ring behind, which for keystrokes is the right failure. */
static void ring_push(uint8_t c) {
    if (!ring)
        return;
    uint32_t head = ring->head;
    uint32_t next = (head + 1) % KBD_RING_SIZE;
    if (next == (ring->tail % KBD_RING_SIZE))
        return;
    ring->buf[head] = c;
    /* Publish the byte before the index that advertises it, or the
     * consumer can read the new head and find a stale byte under it. */
    __asm__ volatile ("dmb ishst" ::: "memory");
    ring->head = next;
}

static void drain_uart(void) {
    int c;
    while ((c = serial_try_getc()) != -1) {
        if (!tty_handle_input_byte(&tty_table[0], (uint8_t)c)) {
            tty_push_input(&tty_table[0], (char)c);
        }
        ring_push((uint8_t)c);
    }
}
static void kbd_irq_handler(struct aarch64_frame *f) {
    (void)f;
    drain_uart();
}

void init_kbd(void) {
    ring_phys = (phys_addr_t)pmm_alloc_page();
    if (ring_phys) {
        ring = (struct kbd_ring *)phys_to_virt_hhdm(ring_phys);
        memset(ring, 0, PAGE_SIZE);
    } else {
        kprintf("[KBD] no memory for the shared input ring\n");
    }

    register_irq_handler(GIC_SPI_UART0, kbd_irq_handler);
    gic_enable_irq(GIC_SPI_UART0);
    serial_enable_rx_irq();
}

int kbd_getc_interruptible(char *out) {
    if (!out)
        return -1;
    irq_spin_lock(&kbuf.lock);
    while (kbuf.head == kbuf.tail) {
        /* sleep() releases kbuf.lock, marks this thread THREAD_SLEEPING on
         * channel &kbuf, and schedule()s away — a real context switch
         * to whatever else is runnable, not a busy wfe. It reacquires
         * kbuf.lock itself once kbd_irq_handler()'s wakeup(&kbuf) (above)
         * resumes this proc, so the loop's re-check below is safe. */
        sleep(&kbuf, &kbuf.lock);
        if (signal_pending_unblocked(my_thread())) {
            irq_spin_unlock(&kbuf.lock);
            return -1;
        }
    }
    *out = kbuf.buf[kbuf.tail];
    kbuf.tail = (kbuf.tail + 1) % KBD_BUFFER_SIZE;
    irq_spin_unlock(&kbuf.lock);
    return 1;
}

char kbd_getc(void) {
    char out = 0;
    while (kbd_getc_interruptible(&out) < 0) { }
    return out;
}

int kbd_try_getc(char *out) {
    if (!out)
        return 0;
    irq_spin_lock(&kbuf.lock);
    if (kbuf.head == kbuf.tail) {
        irq_spin_unlock(&kbuf.lock);
        return 0;
    }
    *out = kbuf.buf[kbuf.tail];
    kbuf.tail = (kbuf.tail + 1) % KBD_BUFFER_SIZE;
    irq_spin_unlock(&kbuf.lock);
    return 1;
}

int kbd_input_ready(void) {
    irq_spin_lock(&kbuf.lock);
    int ready = kbuf.head != kbuf.tail;
    irq_spin_unlock(&kbuf.lock);
    return ready;
}

void kbd_flush_input(void) {
    irq_spin_lock(&kbuf.lock);
    kbuf.tail = kbuf.head;
    irq_spin_unlock(&kbuf.lock);
}

int kbd_wait_for_input(int timeout_ms) {
    irq_spin_lock(&kbuf.lock);
    if (kbuf.head == kbuf.tail && timeout_ms != 0) {
        struct thread *t = my_thread();
        if (timeout_ms > 0) {
            uint64_t ticks = ((uint64_t)timeout_ms * timer_ticks_per_second() + 999) / 1000;
            if (!ticks) ticks = 1;
            t->sleep_until = timer_ticks() + ticks;
        } else {
            t->sleep_until = 0;
        }
        sleep(&kbuf, &kbuf.lock);
        t->sleep_until = 0;
    }
    int ready = kbuf.head != kbuf.tail;
    irq_spin_unlock(&kbuf.lock);
    return ready;
}
