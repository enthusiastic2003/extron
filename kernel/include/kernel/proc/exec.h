#ifndef EXEC_H
#define EXEC_H
#define USER_STACK_TOP  0x00007FFFFFFFF000
#define USER_STACK_SIZE (16 * 1024)        // 16 KB to star

void enter_userspace(uint64_t entry, uint64_t user_rsp);
int exec(const char* binary_path);

#endif // !EXEC_H
