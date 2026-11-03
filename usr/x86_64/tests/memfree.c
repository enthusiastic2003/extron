/* Prints the kernel's count of free physical pages (for leak checks across
 * repeated test runs: tools/x86_run_tests.py compares successive values). */
#include <stdio.h>
#include <unistd.h>
int main(void) {
    long free_pages = sysconf(_SC_AVPHYS_PAGES);
    printf("MEMFREE %ld\n", free_pages);
    return free_pages > 0 ? 0 : 1;
}
