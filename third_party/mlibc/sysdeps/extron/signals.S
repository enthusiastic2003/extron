.section .text
.global __mlibc_signal_restore
.type __mlibc_signal_restore, %function
__mlibc_signal_restore:
	mov x8, #38
	svc #0
	brk #1

.global __mlibc_do_asm_cp_syscall
.global __mlibc_syscall_begin
.global __mlibc_syscall_end
.type __mlibc_do_asm_cp_syscall, %function
__mlibc_do_asm_cp_syscall:
	/* x0=number, x1-x3=arguments. */
	mov x8, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3

	/* mlibc's AArch64 thread pointer points 0x10 bytes past the TCB;
	 * cancelBits is 80 bytes before that ABI pointer. */
	mrs x7, tpidr_el0
	ldr w7, [x7, #-80]
__mlibc_syscall_begin:
	mov x9, #((1 << 0) | (1 << 2))
	and x7, x7, x9
	cmp x7, x9
	b.eq 1f
	svc #0
__mlibc_syscall_end:
	ret
1:
	bl __mlibc_do_cancel
	brk #0

.section .note.GNU-stack,"",%progbits
