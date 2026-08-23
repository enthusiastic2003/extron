/*
 * Resets the machine via SYS_REBOOT (kernel/proc/syscall.c's
 * sys_reboot(), kernel/drivers/power.c's power_reset()) — the BCM2711
 * PM watchdog, the only way to reset the SoC from software. Root-only,
 * same as real Unix's reboot(2); the check here is just a friendlier
 * error message; the kernel enforces it regardless.
 *
 * include/extron/syscall.h, not <extron/syscall.h>: this is a relative
 * path from usr/ itself, the same trick usr/doom/doomgeneric_extron.c
 * uses one directory level down — the mlibc sysroot doesn't expose
 * usr/include/extron/ on its own search path.
 */
#include <stdio.h>
#include <unistd.h>
#include "include/extron/syscall.h"

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "reboot: must be root\n");
        return 1;
    }

    printf("Rebooting...\n");
    fflush(stdout);

    long result = sys_reboot();

    /* Only reachable if the kernel refused — success never returns. */
    fprintf(stderr, "reboot: failed (%ld)\n", result);
    return 1;
}
