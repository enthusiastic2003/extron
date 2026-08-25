import sys

keyboard_c = "kernel/drivers/keyboard.c"
with open(keyboard_c, "r") as f:
    content = f.read()

# Replace drain_uart
start_idx = content.find("static void drain_uart(void) {")
end_idx = content.find("static void kbd_irq_handler")

new_drain = """static void drain_uart(void) {
    int c;
    while ((c = serial_try_getc()) != -1) {
        if (!tty_handle_input_byte(&tty_table[0], (uint8_t)c)) {
            tty_push_input(&tty_table[0], (char)c);
        }
        ring_push((uint8_t)c);
    }
}
"""
content = content[:start_idx] + new_drain + content[end_idx:]

with open(keyboard_c, "w") as f:
    f.write(content)
