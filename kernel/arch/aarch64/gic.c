#include <arch/gic.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>

/*
 * GIC-400 bring-up — RPi4's actual interrupt controller (not the 8259
 * PIC x86 uses; kernel/arch/x86_64/pic/pic.c has no equivalent here at
 * all).
 *
 * GICD_BASE/GICC_BASE below are NOT the raw "reg" value from the device
 * tree's "interrupt-controller@40041000" node (0x40041000/0x40042000) —
 * that value is expressed in /soc's child bus-address space, not real
 * CPU-physical. /soc's own "ranges" property (verified against the real
 * device tree) translates child [0x40000000, 0x40800000) to physical
 * [0xFF800000, 0xFF800000+0x800000): reg 0x40041000 -> physical
 * 0xFF800000 + (0x40041000-0x40000000) = 0xFF841000. Using the untranslated
 * address was a real, previously-undiagnosed bug: reads/writes there
 * round-tripped cleanly (so it LOOKED like correct GIC configuration)
 * because that low child-address window is also where the legacy QA7
 * local interrupt controller (device tree's "local_intc") lives — every
 * register write was landing on real, RW-consistent hardware, just never
 * the actual GIC-400, which is why no amount of "correct" GICD_CTLR/
 * GICD_IGROUPR/GICC_CTLR configuration ever caused a single interrupt to
 * be delivered.
 *
 * Mapped at NEW_HDDM + physaddr via kmap(), same convention the HHDM
 * itself uses for RAM — but with PAGE_CACHE_DISABLE, since these are
 * MMIO registers, not memory (see paging_aarch64.c's init_paging()
 * comment on why the old, now-removed map_uart_device() never actually
 * worked: a bare physical-looking address there would be silently inert
 * for the same reason a Normal-memory GIC mapping would be actively
 * wrong here — caching device register accesses can drop or reorder the
 * side effects that make them work at all).
 */

#define GICD_BASE 0xFF841000ULL
#define GICC_BASE 0xFF842000ULL

#define GICD_CTLR       0x000
#define GICD_IGROUPR    0x080
#define GICD_ISENABLER  0x100
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800

#define GICC_CTLR 0x000
#define GICC_PMR  0x004
#define GICC_IAR  0x00C
#define GICC_EOIR 0x010

static volatile uint32_t *gicd;
static volatile uint32_t *gicc;

static volatile uint32_t *mmio_map_device(uint64_t phys, uint64_t size) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        int res = kmap(NEW_HDDM + phys + off, phys + off,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE);
        if (res != 0) panic("gic: failed to map MMIO at %p (res %d)", (void *)(phys + off), res);
    }
    return (volatile uint32_t *)(NEW_HDDM + phys);
}

void gic_init(void) {
    gicd = mmio_map_device(GICD_BASE, 0x1000);
    gicc = mmio_map_device(GICC_BASE, 0x2000);

    gicd[GICD_CTLR / 4] = 0; /* disable while configuring */

    /* Move every SGI/PPI/SPI into Group 1 (non-secure). GICD_IGROUPRn is
     * an ARRAY, one 32-bit register per 32 interrupt IDs — GICD_IGROUPR0
     * (below) only covers IDs 0-31 (SGIs/PPIs, enough for the timer).
     * SPIs (32+, e.g. GIC_SPI_UART0=153, which lands in bank index 4)
     * live in DIFFERENT registers this loop must also reach, or they'd
     * sit at their own Group 0 reset default — the exact bug that cost
     * a full debugging pass for the timer PPI, just for a different
     * interrupt ID. 16 banks covers IDs 0-511, well past anything
     * BCM2711 implements. If the GIC's security extensions aren't
     * implemented at all, these writes are simply ignored (no separate
     * groups to move between) — harmless either way, and required if
     * they ARE implemented: with them active, GICD_CTLR's bit0 only
     * enables Group 0, and an interrupt left in its Group 0 default
     * would sit pending at the distributor forever, never forwarded to
     * the CPU interface. */
    for (int bank = 0; bank < 16; bank++) {
        gicd[GICD_IGROUPR / 4 + bank] = 0xFFFFFFFF;
    }

    gicd[GICD_CTLR / 4] = 0x3; /* enable Group 0 + Group 1 */
    gicc[GICC_PMR / 4]  = 0xFF; /* allow all interrupt priorities through */

    /* Not just 0x3 (EnableGrp0|EnableGrp1): a plain store overwrites the
     * WHOLE register, and this GIC-400 instantiation apparently doesn't
     * gate GICC_CTLR's extra bits behind Secure/Non-secure aliasing the
     * way the architecture allows — confirmed by readback after a 0x3
     * write showing exactly 0x3, not some NS-only subset. Our own EL3
     * armstub (boot/aarch64/armstub8-gic.bin, built from Raspberry Pi's
     * official armstub8.S with -DGIC=1 -DBCM2711=1) sets GICC_CTLR to
     * 0x1e7 = EnableGrp0|EnableGrp1|AckCtl|the four *BypDisGrp* bits;
     * writing plain 0x3 here was clobbering AckCtl (bit 2) right back to
     * 0, which is what caused gic_ack_irq() to return 1022 ("a Group 0
     * interrupt is pending but hidden from Non-secure view" — GICv2's
     * sentinel for exactly this misconfiguration) instead of the real
     * timer IRQ's ID, even though GICD_IGROUPR had already moved it to
     * Group 1. Matching the stub's value here keeps it self-contained
     * rather than depending on us never re-touching this register after
     * the stub configured it. */
    gicc[GICC_CTLR / 4] = 0x1e7;
}

void gic_enable_irq(unsigned id) {
    /* SPIs (32+) need explicit CPU targeting on GICv2 — unlike PPIs
     * (implicitly banked per-core, no routing decision to make), an SPI
     * with no target CPU set in GICD_ITARGETSRn just sits pending at
     * the distributor forever, never forwarded to any CPU interface —
     * enabled, correctly grouped, genuinely pending, and still never
     * delivered. GICD_ITARGETSRn is byte-per-interrupt-ID (4 IDs per
     * 32-bit register, unlike the bit-per-ID IGROUPR/ISENABLER above),
     * so this is a byte-level read-modify-write: value 0x01 in an ID's
     * byte targets CPU interface 0, the only core running this kernel
     * (boot.S parks the others). PPI/SGI bytes (id<32) are read-only,
     * hardwired to the banked "this CPU" affinity — writing them would
     * just be silently ignored, so gating on id>=32 is a clarity choice,
     * not a correctness requirement. */
    if (id >= 32) {
        gicd[GICD_ITARGETSR / 4 + id / 4] |= (1U << ((id % 4) * 8));
    }
    gicd[GICD_ISENABLER / 4 + (id / 32)] = (1U << (id % 32));
}

unsigned gic_ack_irq(void) {
    return gicc[GICC_IAR / 4] & 0x3FF; /* low 10 bits: interrupt ID */
}

void gic_eoi_irq(unsigned id) {
    gicc[GICC_EOIR / 4] = id;
}
