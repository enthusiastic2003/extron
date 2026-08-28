#ifndef KERNEL_PROC_EXEC_H
#define KERNEL_PROC_EXEC_H

#include <kernel/proc/proc.h>

/*
 * Loads an ELF binary through the VFS (kernel/proc/exec.c's
 * load_binary_bytes(), against the caller's own cwd/credentials — root's
 * for a kernel-initiated boot spawn) into a fresh address space and
 * returns a ready-to-schedule struct proc, or NULL on failure (not
 * found, not executable, or a bad ELF). Mirrors x86's
 * proc_create_from_binary() (kernel/proc/exec.c on that side) — same
 * role, same name, simplified for now: no argv/envp/fork parent. The
 * returned process initially owns one schedulable main thread.
 *
 * Neither the framebuffer nor the keyboard input ring get special
 * exec-time treatment any more — both are real devices (/dev/fb0,
 * /dev/input; kernel/fs/devfs.c) a process opens and mmap()s itself,
 * through the real mmap() syscall, the same way any other program would
 * reach any other device. That's also why this takes no flags: execve()
 * is the plain three-argument POSIX call, with nothing extra to opt in
 * to — see the mistake the UART's identity mapping was until 268c962,
 * which PROC_MAP_FRAMEBUFFER (now removed) had repeated for the display.
 */

/* Bounds on what execve() will carry across. Small on purpose: the
 * whole argument block is laid out inside the single top page of the
 * new stack, so it cannot spill into the stack the process is about to
 * start using. */
#define EXEC_MAX_ARGS  32
#define EXEC_ARG_BYTES 1024
#define EXEC_MAX_ENVS  32
#define EXEC_ENV_BYTES 2048   /* total bytes of argv strings */

/* The current stack is eagerly mapped and fixed-size. Resource-limit
 * reporting uses this same definition so RLIMIT_STACK cannot drift away
 * from what exec actually constructs. */
#define EXEC_USER_STACK_BYTES (128UL * 1024UL)

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

struct proc *proc_create_from_binary(const char *binary_path);
struct proc *proc_create_from_binary_argv(const char *binary_path,
                                          const char *const *args, int argc);
struct proc *proc_create_from_binary_argv_env(const char *binary_path,
                                              const char *const *args, int argc,
                                              const char *const *envp, int envc);

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
                      const char *const *envp, int envc,
                      struct exec_image *out);

#endif
