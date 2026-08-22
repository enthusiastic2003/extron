#ifndef KERNEL_PROC_EXEC_H
#define KERNEL_PROC_EXEC_H

#include <kernel/proc/proc.h>

/*
 * Loads an ELF binary from the initrd (kernel/fs/tar.h) into a fresh
 * address space and returns a ready-to-schedule struct proc, or NULL on
 * failure (binary not found in the initrd, or a bad ELF). Mirrors
 * x86's proc_create_from_binary() (kernel/proc/exec.c on that side) —
 * same role, same name, simplified for now: no argv/envp/fork parent
 * (no syscalls, no fork yet), one execution context per proc.
 *
 * PROC_MAP_FRAMEBUFFER maps the display into the new process, at a
 * fixed VA with a descriptor page alongside it (see USER_FB_* in
 * kernel/proc/exec.c). Opt-in per process rather than mapped into
 * everything: a process with no business drawing shouldn't have the
 * display in its address space, which is the mistake the UART carried
 * until 268c962.
 *
 * Deliberately NOT a syscall. Handing a process its framebuffer at
 * creation is the same kind of act as handing it its stack, and
 * doomgeneric's porting contract is five DG_* functions with no
 * platform calls beyond them — a SYS_MAP_FB would be a syscall existing
 * only to undo a decision made moments earlier in the same kernel.
 */
#define PROC_MAP_FRAMEBUFFER (1u << 0)

struct proc *proc_create_from_binary(const char *binary_path, unsigned flags);

#endif
