#ifndef KERNEL_DRIVERS_EMMC_H
#define KERNEL_DRIVERS_EMMC_H

#include <stdint.h>
#include <stddef.h>

/* eMMC initialization and testing.
 * For v1, this will just init the controller and read Sector 0 (MBR). */
void emmc_init(void);

/* Block device read abstraction to hook into ext2 later. */
int emmc_read_sectors(uint64_t lba, size_t count, void *buf);

#endif /* KERNEL_DRIVERS_EMMC_H */
