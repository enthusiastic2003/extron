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

static inline void *anon_alloc(unsigned long size)
{
    void *ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(4),          // SYS_ANON_ALLOC
          "D"(size)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long anon_free(void *addr, unsigned long size)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(5),          // SYS_ANON_FREE
          "D"(addr),
          "S"(size)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// ----------------------------------------------------------------
// Minimal userspace bump allocator
//
// Requests one page at a time from the kernel's 1 GB heap region
// (USER_HEAP_START … +1 GB) and sub-allocates from it with a
// simple pointer bump.  No free — bump allocators don't support it.
// ----------------------------------------------------------------

#define HEAP_CHUNK 4096   // one page per refill

static char  *bump_start = (char*)0;
static char  *bump_ptr   = (char*)0;
static char  *bump_end   = (char*)0;

static void *bump_malloc(unsigned long size)
{
    // 8-byte alignment
    size = (size + 7UL) & ~7UL;

    if (!bump_start || bump_ptr + size > bump_end) {
        unsigned long chunk = size > HEAP_CHUNK ? size : HEAP_CHUNK;
        void *block = anon_alloc(chunk);
        if (block == (void*)(unsigned long)-1 || !block)
            return (void*)0;
        bump_start = (char*)block;
        bump_ptr   = bump_start;
        bump_end   = bump_start + chunk;
    }

    void *p   = bump_ptr;
    bump_ptr += size;
    return p;
}

// ----------------------------------------------------------------
// Simple itoa for printing numbers without printf
// ----------------------------------------------------------------
static void print_hex(unsigned long v)
{
    const char hex[] = "0123456789abcdef";
    char buf[18];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 2; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    write(1, buf, 18);
}

// ----------------------------------------------------------------
// Entry Point
// ----------------------------------------------------------------

void _start(void) {
    // write(1, "Test2: heap test\n", 17);

    // Allocate a small buffer from the userspace bump allocator
    char *msg = (char*)bump_malloc(32);
    if (!msg) {
        write(1, "ALLOC FAILED\n", 13);
    } else {
        // Write a string into heap memory and echo it back
        const char src[] = "hello from the heap!\n";
        for (int i = 0; src[i]; i++) msg[i] = src[i];
        write(1, "heap addr: ", 11);
        print_hex((unsigned long)msg);
        write(1, "\n", 1);
        write(1, msg, 21);
    }

    for(;;){
        write(1, "Test2: sleeping for 2 seconds...\n", 33);
        sleep(2);
        write(1, "Test2: woke up!\n", 16);
        proc_dump();
    }
}