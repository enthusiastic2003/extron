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

static inline long proc_dump(void)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(3)           // SYS_PROC_DUMP
        : "rcx", "r11", "memory"
    );
    return ret;
}

// ----------------------------------------------------------------
// Entry Point
// ----------------------------------------------------------------

void _start(void) {
    for(;;){
        write(1, "Test2: sleeping for 2 seconds...\n", 33);
        sleep(2);
        write(1, "Test2: woke up!\n", 16);
        proc_dump();
    }
}