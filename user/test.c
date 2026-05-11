static inline long write(int fd, const void* buf, unsigned long count)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(1), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long sleep(unsigned long seconds)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(2), "D"(seconds)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void exit(int status)
{
    __asm__ volatile (
        "syscall"
        :: "a"(7), "D"(status)   // SYS_EXIT
        : "rcx", "r11", "memory"
    );
    for (;;);   // unreachable — silences noreturn warning
}

// ----------------------------------------------------------------
// Entry Point — runs 3 times then exits cleanly
// ----------------------------------------------------------------

void _start(void)
{
    for (int i = 0; i < 3; i++) {
        write(1, "test: alive\n", 12);
        sleep(1);
    }

    write(1, "test: calling exit(0)\n", 22);
    exit(0);
}
