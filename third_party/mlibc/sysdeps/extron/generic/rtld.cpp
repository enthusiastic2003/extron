#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>

// The runtime linker cannot call libc: it is responsible for loading libc.
// These wrappers are therefore intentionally self-contained raw AArch64
// syscalls. Keep the numbers synchronized with kernel/proc/syscall.h.
#define SYS_READ         0
#define SYS_WRITE        1
#define SYS_ANON_ALLOC   4
#define SYS_ANON_FREE    5
#define SYS_TCB_SET      6
#define SYS_EXIT         7
#define SYS_OPEN        13
#define SYS_CLOSE       14
#define SYS_LSEEK       15
#define SYS_GETTID      29
#define SYS_FUTEX_WAIT  33
#define SYS_FUTEX_WAKE  34
#define SYS_MMAP        69
#define SYS_MUNMAP      70
#define SYS_MPROTECT    73

namespace {

inline long syscall0(long number) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0");
    __asm__ volatile ("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

inline long syscall1(long number, long a0) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

inline long syscall2(long number, long a0, long a1) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1)
            : "memory", "cc");
    return x0;
}

inline long syscall3(long number, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
            : "memory", "cc");
    return x0;
}

struct mmap_request {
    uint64_t hint;
    uint64_t size;
    int64_t prot;
    int64_t flags;
    int64_t fd;
    int64_t offset;
};

} // namespace

namespace mlibc {

void sys_libc_log(const char *message) {
    size_t length = 0;
    while (message[length])
        ++length;
    syscall3(SYS_WRITE, 2, reinterpret_cast<long>(message), length);
}

void sys_libc_panic() {
    static constexpr char message[] = "\n[mlibc rtld] fatal error\n";
    syscall3(SYS_WRITE, 2, reinterpret_cast<long>(message), sizeof(message) - 1);
    syscall1(SYS_EXIT, 127);
    for (;;)
        __asm__ volatile ("");
}

int sys_anon_allocate(size_t size, void **pointer) {
    long result = syscall1(SYS_ANON_ALLOC, size);
    if (result < 0)
        return -result;
    *pointer = reinterpret_cast<void *>(result);
    return 0;
}

int sys_anon_free(void *pointer, size_t size) {
    long result = syscall2(SYS_ANON_FREE, reinterpret_cast<long>(pointer), size);
    return result < 0 ? -result : 0;
}

int sys_tcb_set(void *pointer) {
    // This is the AArch64 TLS layout used by mlibc's regular Extron sysdeps.
    auto tp = reinterpret_cast<char *>(pointer) + sizeof(Tcb) - 0x10;
    long result = syscall1(SYS_TCB_SET, reinterpret_cast<long>(tp));
    return result < 0 ? -result : 0;
}

int sys_open(const char *path, int flags, mode_t mode, int *fd) {
    long result = syscall3(SYS_OPEN, reinterpret_cast<long>(path), flags, mode);
    if (result < 0)
        return -result;
    *fd = result;
    return 0;
}

int sys_close(int fd) {
    long result = syscall1(SYS_CLOSE, fd);
    return result < 0 ? -result : 0;
}

int sys_read(int fd, void *buffer, size_t count, ssize_t *bytes_read) {
    long result = syscall3(SYS_READ, fd, reinterpret_cast<long>(buffer), count);
    if (result < 0)
        return -result;
    *bytes_read = result;
    return 0;
}

int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) {
    long result = syscall3(SYS_LSEEK, fd, offset, whence);
    if (result < 0)
        return -result;
    *new_offset = result;
    return 0;
}

int sys_vm_map(void *hint, size_t size, int prot, int flags, int fd,
        off_t offset, void **window) {
    mmap_request request{
        reinterpret_cast<uint64_t>(hint), size, prot, flags, fd, offset
    };
    long result = syscall1(SYS_MMAP, reinterpret_cast<long>(&request));
    if (result < 0)
        return -result;
    *window = reinterpret_cast<void *>(result);
    return 0;
}

int sys_vm_unmap(void *pointer, size_t size) {
    long result = syscall2(SYS_MUNMAP, reinterpret_cast<long>(pointer), size);
    return result < 0 ? -result : 0;
}

int sys_vm_protect(void *pointer, size_t size, int prot) {
    long result = syscall3(SYS_MPROTECT, reinterpret_cast<long>(pointer), size, prot);
    return result < 0 ? -result : 0;
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *timeout) {
    // The rtld only uses untimed waits for its internal locks. Rejecting a
    // future timed use is safer than silently changing its semantics.
    if (timeout)
        return ENOSYS;
    long result = syscall3(SYS_FUTEX_WAIT,
            reinterpret_cast<long>(pointer), expected, 0);
    if (result == -2)
        return EAGAIN;
    if (result == -3)
        return ETIMEDOUT;
    if (result == -4)
        return EINTR;
    return result < 0 ? -result : 0;
}

int sys_futex_wake(int *pointer) {
    long result = syscall1(SYS_FUTEX_WAKE, reinterpret_cast<long>(pointer));
    return result < 0 ? -result : 0;
}

int sys_futex_tid() {
    return syscall0(SYS_GETTID);
}

} // namespace mlibc
