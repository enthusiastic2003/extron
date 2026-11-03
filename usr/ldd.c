#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__)
#define LDD_MACHINE EM_X86_64
#else
#define LDD_MACHINE EM_AARCH64
#endif

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
            || ehdr.e_machine != LDD_MACHINE
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


/* Shared objects have no PT_INTERP and cannot be run under
   LD_TRACE_LOADED_OBJECTS, so walk their DT_NEEDED entries ourselves using the
   same search order as ld.so: the requester's RUNPATH, then /lib and /usr/lib. */
#define MAX_SEEN 64
static char *seen[MAX_SEEN];
static int nseen;

static int read_at(int fd, void *buf, size_t n, off_t off) {
    return pread(fd, buf, n, off) == (ssize_t)n ? 0 : -1;
}

static off_t vaddr_to_off(const Elf64_Phdr *ph, int n, Elf64_Addr va) {
    for (int i = 0; i < n; ++i)
        if (ph[i].p_type == PT_LOAD && va >= ph[i].p_vaddr && va < ph[i].p_vaddr + ph[i].p_filesz)
            return (off_t)(va - ph[i].p_vaddr + ph[i].p_offset);
    return -1;
}

static void list_needed(const char *path, int depth) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    Elf64_Ehdr eh;
    Elf64_Phdr *ph = NULL;
    Elf64_Dyn *dyn = NULL;
    char *str = NULL, *runpath = NULL;
    if (read_at(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_phnum > 64)
        goto out;
    ph = calloc(eh.e_phnum, sizeof *ph);
    if (!ph || read_at(fd, ph, eh.e_phnum * sizeof *ph, (off_t)eh.e_phoff))
        goto out;
    size_t dynsz = 0; off_t dynoff = -1;
    for (int i = 0; i < eh.e_phnum; ++i)
        if (ph[i].p_type == PT_DYNAMIC) { dynsz = ph[i].p_filesz; dynoff = (off_t)ph[i].p_offset; }
    if (dynoff < 0 || dynsz > 65536)
        goto out;
    dyn = malloc(dynsz);
    if (!dyn || read_at(fd, dyn, dynsz, dynoff))
        goto out;
    size_t nd = dynsz / sizeof *dyn, strsz = 0;
    Elf64_Addr stra = 0;
    for (size_t i = 0; i < nd && dyn[i].d_tag != DT_NULL; ++i) {
        if (dyn[i].d_tag == DT_STRTAB) stra = dyn[i].d_un.d_ptr;
        if (dyn[i].d_tag == DT_STRSZ) strsz = dyn[i].d_un.d_val;
    }
    off_t stroff = vaddr_to_off(ph, eh.e_phnum, stra);
    if (stroff < 0 || strsz == 0 || strsz > (1 << 20))
        goto out;
    str = malloc(strsz + 1);
    if (!str || read_at(fd, str, strsz, stroff))
        goto out;
    str[strsz] = 0;
    for (size_t i = 0; i < nd && dyn[i].d_tag != DT_NULL; ++i)
        if ((dyn[i].d_tag == DT_RUNPATH || dyn[i].d_tag == DT_RPATH) && dyn[i].d_un.d_val < strsz)
            runpath = str + dyn[i].d_un.d_val;
    for (size_t i = 0; i < nd && dyn[i].d_tag != DT_NULL; ++i) {
        if (dyn[i].d_tag != DT_NEEDED || dyn[i].d_un.d_val >= strsz)
            continue;
        const char *name = str + dyn[i].d_un.d_val;
        char found[512] = "";
        char dirs[1024];
        snprintf(dirs, sizeof dirs, "%s%s/lib:/usr/lib", runpath ? runpath : "", runpath ? ":" : "");
        for (char *save, *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            char cand[512];
            snprintf(cand, sizeof cand, "%s/%s", d, name);
            if (access(cand, R_OK) == 0) { snprintf(found, sizeof found, "%s", cand); break; }
        }
        int dup = 0;
        for (int k = 0; k < nseen; ++k)
            if (!strcmp(seen[k], name)) dup = 1;
        if (dup)
            continue;
        if (nseen < MAX_SEEN) seen[nseen++] = strdup(name);
        if (found[0]) printf("\t%s => %s\n", name, found);
        else printf("\t%s => not found\n", name);
        if (found[0] && depth < 16)
            list_needed(found, depth + 1);
    }
out:
    free(ph); free(dyn); free(str);
    close(fd);
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
        list_needed(argv[1], 0);
        if (!nseen)
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
