#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int failures;

static void check(const char *name, int ok) {
    printf("[file_test] %-48s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    char buf[32] = {0};
    FILE *f = fopen("hello.txt", "rb");
    check("fopen initrd file", f != NULL);
    if (f) {
        check("fread returns requested bytes", fread(buf, 1, 5, f) == 5);
        check("fread content", memcmp(buf, "Hello", 5) == 0);
        check("fseek SEEK_SET", fseek(f, 6, SEEK_SET) == 0);
        memset(buf, 0, sizeof(buf));
        check("fread after seek", fread(buf, 1, 4, f) == 4 &&
                                  memcmp(buf, "from", 4) == 0);
        check("fclose", fclose(f) == 0);
    }
    check("missing file reports failure", fopen("missing", "rb") == NULL);

    check("mkdir creates a ramfs directory", mkdir("saves", 0755) == 0);
    FILE *created = fopen("default.cfg", "wb");
    check("fopen creates writable ramfs file", created != NULL);
    if (created) {
        check("fwrite ramfs file", fwrite("volume=8\n", 1, 9, created) == 9);
        check("close written ramfs file", fclose(created) == 0);
    }
    created = fopen("default.cfg", "rb");
    memset(buf, 0, sizeof(buf));
    check("reopen created file", created != NULL);
    if (created) {
        check("created file retained contents",
              fread(buf, 1, 9, created) == 9 && memcmp(buf, "volume=8\n", 9) == 0);
        fclose(created);
    }
    created = fopen("default.cfg", "wb");
    check("truncate existing file", created && fclose(created) == 0);
    created = fopen("default.cfg", "rb");
    check("truncated file is empty", created && fgetc(created) == EOF);
    if (created) fclose(created);

    FILE *shared = fopen("hello.txt", "rb");
    int fd = fileno(shared);
    char c = 0;
    check("descriptor read before fork", read(fd, &c, 1) == 1 && c == 'H');
    pid_t pid = fork();
    if (pid == 0) {
        char child = 0;
        _Exit(read(fd, &child, 1) == 1 && child == 'e' ? 0 : 1);
    }
    int status = 0;
    check("fork child consumed shared offset",
          wait(&status) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    c = 0;
    check("parent observes shared offset", read(fd, &c, 1) == 1 && c == 'l');
    fclose(shared);

    printf("[file_test] === %d failure(s) ===\n", failures);
    return failures != 0;
}
