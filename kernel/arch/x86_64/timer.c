#include <kernel/drivers/timer.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/irq_spinlock.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/console.h>
#include "io.h"

/*
 * 8254 PIT channel 0 as the periodic tick (IRQ0), TSC for sub-tick time.
 * The TSC frequency is measured once against PIT channel 2 in one-shot
 * mode before the periodic tick starts. Wall-clock time is seeded from
 * the CMOS RTC. x86 counterpart of the aarch64 generic-timer driver.
 */

#define PIT_HZ 1193182UL

static volatile uint64_t tick_count;
static unsigned configured_hz;
static uint64_t tsc_hz;
static uint64_t boot_tsc;
static spinlock_t realtime_lock = SPINLOCK_INIT;
static int64_t realtime_offset_ns;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t timer_ticks(void) { return tick_count; }
unsigned timer_ticks_per_second(void) { return configured_hz; }

uint64_t timer_uptime_ns(void) {
    if (!tsc_hz)
        return tick_count * (1000000000ULL / (configured_hz ? configured_hz : 1000));
    uint64_t e = rdtsc() - boot_tsc;
    return (e / tsc_hz) * 1000000000ULL + ((e % tsc_hz) * 1000000000ULL) / tsc_hz;
}

uint64_t timer_uptime_ms(void) { return timer_uptime_ns() / 1000000ULL; }

int64_t timer_realtime_ns(void) {
    irq_spin_lock(&realtime_lock);
    int64_t off = realtime_offset_ns;
    irq_spin_unlock(&realtime_lock);
    return (int64_t)timer_uptime_ns() + off;
}

void timer_set_realtime_ns(int64_t value) {
    irq_spin_lock(&realtime_lock);
    realtime_offset_ns = value - (int64_t)timer_uptime_ns();
    irq_spin_unlock(&realtime_lock);
}

void timer_set_counter_watch(phys_addr_t a, phys_addr_t b) { (void)a; (void)b; }

static void timer_irq_handler(struct arch_frame *f) {
    (void)f;
    tick_count++;
    thread_wakeup_expired(tick_count);
    schedule();
}

/* Measure the TSC against a PIT channel 2 one-shot of ~20 ms. */
static uint64_t calibrate_tsc(void) {
    const uint32_t count = (uint32_t)(PIT_HZ / 50);   /* 20 ms */
    uint8_t gate = inb(0x61);
    outb(0x61, (gate & ~0x02) | 0x01);                /* gate on, speaker off */
    outb(0x43, 0xB0);                                 /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    /* Restart the gate so the one-shot counts from the value just loaded. */
    outb(0x61, inb(0x61) & ~0x01);
    outb(0x61, inb(0x61) | 0x01);
    uint64_t t0 = rdtsc();
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20)) {
        if (++guard > 50000000u) return 0;            /* PIT not responding */
    }
    uint64_t t1 = rdtsc();
    outb(0x61, gate);
    return (t1 - t0) * 50;
}

static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

/* Seconds since the Unix epoch from the CMOS RTC (assumed UTC). */
static int64_t rtc_unix_seconds(void) {
    while (cmos_read(0x0A) & 0x80) { }   /* update in progress */
    uint8_t sec = cmos_read(0), min = cmos_read(2), hour = cmos_read(4);
    uint8_t day = cmos_read(7), mon = cmos_read(8), year = cmos_read(9);
    uint8_t b = cmos_read(0x0B);
    if (!(b & 0x04)) {
#define BCD(x) (((x) & 0x0F) + ((x) >> 4) * 10)
        sec = BCD(sec); min = BCD(min); day = BCD(day); mon = BCD(mon); year = BCD(year);
        hour = BCD(hour & 0x7F) | (hour & 0x80);
    }
    if (!(b & 0x02) && (hour & 0x80)) hour = ((hour & 0x7F) + 12) % 24;
    int y = 2000 + year;
    static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int64_t days = (int64_t)(y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
    days += cum[(mon - 1) % 12] + (day - 1);
    if (mon > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return days * 86400 + hour * 3600 + min * 60 + sec;
}

void timer_init(unsigned hz) {
    configured_hz = hz;
    tsc_hz = calibrate_tsc();
    boot_tsc = rdtsc();
    kprintf("x86_64: TSC %lu MHz (PIT-calibrated)\n", (unsigned long)(tsc_hz / 1000000));

    timer_set_realtime_ns(rtc_unix_seconds() * 1000000000LL);

    uint32_t div = (uint32_t)(PIT_HZ / hz);
    outb(0x43, 0x34);                 /* ch0, lobyte/hibyte, mode 2 (rate generator) */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);

    register_irq_handler(ARCH_IRQ_TIMER, timer_irq_handler);
    pic_unmask(ARCH_IRQ_TIMER);
}
