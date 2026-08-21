// AArch64 exception vector table — the aarch64 counterpart to
// kernel/arch/x86_64/isr.asm, but a fundamentally different shape: no
// IDT, no 256 individually-vectored stubs. VBAR_EL1 points at exactly
// 16 fixed-size (0x80 byte / 32 instruction) slots — 4 exception types
// (Synchronous/IRQ/FIQ/SError) x 4 source contexts — and the CPU jumps
// to whichever slot matches, with NOTHING pushed onto the stack
// automatically (unlike x86's CPU-pushed error code/CS/RFLAGS/SS). Every
// register in struct aarch64_frame (kernel/arch/aarch64/include/arch/
// exceptions.h) is saved here explicitly, by us.
//
// Our kernel always runs at EL1h (SP_EL1) — set that way back in
// boot.S's EL2->EL1 drop — and has no userspace yet, so only the
// "Current EL, SP_ELx" group (offset 0x200-0x380) is ever legitimately
// reached. The other 12 slots are defensive: if they ever fire, SP_EL1
// isn't necessarily even valid to push onto (e.g. the SP_EL0 group means
// we were somehow using SP_EL0, which nothing has ever initialized), so
// they use a stack-free raw UART writer — same technique boot.S's
// (since-removed) temporary debug helper used — rather than attempting
// a normal register save.
//
// 32-instruction budget per slot means the full save+dispatch+restore
// sequence can't fit inline: each real slot does the save (fits in ~26
// instructions) then branches OUT to common_exception_entry, unconstrained
// by the fixed spacing, for the C dispatch + shared restore + eret.

.macro SAVE_CONTEXT
    sub sp, sp, #288
    stp x0, x1,   [sp, #0]
    stp x2, x3,   [sp, #16]
    stp x4, x5,   [sp, #32]
    stp x6, x7,   [sp, #48]
    stp x8, x9,   [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30,      [sp, #240]

    mrs x9,  elr_el1
    mrs x10, spsr_el1
    mrs x11, esr_el1
    mrs x12, far_el1
    stp x9,  x10, [sp, #248]
    stp x11, x12, [sp, #264]
.endm

.macro RESTORE_CONTEXT
    ldp x9,  x10, [sp, #248]
    msr elr_el1, x9
    msr spsr_el1, x10
    // esr_el1/far_el1 aren't restored: they're set by hardware on the
    // NEXT exception, not meant to be written back on a normal return.

    ldp x0, x1,   [sp, #0]
    ldp x2, x3,   [sp, #16]
    ldp x4, x5,   [sp, #32]
    ldp x6, x7,   [sp, #48]
    ldp x8, x9,   [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30,      [sp, #240]
    add sp, sp, #288
.endm

// x0 = frame pointer, x1 = exception type, then branch to the shared
// dispatch/restore path (outside the fixed-size table).
.macro VECTOR_STUB type
    SAVE_CONTEXT
    mov x1, #\type
    mov x0, sp
    b   common_exception_entry
.endm

// Stack-free: writes one fixed byte to the UART using only registers,
// for vectors that fire in a context where the current SP isn't known
// to be valid. Reuses the raw physical UART address — still reachable
// via TTBR0_EL1's identity mapping (boot.S), unconditionally, regardless
// of which half of the address space is otherwise in use.
.macro SPURIOUS_VECTOR marker
    ldr  x9, =0xFE201000
1:  ldr  w10, [x9, #0x18]
    tst  w10, #0x20
    b.ne 1b
    mov  w11, #\marker
    str  w11, [x9, #0x00]
2:  wfe
    b    2b
.endm

.section ".text"
.balign 0x800
.global vector_table
vector_table:

// --- Current EL, SP_EL0 (unreachable: we always run EL1h) ---
.balign 0x80
    SPURIOUS_VECTOR 'a'
.balign 0x80
    SPURIOUS_VECTOR 'b'
.balign 0x80
    SPURIOUS_VECTOR 'c'
.balign 0x80
    SPURIOUS_VECTOR 'd'

// --- Current EL, SP_ELx (EL1h — the ones we actually use) ---
.balign 0x80
    VECTOR_STUB 0 // Synchronous
.balign 0x80
    VECTOR_STUB 1 // IRQ
.balign 0x80
    VECTOR_STUB 2 // FIQ
.balign 0x80
    VECTOR_STUB 3 // SError

// --- Lower EL, AArch64 (unreachable: no userspace yet) ---
.balign 0x80
    SPURIOUS_VECTOR 'e'
.balign 0x80
    SPURIOUS_VECTOR 'f'
.balign 0x80
    SPURIOUS_VECTOR 'g'
.balign 0x80
    SPURIOUS_VECTOR 'h'

// --- Lower EL, AArch32 (unreachable: we never run 32-bit code) ---
.balign 0x80
    SPURIOUS_VECTOR 'i'
.balign 0x80
    SPURIOUS_VECTOR 'j'
.balign 0x80
    SPURIOUS_VECTOR 'k'
.balign 0x80
    SPURIOUS_VECTOR 'l'

// Outside the fixed-size table: no 32-instruction constraint here.
common_exception_entry:
    // x0 = frame pointer, x1 = exception type — both already correctly
    // placed as exception_dispatch(struct aarch64_frame *f, int type)'s
    // first two AAPCS64 arguments.
    bl  exception_dispatch
    RESTORE_CONTEXT
    eret
