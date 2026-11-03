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

/* Scheduler time in ticks, derived from the TSC rather than counted by the
 * PIT interrupt. Syscalls run with interrupts masked, so a long one (a big
 * framebuffer write, say) can swallow several 1 ms PIT ticks; counting
 * interrupts would make every sleep() deadline drift late by that much,
 * forever. Deriving from the TSC makes missed ticks self-correcting: the next
 * interrupt that does arrive sees the true time and wakes everything due. */
static uint64_t tsc_per_tick;

uint64_t timer_ticks(void) {
    if (!tsc_per_tick)
        return tick_count;
    return (rdtsc() - boot_tsc) / tsc_per_tick;
}
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
    thread_wakeup_expired(timer_ticks());
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
static void cmos_write(uint8_t reg, uint8_t v) { outb(0x70, reg); outb(0x71, v); }

#define CMOS_CENTURY 0x32    /* the usual FADT-advertised century register */

/* Civil date <-> days since 1970-01-01 (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

struct rtc_raw { uint8_t sec, min, hour, day, mon, year, century; };

static void rtc_read_raw(struct rtc_raw *r) {
    while (cmos_read(0x0A) & 0x80) { }                 /* update in progress */
    r->sec = cmos_read(0);  r->min = cmos_read(2);  r->hour = cmos_read(4);
    r->day = cmos_read(7);  r->mon = cmos_read(8);  r->year = cmos_read(9);
    r->century = cmos_read(CMOS_CENTURY);
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Seconds since the Unix epoch from the CMOS RTC (assumed UTC). The clock can
 * tick over between two register reads, so read until two passes agree. */
static int64_t rtc_unix_seconds(void) {
    struct rtc_raw a, b;
    rtc_read_raw(&a);
    for (int tries = 0; tries < 5; tries++) {
        b = a;
        rtc_read_raw(&a);
        if (!memcmp(&a, &b, sizeof(a)))
            break;
    }
    uint8_t regb = cmos_read(0x0B);
    bool binary = regb & 0x04, h24 = regb & 0x02;
    bool pm = !h24 && (a.hour & 0x80);
    uint8_t hour = a.hour & 0x7F;
    if (!binary) {
        a.sec = bcd2bin(a.sec); a.min = bcd2bin(a.min); hour = bcd2bin(hour);
        a.day = bcd2bin(a.day); a.mon = bcd2bin(a.mon); a.year = bcd2bin(a.year);
        a.century = bcd2bin(a.century);
    }
    if (!h24)                                          /* 12 AM -> 0, 12 PM -> 12 */
        hour = (uint8_t)((hour % 12) + (pm ? 12 : 0));
    int century = (a.century >= 19 && a.century <= 21) ? a.century : 20;
    int y = century * 100 + a.year;
    if (a.mon < 1 || a.mon > 12 || a.day < 1 || a.day > 31)
        return 0;                                      /* garbage: leave the epoch */
    return days_from_civil(y, a.mon, a.day) * 86400 + hour * 3600 + a.min * 60 + a.sec;
}

/* Write the current system time back to the RTC, so a clock set with
 * settimeofday/clock_settime (date -s) survives a reboot. */
void timer_sync_rtc(void) {
    int64_t secs = timer_realtime_ns() / 1000000000LL;
    if (secs < 0)
        return;
    int64_t days = secs / 86400;
    int rem = (int)(secs % 86400);
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    uint8_t regb = cmos_read(0x0B);
    bool binary = regb & 0x04, h24 = regb & 0x02;
    uint8_t hour = (uint8_t)(rem / 3600);
    uint8_t hw = hour;
    if (!h24)
        hw = (uint8_t)(((hour % 12) == 0 ? 12 : hour % 12) | (hour >= 12 ? 0x80 : 0));
    uint8_t v[7] = { (uint8_t)(rem % 60), (uint8_t)((rem / 60) % 60), 0,
                     (uint8_t)d, (uint8_t)m, (uint8_t)(y % 100), (uint8_t)(y / 100) };
    cmos_write(0x0B, regb | 0x80);                     /* SET: freeze updates */
    uint8_t enc[7];
    for (int i = 0; i < 7; i++)
        enc[i] = (i == 2) ? 0 : (binary ? v[i] : bin2bcd(v[i]));
    enc[2] = binary ? hw : (uint8_t)(bin2bcd(hw & 0x7F) | (hw & 0x80));
    cmos_write(0, enc[0]); cmos_write(2, enc[1]); cmos_write(4, enc[2]);
    cmos_write(7, enc[3]); cmos_write(8, enc[4]); cmos_write(9, enc[5]);
    cmos_write(CMOS_CENTURY, enc[6]);
    cmos_write(0x0B, regb & (uint8_t)~0x80);           /* let the clock run */
}

void timer_init(unsigned hz) {
    configured_hz = hz;
    tsc_hz = calibrate_tsc();
    boot_tsc = rdtsc();
    tsc_per_tick = tsc_hz ? tsc_hz / hz : 0;
    kprintf("x86_64: TSC %lu MHz (PIT-calibrated)\n", (unsigned long)(tsc_hz / 1000000));

    timer_set_realtime_ns(rtc_unix_seconds() * 1000000000LL);

    uint32_t div = (uint32_t)(PIT_HZ / hz);
    outb(0x43, 0x34);                 /* ch0, lobyte/hibyte, mode 2 (rate generator) */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);

    register_irq_handler(ARCH_IRQ_TIMER, timer_irq_handler);
    pic_unmask(ARCH_IRQ_TIMER);
}
