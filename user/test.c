#include "../kernel/include/kernel/proc/syscall.h"

static inline long write(int fd,
                         const void* buf,
                         unsigned long count)
{
    long ret;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(1),
          "D"(fd),
          "S"(buf),
          "d"(count)
        : "rcx", "r11", "memory"
    );

    return ret;
}

void _start(void) {
    write(1, "Hello from userland!!\n", 22);
    for (;;);
}