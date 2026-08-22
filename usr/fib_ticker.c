/*
 * Prints the next Fibonacci number once a second, forever.
 *
 * The point isn't the arithmetic — it's that this runs *while DOOM is
 * running*, on one core, with neither process aware of the other. DOOM
 * is CPU-bound and renders continuously; this one sleeps through
 * almost its entire life and wakes on a timer. Both make progress.
 *
 * The elapsed-time column is the actual measurement. Each line reports
 * how long the process has really been alive (SYS_UPTIME_MS, read off
 * CNTPCT_EL0) next to how many one-second sleeps it has performed. If
 * the scheduler were starving this process behind DOOM's render loop,
 * or if SYS_SLEEP were drifting, those two numbers would separate and
 * keep separating. They should stay within a tick or two of each other.
 */
#include <stdio.h>
#include <extron/syscall.h>

int main(void) {
    /* fib(93) is the largest that fits in 64 bits; fib(94) overflows.
     * Restart there rather than printing wrapped garbage. */
    const int FIB_MAX = 93;

    uint64_t start = sys_uptime_ms();

    for (;;) {
        uint64_t a = 0, b = 1;

        for (int n = 1; n <= FIB_MAX; n++) {
            sys_sleep(1, 0);

            uint64_t elapsed = sys_uptime_ms() - start;
            /* Deliberately one short line: sys_write runs with interrupts
             * masked for its whole duration, and at 115200 baud every
             * character costs ~87us. A chatty line here would be a
             * visible hitch in DOOM once a second. */
            printf("[fib] t=%lus n=%d %lu\n",
                   (unsigned long)(elapsed / 1000), n, (unsigned long)b);

            uint64_t next = a + b;
            a = b;
            b = next;
        }

        printf("[fib] 64-bit limit reached, restarting\n");
    }
    return 0;
}
