#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
int main(void) {
    int fds[2]; pipe(fds);
    write(fds[1], "abc", 3);
    char buf[4096]; memset(buf, 'Z', sizeof buf);
    ssize_t n = read(fds[0], buf, sizeof buf);
    printf("same-process pipe read returned %ld [%.3s]\n", (long)n, buf);
    pid_t c = fork();
    if (c == 0) { close(fds[0]); write(fds[1], "xyz", 3); _exit(0); }
    close(fds[1]);
    n = read(fds[0], buf, sizeof buf);
    printf("child pipe read returned %ld [%.3s] (expect 3)\n", (long)n, buf);
    n = read(fds[0], buf, sizeof buf);
    printf("then EOF read returned %ld (expect 0)\n", (long)n);
    waitpid(c, 0, 0);
    return 0;
}
