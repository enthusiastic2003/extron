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
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>            // exit() — the real termination path, see __mlibc_start_main
#include <mlibc/elf/startup.h> // mlibc::entry_stack — argc/argv, parsed by __dlapi_enter()
#include <mlibc/tcb.hpp>

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
#define SYS_PIPE        25
#define SYS_DUP         26
#define SYS_DUP2        27
#define SYS_FCNTL       28
#define SYS_GETTID      29
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT 31
#define SYS_THREAD_JOIN 32
#define SYS_FUTEX_WAIT  33
#define SYS_FUTEX_WAKE  34
#define SYS_SIGACTION   35
#define SYS_KILL        36
#define SYS_SIGPROCMASK 37
#define SYS_SIGRETURN   38
#define SYS_TGKILL      39
#define SYS_GETPGID     40
#define SYS_SETPGID     41
#define SYS_SETSID      42
#define SYS_UNLINK      43
#define SYS_RMDIR       44
#define SYS_RENAME      45
#define SYS_LINK        46
#define SYS_SYMLINK     47
#define SYS_READLINK    48
#define SYS_PATH_AT     49
#define SYS_GETUID      50
#define SYS_GETEUID     51
#define SYS_GETGID      52
#define SYS_GETEGID     53
#define SYS_SETUID      54
#define SYS_SETEUID     55
#define SYS_SETGID      56
#define SYS_SETEGID     57
#define SYS_GETGROUPS   58
#define SYS_SETGROUPS   59
#define SYS_UMASK       60
#define SYS_CLOCK_GET   61
#define SYS_CLOCK_SET   62
#define SYS_FCHMOD      63
#define SYS_FCHOWN      64
#define SYS_FUTIMENS    65
#define SYS_FCHDIR      66
#define SYS_SETRESUID   67
#define SYS_SETRESGID   68
#define SYS_MMAP        69
#define SYS_MUNMAP      70
#define SYS_PAUSE       72
#define SYS_MPROTECT    73
#define SYS_MSYNC       74


using main_fn = int (*)(int, char **);

struct path_at_request {
    uint64_t op;
    int64_t dirfd1;
    uint64_t path1;
    int64_t dirfd2;
    uint64_t path2;
    uint64_t buffer;
    uint64_t size;
    uint64_t flags;
};

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
#ifdef MLIBC_STATIC_BUILD
extern "C" void *__dso_handle = &__dso_handle;
#endif

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

static long path_at(path_at_request &request) {
    return syscall1(SYS_PATH_AT, (long)&request);
}

extern "C" long __mlibc_do_asm_cp_syscall(long, long, long, long);

static inline long cp_syscall3(long n, long a1, long a2, long a3) {
    return __mlibc_do_asm_cp_syscall(n, a1, a2, a3);
}

// ----------------------------------------------------------------
// 2. mlibc Required Sysdeps
// ----------------------------------------------------------------



namespace mlibc {

/* Static archives are only searched for unresolved strong symbols. mlibc's
 * optional sysdep declarations are weak, so this anchor makes the separate
 * threading translation unit (and its assembly trampoline) part of libc users
 * that already pull this generic sysdep object in. */
extern "C" void __extron_thread_sysdeps_anchor();

pid_t sys_getpid() { return (pid_t)syscall0(SYS_GETPID); }
pid_t sys_getppid() { return (pid_t)syscall0(SYS_GETPPID); }
uid_t sys_getuid() { return (uid_t)syscall0(SYS_GETUID); }
uid_t sys_geteuid() { return (uid_t)syscall0(SYS_GETEUID); }
gid_t sys_getgid() { return (gid_t)syscall0(SYS_GETGID); }
gid_t sys_getegid() { return (gid_t)syscall0(SYS_GETEGID); }
int sys_setuid(uid_t uid) {
    long ret = syscall1(SYS_SETUID, uid);
    return ret < 0 ? -ret : 0;
}
int sys_seteuid(uid_t uid) {
    long ret = syscall1(SYS_SETEUID, uid);
    return ret < 0 ? -ret : 0;
}
int sys_setgid(gid_t gid) {
    long ret = syscall1(SYS_SETGID, gid);
    return ret < 0 ? -ret : 0;
}
int sys_setegid(gid_t gid) {
    long ret = syscall1(SYS_SETEGID, gid);
    return ret < 0 ? -ret : 0;
}
int sys_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    long ret = syscall3(SYS_SETRESUID, ruid, euid, suid);
    return ret < 0 ? -ret : 0;
}
int sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
    long ret = syscall3(SYS_SETRESGID, rgid, egid, sgid);
    return ret < 0 ? -ret : 0;
}
int sys_getgroups(size_t size, gid_t *list, int *ret_count) {
    long ret = syscall2(SYS_GETGROUPS, size, (long)list);
    if (ret < 0) return -ret;
    *ret_count = (int)ret;
    return 0;
}
int sys_setgroups(size_t size, const gid_t *list) {
    long ret = syscall2(SYS_SETGROUPS, size, (long)list);
    return ret < 0 ? -ret : 0;
}
int sys_umask(mode_t mode, mode_t *old) {
    *old = (mode_t)syscall1(SYS_UMASK, mode);
    return 0;
}

int sys_getcwd(char *buffer, size_t size) {
    long ret = syscall2(SYS_GETCWD, (long)buffer, size);
    return ret < 0 ? -ret : 0;
}

int sys_chdir(const char *path) {
    long ret = syscall1(SYS_CHDIR, (long)path);
    return ret < 0 ? -ret : 0;
}

int sys_fchdir(int fd) {
    long ret = syscall1(SYS_FCHDIR, fd);
    return ret < 0 ? -ret : 0;
}

int sys_open_dir(const char *path, int *handle) {
    long ret = syscall3(SYS_OPEN, (long)path, 0200000, 0);
    if (ret < 0) return -ret;
    *handle = (int)ret;
    return 0;
}

int sys_read_entries(int handle, void *buffer, size_t max_size,
                     size_t *bytes_read) {
    long ret = syscall3(SYS_READDIR, handle, (long)buffer, max_size);
    if (ret < 0) return -ret;
    *bytes_read = (size_t)ret;
    return 0;
}

int sys_stat(fsfd_target target, int fd, const char *path, int flags,
             struct stat *statbuf) {
    long ret;
    if (target == fsfd_target::fd)
        ret = syscall3(SYS_STAT, 1, fd, (long)statbuf);
    else if (target == fsfd_target::path
            || (target == fsfd_target::fd_path && fd == AT_FDCWD))
        ret = syscall3(SYS_STAT, flags & AT_SYMLINK_NOFOLLOW ? 2 : 0,
                       (long)path, (long)statbuf);
    else if (target == fsfd_target::fd_path) {
        path_at_request request{6, fd, (uint64_t)path, 0, 0,
                                (uint64_t)statbuf, sizeof(*statbuf),
                                (uint64_t)flags};
        ret = path_at(request);
    }
    else
        return ENOTSUP;
    return ret < 0 ? -ret : 0;
}

int sys_access(const char *path, int mode) {
    return sys_faccessat(AT_FDCWD, path, mode, 0);
}

extern "C" void __mlibc_signal_restore();

int sys_sigaction(int signo, const struct sigaction *action,
                  struct sigaction *oldact) {
    struct sigaction installed;
    const struct sigaction *input = action;
    if (action) {
        installed = *action;
        installed.sa_flags |= SA_RESTORER;
        installed.sa_restorer = __mlibc_signal_restore;
        input = &installed;
    }
    long ret = syscall3(SYS_SIGACTION, signo, (long)input, (long)oldact);
    return ret < 0 ? -ret : 0;
}

int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    long ret = syscall3(SYS_SIGPROCMASK, how, (long)set, (long)oldset);
    return ret < 0 ? -ret : 0;
}

int sys_thread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    return sys_sigprocmask(how, set, oldset);
}

int sys_kill(int pid, int signal) {
    long ret = syscall2(SYS_KILL, pid, signal);
    return ret < 0 ? -ret : 0;
}

int sys_tgkill(int pid, int tid, int signal) {
    long ret = syscall3(SYS_TGKILL, pid, tid, signal);
    return ret < 0 ? -ret : 0;
}

extern "C" const char __mlibc_syscall_begin[1];
extern "C" const char __mlibc_syscall_end[1];

int sys_before_cancellable_syscall(ucontext_t *context) {
    uintptr_t pc = context->uc_mcontext.pc;
    return pc >= reinterpret_cast<uintptr_t>(__mlibc_syscall_begin)
        && pc <= reinterpret_cast<uintptr_t>(__mlibc_syscall_end);
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
    __extron_thread_sysdeps_anchor();
    /* AArch64's ABI thread pointer is 16 bytes before the TLS block, not
     * the address of mlibc's Tcb object itself. */
    auto tp = reinterpret_cast<char *>(pointer) + sizeof(Tcb) - 0x10;
    long ret = syscall1(SYS_TCB_SET, (long)tp);
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
 * The kernel's own SYS_WAIT (kernel/proc/syscall.c's sys_wait()) now
 * takes a real pid selector (-1 for "any child") and WNOHANG/WUNTRACED/
 * WCONTINUED, via proc_find_waitable_child() — this used to be narrower
 * (blocking-only, "any child" only), which is what the paragraph here
 * described until the job-control work generalized it. Only a flag
 * outside that set is still refused (with a plain failure, not ENOSYS —
 * there's nothing left this sysdep can't express).
 */
int sys_waitpid(pid_t pid, int *status, int flags, struct rusage *ru, pid_t *ret_pid) {
    (void)ru;
    if (flags & ~(WNOHANG | WUNTRACED | WCONTINUED))
        return EINVAL;

    int st = 0;
    long reaped = syscall3(SYS_WAIT, (long)&st, pid, flags);
    if (reaped < 0)
        return ECHILD; /* the kernel's only failure mode: no children at all */

    /* The kernel's SYS_WAIT hands back either the raw value the child passed
     * to sys_exit() (42 means literally 42), or a negative signal number for
     * a fatal EL0 fault. WIFEXITED/WEXITSTATUS
     * (sysdeps/extron/include/abi-bits/wait.h, same encoding as Linux)
     * expect the POSIX wait-status encoding instead: the low 7 bits
     * are a termination signal (0 for "exited normally"), and the exit code
     * lives in bits 8-15. Encoding it here, once, is
     * what WIFEXITED(status) && WEXITSTATUS(status) == N actually
     * needs to see N — found by that exact check failing although the
     * kernel's own [SYSCALL exit] log line confirmed the child really
     * did exit with status 42. */
    if (status) {
        /* Internal transition tags are small positive values.  Testing a
         * single tag bit is incorrect for signal deaths: e.g. -SIGTERM is
         * 0xfffffff1 and therefore has every high tag bit set. */
        if ((st & ~0xff) == 0x10000)
            *status = ((st & 0xff) << 8) | 0x7f;
        else if (st == 0x20000)
            *status = 0xffff;
        else
            *status = st < 0 ? (-st & 0x7f) : ((st & 0xff) << 8);
    }
    *ret_pid = (pid_t)reaped;
    return 0;
}

int sys_getpgid(pid_t pid, pid_t *pgid) {
    long ret = syscall1(SYS_GETPGID, pid);
    if (ret < 0) return -ret;
    *pgid = (pid_t)ret;
    return 0;
}

int sys_setpgid(pid_t pid, pid_t pgid) {
    long ret = syscall2(SYS_SETPGID, pid, pgid);
    return ret < 0 ? -ret : 0;
}

int sys_setsid(pid_t *sid) {
    long ret = syscall0(SYS_SETSID);
    if (ret < 0) return -ret;
    *sid = (pid_t)ret;
    return 0;
}

int sys_execve(const char *path, char *const argv[], char *const envp[]) {
    long ret = syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
    if (ret < 0) return -ret;
    return 0;
}

int sys_pause() {
    // The kernel side (kernel/proc/syscall.c's sys_pause()) only ever
    // returns via a signal — there is no successful-completion path for
    // pause() at all, per POSIX (see unistd.cpp's own __ensure on this) —
    // so the raw syscall's result is always a negative errno (-EINTR),
    // never 0. cp_syscall3, not the plain syscall helper, since a
    // thread blocked in pause() must be a valid pthread_cancel() target
    // like any other blocking call.
    long ret = cp_syscall3(SYS_PAUSE, 0, 0, 0);
    return (int)-ret;
}

int sys_sleep(time_t *secs, long *nanos) {
    long ret = cp_syscall3(SYS_SLEEP, *secs, nanos ? *nanos : 0, 0);
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
    long ret = cp_syscall3(SYS_READ, fd, (long)buf, count);
    if (ret < 0) {
        return -ret;
    }
    *bytes_read = ret;
    return 0;
}

int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
    long ret = cp_syscall3(SYS_WRITE, fd, (long)buf, count);
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
        return -ret;
    *fd = (int)ret;
    return 0;
}

int sys_openat(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
    path_at_request request{7, dirfd, (uint64_t)path, 0, 0, 0,
                            (uint64_t)mode, (uint64_t)flags};
    long ret = path_at(request);
    if (ret < 0) return -ret;
    *fd = (int)ret;
    return 0;
}

int sys_close(int fd) {
    long ret = syscall1(SYS_CLOSE, fd);
    return ret < 0 ? -ret : 0;
}

int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) {
    long ret = syscall3(SYS_LSEEK, fd, offset, whence);
    if (ret < 0)
        return -ret;
    *new_offset = ret;
    return 0;
}

int sys_mkdir(const char *path, mode_t mode) {
    long ret = syscall2(SYS_MKDIR, (long)path, mode);
    return ret < 0 ? -ret : 0;
}

int sys_mkdirat(int dirfd, const char *path, mode_t mode) {
    path_at_request request{8, dirfd, (uint64_t)path, 0, 0, 0,
                            (uint64_t)mode, 0};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_faccessat(int dirfd, const char *path, int mode, int flags) {
    if (mode & ~7)
        return EINVAL;
    path_at_request request{9, dirfd, (uint64_t)path, 0, 0, 0,
                            (uint64_t)mode, (uint64_t)flags};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_unlinkat(int dirfd, const char *path, int flags) {
    if (flags & ~AT_REMOVEDIR)
        return EINVAL;
    path_at_request request{1, dirfd, (uint64_t)path, 0, 0, 0, 0,
                            (uint64_t)flags};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_rmdir(const char *path) {
    long ret = syscall1(SYS_RMDIR, (long)path);
    return ret < 0 ? -ret : 0;
}

int sys_rename(const char *old_path, const char *new_path) {
    long ret = syscall2(SYS_RENAME, (long)old_path, (long)new_path);
    return ret < 0 ? -ret : 0;
}

int sys_renameat(int olddirfd, const char *old_path, int newdirfd,
                 const char *new_path) {
    path_at_request request{2, olddirfd, (uint64_t)old_path, newdirfd,
                            (uint64_t)new_path, 0, 0, 0};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_link(const char *old_path, const char *new_path) {
    long ret = syscall3(SYS_LINK, (long)old_path, (long)new_path, 0);
    return ret < 0 ? -ret : 0;
}

int sys_linkat(int olddirfd, const char *old_path, int newdirfd,
               const char *new_path, int flags) {
    if (flags & ~AT_SYMLINK_FOLLOW)
        return EINVAL;
    path_at_request request{3, olddirfd, (uint64_t)old_path, newdirfd,
                            (uint64_t)new_path, 0, 0, (uint64_t)flags};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_symlink(const char *target, const char *link_path) {
    long ret = syscall2(SYS_SYMLINK, (long)target, (long)link_path);
    return ret < 0 ? -ret : 0;
}

int sys_symlinkat(const char *target, int dirfd, const char *link_path) {
    path_at_request request{4, 0, (uint64_t)target, dirfd,
                            (uint64_t)link_path, 0, 0, 0};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_readlink(const char *path, void *buffer, size_t size, ssize_t *length) {
    long ret = syscall3(SYS_READLINK, (long)path, (long)buffer, size);
    if (ret < 0) return -ret;
    *length = ret;
    return 0;
}

int sys_readlinkat(int dirfd, const char *path, void *buffer, size_t size,
                   ssize_t *length) {
    path_at_request request{5, dirfd, (uint64_t)path, 0, 0,
                            (uint64_t)buffer, size, 0};
    long ret = path_at(request);
    if (ret < 0) return -ret;
    *length = ret;
    return 0;
}

int sys_chmod(const char *path, mode_t mode) {
    path_at_request request{10, AT_FDCWD, (uint64_t)path, 0, 0, 0,
                            (uint64_t)mode, 0};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_fchmod(int fd, mode_t mode) {
    long ret = syscall2(SYS_FCHMOD, fd, mode);
    return ret < 0 ? -ret : 0;
}

int sys_fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return EINVAL;
    path_at_request request{10, dirfd, (uint64_t)path, 0, 0, 0,
                            (uint64_t)mode, (uint64_t)flags};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_fchownat(int dirfd, const char *path, uid_t uid, gid_t gid,
                 int flags) {
    if ((flags & AT_EMPTY_PATH) && path && !*path) {
        long ret = syscall3(SYS_FCHOWN, dirfd, uid, gid);
        return ret < 0 ? -ret : 0;
    }
    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return EINVAL;
    path_at_request request{11, dirfd, (uint64_t)path, 0, 0,
                            (uint64_t)uid, (uint64_t)gid, (uint64_t)flags};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_utimensat(int dirfd, const char *path, const struct timespec times[2],
                  int flags) {
    if (!path) {
        long ret = syscall2(SYS_FUTIMENS, dirfd, (long)times);
        return ret < 0 ? -ret : 0;
    }
    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return EINVAL;
    path_at_request request{12, dirfd, (uint64_t)path, 0, 0,
                            (uint64_t)times, 0, (uint64_t)flags};
    long ret = path_at(request);
    return ret < 0 ? -ret : 0;
}

int sys_pipe(int *fds, int flags) {
    long ret = syscall2(SYS_PIPE, (long)fds, flags);
    return ret < 0 ? -ret : 0;
}

int sys_dup(int fd, int flags, int *newfd) {
    long ret = syscall2(SYS_DUP, fd, flags);
    if (ret < 0) return -ret;
    *newfd = (int)ret;
    return 0;
}

int sys_dup2(int fd, int flags, int newfd) {
    long ret = syscall3(SYS_DUP2, fd, newfd, flags);
    return ret < 0 ? -ret : 0;
}

int sys_fcntl(int fd, int request, va_list args, int *result) {
    long argument = 0;
    switch (request) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETFL:
            argument = va_arg(args, int);
            break;
        case F_GETFD:
        case F_GETFL:
            break;
        default:
            return EINVAL;
    }
    long ret = syscall3(SYS_FCNTL, fd, request, argument);
    if (ret < 0) return -ret;
    *result = (int)ret;
    return 0;
}

/* mmap()'s six real arguments don't fit a three-register syscall, so
 * they're bundled into one struct and passed by pointer — the same
 * shape SYS_PATH_AT already uses for the same reason (kernel/proc/
 * syscall.c's struct mmap_request must match this byte for byte). */
struct mmap_request {
    uint64_t hint;
    uint64_t size;
    int64_t  prot;
    int64_t  flags;
    int64_t  fd;
    int64_t  offset;
};

int sys_vm_map(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
    struct mmap_request request = {
        .hint = (uint64_t)hint, .size = size, .prot = prot,
        .flags = flags, .fd = fd, .offset = offset,
    };
    long ret = syscall1(SYS_MMAP, (long)&request);
    if (ret < 0) return (int)-ret;
    *window = (void *)ret;
    return 0;
}

int sys_vm_unmap(void *pointer, size_t size) {
    long ret = syscall2(SYS_MUNMAP, (long)pointer, size);
    if (ret < 0) return -ret;
    return 0;
}

int sys_vm_protect(void *pointer, size_t size, int prot) {
    long ret = syscall3(SYS_MPROTECT, (long)pointer, size, prot);
    if (ret < 0) return -ret;
    return 0;
}

int sys_msync(void *addr, size_t length, int flags) {
    long ret = syscall3(SYS_MSYNC, (long)addr, length, flags);
    if (ret < 0) return -ret;
    return 0;
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time) {
    uint64_t timeout_ms = 0;
    if (time) {
        if (time->tv_sec < 0 || time->tv_nsec < 0
                || time->tv_nsec >= 1000000000L)
            return EINVAL;
        timeout_ms = static_cast<uint64_t>(time->tv_sec) * 1000
                   + (static_cast<uint64_t>(time->tv_nsec) + 999999) / 1000000;
        if (!timeout_ms)
            timeout_ms = 1;
    }
    long ret = cp_syscall3(SYS_FUTEX_WAIT, (long)pointer, expected, timeout_ms);
    if (ret == -2) return EAGAIN;
    if (ret == -3) return ETIMEDOUT;
    if (ret == -4) return EINTR;
    return ret < 0 ? -ret : 0;
}
int sys_futex_wake(int *pointer) {
    long ret = syscall1(SYS_FUTEX_WAKE, (long)pointer);
    return ret < 0 ? -ret : 0;
}

int sys_futex_tid() {
    return (int)syscall0(SYS_GETTID);
}

int sys_clock_get(int clock, time_t *secs, long *nanos) {
    long ret = syscall1(SYS_CLOCK_GET, clock);
    if (ret < 0) return -ret;
    *secs = ret / 1000000000L;
    *nanos = ret % 1000000000L;
    return 0;
}

int sys_clock_set(int clock, time_t secs, long nanos) {
    long ret = syscall3(SYS_CLOCK_SET, clock, secs, nanos);
    return ret < 0 ? -ret : 0;
}

int sys_ioctl(int fd, unsigned long request, void *arg, int *result) {
    long ret = syscall3(SYS_IOCTL, fd, request, (long)arg);
    if (ret < 0) return -ret;
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
    long ret = cp_syscall3(SYS_POLL, (long)fds, count, timeout);
    if (ret < 0) return -ret;
    *num_events = (int)ret;
    return 0;
}

// Add any other functions mlibc complains about during linking as ENOSYS stubs here...

int sys_isatty(int fd) {
    struct termios termios;
    long ret = syscall3(SYS_IOCTL, fd, TCGETS, (long)&termios);
    return ret < 0 ? ENOTTY : 0;
}



} // namespace mlibc

#include <sys/select.h>
#include <poll.h>

namespace mlibc {
int sys_pselect(int num_fds, fd_set *read_set, fd_set *write_set,
		fd_set *except_set, const struct timespec *timeout, const sigset_t *sigmask, int *num_events) {
    (void)sigmask;
	struct pollfd fds[num_fds];
	nfds_t count = 0;
	for (int i = 0; i < num_fds; i++) {
		short events = 0;
		if (read_set && FD_ISSET(i, read_set)) events |= POLLIN;
		if (write_set && FD_ISSET(i, write_set)) events |= POLLOUT;
		if (except_set && FD_ISSET(i, except_set)) events |= POLLPRI;
		if (events) {
			fds[count].fd = i;
			fds[count].events = events;
			fds[count].revents = 0;
			count++;
		}
	}
	int timeout_ms = -1;
	if (timeout) {
		timeout_ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
	}
	int ret = sys_poll(fds, count, timeout_ms, num_events);
	if (ret) return ret;

	if (read_set) FD_ZERO(read_set);
	if (write_set) FD_ZERO(write_set);
	if (except_set) FD_ZERO(except_set);

	for (nfds_t i = 0; i < count; i++) {
		if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (read_set) FD_SET(fds[i].fd, read_set);
		}
		if (fds[i].revents & (POLLOUT | POLLERR | POLLHUP)) {
			if (write_set) FD_SET(fds[i].fd, write_set);
		}
		if (fds[i].revents & POLLPRI) {
			if (except_set) FD_SET(fds[i].fd, except_set);
		}
	}
	return 0;
}
}

/* Both belong in namespace mlibc (posix-sysdeps.hpp declares these
 * weak symbols there) and both call the 4-argument mlibc::sys_ioctl()
 * defined above (fd, request, arg-pointer-filled-by-the-driver,
 * out-pointer-for-the-raw-syscall-result) — neither held before this
 * fix: these were sitting at global scope, meaning they never actually
 * overrode the weak mlibc::sys_unlockpt()/mlibc::sys_ptsname()
 * defaults at all, and called a nonexistent 3-argument sys_ioctl() on
 * top of that, which failed to even compile. */
namespace mlibc {

int sys_unlockpt(int fd) {
    int zero = 0;
    int result;
    return sys_ioctl(fd, 0x40045431 /* TIOCSPTLCK */, &zero, &result);
}

int sys_ptsname(int fd, char *buffer, size_t length) {
    int pty_num;
    int result;
    if (sys_ioctl(fd, 0x80045430 /* TIOCGPTN */, &pty_num, &result) != 0) {
        return ENOTTY;
    }
    
    // Extron DevFS dynamic nodes are at /dev/ptsN
    // Format the path string manually since snprintf might not be fully linked here?
    // Actually, mlibc has snprintf. But it's a sysdep so we can just use manual strcpy.
    const char *prefix = "/dev/pts";
    if (length < 16) return ERANGE;
    
    int i = 0;
    while (prefix[i]) { buffer[i] = prefix[i]; i++; }
    
    // Convert number to string
    if (pty_num == 0) {
        buffer[i++] = '0';
    } else {
        int temp = pty_num;
        int num_digits = 0;
        while (temp > 0) { temp /= 10; num_digits++; }
        temp = pty_num;
        for (int j = num_digits - 1; j >= 0; j--) {
            buffer[i + j] = '0' + (temp % 10);
            temp /= 10;
        }
        i += num_digits;
    }
    buffer[i] = '\0';
    return 0;
}

} // namespace mlibc
