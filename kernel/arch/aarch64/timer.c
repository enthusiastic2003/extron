#include <arch/timer.h>
#include <arch/gic.h>
#include <arch/exceptions.h>
#include <kernel/console.h>

/*
 * ARM generic timer, non-secure physical timer (CNTP_TVAL_EL0/
 * CNTP_CTL_EL0) — the aarch64 counterpart to kernel/arch/x86_64/irq/
 * pit_irq.c, but a genuinely different timer: the PIT is a discrete,
 * port-I/O-programmed chip; the ARM generic timer is a per-core system
 * register counting a fixed reference frequency (read from CNTFRQ_EL0,
 * not something we choose), with no equivalent to the PIT's own
 * divisor/reload-port programming model.
 */

static uint64_t ticks_per_period;
static volatile uint64_t tick_count = 0;

static uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void write_tval(uint64_t v) {
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(v));
}

static void write_ctl(uint64_t v) {
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(v));
}

static void timer_irq_handler(struct aarch64_frame *f) {
    (void)f;
    write_tval(ticks_per_period); /* re-arm for the next period */
    tick_count++;
    kprintf("aarch64: timer tick %lu\n", (unsigned long)tick_count);
}

void timer_init(unsigned hz) {
    uint64_t freq = read_cntfrq();
    ticks_per_period = freq / hz;

    register_irq_handler(GIC_PPI_NS_PHYS_TIMER, timer_irq_handler);
    gic_enable_irq(GIC_PPI_NS_PHYS_TIMER);

    write_tval(ticks_per_period);
    write_ctl(1); /* enable, unmasked */
}
