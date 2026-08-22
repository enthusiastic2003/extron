#include <kernel/drivers/timer.h>
#include <arch/gic.h>
#include <arch/exceptions.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/console.h>
#include <kernel/mm/paging.h>

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
static unsigned configured_hz;

uint64_t timer_ticks(void) {
    return tick_count;
}

unsigned timer_ticks_per_second(void) {
    return configured_hz;
}

/* Debug-only: physical addresses of the two test procs' shared counter
 * pages (kernel_aarch64.c's 2-process scheduler test), watched here so
 * progress can be reported without either proc making a syscall — see
 * the scheduler bring-up plan. 0 = unset. */
static phys_addr_t watch_counter_a = 0;
static phys_addr_t watch_counter_b = 0;

void timer_set_counter_watch(phys_addr_t a, phys_addr_t b) {
    watch_counter_a = a;
    watch_counter_b = b;
}

static uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* Raw generic-timer counter. The isb is architecturally required: without
 * it the read can be speculated ahead of surrounding work and come back
 * stale, which is exactly the kind of thing that produces a clock that
 * occasionally ticks backwards. */
static uint64_t read_cntpct(void) {
    uint64_t v;
    __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

static uint64_t boot_cntpct;

/*
 * Milliseconds since timer_init(), read straight off CNTPCT_EL0 rather
 * than derived from tick_count.
 *
 * tick_count would be the obvious source and is the wrong one: the timer
 * runs at 20Hz, so it can only resolve 50ms. Anything wanting a real
 * clock — DOOM's tic rate is 35/sec, i.e. ~28.6ms — would get a value
 * quantised to less than half the resolution it needs. CNTPCT_EL0 runs
 * at CNTFRQ_EL0 (~54MHz on a Pi4), giving sub-microsecond resolution
 * with no interrupt involved at all, and is monotonic by construction.
 *
 * Rebased to boot so callers get a small number that can't plausibly
 * overflow the *1000: CNTPCT itself counts from system reset, and
 * multiplying an unrebased value by 1000 would wrap after ~10 years of
 * uptime. Rebasing makes that unreachable rather than merely unlikely.
 */
uint64_t timer_uptime_ms(void) {
    uint64_t freq = read_cntfrq();
    if (!freq)
        return 0;
    return ((read_cntpct() - boot_cntpct) * 1000ULL) / freq;
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
    proc_wakeup_expired(tick_count);

    if (watch_counter_a && watch_counter_b && (tick_count % 20) == 0) {
        uint64_t a = *(volatile uint64_t *)phys_to_virt_hhdm(watch_counter_a);
        uint64_t b = *(volatile uint64_t *)phys_to_virt_hhdm(watch_counter_b);
        kprintf("aarch64: tick %lu — proc A counter=%lu, proc B counter=%lu\n",
                (unsigned long)tick_count, (unsigned long)a, (unsigned long)b);
    }

    schedule();
}

void timer_init(unsigned hz) {
    uint64_t freq = read_cntfrq();
    ticks_per_period = freq / hz;
    configured_hz = hz;
    boot_cntpct = read_cntpct();

    register_irq_handler(GIC_PPI_NS_PHYS_TIMER, timer_irq_handler);
    gic_enable_irq(GIC_PPI_NS_PHYS_TIMER);

    write_tval(ticks_per_period);
    write_ctl(1); /* enable, unmasked */
}
