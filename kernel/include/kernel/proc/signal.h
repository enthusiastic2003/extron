#ifndef KERNEL_PROC_SIGNAL_H
#define KERNEL_PROC_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

struct proc;
struct thread;
struct aarch64_frame;

#define SIGNAL_MAX 64

#define SIGNAL_DFL 0ULL
#define SIGNAL_IGN 1ULL

#define SIGNAL_SA_SIGINFO   0x00000004UL
#define SIGNAL_SA_RESTORER  0x04000000UL
#define SIGNAL_SA_NODEFER   0x40000000UL
#define SIGNAL_SA_RESETHAND 0x80000000UL

struct signal_action {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

/* The subset of POSIX siginfo_t that the kernel needs to retain while a
 * standard (non-real-time) signal is pending. The userspace ABI object is
 * constructed only when delivery happens. */
struct signal_info {
    int32_t signo;
    int32_t error;
    int32_t code;
    int32_t _padding;
    uint64_t sender_pid;
    uint64_t sender_uid;
    uint64_t fault_address;
    int32_t child_status;
};

void signal_process_init(struct proc *p);
void signal_process_fork(struct proc *child, const struct proc *parent);
void signal_process_exec(struct proc *p);

int signal_action_get(struct proc *p, int signo, struct signal_action *out);
int signal_action_set(struct proc *p, int signo,
                      const struct signal_action *action);
int signal_send(struct proc *target, int signo);
int signal_send_thread(struct proc *target, struct thread *thread, int signo);
int signal_send_group(uint64_t pgid, int signo);
void signal_notify_parent(struct proc *child, int code, int status);
int signal_mask_update(struct thread *t, int how, const uint64_t *set,
                       uint64_t *old);
bool signal_pending_unblocked(struct thread *t);

bool signal_deliver_pending(struct aarch64_frame *f);
bool signal_deliver_sync(struct aarch64_frame *f, int signo);
uint64_t signal_sigreturn(struct aarch64_frame *f);

#endif
