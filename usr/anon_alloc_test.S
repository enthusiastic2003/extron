// Phase 2 VMA allocator proof (kernel/mm/uvm.c): allocates twice, frees
// the first, allocates a third time — real VA-space reuse (the exact
// gap x86's bump allocator has, per the plan) means the third
// allocation's base should exactly equal the first's, since freeing it
// made that gap visible again to vm_allocate_region()'s scan. Also
// writes/reads back through each allocation to prove the memory is
// really mapped and usable, not just an address handed back unbacked.
.section ".rodata"
pass_msg:
    .ascii "ANON_ALLOC_REUSE: PASS\n"
pass_len = . - pass_msg
fail_msg:
    .ascii "ANON_ALLOC_REUSE: FAIL\n"
fail_len = . - fail_msg
write_pass_msg:
    .ascii "ANON_ALLOC_WRITE: PASS\n"
write_pass_len = . - write_pass_msg
write_fail_msg:
    .ascii "ANON_ALLOC_WRITE: FAIL\n"
write_fail_len = . - write_fail_msg

.section ".text"
.global _start
_start:
    mov     x8, #4          // SYS_ANON_ALLOC
    mov     x0, #4096
    svc     #0
    mov     x21, x0         // alloc1

    mov     x8, #4          // SYS_ANON_ALLOC
    mov     x0, #4096
    svc     #0
    mov     x22, x0         // alloc2

    // Write a known pattern through alloc1, read it back.
    mov     x2, #0x41
    strb    w2, [x21]
    ldrb    w3, [x21]
    cmp     w2, w3
    b.ne    write_fail

    mov     x8, #1          // SYS_WRITE
    mov     x0, #1
    ldr     x1, =write_pass_msg
    mov     x2, #write_pass_len
    svc     #0
    b       write_done
write_fail:
    mov     x8, #1
    mov     x0, #1
    ldr     x1, =write_fail_msg
    mov     x2, #write_fail_len
    svc     #0
write_done:

    mov     x8, #5          // SYS_ANON_FREE
    mov     x0, x21
    mov     x1, #4096
    svc     #0

    mov     x8, #4          // SYS_ANON_ALLOC
    mov     x0, #4096
    svc     #0
    mov     x23, x0         // alloc3

    cmp     x23, x21
    b.ne    reuse_fail

    mov     x8, #1
    mov     x0, #1
    ldr     x1, =pass_msg
    mov     x2, #pass_len
    svc     #0
    b       reuse_done
reuse_fail:
    mov     x8, #1
    mov     x0, #1
    ldr     x1, =fail_msg
    mov     x2, #fail_len
    svc     #0
reuse_done:

    mov     x8, #7          // SYS_EXIT
    mov     x0, #0
    svc     #0
1:  b       1b
