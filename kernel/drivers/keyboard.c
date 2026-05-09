#include <arch/io.h>
#include <kernel/console.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/klibc/string.h>
#include <kernel/time.h>
#include <kernel/sync/spinlock.h> 

// ----------------------------------------------------------------
// OS Synchronization Primitives (Implemented in your proc/sched layer)
// ----------------------------------------------------------------
extern void sleep(void *chan, spinlock_t *lk);
extern void wakeup(void *chan);

// ----------------------------------------------------------------
// Keyboard State
// ----------------------------------------------------------------

static const char scancode_to_ascii[] = {
    0,27,'1','2','3','4','5','6','7','8',
    '9','0','-','=','\b',
    '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,
    '\\','z','x','c','v','b','n',
    'm',',','.','/',
    0,
    '*',
    0,
    ' ',
};

void init_kbd(void) {
    memset(&kbd_buf, 0, sizeof(kbd_buf));
    kbd_buf.lock.locked = 0;
}

// ----------------------------------------------------------------
// Bottom Half: The Producer (Runs in Interrupt Context)
// ----------------------------------------------------------------
void kbd_buf_push(uint8_t sc) {

    uint32_t next = (kbd_buf.head + 1) % KEYBOARD_BUFFER_SIZE;

    // 2. If buffer is not full, push the scancode
    if (next != kbd_buf.tail) {
        kbd_buf.kb_buff[kbd_buf.head] = sc;
        kbd_buf.head = next;
        
        // 3. Wake up any process sleeping on this buffer
        wakeup(&kbd_buf);
    }

}

// ----------------------------------------------------------------
// Top Half: The Consumer (Runs in Syscall/Process Context)
// ----------------------------------------------------------------
uint64_t kbd_read(char *buf, uint64_t count)
{
    uint64_t n = 0;

    while (n < count) {
        
        // 1. Lock the buffer to safely check state
        irq_spin_lock(&kbd_buf.lock);
        
        // 2. The Wait Loop (Prevents Lost Wakeups)
        while (kbd_buf.tail == kbd_buf.head) {
            // sleep() must internally release the lock, context switch, 
            // and re-acquire the lock before returning to us.
            sleep(&kbd_buf, &kbd_buf.lock);
        }

        // 3. Extract exactly one scancode
        uint8_t sc = kbd_buf.kb_buff[kbd_buf.tail];
        kbd_buf.tail = (kbd_buf.tail + 1) % KEYBOARD_BUFFER_SIZE;
        
        // 4. Release the lock IMMEDIATELY. We don't need it to process the byte.
        irq_spin_unlock(&kbd_buf.lock);

        // --------------------------------------------------------
        // Lock-Free Processing Phase
        // --------------------------------------------------------
        
        if (sc & 0x80) continue; // key release
        if (sc >= sizeof(scancode_to_ascii)) continue; // out of bounds

        char c = scancode_to_ascii[sc];
        if (!c) continue; // unmapped key

        // Handle visual backspace
        if (c == '\b') {
            if (n > 0) {
                n--;
                kprintf("\b \b"); // Erase character from screen
            }
            continue;
        }

        // Store and Echo
        buf[n++] = c;
        kprintf("%c", c);

        // Canonical Mode: Stop reading when the user hits Enter
        if (c == '\n' || c == '\r') {
            buf[n - 1] = '\n'; // Normalize CR to LF for mlibc
            break; 
        }
    }

    return n;
}