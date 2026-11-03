/*
 * x86-64 kernel entry. Reached from the UEFI loader (boot/uefi/loader.c)
 * after ExitBootServices(), in 64-bit long mode with the firmware's
 * identity-mapped page tables still active, interrupts disabled, and:
 *     rdi = physical address of the multiboot2-shaped boot info block
 * The image executes at its physical load address (KERNEL_LMA) until the
 * new page tables below are live; only then does it jump to the higher
 * half. All symbol addresses are link-time higher-half VMAs, so anything
 * touched before the switch is converted with (sym - KERNEL_VMA).
 */
#define KERNEL_VMA 0xFFFFFFFF80000000

.section .text.boot, "ax"
.code64
.global _start
_start:
    cli
    cld

    /* Temporary stack inside the image (physical == identity-mapped). */
    movabs $_stack_top, %rax
    movabs $KERNEL_VMA, %rcx
    sub    %rcx, %rax
    mov    %rax, %rsp

    mov    %rdi, %r15              /* preserve boot info across the table build */

    /* Zero .bss (physical alias). */
    movabs $KERNEL_VMA, %rax
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    sub    %rdi, %rcx
    sub    %rax, %rdi
    xor    %eax, %eax
    rep stosb

    /* ---- Build bootstrap page tables (all in .bss, physical addrs) ---- */
    movabs $boot_pml4, %rdi
    movabs $KERNEL_VMA, %rax
    sub    %rax, %rdi              /* rdi = phys(boot_pml4) */
    movabs $boot_pdpt_lo, %rsi
    sub    %rax, %rsi              /* rsi = phys(boot_pdpt_lo) */
    movabs $boot_pdpt_hi, %rdx
    sub    %rax, %rdx              /* rdx = phys(boot_pdpt_hi) */
    movabs $boot_pd, %rcx
    sub    %rax, %rcx              /* rcx = phys(boot_pd[0]) : 4 consecutive PDs */

    /* PML4[0] -> pdpt_lo (identity, first 4 GiB);
       PML4[511] -> pdpt_hi (kernel at -2 GiB). */
    mov    %rsi, %rax
    or     $3, %rax
    mov    %rax, (%rdi)
    mov    %rdx, %rax
    or     $3, %rax
    mov    %rax, 511*8(%rdi)

    /* pdpt_lo[0..3] -> pd[0..3]; pdpt_hi[510..511]: map first 2 GiB of
       physical memory at KERNEL_VMA via pd[0..1] (2 MiB pages, RWX). */
    xor    %r8d, %r8d
1:  mov    %r8, %rax
    shl    $12, %rax
    add    %rcx, %rax
    or     $3, %rax
    mov    %rax, (%rsi,%r8,8)
    inc    %r8
    cmp    $4, %r8
    jb     1b

    mov    (%rsi), %rax
    mov    %rax, 510*8(%rdx)
    mov    8(%rsi), %rax
    mov    %rax, 511*8(%rdx)

    /* Fill 4 PDs: 2048 entries, entry i = i*2MiB | PS | RW | P. */
    xor    %r8d, %r8d
2:  mov    %r8, %rax
    shl    $21, %rax
    or     $0x83, %rax
    mov    %rax, (%rcx,%r8,8)
    inc    %r8
    cmp    $2048, %r8
    jb     2b

    /* Make sure CR4.PAE and EFER.LME/NXE are set (they are under UEFI, but
       be explicit; NX is needed for PAGE_NX). */
    mov    %cr4, %rax
    or     $0x20, %rax
    mov    %rax, %cr4
    mov    $0xC0000080, %ecx
    rdmsr
    or     $0x900, %eax            /* LME | NXE */
    wrmsr

    mov    %rdi, %cr3              /* switch to bootstrap tables */

    /* Jump to the higher half and continue there. */
    movabs $higher_half, %rax
    jmp    *%rax

.section .text
higher_half:
    /* Real stack in the higher half; the identity map stays live until
       init_paging() installs the kernel's own tables. */
    movabs $_stack_top, %rsp
    xor    %ebp, %ebp

    /* Load our own GDT before anything can fault. */
    call   gdt_early_init

    mov    %r15, %rdi
    call   kernel_x86_64_main

1:  cli
    hlt
    jmp    1b

.section .bss
.balign 4096
boot_pml4:    .skip 4096
boot_pdpt_lo: .skip 4096
boot_pdpt_hi: .skip 4096
boot_pd:      .skip 4096*4
