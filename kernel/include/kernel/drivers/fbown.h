#ifndef KERNEL_DRIVERS_FBOWN_H
#define KERNEL_DRIVERS_FBOWN_H

#include <stdbool.h>

struct proc;

/*
 * Display ownership. Mapping /dev/fb0 takes the display for the lifetime of the
 * mapping: the kernel text console stops painting (its cell grid keeps tracking
 * output, so it repaints in full afterwards), and the keyboard is diverted from
 * the console tty to the program's input rings (Ctrl-C and Ctrl-\ still reach the
 * tty, so SIGINT/SIGQUIT remain an escape hatch).
 *
 * The owner is the process whose address space holds a VM_FBOWNER mapping. There
 * is no release call to forget: exit, kill, munmap() and exec all end it, and the
 * kernel notices lazily (fbown_check() runs on exit, munmap and every console
 * write, so the console comes back as soon as anything touches it).
 *
 * Only one process owns the display at a time: a second mmap() of /dev/fb0 while
 * the first is alive fails with EBUSY.
 */

/* Take the display for `p`, or -EBUSY if another live process holds it. */
int  fbown_acquire(struct proc *p);
/* Give the display back if the owner is gone or no longer maps it. */
void fbown_check(void);
/* True while a live process owns the display. */
bool fbown_active(void);

#endif
