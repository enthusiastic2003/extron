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

/* Bounds on what execve() will carry across. Small on purpose: the
 * whole argument block is laid out inside the single top page of the
 * new stack, so it cannot spill into the stack the process is about to
 * start using. */
#define EXEC_MAX_ARGS  32
#define EXEC_ARG_BYTES 1024   /* total bytes of argv strings */

/* A fully built, not-yet-installed user address space. execve()'s whole
 * safety argument is that this can be constructed and thrown away
 * without the caller's own address space ever being touched. */
struct exec_image {
    struct vm_space *mm;
    phys_addr_t      ttbr0;
    virt_addr_t      entry;
    virt_addr_t      user_sp;
    uint64_t         argc;
    virt_addr_t      argv;      /* user VA of the argv[] array */
};

struct proc *proc_create_from_binary(const char *binary_path, unsigned flags);
struct proc *proc_create_from_binary_argv(const char *binary_path, unsigned flags,
                                          const char *const *args, int argc);

/*
 * execve()'s address-space half: build `binary_path` into a new image
 * and swap it in for `p`'s current one, freeing the old.
 *
 * Returns 0 with *out describing where the process must resume (entry,
 * stack pointer, argc/argv), or -1 having changed nothing at all — the
 * failed-exec case where the caller must keep running its existing
 * program, which is exactly why the new image is built to completion
 * before the old one is disturbed.
 *
 * Only rewrites the address space. Getting the process to actually
 * resume at the new entry point is the caller's job (sys_execve()
 * rewrites the trap frame; see kernel/proc/syscall.c).
 */
int proc_exec_replace(struct proc *p, const char *binary_path,
                      const char *const *args, int argc,
                      struct exec_image *out);

#endif
