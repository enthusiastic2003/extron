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
// boot.S's EL2->EL1 drop — so the "Current EL, SP_ELx" group (offset
// 0x200-0x380) handles every kernel-on-itself exception. The "Lower EL,
// AArch64" group (0x400-0x780) is now reachable too, since
// kernel_aarch64.c can eret into a TTBR0-mapped EL0 payload (see
// el0_test_stub.S) — Sync/IRQ/FIQ there use the same VECTOR_STUB. The
// remaining slots stay defensive: SP_EL0 (we never select it) and Lower
// EL AArch32 (we never run 32-bit code) fire in contexts where SP_EL1
// isn't necessarily even valid to push onto, so they use a stack-free
// raw UART writer — same technique boot.S's (since-removed) temporary
// debug helper used — rather than attempting a normal register save.
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

    // SP_EL0: the interrupted context's own stack pointer. Banked, so it
    // survives the exception itself — but nothing preserves it across a
    // context switch, so without this a process resumes on whatever
    // stack pointer the previously-running process left behind. x13 is
    // already saved above (with x12, at #96), so it's free here.
    mrs x13, sp_el0
    str x13,      [sp, #280]
.endm

.macro RESTORE_CONTEXT
    ldp x9,  x10, [sp, #248]
    msr elr_el1, x9
    msr spsr_el1, x10
    // esr_el1/far_el1 aren't restored: they're set by hardware on the
    // NEXT exception, not meant to be written back on a normal return.

    // Before the general registers, while x9 is still scratch. Writing
    // it when returning to EL1 is harmless: EL1h runs on SP_EL1, so the
    // value just goes back unchanged.
    ldr x9,       [sp, #280]
    msr sp_el0, x9

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
// to be valid.
//
// Uses the UART's high-half alias (NEW_HDDM + 0xFE201000), not the raw
// physical address. The raw address is a LOW address, so it resolves
// through TTBR0 — whatever process table is loaded — which meant this
// path only ever worked because exec.c identity-mapped the page into
// every process. That mapping is gone; the high alias lives in the
// kernel's own TTBR1 table and is therefore reachable under any
// process, which is exactly what a last-ditch panic path needs.
//
// Safe unconditionally: boot.S never sets VBAR_EL1, so this table only
// becomes live in exceptions_init(), which runs after init_paging()
// has both created the mapping and made TTBR1 current.
.macro SPURIOUS_VECTOR marker
    ldr  x9, =0xFFFF8000FE201000
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

// --- Lower EL, AArch64 (EL0 exists now — see el0_test_stub.S. Sync/IRQ/
// FIQ wired identically to the Current-EL group above: SAVE_CONTEXT
// always targets SP_EL1, which the architecture unconditionally selects
// on any exception taken to EL1 regardless of which EL it came from, so
// the same stub is valid here. SError stays spurious: DAIF.A is never
// unmasked (exceptions_enable_irqs()), so it architecturally cannot
// fire from EL0 either. ---
.balign 0x80
    VECTOR_STUB 0 // Synchronous (e.g. svc from EL0)
.balign 0x80
    VECTOR_STUB 1 // IRQ
.balign 0x80
    VECTOR_STUB 2 // FIQ
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

// --- fork() return path ---
//
// Where a freshly forked child starts executing. Never called: it is
// reached only by context_switch's `ret` landing on a saved context.lr,
// exactly like proc_bootstrap_trampoline() — but where that one
// synthesizes a first entry into EL0 from an entry point and a stack
// pointer, this one resumes a FULL register state that already exists.
//
// proc_fork() (kernel/proc/proc_fork.c) copies the parent's trap frame
// to the top of the child's kernel stack, sets x0 to 0 in the copy, and
// points the child's context.sp at it. So by the time this runs, sp is
// already the frame pointer RESTORE_CONTEXT expects, and the child
// returns from its `svc #0` at the same instruction the parent does,
// with the same registers, on its own copy of the same stack — the only
// difference being the return value.
//
// This is the same landing pad common_exception_entry uses after
// exception_dispatch(); the child simply joins it partway through,
// skipping the dispatch it never made.
.global proc_fork_trampoline
proc_fork_trampoline:
    RESTORE_CONTEXT
    eret
