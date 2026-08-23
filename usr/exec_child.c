/*
 * The program on the far side of an execve().
 *
 * Prints its own argv and exits with a fixed status. Between them those
 * two things cover everything execve has to get right that fork alone
 * doesn't:
 *
 *  - reaching main() at all means a whole new ELF image was loaded into
 *    a live process and the trap frame was rewritten to land on its
 *    entry point rather than resuming the caller;
 *  - printing argv means the argument strings survived the destruction
 *    of the address space they were read out of (sys_execve copies them
 *    into the kernel before touching anything) and were laid back out
 *    on a stack that did not exist when they were copied;
 *  - the exit status is what the parent's wait() has to report back, so
 *    a wrong value there means the reaping path lost it.
 *
 * Deliberately does NOT print a PASS/FAIL of its own. The claims being
 * tested are about what the PARENT observes, and a child asserting on
 * its own existence proves less than a parent asserting on a status it
 * received from a process that no longer exists.
 */
#include <stdio.h>
#include <extron/syscall.h>

#define EXEC_CHILD_STATUS      7
#define EXEC_CHILD_STATUS_BAD  8

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv) {
    printf("[exec_child] running, argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("[exec_child]   argv[%d] = \"%s\"\n", i, argv[i]);

    /* argv[argc] must be NULL — the terminator C requires, and the one
     * thing about the vector a caller can rely on without being told
     * how long it is. */
    printf("[exec_child]   argv[%d] = %s\n", argc,
           argv[argc] == 0 ? "NULL (correct)" : "NOT NULL (BUG)");

    /* The forking parent cannot see what its child observed about the
     * FP registers it inherited — the child replaces itself with this
     * program and is gone. So the verdict arrives as an argument and
     * leaves as an exit status, which is the one channel that outlives
     * the process. Any argv carrying "fp-lost" (or carrying no verdict
     * at all, as the plain leak-check cycles do) still exits 7 unless
     * it explicitly says the state was lost. */
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "fp-lost")) {
            printf("[exec_child] told FP state was LOST across the fork\n");
            sys_exit(EXEC_CHILD_STATUS_BAD);
        }
    }

    sys_exit(EXEC_CHILD_STATUS);
    return 0;
}
