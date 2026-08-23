#include <bits/ensure.h>
#include <errno.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <stddef.h>
#include <stdint.h>

#define SYS_ANON_ALLOC    4
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT   31

static inline long extron_syscall1(long number, long arg) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

extern "C" void __extron_thread_sysdeps_anchor() { }

extern "C" void __mlibc_enter_thread(void *entry, void *user_arg, Tcb *tcb) {
    while (!__atomic_load_n(&tcb->tid, __ATOMIC_ACQUIRE))
        mlibc::sys_futex_wait(&tcb->tid, 0, nullptr);

    if (mlibc::sys_tcb_set(tcb))
        __ensure(!"sys_tcb_set() failed");

    tcb->invokeThreadFunc(entry, user_arg);

    /* The kernel publishes didExit and performs the wake atomically with
     * taking this thread out of scheduling. */
    mlibc::sys_thread_exit();
}

namespace mlibc {

static constexpr size_t default_stack_size = 2 * 1024 * 1024;

int sys_prepare_stack(void **stack, void *entry, void *user_arg, void *tcb,
                      size_t *stack_size, size_t *guard_size,
                      void **stack_base) {
    if (!*stack_size)
        *stack_size = default_stack_size;

    /* vm_map is not present yet, so anonymous allocation cannot reserve an
     * inaccessible userspace guard page. The kernel stack still has one. */
    *guard_size = 0;
    if (*stack) {
        *stack_base = *stack;
    } else {
        long base = extron_syscall1(SYS_ANON_ALLOC, (long)*stack_size);
        if (base < 0)
            return ENOMEM;
        *stack_base = reinterpret_cast<void *>(base);
    }

    uintptr_t *sp = reinterpret_cast<uintptr_t *>(
        reinterpret_cast<uintptr_t>(*stack_base) + *stack_size);
    /* Four words preserve the AArch64 16-byte SP alignment. */
    *--sp = 0;
    *--sp = reinterpret_cast<uintptr_t>(tcb);
    *--sp = reinterpret_cast<uintptr_t>(user_arg);
    *--sp = reinterpret_cast<uintptr_t>(entry);
    *stack = sp;
    return 0;
}

extern "C" void __mlibc_start_thread();

struct thread_create_args {
    uintptr_t entry;
    uintptr_t user_sp;
    uintptr_t tls;
    uintptr_t exit_word;
};

int sys_clone(void *tcb_pointer, pid_t *tid_out, void *stack) {
    auto *tcb = reinterpret_cast<Tcb *>(tcb_pointer);
    thread_create_args args = {
        reinterpret_cast<uintptr_t>(&__mlibc_start_thread),
        reinterpret_cast<uintptr_t>(stack),
        reinterpret_cast<uintptr_t>(tcb) + sizeof(Tcb) - 0x10,
        reinterpret_cast<uintptr_t>(&tcb->didExit),
    };
    long ret = extron_syscall1(SYS_THREAD_CREATE,
                               reinterpret_cast<long>(&args));
    if (ret < 0)
        return EAGAIN;
    *tid_out = (pid_t)ret;
    return 0;
}

[[noreturn]] void sys_thread_exit() {
    extron_syscall1(SYS_THREAD_EXIT, 0);
    for (;;) __asm__ volatile ("");
}

} // namespace mlibc
