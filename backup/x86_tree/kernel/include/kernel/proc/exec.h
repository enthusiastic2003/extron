#ifndef EXEC_H
#define EXEC_H
#define USER_STACK_TOP  0x00007FFFFFFFF000
#define USER_STACK_SIZE (16 * 1024)



void enter_userspace(uint64_t entry, uint64_t user_rsp);
int exec_load_binary(const char *binary_path, int argc, char **argv, int envc, char **envp, phys_addr_t *out_pml4, virt_addr_t *out_entry, virt_addr_t *out_stack);
struct proc* proc_create_from_binary(const char* binary_path, struct proc* parent);

#endif // !EXEC_H
