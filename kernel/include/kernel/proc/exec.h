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
 */
struct proc *proc_create_from_binary(const char *binary_path);

#endif
