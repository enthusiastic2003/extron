#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t user_rsp;
} __attribute__((packed));

int main() {
    printf("size: %lu, rax offset: %lu\n", sizeof(struct syscall_frame), offsetof(struct syscall_frame, rax));
    return 0;
}
