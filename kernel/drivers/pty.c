#include <kernel/drivers/tty.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/devfs.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>
#include <kernel/errno.h>
#include <arch/irq_spinlock.h>

#define MAX_PTYS (MAX_TTYS - 1)

struct pty_pair {
    int index;
    bool allocated;
    struct tty *slave_tty;
    struct vfs_node master_node;
    char master_buf[TTY_BUFFER_SIZE];
    uint32_t master_head;
    uint32_t master_tail;
    spinlock_t lock;
};

static struct pty_pair ptys[MAX_PTYS];

static void pty_slave_write_out(struct tty *t, const char *buf, size_t count) {
    struct pty_pair *pair = t->private_data;
    if (!pair) return;
    irq_spin_lock(&pair->lock);
    for (size_t i = 0; i < count; i++) {
        uint32_t next = (pair->master_head + 1) % TTY_BUFFER_SIZE;
        if (next != pair->master_tail) {
            pair->master_buf[pair->master_head] = buf[i];
            pair->master_head = next;
        }
    }
    irq_spin_unlock(&pair->lock);
    wakeup(&pair->master_buf);
}

static long master_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    (void)off;
    struct pty_pair *pair = n->private;
    irq_spin_lock(&pair->lock);
    while (pair->master_head == pair->master_tail) {
        sleep(&pair->master_buf, &pair->lock);
        if (signal_pending_unblocked(my_thread())) {
            irq_spin_unlock(&pair->lock);
            return -4; /* EINTR */
        }
    }
    size_t read = 0;
    char *out = buf;
    while (read < count && pair->master_head != pair->master_tail) {
        out[read++] = pair->master_buf[pair->master_tail];
        pair->master_tail = (pair->master_tail + 1) % TTY_BUFFER_SIZE;
    }
    irq_spin_unlock(&pair->lock);
    return read;
}

static long master_write(struct vfs_node *n, size_t off, const void *buf, size_t count) {
    (void)off;
    struct pty_pair *pair = n->private;
    const char *in = buf;
    for (size_t i = 0; i < count; i++) {
        if (!tty_handle_input_byte(pair->slave_tty, (uint8_t)in[i])) {
            tty_push_input(pair->slave_tty, in[i]);
        }
    }
    return count;
}

static int master_getattr(struct vfs_node *n, struct vfs_attr *attr) {
    memset(attr, 0, sizeof(*attr));
    attr->type = VFS_NODE_DEVICE;
    attr->mode = 0666;
    return 0;
}

static const struct vfs_node_ops master_ops = {
    .read = master_read,
    .write = master_write,
    .getattr = master_getattr,
};

void pty_init(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        ptys[i].index = i + 1;
        ptys[i].allocated = false;
        ptys[i].lock = (spinlock_t)SPINLOCK_INIT;
        ptys[i].slave_tty = &tty_table[i + 1];
        ptys[i].slave_tty->private_data = &ptys[i];
        ptys[i].slave_tty->write_out = pty_slave_write_out;
        vfs_node_init(&ptys[i].master_node, &master_ops, &ptys[i], VFS_NODE_DEVICE);
    }
}

struct vfs_node *pty_allocate_master(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].allocated) {
            ptys[i].allocated = true;
            ptys[i].slave_tty->termios = tty_table[0].termios; 
            tty_flush_input(ptys[i].slave_tty);
            ptys[i].master_head = ptys[i].master_tail = 0;
            return &ptys[i].master_node;
        }
    }
    return NULL;
}

struct tty *pty_get_slave(int index) {
    if (index >= 1 && index < MAX_TTYS) {
        if (ptys[index - 1].allocated)
            return ptys[index - 1].slave_tty;
    }
    return NULL;
}

int pty_get_index(struct vfs_node *master) {
    if (master->ops == &master_ops) {
        struct pty_pair *pair = master->private;
        return pair->index;
    }
    return -1;
}

