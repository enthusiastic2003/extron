// void context_switch(struct cpu_context *old, struct cpu_context *new);
//                      x0                       x1
//
// aarch64 counterpart to kernel/arch/x86_64/proc/switch.asm — but
// simpler: x86 has to fabricate a synthetic ".resume" label and store
// its address as the saved "rip", because call/ret communicate the
// return address via the stack, not a register — a plain ret after
// swapping rsp would land on whatever's at the new stack, not back at
// the right C call site. AArch64's bl/ret convention already keeps the
// return address in a normal register (x30/lr) for the whole call — one
// already in the callee-saved set below — so no stub is needed: saving
// and restoring lr like any other register, then a plain `ret`, is
// already correct.
//
// struct cpu_context field offsets (kernel/arch/aarch64/include/arch/proc.h):
//   x19..x28 = 0x00..0x48 (8 regs)   fp = 0x50   lr = 0x58   sp = 0x60

.section ".text"
.global context_switch
context_switch:
    stp x19, x20, [x0, #0x00]
    stp x21, x22, [x0, #0x10]
    stp x23, x24, [x0, #0x20]
    stp x25, x26, [x0, #0x30]
    stp x27, x28, [x0, #0x40]
    stp x29, x30, [x0, #0x50]
    mov x9, sp
    str x9,       [x0, #0x60]

    ldp x19, x20, [x1, #0x00]
    ldp x21, x22, [x1, #0x10]
    ldp x23, x24, [x1, #0x20]
    ldp x25, x26, [x1, #0x30]
    ldp x27, x28, [x1, #0x40]
    ldp x29, x30, [x1, #0x50]
    ldr x9,       [x1, #0x60]
    mov sp, x9

    ret
