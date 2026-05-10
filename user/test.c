// syscalls.h (or directly in your test file)

static inline long write(int fd, const void* buf, unsigned long count)
{
    long ret;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(1),           // SYS_WRITE
          "D"(fd),
          "S"(buf),
          "d"(count)
        : "rcx", "r11", "memory"
    );

    return ret;
}

static inline long read(int fd, void* buf, unsigned long count)
{
    long ret;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(0),           // SYS_READ
          "D"(fd),
          "S"(buf),
          "d"(count)
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
        : "a"(2),           // SYS_SLEEP
          "D"(seconds)
        : "rcx", "r11", "memory"
    );

    return ret;
}

// ----------------------------------------------------------------
// Entry Point
// ----------------------------------------------------------------

void _start(void)
{
    char buf[128];

    for (;;) {

        write(1, "Input: ", 7);

        long n = read(0, buf, sizeof(buf));

        write(1, "You typed: ", 11);

        if (n > 0)
            write(1, buf, n);

        write(1, "\nSleeping for 5 seconds...\n", 27);

        for (int i = 0; i < 5; i++) {
            sleep(1);
            write(1, ".", 1);
        }

        write(1, "\n\n", 2);
    }
}