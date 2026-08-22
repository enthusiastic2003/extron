#ifndef KERNEL_DRIVERS_MAILBOX_H
#define KERNEL_DRIVERS_MAILBOX_H

#include <stdint.h>
#include <stddef.h>

/*
 * VideoCore mailbox, property channel (channel 8).
 *
 * The GPU on a Pi is not a peripheral the ARM drives directly — it runs
 * its own firmware and owns the display, the clocks and its own slice of
 * RAM. The mailbox is the RPC channel to it: the ARM writes the bus
 * address of a message buffer to one register, the GPU reads that buffer
 * out of RAM, edits it in place with its answers, and signals back.
 *
 * Everything about that shape matters here. Because the GPU reads the
 * buffer out of RAM by itself rather than through the ARM's caches, the
 * message has to be flushed to the point of coherency before it is
 * handed over and invalidated before the reply is read — see
 * mailbox_property_call(). And because the GPU addresses memory through
 * its own bus window rather than by ARM physical address, every address
 * crossing this interface needs translating in both directions.
 */

/* Peripheral-block address of the mailbox registers. Same 0xFE000000
 * block as the UART, so it needs the same Device-memory mapping in the
 * kernel's own table — see mailbox_init(). */
#define MAILBOX_MMIO_PHYS 0xFE00B000ULL
#define MAILBOX_MMIO_SIZE 0x1000ULL

/*
 * ARM physical <-> VideoCore bus address.
 *
 * The GPU sees RAM through a window at 0xC0000000 (the "L2 coherent"
 * alias in Broadcom's terms). An ARM physical address must be presented
 * to it with that window applied, and an address it hands back must have
 * it stripped, or the value is off by 3GB and points at nothing.
 *
 * Valid for the low 1GB, which is where anything we hand the GPU lives:
 * the PMM allocates from a cursor that starts at 0 and the first memory
 * region is [0, 0x3B400000).
 */
#define BUS_TO_PHYS(b) ((uint64_t)(b) & 0x3FFFFFFFULL)
#define PHYS_TO_BUS(p) ((uint32_t)(((uint64_t)(p) & 0x3FFFFFFFULL) | 0xC0000000ULL))

/* Property tags used so far. The full set is large; these are what the
 * framebuffer path and its bring-up test need. */
#define TAG_GET_FIRMWARE_REV   0x00000001
#define TAG_GET_BOARD_MODEL    0x00010001
#define TAG_GET_ARM_MEMORY     0x00010005
#define TAG_GET_VC_MEMORY      0x00010006
#define TAG_FB_ALLOCATE        0x00040001
#define TAG_FB_GET_PITCH       0x00040008
#define TAG_FB_SET_PHYS_SIZE   0x00048003
#define TAG_FB_SET_VIRT_SIZE   0x00048004
#define TAG_FB_SET_DEPTH       0x00048005
#define TAG_FB_SET_PIXEL_ORDER 0x00048006
#define TAG_FB_SET_VIRT_OFFSET 0x00048009

/* Maps the mailbox registers into the kernel's table. Call once, after
 * paging is up. */
void mailbox_init(void);

/*
 * Send one property-channel message and wait for the reply.
 *
 * `msg` must be 16-byte aligned (the low 4 bits of the address carry the
 * channel number, so they have to be free) and msg[0] must hold the
 * total message size in bytes. The GPU edits the buffer in place, so
 * responses are read back out of the same array the request was built
 * in.
 *
 * Returns 0 if the firmware reported success, -1 otherwise.
 */
int mailbox_property_call(volatile uint32_t *msg);

/* Bring-up check: queries firmware revision, board model, and the ARM
 * and VideoCore memory splits, printing what comes back. Proves the
 * transport independently of any framebuffer code, and the VC memory
 * answer says exactly where the GPU's RAM is. */
void mailbox_report(void);

#endif
