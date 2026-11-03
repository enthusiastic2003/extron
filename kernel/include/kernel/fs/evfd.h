#ifndef KERNEL_FS_EVFD_H
#define KERNEL_FS_EVFD_H

#include <stddef.h>
#include <stdint.h>

struct proc;
struct open_file;

/* epoll: events are the Linux EPOLL* bits (same values as POLL*). */
#if defined(__x86_64__)
struct kepoll_event { uint32_t events; uint64_t data; } __attribute__((packed));
#else
struct kepoll_event { uint32_t events; uint64_t data; };
#endif
#define EPOLL_MAX_EVENTS_PER_CALL 64

long epoll_create_fd(struct proc *p, int flags);
long epoll_ctl_fd(struct proc *p, int epfd, int op, int fd, const struct kepoll_event *ev);
/* Fills out[] with up to `max` ready events; blocks for up to timeout_ms
 * (-1 forever, 0 poll). Returns the count or a negative errno. */
long epoll_wait_fd(struct proc *p, int epfd, struct kepoll_event *out, int max,
                   int64_t timeout_ms);

long eventfd_create_fd(struct proc *p, unsigned initval, int flags);
long timerfd_create_fd(struct proc *p, int clockid, int flags);
long timerfd_settime_fd(struct proc *p, int fd, int flags,
                        const uint64_t new_spec[4], uint64_t old_spec[4]);
long timerfd_gettime_fd(struct proc *p, int fd, uint64_t cur_spec[4]);
long signalfd_create_fd(struct proc *p, int fd, uint64_t mask, int flags);

/* Descriptor-layer hooks. */
long evfd_read(struct open_file *f, void *buffer, size_t count);
long evfd_write(struct open_file *f, const void *buffer, size_t count);
void evfd_poll(struct open_file *f, int events, short *revents);
/* Uptime-ns at which a pending timerfd expiry will make `f` readable, or 0. */
uint64_t evfd_next_event_ns(struct open_file *f);
void evfd_release(struct open_file *f);          /* last close of a description */
void epoll_forget_file(struct open_file *f);     /* drop `f` from every epoll set */

#endif
