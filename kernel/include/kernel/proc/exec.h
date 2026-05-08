#ifndef EXEC_H
#define EXEC_H
#define USER_STACK_TOP  0x00007FFFFFFFF000
#define USER_STACK_SIZE (16 * 1024)

// Size of the per-process kernel stack (used for interrupts + syscalls)
#define PROC_KERNEL_STACK_PAGES 8           // 32 KB

void enter_userspace(uint64_t entry, uint64_t user_rsp);
int exec(const char* binary_path);

#endif // !EXEC_H
