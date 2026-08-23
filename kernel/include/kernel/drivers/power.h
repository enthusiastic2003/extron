#ifndef KERNEL_DRIVERS_POWER_H
#define KERNEL_DRIVERS_POWER_H

/* Maps the BCM2711 PM block's one page into the kernel's own high-half
 * table (same convention as mailbox_init()/gic.c) — call once during
 * boot, before power_reset() can be used. */
void power_init(void);

/* Triggers a full hardware reset via the BCM2711 PM watchdog — the same
 * mechanism the firmware itself and every other bare-metal Pi kernel
 * uses, since there is no other way to reset the SoC from software.
 * Never returns: either the watchdog fires and the machine restarts, or
 * (SoC held reset via JTAG/QEMU not implementing the block) it spins
 * forever rather than returning control somewhere the caller isn't
 * expecting it back from. */
void power_reset(void) __attribute__((noreturn));

#endif
