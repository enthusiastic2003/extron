/* Calls SYS_PROC_DUMP: prints the process table (and, in a KTRACK=1 kernel
 * build, the live kernel-heap allocations grouped by call site). */
#include "../../mlibc_tests/arch_compat.h"
int main(void) { extron_raw_syscall(3, 0, 0, 0); return 0; }
