#include <arch/exceptions.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/proc/syscall.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/resource.h>
#include <kernel/proc/exec.h>
#include <kernel/mm/uvm.h>
#include <arch/vma.h>
#include <kernel/klibc/string.h>
#include <stdbool.h>

/*
 * GDT / TSS / IDT, the interrupt-dispatch table, and the syscall MSRs —
 * everything the aarch64 tree's exceptions.c + exception_vectors.S + gic.c
 * do between them. Selector layout (fixed by STAR's sysret arithmetic):
 *   0x08 kernel code   0x10 kernel data   0x18 user data   0x20 user code
 *   0x28 TSS (16 bytes)
 */

struct gdt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));
struct idt_entry {
    uint16_t off_lo, sel;
    uint8_t  ist, type;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} __attribute__((packed));
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} __attribute__((packed));

static uint64_t gdt[7] __attribute__((aligned(16)));
static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));
static uint8_t df_stack[16384] __attribute__((aligned(16)));

/* Read by syscall_entry (isr.S). */
uint64_t kernel_rsp0;
uint64_t syscall_user_rsp;

extern void syscall_entry(void);
extern void *isr_stub_table[256];

void arch_set_kernel_stack(uint64_t top) {
    kernel_rsp0 = top;
    tss.rsp[0] = top;
}

static void gdt_install(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* kernel code, L=1 */
    gdt[2] = 0x00CF92000000FFFFULL;   /* kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* user data, DPL3 */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* user code, L=1, DPL3 */

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40)
           | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdt_ptr gp = { sizeof(gdt) - 1, (uint64_t)gdt };
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :: "m"(gp) : "rax", "memory");
}

/* Called from boot.S before anything can fault. */
void gdt_early_init(void) {
    tss.iomap_base = sizeof(tss);
    tss.ist[0] = (uint64_t)df_stack + sizeof(df_stack);
    gdt_install();
    __asm__ volatile ("ltr %%ax" :: "a"(0x28));
}

static void idt_set(int v, void *handler, uint8_t type, uint8_t ist) {
    uint64_t h = (uint64_t)handler;
    idt[v].off_lo = h & 0xFFFF;
    idt[v].sel = 0x08;
    idt[v].ist = ist;
    idt[v].type = type;
    idt[v].off_mid = (h >> 16) & 0xFFFF;
    idt[v].off_hi = h >> 32;
    idt[v].zero = 0;
}

#define IRQ_HANDLER_MAX 16
#define IRQ_SHARE_MAX 4      /* PCI INTx lines are shared: disk and NIC may land on one */
static arch_irq_handler_fn irq_handlers[IRQ_HANDLER_MAX][IRQ_SHARE_MAX];

void register_irq_handler(unsigned irq, arch_irq_handler_fn handler) {
    if (irq >= IRQ_HANDLER_MAX)
        return;
    for (int i = 0; i < IRQ_SHARE_MAX; i++) {
        if (irq_handlers[irq][i] == handler)
            return;
        if (!irq_handlers[irq][i]) {
            irq_handlers[irq][i] = handler;
            return;
        }
    }
}

void exceptions_init(void) {
    for (int i = 0; i < 256; i++)
        idt_set(i, isr_stub_table[i], 0x8E, i == 8 ? 1 : 0);   /* interrupt gate, DPL0 */
    idt_set(0x80, isr_stub_table[0x80], 0xEE, 0);              /* int 0x80, DPL3 */
    struct gdt_ptr ip = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" :: "m"(ip));

    /* SSE for user code: OSFXSR + OSXMMEXCPT, x87 enabled. The kernel itself
     * never touches FP/SIMD (-mgeneral-regs-only), as on aarch64. */
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);   /* EM */
    cr0 |= (1ULL << 1);    /* MP */
    cr0 |= (1ULL << 16);   /* WP: kernel honours read-only user pages */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    {   /* SMEP (CR4.20): ring 0 must never execute user-mapped pages. SMAP is
         * deliberately left off — syscall handlers dereference validated user
         * pointers directly rather than through stac/clac-wrapped copies. */
        uint32_t a = 7, b, c = 0, d;
        __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        if (b & (1u << 7)) cr4 |= (1ULL << 20);
    }
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile ("fninit");

    /* syscall/sysret: EFER.SCE, STAR (kernel cs 0x08, sysret base 0x10),
     * LSTAR, FMASK clears IF/DF/TF on entry. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);
    wrmsr(MSR_STAR, ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x700);
}

void exceptions_enable_irqs(void) {
    __asm__ volatile ("sti" ::: "memory");
}

static const char *exc_name(uint64_t v) {
    static const char *names[32] = {
        "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND range",
        "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
        "Invalid TSS", "Segment not present", "Stack fault", "General protection",
        "Page fault", "Reserved", "x87 FP exception", "Alignment check",
        "Machine check", "SIMD FP exception", "Virtualization", "Control protection",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor injection", "VMM communication", "Security", "Reserved" };
    return v < 32 ? names[v] : "Unknown";
}

static int fault_signal(uint64_t v) {
    switch (v) {
        case 0: case 16: case 19: return 8;   /* SIGFPE */
        case 1: case 3:           return 5;   /* SIGTRAP */
        case 6: case 7:           return 4;   /* SIGILL */
        case 13: case 14: case 12: case 11: return 11; /* SIGSEGV */
        case 17:                  return 7;   /* SIGBUS */
        default:                  return 4;
    }
}

static void dump_and_panic(struct arch_frame *f) {
    struct proc *cur = my_proc();
    struct thread *thread = my_thread();
    panic("CPU EXCEPTION %lu (%s) err=%lx\n"
          "pid=%ld frame=%p kstack=[%p,%p)\n"
          "RIP=%p CS=%lx RFLAGS=%lx RSP=%p\n"
          "CR2=%p\n"
          "RAX=%lx RBX=%lx RCX=%lx RDX=%lx\nRSI=%lx RDI=%lx RBP=%lx\n",
          (unsigned long)f->vector, exc_name(f->vector), (unsigned long)f->error_code,
          cur ? (long)cur->pid : -1, (void *)f,
          thread ? (void *)thread->kernel_stack_base : 0,
          thread ? (void *)thread->kernel_stack_top : 0,
          (void *)f->rip, (unsigned long)f->cs, (unsigned long)f->rflags, (void *)f->rsp,
          (void *)f->cr2,
          (unsigned long)f->rax, (unsigned long)f->rbx, (unsigned long)f->rcx,
          (unsigned long)f->rdx, (unsigned long)f->rsi, (unsigned long)f->rdi,
          (unsigned long)f->rbp);
}

void exception_dispatch(struct arch_frame *f) {
    resource_account_exception_enter(f);

    if (f->vector >= 32 && f->vector < 48) {
        unsigned irq = (unsigned)f->vector - 32;
        /* EOI before the handler, for the same reason the aarch64 path EOIs
         * first: a handler that reschedules may never return here. */
        pic_eoi(irq);
        for (int i = 0; i < IRQ_SHARE_MAX; i++)
            if (irq_handlers[irq][i])
                irq_handlers[irq][i](f);
        signal_deliver_pending(f);
        resource_account_exception_leave(f);
        return;
    }

    if (f->vector == 0x80) {
        FRAME_RET(f) = syscall_dispatch(f);
        signal_deliver_pending(f);
        resource_account_exception_leave(f);
        return;
    }

    if (f->vector == 255 || (f->vector >= 48 && f->vector != 0x80)) {
        /* Spurious / unexpected software vector. */
        resource_account_exception_leave(f);
        return;
    }

    struct proc *cur = my_proc();
    struct thread *thread = my_thread();
    if (FRAME_FROM_USER(f) && cur && thread) {
        /* Not-present fault just below the main stack: grow it (RLIMIT-style
         * 8 MiB cap), as Linux does, instead of killing the process. */
        if (f->vector == 14 && !(f->error_code & 1) && cur->mm &&
            vm_grow_stack(cur->mm, f->cr2, f->rsp, ARCH_USER_STACK_TOP,
                          USER_STACK_MAX_BYTES) == 0) {
            resource_account_exception_leave(f);
            return;
        }
        int signal = fault_signal(f->vector);
        if (signal_deliver_sync(f, signal)) {
            resource_account_exception_leave(f);
            return;
        }
        kprintf("\n[USER FAULT] pid=%lu tid=%lu signal=%d (%s)\n"
                "RIP=%p CR2=%p err=%lx RSP=%p\n",
                (unsigned long)cur->pid, (unsigned long)thread->tid,
                signal, exc_name(f->vector), (void *)f->rip, (void *)f->cr2,
                (unsigned long)f->error_code, (void *)f->rsp);
        kprintf("RAX=%lx RBX=%lx RCX=%lx RDX=%lx RSI=%lx RDI=%lx RBP=%lx\n"
                "R8=%lx R9=%lx R10=%lx R11=%lx R12=%lx R13=%lx R14=%lx R15=%lx\n",
                (unsigned long)f->rax, (unsigned long)f->rbx, (unsigned long)f->rcx,
                (unsigned long)f->rdx, (unsigned long)f->rsi, (unsigned long)f->rdi,
                (unsigned long)f->rbp, (unsigned long)f->r8, (unsigned long)f->r9,
                (unsigned long)f->r10, (unsigned long)f->r11, (unsigned long)f->r12,
                (unsigned long)f->r13, (unsigned long)f->r14, (unsigned long)f->r15);
        if (f->rip < 0x00007FFFFFFFF000ULL) {   /* RIP page was mapped to execute */ 
            kprintf("code @RIP:");
            for (int i = 0; i < 16; i++)
                kprintf(" %02x", ((const uint8_t *)f->rip)[i]);
            kprintf("\n");
        }
        /* A few words from the top of the user stack (never crossing a page): return addresses
         * to match against the program's and libraries' symbols. */
        if (f->rsp >= 0x1000 && f->rsp < 0x00007FFFFFFFF000ULL - 64 && (f->rsp & 0xFFF) <= 0xFB8) {
            kprintf("stack @RSP:");
            for (int i = 0; i < 8; i++)
                kprintf(" %lx", (unsigned long)((const uint64_t *)f->rsp)[i]);
            kprintf("\n");
        }
        proc_exit_current(-signal);
    }

    dump_and_panic(f);
}
