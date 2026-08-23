#include <kernel/proc/exec.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/proc/sched.h>
#include <kernel/fs/tar.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/uvm.h>
#include <kernel/console.h>
#include <kernel/drivers/fb.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/klibc/string.h>

/* Fixed per-proc user stack VA — same for every proc, safe since each
 * has its own independent TTBR0 (see kernel/arch/aarch64/proc.c's
 * proc_init(), which this calls).
 *
 * This used to start at 0x500000, only 1 MiB above the required ELF base.
 * That fit the small test programs but collided with mlibc DOOM: its second
 * PT_LOAD ends at 0x557018. Keep generous image-growth room while remaining
 * far below the userspace heap at 0x10000000. */
#define USER_STACK_VA 0x1000000

/* One page was enough while every payload was hand-written assembly with
 * no call depth and no locals. C code blows through that immediately —
 * a single printf frame with a format buffer can approach it — and there
 * is no guard page, so overflow silently corrupts whatever sits below
 * rather than faulting. 128KB is cheap per process and leaves room for
 * the DOOM port's call depth. */
#define USER_STACK_PAGES 32
#define USER_STACK_TOP   (USER_STACK_VA + USER_STACK_PAGES * PAGE_SIZE)

/* Where a PROC_MAP_FRAMEBUFFER process finds the display.
 *
 * A fixed VA plus a descriptor page, rather than a syscall that returns
 * them. The process needs six numbers it cannot compute — base, width,
 * height, pitch, depth and byte order — and every one is the firmware's
 * answer, not something userspace may assume. In particular pitch is not
 * width*4 in general, so hardcoding the geometry would reintroduce the
 * exact bug fb.c reads it back to avoid.
 *
 * The descriptor is mapped read-only: it describes the process's own
 * address space, and nothing good comes of the process editing it. */
#define USER_FB_INFO_VA 0x50000000UL
#define USER_FB_VA      0x50001000UL

/* The shared keystroke ring (kernel/drivers/keyboard.c), mapped
 * alongside the framebuffer for the same reason: a game loop has to
 * poll input without blocking, and SYS_READ blocks by design. Writable
 * because the consumer owns the tail index — see struct kbd_ring on why
 * that needs no lock and why a process can only hurt itself with it. */
#define USER_INPUT_VA   0x4FFFF000UL

/* Must match struct extron_fb_info in usr/include/extron/fb.h. Written
 * out twice deliberately — this is the ABI boundary, the same reason the
 * syscall numbers are duplicated rather than shared through a header. */
struct user_fb_info {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t depth;
    uint32_t rgb_order;
    uint32_t size;
};

/* Maps the framebuffer and its descriptor into `mm`. Returns 0 on
 * success, -1 if there is no framebuffer or a mapping fails.
 *
 * Every mapping made here is also registered with vm_insert_region().
 * The framebuffer and the input ring are registered NOT-owned: they are
 * VideoCore memory and a kernel-side ring respectively, so fork() must
 * share them rather than copy, and teardown must unmap without handing
 * either back to the PMM. The descriptor page IS owned — it's a page
 * allocated right here, per process. */
static int map_framebuffer_into(struct vm_space *mm, const char *binary_path) {
    const struct framebuffer *fb = fb_get();
    if (!fb) {
        kprintf("[EXEC] %s wants the framebuffer but none is initialised\n",
                binary_path);
        return -1;
    }

    /* The descriptor is a fresh page the kernel fills in; the framebuffer
     * itself is VideoCore memory that already exists, so it is mapped,
     * never allocated. */
    phys_addr_t info_phys = (phys_addr_t)pmm_alloc_page();
    if (!info_phys) {
        kprintf("[EXEC] out of memory allocating fb descriptor for %s\n",
                binary_path);
        return -1;
    }
    struct user_fb_info *info =
        (struct user_fb_info *)phys_to_virt_hhdm(info_phys);
    memset(info, 0, PAGE_SIZE);
    info->base      = USER_FB_VA;
    info->width     = fb->width;
    info->height    = fb->height;
    info->pitch     = fb->pitch;
    info->depth     = fb->depth;
    info->rgb_order = fb->rgb_order;
    info->size      = fb->size;

    if (map_page(mm->ttbr0, USER_FB_INFO_VA, info_phys,
                 PAGE_PRESENT | PAGE_USER | PAGE_NX) != 0) {
        pmm_free_page((void *)info_phys);
        return -1;
    }
    if (vm_insert_region(mm, USER_FB_INFO_VA, PAGE_SIZE, true) != 0) {
        /* Unrecorded, so teardown would not find it — hand it back here
         * rather than leaking a page on the way out of a failure. */
        unmap_page(mm->ttbr0, USER_FB_INFO_VA);
        pmm_free_page((void *)info_phys);
        return -1;
    }

    phys_addr_t ring = kbd_ring_phys();
    if (ring) {
        if (map_page(mm->ttbr0, USER_INPUT_VA, ring,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX) != 0) {
            kprintf("[EXEC] failed mapping the input ring for %s\n", binary_path);
            return -1;
        }
        if (vm_insert_region(mm, USER_INPUT_VA, PAGE_SIZE, false) != 0)
            return -1;
    }

    /* PAGE_NORMAL_NC for the same reason the kernel's own mapping uses
     * it (kernel/drivers/fb.c): the GPU scans this memory out
     * continuously so it must not sit dirty in a cache, but Device
     * memory would forbid the unaligned accesses a memcpy emits — and
     * DG_DrawFrame is precisely a memcpy. */
    for (uint32_t off = 0; off < fb->size; off += PAGE_SIZE) {
        if (map_page(mm->ttbr0, USER_FB_VA + off, fb->phys + off,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX
                     | PAGE_NORMAL_NC) != 0) {
            kprintf("[EXEC] failed mapping framebuffer for %s\n", binary_path);
            return -1;
        }
    }
    if (vm_insert_region(mm, USER_FB_VA, fb->size, false) != 0)
        return -1;

    return 0;
}

/* The whole argument block has to fit in the single page it is written
 * into, or build_arg_stack() would run off the bottom of that page and
 * corrupt the stack page below it. Sized so it cannot: worst case is
 * every argument present, every byte used, plus argc's own word, the
 * argv pointer array, argv's NULL terminator, envp's (empty) NULL
 * terminator, plus 16 bytes of alignment slack. */
_Static_assert(EXEC_ARG_BYTES + (EXEC_MAX_ARGS + 3) * sizeof(uint64_t) + 16
               < PAGE_SIZE, "exec argument block must fit one stack page");

/*
 * Lay out a REAL argc/argv/envp stack — the actual AArch64/SysV ELF
 * entry-point ABI (argc at [sp], argv[] after it, a NULL, then envp[]
 * — empty here, so just one more NULL), not the simplified
 * "argc/argv in registers only" shape this kernel used until now.
 *
 * That simplification was fine for usr/lib/crt0.S, which never reads
 * sp for anything — it takes argc/argv straight from x0/x1 (still set
 * up the same way below, untouched). It silently was NOT fine for
 * mlibc: __dlapi_enter()'s bootstrap (sysdeps/extron/generic/
 * generic.cpp's __mlibc_start_main()) runs a real
 * [[gnu::constructor]] — options/elf/generic/startup.cpp's
 * init_libc() — completely automatically, as part of running this
 * executable's own .init_array, and that constructor unconditionally
 * parses whatever pointer __dlapi_enter() was given as a genuine
 * SysV stack. Handing it anything else (a bare argv array, a null,
 * anything not shaped exactly like this) is read as valid argc/argv/
 * envp and corrupts mlibc's own startup state before main() ever
 * runs. There is no way to opt out of that constructor short of
 * patching mlibc's own options/elf code — building the real shape
 * once, here, is far less invasive than that would be.
 *
 * The strings and this whole block live ABOVE the returned stack
 * pointer, so the first thing the process does — establishing its
 * own frame below sp — can't tread on them.
 *
 * `top_phys` is the physical page backing [USER_STACK_TOP-PAGE_SIZE,
 * USER_STACK_TOP). Writing through the HHDM is what lets this run
 * before the address space is ever installed — during execve the
 * process is still executing out of the image about to be replaced, so
 * the new stack simply isn't reachable by its own VA yet.
 */
static virt_addr_t build_arg_stack(phys_addr_t top_phys,
                                   const char *const *args, int argc,
                                   virt_addr_t *out_argv_va) {
    uint8_t     *page    = (uint8_t *)phys_to_virt_hhdm(top_phys);
    virt_addr_t  page_va = USER_STACK_TOP - PAGE_SIZE;
    virt_addr_t  str_va[EXEC_MAX_ARGS];
    size_t       off = PAGE_SIZE;

    /* Strings first, from the top down, so the pointer array below them
     * can be filled in with addresses that are already final. */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(args[i]) + 1;
        off -= len;
        memcpy(page + off, args[i], len);
        str_va[i] = page_va + off;
    }

    off &= ~(size_t)7;
    /* argc's own word, argc argv pointers, argv's NULL terminator,
     * envp's (empty) NULL terminator. */
    off -= (size_t)(argc + 3) * sizeof(uint64_t);
    off &= ~(size_t)15;             /* AAPCS64: sp must be 16-byte aligned */

    uint64_t *sp_words = (uint64_t *)(page + off);
    sp_words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        sp_words[1 + i] = str_va[i];
    sp_words[1 + argc]     = 0;     /* argv[argc] == NULL, as C requires */
    sp_words[1 + argc + 1] = 0;     /* envp[0] == NULL: no environment yet */

    /* x1 (our own crt0's argv register) points at the argv array
     * specifically — one word past argc — not at sp itself. */
    *out_argv_va = page_va + off + sizeof(uint64_t);
    return page_va + off;           /* sp: argc, then argv[], as mlibc's
                                      * init_libc() constructor expects */
}

/*
 * Build a complete, ready-to-run address space for `binary_path`.
 *
 * Everything a process needs and nothing that belongs to whoever asked:
 * the caller's own address space is never touched, so a failure here
 * costs the memory this released on its way out and nothing else. That
 * property is what makes execve() safe to attempt — the old image is
 * only discarded once there is definitely a new one to replace it with.
 */
static int exec_image_build(const char *binary_path, unsigned flags,
                            const char *const *args, int argc,
                            struct exec_image *out) {
    struct tar_file f;
    if (!tar_open(binary_path, &f)) {
        kprintf("[EXEC] %s not found in initrd\n", binary_path);
        return -1;
    }
    if (argc < 1 || argc > EXEC_MAX_ARGS)
        return -1;

    out->mm = NULL;

    phys_addr_t ttbr0 = create_user_pml4();
    if (!ttbr0) {
        kprintf("[EXEC] out of memory creating page table for %s\n", binary_path);
        return -1;
    }

    struct vm_space *mm = vm_space_create(ttbr0);
    if (!mm) {
        kprintf("[EXEC] out of memory allocating vm_space for %s\n", binary_path);
        free_user_page_tables(ttbr0);
        return -1;
    }
    out->mm    = mm;
    out->ttbr0 = ttbr0;

    if (parse_and_load_binary((virt_addr_t)f.data, f.size, ttbr0,
                              &out->entry, mm) != 0) {
        kprintf("[EXEC] ELF load failed for %s\n", binary_path);
        goto fail;
    }

    phys_addr_t top_phys = 0;
    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        phys_addr_t stack_phys = (phys_addr_t)pmm_alloc_page();
        if (!stack_phys) {
            kprintf("[EXEC] out of memory allocating stack for %s\n", binary_path);
            goto fail;
        }
        /* Zeroed for the same reason vm_allocate_region() zeroes: a
         * fresh stack must not hand the new process whatever the
         * previous owner of that frame left in it. */
        memset(phys_to_virt_hhdm(stack_phys), 0, PAGE_SIZE);
        if (map_page(ttbr0, USER_STACK_VA + i * PAGE_SIZE, stack_phys,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX) != 0) {
            kprintf("[EXEC] stack VA collision at %p while loading %s\n",
                    (void *)(USER_STACK_VA + i * PAGE_SIZE), binary_path);
            pmm_free_page((void *)stack_phys);
            goto fail;
        }
        if (i == USER_STACK_PAGES - 1)
            top_phys = stack_phys;
    }
    if (vm_insert_region(mm, USER_STACK_VA,
                         USER_STACK_PAGES * PAGE_SIZE, true) != 0)
        goto fail;

    out->user_sp = build_arg_stack(top_phys, args, argc, &out->argv);
    out->argc    = (uint64_t)argc;

    if (flags & PROC_MAP_FRAMEBUFFER) {
        if (map_framebuffer_into(mm, binary_path) != 0)
            goto fail;
    }

    /* No MMIO mapping here any more. The UART used to be identity-mapped
     * into every process purely so kernel kprintf()s would survive a
     * TTBR0 swap — uart.c now reaches it through the kernel's own
     * high-half Device mapping (serial_remap_to_hhdm(), called by
     * init_paging()), so a user table contains only what that process
     * actually owns. */
    return 0;

fail:
    /* vm_space_destroy() unwinds whatever got as far as being recorded —
     * segments, stack pages, the page tables themselves — and leaves
     * nothing behind for the caller to clean up. */
    vm_space_destroy(mm);
    out->mm = NULL;
    return -1;
}

struct proc *proc_create_from_binary_argv(const char *binary_path, unsigned flags,
                                          const char *const *args, int argc) {
    struct exec_image img;

    if (exec_image_build(binary_path, flags, args, argc, &img) != 0)
        return NULL;

    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p) {
        kprintf("[EXEC] out of memory allocating struct proc for %s\n", binary_path);
        vm_space_destroy(img.mm);
        return NULL;
    }

    /* Reserve identity first, initialize privately, then publish. Timer
     * wakeup scans must never observe a half-initialized thread list. */
    uint64_t pid = proc_alloc_pid();
    proc_init(p, pid, img.entry, img.user_sp, img.ttbr0);

    p->mm        = img.mm;
    p->user_argc = img.argc;
    p->user_argv = img.argv;
    proc_table_add(p);
    return p;
}

struct proc *proc_create_from_binary(const char *binary_path, unsigned flags) {
    const char *args[1] = { binary_path };
    return proc_create_from_binary_argv(binary_path, flags, args, 1);
}

int proc_exec_replace(struct proc *p, const char *binary_path,
                      const char *const *args, int argc,
                      struct exec_image *out) {
    struct thread *t = my_thread();
    if (!p || !t || t->process != p)
        return -1;
    if (exec_image_build(binary_path, 0, args, argc, out) != 0)
        return -1;

    struct vm_space *old = p->mm;

    /* Install first, tear down second. vm_space_destroy() frees the
     * page tables it walks, so running it against the address space
     * TTBR0_EL1 still points at would be freeing the tables the MMU is
     * using — and the PMM would then hand those frames out to somebody
     * else while a stale TLB entry still refers to them. */
    p->mm      = out->mm;
    p->ttbr0   = out->ttbr0;
    t->entry   = out->entry;
    t->user_sp = out->user_sp;
    p->user_argc = out->argc;
    p->user_argv = out->argv;

    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(p->ttbr0) : "memory");
    flush_tlb();

    vm_space_destroy(old);
    file_table_close_cloexec(p);
    return 0;
}
