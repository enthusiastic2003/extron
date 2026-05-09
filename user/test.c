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
    char buf[128];
    long bytes_read;

    write(1, "Welcome to Ucerland!\n", 21);
    write(1, "Welcome to Userland! Twicce\n", 21);

    while (1) {
        write(1, "> ", 2);

        // Block until the user presses Enter (Canonical mode)
        bytes_read = read(0, buf, sizeof(buf));

        if (bytes_read > 0) {
            write(1, "Echo: ", 6);
            write(1, buf, bytes_read);
        } else if (bytes_read == 0) {
            // EOF received
            break;
        } else {
            // Error occurred
            write(1, "Read error!\n", 12);
            break;
        }
    }

    write(1, "Exiting...\n", 11);
    // exit(0);
    for(;;);
}