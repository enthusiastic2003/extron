#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

enum target_kind {
    TARGET_ERROR = -1,
    TARGET_NO_INTERPRETER,
    TARGET_DYNAMIC
};

static enum target_kind inspect_target(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return TARGET_ERROR;

    Elf64_Ehdr ehdr;
    ssize_t count = read(fd, &ehdr, sizeof(ehdr));
    if (count != (ssize_t)sizeof(ehdr)
            || memcmp(ehdr.e_ident, ELFMAG, SELFMAG)
            || ehdr.e_ident[EI_CLASS] != ELFCLASS64
            || ehdr.e_ident[EI_DATA] != ELFDATA2LSB
            || ehdr.e_machine != EM_AARCH64
            || (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN)
            || ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
        close(fd);
        errno = ENOEXEC;
        return TARGET_ERROR;
    }

    if (lseek(fd, (off_t)ehdr.e_phoff, SEEK_SET) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return TARGET_ERROR;
    }

    int has_interpreter = 0;
    for (Elf64_Half i = 0; i < ehdr.e_phnum; ++i) {
        Elf64_Phdr phdr;
        if (read(fd, &phdr, sizeof(phdr)) != (ssize_t)sizeof(phdr)) {
            close(fd);
            errno = ENOEXEC;
            return TARGET_ERROR;
        }
        if (phdr.p_type == PT_INTERP)
            has_interpreter = 1;
    }

    close(fd);
    return has_interpreter ? TARGET_DYNAMIC : TARGET_NO_INTERPRETER;
}

static char **make_trace_environment(void) {
    static const char trace[] = "LD_TRACE_LOADED_OBJECTS=1";
    size_t count = 0;
    size_t kept = 0;

    while (environ && environ[count])
        ++count;

    char **result = calloc(count + 2, sizeof(*result));
    if (!result)
        return NULL;

    for (size_t i = 0; i < count; ++i) {
        if (!strncmp(environ[i], "LD_TRACE_LOADED_OBJECTS=", 24))
            continue;
        result[kept++] = environ[i];
    }
    result[kept++] = (char *)trace;
    result[kept] = NULL;
    return result;
}

static void usage(FILE *stream) {
    fprintf(stream, "Usage: ldd FILE\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout);
        puts("Print the shared-library dependencies selected by Extron's runtime linker.");
        return 0;
    }

    if (argc != 2) {
        usage(stderr);
        return 2;
    }

    enum target_kind kind = inspect_target(argv[1]);
    if (kind == TARGET_ERROR) {
        fprintf(stderr, "ldd: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (kind == TARGET_NO_INTERPRETER) {
        puts("\tnot a dynamically linked executable");
        return 0;
    }

    char **trace_environment = make_trace_environment();
    if (!trace_environment) {
        fprintf(stderr, "ldd: cannot prepare environment: %s\n", strerror(errno));
        return 1;
    }

    /* mlibc's early runtime-linker logger uses stderr. ldd output is ordinary
       command output, so make it follow stdout redirections before exec. */
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        fprintf(stderr, "ldd: cannot redirect loader output: %s\n", strerror(errno));
        free(trace_environment);
        return 1;
    }

    char *target_argv[] = {argv[1], NULL};
    execve(argv[1], target_argv, trace_environment);

    int error = errno;
    fprintf(stderr, "ldd: %s: %s\n", argv[1], strerror(error));
    free(trace_environment);
    return error == ENOENT ? 127 : 126;
}
