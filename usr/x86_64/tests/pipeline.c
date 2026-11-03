#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
    int fds[2]; pipe(fds);
    pid_t a = fork();
    if (a == 0) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); write(1, "abc", 3); _exit(0); }
    pid_t b = fork();
    if (b == 0) { dup2(fds[0], 0); close(fds[0]); close(fds[1]);
        execl("/bin/readdump", "readdump", (char *)0); _exit(99); }
    close(fds[0]); close(fds[1]);
    int st; waitpid(a, &st, 0); printf("A status %d\n", st);
    waitpid(b, &st, 0); printf("B status %d\n", st);
    return 0;
}
