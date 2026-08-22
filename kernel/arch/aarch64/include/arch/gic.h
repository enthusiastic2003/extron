#ifndef ARCH_GIC_H
#define ARCH_GIC_H

/* Non-secure physical timer PPI — verified against the real device tree
 * (boot/aarch64/bcm2711-rpi-4-b.dtb): the "timer" node's second
 * interrupts entry, <1 0x0e 0xf08> (PPI number 14), giving INTID 16+14.
 * EL1 without secure/hypervisor involvement uses CNTP_TVAL_EL0/
 * CNTP_CTL_EL0 (the "physical timer"), which is exactly this one. */
#define GIC_PPI_NS_PHYS_TIMER 30

/* UART0 (PL011) SPI — verified against the real device tree: the
 * "serial@7e201000" node's interrupts property, <0 0x79 4> (type 0 =
 * SPI, ID 0x79 = 121, level-high). SPI numbering starts at GIC INTID
 * 32, so the real interrupt ID is 32 + 121 = 153. */
#define GIC_SPI_UART0 153

void gic_init(void);

/* Enables one interrupt ID at the distributor (SGI/PPI: 0-31; SPI: 32+).
 * Does not set priority/routing beyond GIC-400's power-on defaults,
 * which is adequate for the single-core, single-priority use this
 * milestone needs. */
void gic_enable_irq(unsigned id);

/* GICC_IAR / GICC_EOIR — acknowledge (read the pending interrupt ID) and
 * end-of-interrupt (mark it serviced), called from exceptions.c's IRQ
 * dispatch path. */
unsigned gic_ack_irq(void);
void gic_eoi_irq(unsigned id);

#endif
