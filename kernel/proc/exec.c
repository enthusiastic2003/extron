#include <kernel/proc/exec.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/proc/sched.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/uvm.h>
#include <kernel/console.h>
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

/* The whole argument block has to fit in the single page it is written
 * into, or build_arg_stack() would run off the bottom of that page and
 * corrupt the stack page below it. Sized so it cannot: worst case is
 * every argument present, every byte used, plus argc's own word, the
 * argv pointer array, argv's NULL terminator, envp's (empty) NULL
 * terminator, plus 16 bytes of alignment slack. */
_Static_assert(EXEC_ARG_BYTES + EXEC_ENV_BYTES + (EXEC_MAX_ARGS + EXEC_MAX_ENVS + 3) * sizeof(uint64_t) + 16
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
                                   const char *const *envp, int envc,
                                   virt_addr_t *out_argv_va) {
    uint8_t     *page    = (uint8_t *)phys_to_virt_hhdm(top_phys);
    virt_addr_t  page_va = USER_STACK_TOP - PAGE_SIZE;
    virt_addr_t  str_va[EXEC_MAX_ARGS];
    virt_addr_t  env_va[EXEC_MAX_ENVS];
    size_t       off = PAGE_SIZE;

    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        off -= len;
        memcpy(page + off, envp[i], len);
        env_va[i] = page_va + off;
    }

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(args[i]) + 1;
        off -= len;
        memcpy(page + off, args[i], len);
        str_va[i] = page_va + off;
    }

    off &= ~(size_t)7;
    off -= (size_t)(argc + envc + 3) * sizeof(uint64_t);
    off &= ~(size_t)15;             /* AAPCS64: sp must be 16-byte aligned */

    uint64_t *sp_words = (uint64_t *)(page + off);
    sp_words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        sp_words[1 + i] = str_va[i];
    sp_words[1 + argc] = 0;
    
    for (int i = 0; i < envc; i++)
        sp_words[1 + argc + 1 + i] = env_va[i];
    sp_words[1 + argc + 1 + envc] = 0;

    *out_argv_va = page_va + off + sizeof(uint64_t);
    return page_va + off;
}

/*
 * Reads the whole of `binary_path` through the VFS into a fresh kmalloc
 * buffer — not the initrd tar directly, so anything the ramfs namespace
 * can resolve (an initrd-seeded file, one created after boot, one
 * reached through a mount) is loadable. `requester` supplies the cwd and
 * credentials path resolution runs as; NULL means a kernel-initiated
 * boot spawn, which resolves from / as root.
 *
 * On success *out_size is the file's length and the return value is a
 * buffer the caller must kfree(); on failure returns NULL.
 */
static void *load_binary_bytes(struct proc *requester, const char *binary_path,
                               size_t *out_size) {
    struct vfs_cred cred = {0};
    struct vfs_path cwd = {0};
    if (requester) {
        proc_vfs_cred_snapshot(requester, &cred);
        proc_cwd_snapshot(requester, &cwd);
    } else if (vfs_root_path(&cwd) < 0) {
        return NULL;
    }

    struct vfs_node *node;
    struct vfs_path opened;
    int result = vfs_open(&cwd, binary_path, 0, 0, &cred, &node, &opened);
    vfs_path_release(&cwd);
    if (result < 0) {
        kprintf("[EXEC] %s not found (%d)\n", binary_path, result);
        return NULL;
    }
    if (node->type != VFS_NODE_REGULAR
            || vfs_check_access(node, &cred, VFS_ACCESS_EXEC) < 0) {
        kprintf("[EXEC] %s is not an executable regular file\n", binary_path);
        vfs_node_release(node);
        vfs_path_release(&opened);
        return NULL;
    }

    struct vfs_attr attr;
    if (vfs_getattr(node, &attr) < 0) {
        vfs_node_release(node);
        vfs_path_release(&opened);
        return NULL;
    }
    void *buffer = attr.size ? kmalloc(attr.size) : NULL;
    if (attr.size && !buffer) {
        kprintf("[EXEC] out of memory reading %s\n", binary_path);
        vfs_node_release(node);
        vfs_path_release(&opened);
        return NULL;
    }
    long read = attr.size ? vfs_read(node, 0, buffer, attr.size) : 0;
    vfs_node_release(node);
    vfs_path_release(&opened);
    if (read < 0 || (size_t)read != attr.size) {
        kprintf("[EXEC] short read loading %s\n", binary_path);
        if (buffer) kfree(buffer);
        return NULL;
    }
    *out_size = attr.size;
    return buffer;
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
static int exec_image_build(struct proc *requester, const char *binary_path,
                            const char *const *args, int argc,
                                   const char *const *envp, int envc,
                            struct exec_image *out) {
    size_t binary_size;
    void *binary = load_binary_bytes(requester, binary_path, &binary_size);
    if (!binary)
        return -1;
    if (argc < 1 || argc > EXEC_MAX_ARGS) {
        kfree(binary);
        return -1;
    }

    out->mm = NULL;

    phys_addr_t ttbr0 = create_user_pml4();
    if (!ttbr0) {
        kprintf("[EXEC] out of memory creating page table for %s\n", binary_path);
        kfree(binary);
        return -1;
    }

    struct vm_space *mm = vm_space_create(ttbr0);
    if (!mm) {
        kprintf("[EXEC] out of memory allocating vm_space for %s\n", binary_path);
        free_user_page_tables(ttbr0);
        kfree(binary);
        return -1;
    }
    out->mm    = mm;
    out->ttbr0 = ttbr0;

    int load_result = parse_and_load_binary((virt_addr_t)binary, binary_size,
                                            ttbr0, &out->entry, mm);
    kfree(binary);
    if (load_result != 0) {
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

    out->user_sp = build_arg_stack(top_phys, args, argc, envp, envc, &out->argv);
    out->argc    = (uint64_t)argc;

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

struct proc *proc_create_from_binary_argv(const char *binary_path,
                                          const char *const *args, int argc) {
    struct exec_image img;

    if (exec_image_build(NULL, binary_path, args, argc, NULL, 0, &img) != 0)
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

struct proc *proc_create_from_binary(const char *binary_path) {
    const char *args[1] = { binary_path };
    return proc_create_from_binary_argv(binary_path, args, 1);
}

int proc_exec_replace(struct proc *p, const char *binary_path,
                      const char *const *args, int argc,
                                   const char *const *envp, int envc,
                      struct exec_image *out) {
    struct thread *t = my_thread();
    if (!p || !t || t->process != p)
        return -1;
    if (exec_image_build(p, binary_path, args, argc, envp, envc, out) != 0)
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
