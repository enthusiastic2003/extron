/*
 * Blocks on SYS_READ and reports each keystroke, while DOOM plays.
 *
 * Tests the input path DOOM does NOT use. DOOM polls the shared ring
 * mapped into its address space; this uses the blocking console path —
 * kbd_getc() -> sleep(&kbuf) -> schedule() away, then the UART ISR's
 * wakeup(&kbuf) puts it back on the run queue. That channel-based
 * sleep/wake is the machinery that hung the kernel at the start of this
 * port, and nothing else currently exercises it.
 *
 * It also demonstrates that the two paths are not exclusive. The ISR
 * pushes every byte to both the blocking buffer and the polled ring, so
 * this process and DOOM each see every keystroke — the same 'w' both
 * moves the player and prints here.
 *
 * Prints the gap since the previous key, which is the evidence it really
 * slept: a busy-waiting process would show the same numbers, but it
 * would also have starved DOOM's renderer to a crawl, and DOOM visibly
 * keeps running.
 */
#include <stdio.h>
#include <extron/syscall.h>

int main(void) {
    unsigned long count = 0;
    uint64_t last = sys_uptime_ms();

    printf("[key] monitor ready — type while DOOM runs\n");

    for (;;) {
        unsigned char c;
        if (sys_read(0, &c, 1) != 1) {
            continue;
        }

        uint64_t now = sys_uptime_ms();
        unsigned long gap = (unsigned long)(now - last);
        last = now;
        count++;

        /* One short line: sys_write runs with interrupts masked for its
         * whole duration, and at 115200 baud each character costs ~87us,
         * so a verbose line here becomes a visible hitch in DOOM. */
        if (c >= 0x20 && c < 0x7F) {
            printf("[key] '%c' #%lu +%lums\n", c, count, gap);
        } else {
            printf("[key] 0x%02x #%lu +%lums\n", c, count, gap);
        }
    }
    return 0;
}
