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

// ----------------------------------------------------------------
// Entry Point
// ----------------------------------------------------------------

void _start(void) {
    for(;;){
        write(1, "Welcome from test2\n", 19);
    }
}