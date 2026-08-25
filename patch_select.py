import sys

cpp_file = "third_party/mlibc/sysdeps/extron/generic/generic.cpp"
with open(cpp_file, "r") as f:
    content = f.read()

if "sys_pselect" not in content:
    patch = """
#include <sys/select.h>
#include <poll.h>

namespace mlibc {
int sys_pselect(int num_fds, fd_set *read_set, fd_set *write_set,
		fd_set *except_set, const struct timespec *timeout, const sigset_t *sigmask, int *num_events) {
    (void)sigmask;
	struct pollfd fds[num_fds];
	nfds_t count = 0;
	for (int i = 0; i < num_fds; i++) {
		short events = 0;
		if (read_set && FD_ISSET(i, read_set)) events |= POLLIN;
		if (write_set && FD_ISSET(i, write_set)) events |= POLLOUT;
		if (except_set && FD_ISSET(i, except_set)) events |= POLLPRI;
		if (events) {
			fds[count].fd = i;
			fds[count].events = events;
			fds[count].revents = 0;
			count++;
		}
	}
	int timeout_ms = -1;
	if (timeout) {
		timeout_ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
	}
	int ret = sys_poll(fds, count, timeout_ms, num_events);
	if (ret) return ret;

	if (read_set) FD_ZERO(read_set);
	if (write_set) FD_ZERO(write_set);
	if (except_set) FD_ZERO(except_set);

	for (nfds_t i = 0; i < count; i++) {
		if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (read_set) FD_SET(fds[i].fd, read_set);
		}
		if (fds[i].revents & (POLLOUT | POLLERR | POLLHUP)) {
			if (write_set) FD_SET(fds[i].fd, write_set);
		}
		if (fds[i].revents & POLLPRI) {
			if (except_set) FD_SET(fds[i].fd, except_set);
		}
	}
	return 0;
}
}
"""
    with open(cpp_file, "a") as f:
        f.write(patch)
    print("Patched generic.cpp")
else:
    print("Already patched")
