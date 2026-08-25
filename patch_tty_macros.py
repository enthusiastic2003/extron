import sys

tty_c = "kernel/drivers/tty.c"
with open(tty_c, "r") as f:
    content = f.read()

macros = """
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define IGNCR 0000200
#define ICRNL 0000400
#define INLCR 0000100
#define OPOST 0000001
#define ONLCR 0000004
#define CS8 0000060
#define CREAD 0000200
#define CLOCAL 0004000
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
"""
# insert before struct tty tty_table
idx = content.find("struct tty tty_table")
content = content[:idx] + macros + "\n" + content[idx:]
content = content.replace("t->lock = SPINLOCK_INIT;", "t->lock = (spinlock_t)SPINLOCK_INIT;")
content = content.replace("t->lock = (spinlock_t)(spinlock_t)SPINLOCK_INIT;", "t->lock = (spinlock_t)SPINLOCK_INIT;")
with open(tty_c, "w") as f:
    f.write(content)
