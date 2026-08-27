#ifndef KERNEL_PROC_RESOURCE_H
#define KERNEL_PROC_RESOURCE_H

#include <stdint.h>
#include <stddef.h>

struct proc;
struct thread;
struct aarch64_frame;

/* Matches Extron's mlibc abi-bits/resource.h numbering. */
#define PROC_RLIMIT_CPU         0
#define PROC_RLIMIT_FSIZE       1
#define PROC_RLIMIT_DATA        2
#define PROC_RLIMIT_STACK       3
#define PROC_RLIMIT_CORE        4
#define PROC_RLIMIT_RSS         5
#define PROC_RLIMIT_NPROC       6
#define PROC_RLIMIT_NOFILE      7
#define PROC_RLIMIT_MEMLOCK     8
#define PROC_RLIMIT_AS          9
#define PROC_RLIMIT_LOCKS      10
#define PROC_RLIMIT_SIGPENDING 11
#define PROC_RLIMIT_MSGQUEUE   12
#define PROC_RLIMIT_NICE       13
#define PROC_RLIMIT_RTPRIO     14
#define PROC_RLIMIT_RTTIME     15
#define PROC_RLIMIT_COUNT      16

#define PROC_RLIM_INFINITY UINT64_MAX

#define PROC_RUSAGE_SELF      0
#define PROC_RUSAGE_CHILDREN -1

struct proc_rlimit {
    uint64_t current;
    uint64_t maximum;
};

/* Fixed-width syscall ABI versions of struct timeval/struct rusage. */
struct user_timeval {
    int64_t sec;
    int64_t usec;
};

struct user_rusage {
    struct user_timeval utime;
    struct user_timeval stime;
    int64_t maxrss;
    int64_t ixrss;
    int64_t idrss;
    int64_t isrss;
    int64_t minflt;
    int64_t majflt;
    int64_t nswap;
    int64_t inblock;
    int64_t oublock;
    int64_t msgsnd;
    int64_t msgrcv;
    int64_t nsignals;
    int64_t nvcsw;
    int64_t nivcsw;
};

enum resource_cpu_mode {
    RESOURCE_CPU_NONE = 0,
    RESOURCE_CPU_USER,
    RESOURCE_CPU_SYSTEM,
};

void resource_process_init(struct proc *p);
void resource_process_fork(struct proc *child, struct proc *parent);

int resource_get_limit(struct proc *p, int resource, struct proc_rlimit *out);
int resource_set_limit(struct proc *p, int resource,
                       const struct proc_rlimit *value);
uint64_t resource_nofile_limit(struct proc *p);

void resource_account_exception_enter(const struct aarch64_frame *frame);
void resource_account_exception_leave(const struct aarch64_frame *frame);
void resource_account_switch(struct thread *old, struct thread *next);

void resource_get_self_usage(struct proc *p, struct user_rusage *out);
void resource_get_children_usage(struct proc *p, struct user_rusage *out);
void resource_reap_child(struct proc *parent, struct proc *child,
                         struct user_rusage *child_usage);

#endif
