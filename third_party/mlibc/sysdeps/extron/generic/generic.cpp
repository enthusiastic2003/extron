#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <stdlib.h>            // exit() — the real termination path, see __mlibc_start_main
#include <mlibc/elf/startup.h> // mlibc::entry_stack — argc/argv, parsed by __dlapi_enter()

// ----------------------------------------------------------------
// 1. Raw Syscall Wrappers — aarch64: x8 = number, x0-x2 = args, result
//    in x0. Matches kernel/include/kernel/proc/syscall.h and
//    usr/include/extron/syscall.h's __syscall3() exactly — this is the
//    real AAPCS64/Linux syscall convention, not an extron-specific
//    choice, which is what lets these numbers and this calling
//    convention be shared verbatim with the kernel's own syscall
//    dispatch table.
// ----------------------------------------------------------------

// Make sure these match your kernel's syscall numbers
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_SLEEP       2
#define SYS_PROC_DUMP   3
#define SYS_ANON_ALLOC  4
#define SYS_ANON_FREE   5
#define SYS_TCB_SET     6   /* set TPIDR_EL0 for TLS (mlibc sys_tcb_set) */
#define SYS_EXIT        7   /* terminate current process */
#define SYS_FORK        8
#define SYS_EXECVE      9
#define SYS_WAIT        12  /* block until a child exits, then reap it */
#define SYS_OPEN        13
#define SYS_CLOSE       14
#define SYS_LSEEK       15
#define SYS_MKDIR       16
#define SYS_GETPID      17
#define SYS_GETPPID     18
#define SYS_GETCWD      19
#define SYS_CHDIR       20
#define SYS_READDIR     21
#define SYS_STAT        22
#define SYS_IOCTL       23
#define SYS_POLL        24


using main_fn = int (*)(int, char **);

/*
 * __dso_handle: __cxa_finalize()'s per-module handle, used by
 * options/lsb/generic/dso_exit.cpp's __mlibc_do_destructors() (a real
 * [[gnu::destructor]], run at exit) to invoke this "module"'s global
 * C++ destructors. checked directly against this toolchain's real
 * crtbegin.o/crtend.o (nm shows they exist and ARE on the link line —
 * they were never actually missing, unlike the earlier stage-1-libgcc
 * theory this comment used to have): crtbegin.o defines
 * __frame_dummy_init_array_entry (eh_frame registration), nothing
 * else. __dso_handle genuinely isn't a GCC-provided symbol at all —
 * it comes from libc's own startup code on every real target (glibc's
 * crtstuff.c does the same trick: point it at its own address). This
 * is that same file, just written for this libc instead. Any
 * consistent, process-unique value is correct when there is only ever
 * one module, which is always true here: fully static, no PIE, no
 * dlopen. */
extern "C" void *__dso_handle = &__dso_handle;

/* __ehdr_start: read by interpreterMain() (options/rtld/generic/
 * main.cpp) — the MLIBC_STATIC_BUILD branch of __dlapi_enter()'s own
 * bootstrap self-inspects its own ELF header at this symbol rather
 * than reading AT_PHDR from an auxv (static, non-PIE: the load address
 * is fixed and known at link time, so it doesn't need one).
 *
 * Deliberately NOT defined here as a C variable — `char __ehdr_start[1];`
 * would compile and link cleanly, but it just allocates an ordinary
 * object wherever the linker puts .bss, unrelated to where the image
 * is actually loaded (found the hard way: __ensure(entry_pointer)
 * failed inside interpreterMain, reading e_entry through a symbol that
 * pointed at .bss instead of the ELF header). The definition that
 * actually means what its name claims lives in the linker script
 * instead (lib/gcc/aarch64-extron/16.2.0/extron.ld's
 * `PROVIDE_HIDDEN (__ehdr_start = 0x400000)`, installed via the
 * compiler's own `specs` file so every link gets it automatically) —
 * that script is also what makes the address dereferenceable at all,
 * via FILEHDR+PHDRS on the text segment (ld's default script leaves a
 * page-aligned gap before .text otherwise). Confirmed with readelf -l:
 * the first LOAD segment's file offset is 0, vaddr is exactly 0x400000. */

extern "C" void __dlapi_enter(uintptr_t *);

extern "C" void __mlibc_start_main(uintptr_t *sp, main_fn program_main) {
    /*
     * __dlapi_enter() is mlibc's REAL bootstrap — not something this
     * port works around, but the thing it was missing. For a static,
     * non-PIE build it: finds this executable's own program headers
     * via __ehdr_start (confirmed by reading interpreterMain()
     * directly — the MLIBC_STATIC_BUILD branch never touches `sp` for
     * that part at all); registers the executable with
     * initialRepository (a frg::manual_box — __dlapi_exit() asserts on
     * it being initialized, so this is also what makes program exit
     * work); finds PT_TLS from those same headers and calls
     * allocateTcb() + mlibc::sys_tcb_set() to install a REAL Tcb (not
     * a hand-rolled stand-in — mlibc's own Tcb struct, options/
     * internal/include/mlibc/tcb.hpp, is bigger than the raw aarch64
     * ELF TLS ABI header and sits immediately before it, which is
     * exactly what a hand-rolled version got wrong: fork() crashed
     * dereferencing mlibc::get_current_tcb(), which assumes this real
     * layout); and runs every constructor in .init_array via
     * linker.initObjects() — including options/elf/generic/
     * startup.cpp's own init_libc(), a [[gnu::constructor]] that
     * parses `sp` as a genuine argc/argv/envp stack. That parse is
     * why `sp` has to be real: kernel/proc/exec.c's build_arg_stack()
     * builds exactly that shape now, specifically for this.
     */
    __dlapi_enter(sp);

    auto ret = program_main(mlibc::entry_stack.argc, mlibc::entry_stack.argv);
    /* exit(), not mlibc::sys_exit() directly: exit() is what runs the
     * atexit chain (__mlibc_do_finalize(), options/ansi/generic/
     * stdlib.cpp) — __dlapi_enter() already wired the real
     * initialRepository up, so __dlapi_exit() (called from inside
     * __mlibc_do_finalize()) tears the executable's own object down
     * correctly instead of needing a hand-rolled .fini_array walker. */
    exit(ret);
    __builtin_unreachable();
}


static inline long syscall0(long n) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile ("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long syscall1(long n, long a1) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long syscall2(long n, long a1, long a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}

static inline long syscall3(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

// ----------------------------------------------------------------
// 2. mlibc Required Sysdeps
// ----------------------------------------------------------------



namespace mlibc {

pid_t sys_getpid() { return (pid_t)syscall0(SYS_GETPID); }
pid_t sys_getppid() { return (pid_t)syscall0(SYS_GETPPID); }
uid_t sys_getuid() { return 0; }
uid_t sys_geteuid() { return 0; }
gid_t sys_getgid() { return 0; }
gid_t sys_getegid() { return 0; }

int sys_getcwd(char *buffer, size_t size) {
    return syscall2(SYS_GETCWD, (long)buffer, size) < 0 ? ERANGE : 0;
}

int sys_chdir(const char *path) {
    return syscall1(SYS_CHDIR, (long)path) < 0 ? ENOENT : 0;
}

int sys_open_dir(const char *path, int *handle) {
    long ret = syscall3(SYS_OPEN, (long)path, 0200000, 0);
    if (ret < 0) return ENOENT;
    *handle = (int)ret;
    return 0;
}

int sys_read_entries(int handle, void *buffer, size_t max_size,
                     size_t *bytes_read) {
    long ret = syscall3(SYS_READDIR, handle, (long)buffer, max_size);
    if (ret < 0) return EBADF;
    *bytes_read = (size_t)ret;
    return 0;
}

int sys_stat(fsfd_target target, int fd, const char *path, int,
             struct stat *statbuf) {
    long ret;
    if (target == fsfd_target::fd)
        ret = syscall3(SYS_STAT, 1, fd, (long)statbuf);
    else if (target == fsfd_target::path
            || (target == fsfd_target::fd_path && fd == AT_FDCWD))
        ret = syscall3(SYS_STAT, 0, (long)path, (long)statbuf);
    else
        return ENOTSUP;
    return ret < 0 ? ENOENT : 0;
}

int sys_sigaction(int, const struct sigaction *, struct sigaction *oldact) {
    if (oldact) memset(oldact, 0, sizeof(*oldact));
    return 0;
}

int sys_sigprocmask(int, const sigset_t *, sigset_t *oldset) {
    if (oldset) memset(oldset, 0, sizeof(*oldset));
    return 0;
}

// --- Panic & Logging (Crucial for debugging early boot) ---

void sys_libc_log(const char *message) {
    // Write directly to stdout (FD 1).
    // We calculate length manually since we can't use strlen yet.
    size_t len = 0;
    while (message[len]) len++;
    syscall3(SYS_WRITE, 1, (long)message, len);
}

void sys_libc_panic() {
    sys_libc_log("\n[mlibc] FATAL PANIC! Halting user process.\n");
    syscall1(SYS_EXIT, 1);
    /* Found the hard way: a bare `while (1);` has no side effects the
     * compiler can see, and a side-effect-free infinite loop is
     * undefined behavior in C++ — GCC is entitled to assume it
     * terminates and optimize it away, which at -O2 it actually did
     * here, letting execution fall through into whatever came next
     * (crt1.S's own trailing `brk`/trap, caught as a spurious "BRK
     * executed" exception, EC=0x3C, well after the process should have
     * been long gone). The asm volatile is a real side effect the
     * compiler can't reason away, so the loop is now actually
     * infinite. */
    for (;;) __asm__ volatile ("");
}

// --- Memory Management ---

int sys_anon_allocate(size_t size, void **pointer) {
    long ret = syscall1(SYS_ANON_ALLOC, size);
    if (ret < 0) {
        return -ret; // Return positive errno
    }
    *pointer = (void*)ret;
    return 0; // 0 means success
}

int sys_anon_free(void *pointer, size_t size) {
    long ret = syscall2(SYS_ANON_FREE, (long)pointer, size);
    if (ret < 0) return -ret;
    return 0;
}

// --- Threading & Execution ---

int sys_tcb_set(void *pointer) {
    long ret = syscall1(SYS_TCB_SET, (long)pointer);
    if (ret < 0) return -ret;
    return 0;
}

void sys_exit(int status) {
    syscall1(SYS_EXIT, status);
    /* Same bug as sys_libc_panic() above, and the one that actually
     * surfaced it: a plain `while (1);` is side-effect-free, so it's
     * UB, so GCC at -O2 removed it here — exit(), fully in the middle
     * of running the atexit chain, fell straight through into
     * whatever came after (crt1.S's trailing safety trap), instead of
     * actually halting. The kernel's own SYS_EXIT handler can
     * legitimately return here too (schedule() returns if nothing else
     * is runnable at that exact instant — see its own comment in
     * kernel/proc/syscall.c) — a real, reachable case, not just
     * insurance against a hypothetical. */
    for (;;) __asm__ volatile ("");
}

int sys_fork(pid_t *child) {
    long ret = syscall1(SYS_FORK, 0);
    if (ret < 0) return -ret;
    *child = ret;
    return 0;
}

/*
 * Never wired up before now — options/posix/generic/sys-wait.cpp's real
 * wait()/waitpid() call mlibc::sys_waitpid() ([[gnu::weak]] in
 * ansi-sysdeps.hpp), and with no definition here it resolved to the
 * weak default (null), which MLIBC_CHECK_OR_ENOSYS turns into exactly
 * the "missing sysdep" ENOSYS every other unimplemented syscall in this
 * file gets. Found by actually calling wait() through mlibc rather
 * than through usr/include/extron/syscall.h's own direct sys_wait()
 * wrapper (which never went through this file at all, so the gap was
 * invisible until something used the real libc entry point).
 *
 * The kernel's own SYS_WAIT (kernel/proc/syscall.c's sys_wait()) is
 * narrower than POSIX waitpid(): it always blocks, always reaps
 * whichever child becomes a zombie first, and has no notion of
 * targeting one specific pid or of WNOHANG. Rather than silently
 * pretending to support what it can't, anything outside that one
 * shape — a real pid instead of -1 ("any child"), or any flag at all —
 * is refused with ENOSYS, the same honest signal every other
 * partially-implemented sysdep in this file gives.
 */
int sys_waitpid(pid_t pid, int *status, int flags, struct rusage *ru, pid_t *ret_pid) {
    (void)ru;
    if (pid != -1 || flags != 0)
        return ENOSYS;

    int st = 0;
    long reaped = syscall1(SYS_WAIT, (long)&st);
    if (reaped < 0)
        return ECHILD; /* the kernel's only failure mode: no children at all */

    /* The kernel's SYS_WAIT hands back the raw value the child passed
     * to sys_exit() — 42 means literally 42. WIFEXITED/WEXITSTATUS
     * (sysdeps/extron/include/abi-bits/wait.h, same encoding as Linux)
     * expect the POSIX wait-status encoding instead: the low 7 bits
     * are a termination signal (0 for "exited normally", which is all
     * this kernel's exit ever means — there's no signal delivery yet),
     * and the exit code lives in bits 8-15. Encoding it here, once, is
     * what WIFEXITED(status) && WEXITSTATUS(status) == N actually
     * needs to see N — found by that exact check failing although the
     * kernel's own [SYSCALL exit] log line confirmed the child really
     * did exit with status 42. */
    if (status)
        *status = (st & 0xff) << 8;
    *ret_pid = (pid_t)reaped;
    return 0;
}

int sys_execve(const char *path, char *const argv[], char *const envp[]) {
    long ret = syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
    if (ret < 0) return -ret;
    return 0;
}

int sys_sleep(time_t *secs, long *nanos) {
    long ret = syscall2(SYS_SLEEP, *secs, nanos ? *nanos : 0);
    if (ret < 0) {
        return -ret;
    }

    // We don't support early wakeups from signals yet,
    // so we assume the full duration elapsed.
    *secs = 0;
    if (nanos)
        *nanos = 0;

    return 0;
}

// --- Basic I/O ---

int sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read) {
    long ret = syscall3(SYS_READ, fd, (long)buf, count);
    if (ret < 0) {
        return -ret;
    }
    *bytes_read = ret;
    return 0;
}

int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
    long ret = syscall3(SYS_WRITE, fd, (long)buf, count);
    if (ret < 0) return -ret;
    *bytes_written = ret;
    return 0;
}

// ----------------------------------------------------------------
// 3. Stubbing out the rest
// ----------------------------------------------------------------
// mlibc will compile against these but fail safely if called.

int sys_open(const char *pathname, int flags, mode_t mode, int *fd) {
    long ret = syscall3(SYS_OPEN, (long)pathname, flags, mode);
    if (ret < 0)
        return ENOENT;
    *fd = (int)ret;
    return 0;
}

int sys_close(int fd) {
    long ret = syscall1(SYS_CLOSE, fd);
    return ret < 0 ? EBADF : 0;
}

int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) {
    if (fd == 0 || fd == 1 || fd == 2)
        return ESPIPE;  // not seekable — tells mlibc to treat as pipe_like
    long ret = syscall3(SYS_LSEEK, fd, offset, whence);
    if (ret < 0)
        return EINVAL;
    *new_offset = ret;
    return 0;
}

int sys_mkdir(const char *path, mode_t mode) {
    long ret = syscall2(SYS_MKDIR, (long)path, mode);
    return ret < 0 ? EIO : 0;
}

int sys_vm_map(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
    sys_libc_log("[mlibc stub] sys_vm_map called\n");
    return ENOSYS; // You will implement this later for file-backed mapping
}

int sys_vm_unmap(void *pointer, size_t size) {
    long ret = syscall2(SYS_ANON_FREE, (long)pointer, size);
    if (ret < 0) return -ret;
    return 0;
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time) {
    (void)pointer; (void)expected; (void)time;
    return ENOSYS;
}
int sys_futex_wake(int *pointer) {
    (void)pointer;
    return ENOSYS;
}

int sys_clock_get(int clock, time_t *secs, long *nanos) {
    *secs = 0;
    *nanos = 0;
    return 0;
}

int sys_ioctl(int fd, unsigned long request, void *arg, int *result) {
    long ret = syscall3(SYS_IOCTL, fd, request, (long)arg);
    if (ret < 0) return fd <= 2 ? EINVAL : ENOTTY;
    if (result) *result = (int)ret;
    return 0;
}

int sys_tcgetattr(int fd, struct termios *attr) {
    int result;
    return sys_ioctl(fd, TCGETS, attr, &result);
}

int sys_tcsetattr(int fd, int optional_action, const struct termios *attr) {
    unsigned long request;
    switch (optional_action) {
        case TCSANOW: request = TCSETS; break;
        case TCSADRAIN: request = TCSETSW; break;
        case TCSAFLUSH: request = TCSETSF; break;
        default: return EINVAL;
    }
    int result;
    return sys_ioctl(fd, request, const_cast<struct termios *>(attr), &result);
}

int sys_poll(struct pollfd *fds, nfds_t count, int timeout, int *num_events) {
    long ret = syscall3(SYS_POLL, (long)fds, count, timeout);
    if (ret < 0) return EINVAL;
    *num_events = (int)ret;
    return 0;
}

// Add any other functions mlibc complains about during linking as ENOSYS stubs here...

int sys_isatty(int fd) {

    if (fd == 0 || fd == 1 || fd == 2)
        return 0;
    return ENOTTY;
}



} // namespace mlibc
