#include <elf.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)getauxval(AT_PHDR);
    size_t phnum = getauxval(AT_PHNUM);
    uintptr_t base = 0;
    const Elf64_Phdr *relro = NULL;

    for (size_t i = 0; i < phnum; i++) {
        if (phdr[i].p_type == PT_PHDR)
            base = (uintptr_t)phdr - phdr[i].p_vaddr;
        if (phdr[i].p_type == PT_GNU_RELRO)
            relro = &phdr[i];
    }

    int metadata_ok = relro && relro->p_memsz;
    printf("[relro_test] executable contains PT_GNU_RELRO: %s\n",
           metadata_ok ? "PASS" : "FAIL");
    if (!metadata_ok)
        return 1;

    pid_t child = fork();
    if (child == 0) {
        volatile unsigned char *target =
            (volatile unsigned char *)(base + relro->p_vaddr);
        unsigned char value = *target;
        *target = value;
        _exit(0);
    }
    if (child < 0) {
        printf("[relro_test] fork() failed: FAIL\n");
        return 1;
    }

    int status = 0;
    int waited = waitpid(child, &status, 0) == child;
    int protected = waited && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    printf("[relro_test] write into RELRO faults only the child: %s\n",
           protected ? "PASS" : "FAIL");
    return protected ? 0 : 1;
}
