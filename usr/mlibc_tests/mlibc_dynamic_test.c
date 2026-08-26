#include <elf.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

static int constructor_ran;
static _Thread_local int tls_value = 37;

extern int extron_dso_global;
extern _Thread_local int extron_dso_tls;
int extron_dso_value(void);

__attribute__((constructor))
static void dynamic_constructor(void) {
    constructor_ran = 1;
    tls_value += 5;
}

int main(int argc, char **argv) {
    const char *execfn = (const char *)getauxval(AT_EXECFN);
    int ok = argc > 0 && argv && argv[0] && constructor_ran && tls_value == 42
          && execfn && !strcmp(execfn, argv[0])
          && extron_dso_global == 22 && extron_dso_tls == 11
          && extron_dso_value() == 33;
    printf("[dynamic_test] ld.so + libc/DSO relocations + constructors + TLS: %s\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
