#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc, char **argv) {
    printf("hello from mlibc on x86_64 extron! argc=%d pid=%d\n", argc, (int)getpid());
    char *p = malloc(100000); memset(p, 7, 100000); free(p);
    double d = 0; for (int i = 1; i < 1000; i++) d += 1.0 / i;
    printf("harmonic(999)=%f\n", d);
    pid_t c = fork();
    if (c == 0) { printf("child says hi\n"); fflush(stdout); _exit(3); }
    int st; waitpid(c, &st, 0);
    printf("child exited %d\n", WEXITSTATUS(st));
    return 0;
}
