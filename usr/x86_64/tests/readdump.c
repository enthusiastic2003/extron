#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
int main(void) {
    struct stat st; fstat(0, &st);
    printf("stdin: mode=%o size=%ld isreg=%d isfifo=%d\n", st.st_mode, (long)st.st_size, S_ISREG(st.st_mode), S_ISFIFO(st.st_mode));
    char buf[512]; ssize_t n; long tot = 0;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        printf("read %ld:", (long)n);
        for (int i = 0; i < n && i < 40; i++) printf(" %02x", (unsigned char)buf[i]);
        printf("\n"); tot += n;
    }
    printf("total %ld\n", tot);
    return 0;
}
