#include <kernel/proc/exec.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/elf.h>
#include <kernel/proc/sched.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/uvm.h>
#include <kernel/console.h>
#include <kernel/klibc/string.h>
#include <kernel/drivers/timer.h>
#include <arch/irq_spinlock.h>

/* Fixed per-proc user stack VA — same for every proc, safe since each
 * has its own independent TTBR0 (see kernel/arch/aarch64/proc.c's
 * proc_init(), which this calls).
 *
 * This used to start at 0x500000, only 1 MiB above the required ELF base.
 * That fit the small test programs but collided with mlibc DOOM: its second
 * PT_LOAD ends at 0x557018. Keep generous image-growth room while remaining
 * far below the userspace heap at 0x10000000. */
#define USER_STACK_VA 0x1000000

/* Fixed load address for a PT_INTERP interpreter (a dynamic linker),
 * loaded via parse_and_load_binary()'s bias parameter rather than at
 * its own p_vaddr the way the main ET_EXEC image is — an interpreter
 * is normally ET_DYN, so its p_vaddr values are file-relative, not
 * real addresses. Comfortably clear of the main image's own growth
 * room (DOOM's second PT_LOAD already reaches 0x557018; see
 * USER_STACK_VA's comment above) and far below the stack at
 * 0x1000000 — this project doesn't do dynamic/ASLR-style placement
 * anywhere yet, everything lives at a fixed, well-known address, and
 * the interpreter is no different. */
#define INTERP_BASE 0x900000

/* One page was enough while every payload was hand-written assembly with
 * no call depth and no locals. C code blows through that immediately —
 * a single printf frame with a format buffer can approach it — and there
 * is no guard page, so overflow silently corrupts whatever sits below
 * rather than faulting. 128KB is cheap per process and leaves room for
 * the DOOM port's call depth. */
#define USER_STACK_PAGES 32
#define USER_STACK_TOP   (USER_STACK_VA + USER_STACK_PAGES * PAGE_SIZE)

/* AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY, AT_UID,
 * AT_EUID, AT_GID, AT_EGID, AT_RANDOM, AT_EXECFN, plus AT_NULL — see
 * build_arg_stack()'s auxv block below. Each entry is a (type, value)
 * pair of two uint64_t words. */
#define EXEC_AUXV_ENTRIES 13
#define EXEC_PATH_BYTES 128
#define EXEC_RANDOM_BYTES 16

/* The whole argument block has to fit in the single page it is written
 * into, or build_arg_stack() would run off the bottom of that page and
 * corrupt the stack page below it. Sized so it cannot: worst case is
 * every argument present, every byte used, plus argc's own word, the
 * argv pointer array, argv's NULL terminator, envp's (empty) NULL
 * terminator, the auxv block, plus 16 bytes of alignment slack. */
_Static_assert(EXEC_ARG_BYTES + EXEC_ENV_BYTES
               + EXEC_PATH_BYTES
               + EXEC_RANDOM_BYTES
               + (EXEC_MAX_ARGS + EXEC_MAX_ENVS + 3) * sizeof(uint64_t)
               + EXEC_AUXV_ENTRIES * 2 * sizeof(uint64_t) + 16
               < PAGE_SIZE, "exec argument block must fit one stack page");

static spinlock_t exec_random_lock = SPINLOCK_INIT;
static uint64_t exec_random_sequence;

/* SplitMix64 is used here as a mixer, not claimed as a CSPRNG. Extron does
 * not have a hardware-entropy driver yet. Combining the high-resolution ARM
 * counter with allocation addresses and a serialized per-exec sequence gives
 * each new image different AT_RANDOM bytes and is sufficient to exercise the
 * ELF ABI and seed mlibc's stack guard during bring-up. Security-grade
 * unpredictability still requires a real entropy source and kernel CSPRNG. */
static uint64_t exec_random_mix(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static void fill_exec_random(uint8_t bytes[EXEC_RANDOM_BYTES],
                             phys_addr_t stack_phys,
                             const struct elf_aux_info *aux) {
    irq_spin_lock(&exec_random_lock);
    uint64_t sequence = ++exec_random_sequence;
    irq_spin_unlock(&exec_random_lock);

    uint64_t seed = timer_uptime_ns()
                  ^ ((uint64_t)stack_phys << 17)
                  ^ ((uint64_t)aux->entry << 7)
                  ^ sequence;
    uint64_t words[2];
    words[0] = exec_random_mix(seed);
    words[1] = exec_random_mix(words[0] ^ timer_uptime_ns() ^ sequence);
    memcpy(bytes, words, sizeof(words));
}

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
                                   const char *exec_path,
                                   const char *const *args, int argc,
                                   const char *const *envp, int envc,
                                   const struct elf_aux_info *aux,
                                   virt_addr_t interp_base,
                                   uint32_t uid, uint32_t euid,
                                   uint32_t gid, uint32_t egid,
                                   virt_addr_t *out_argv_va) {
    uint8_t     *page    = (uint8_t *)phys_to_virt_hhdm(top_phys);
    virt_addr_t  page_va = USER_STACK_TOP - PAGE_SIZE;
    virt_addr_t  str_va[EXEC_MAX_ARGS];
    virt_addr_t  env_va[EXEC_MAX_ENVS];
    size_t       off = PAGE_SIZE;

    uint8_t random_bytes[EXEC_RANDOM_BYTES];
    fill_exec_random(random_bytes, top_phys, aux);
    off -= sizeof(random_bytes);
    memcpy(page + off, random_bytes, sizeof(random_bytes));
    virt_addr_t random_va = page_va + off;

    size_t exec_len = strlen(exec_path) + 1;
    off -= exec_len;
    memcpy(page + off, exec_path, exec_len);
    virt_addr_t execfn_va = page_va + off;

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
    off -= (size_t)EXEC_AUXV_ENTRIES * 2 * sizeof(uint64_t);
    off &= ~(size_t)15;             /* AAPCS64: sp must be 16-byte aligned */

    uint64_t *sp_words = (uint64_t *)(page + off);
    sp_words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        sp_words[1 + i] = str_va[i];
    sp_words[1 + argc] = 0;

    for (int i = 0; i < envc; i++)
        sp_words[1 + argc + 1 + i] = env_va[i];
    sp_words[1 + argc + 1 + envc] = 0;

    /* Auxiliary vector — a SysV ELF stack isn't just argc/argv/envp;
     * this is the other half of what init_libc()'s constructor (see
     * this function's own header comment above) reads unconditionally.
     * Nothing currently parses past envp's NULL (mlibc's generic
     * parse_exec_stack() stops there for a static binary), so this is
     * still forward-looking for now — a future dynamic linker built
     * from mlibc's own vendored rtld needs exactly this to bootstrap
     * itself instead of re-parsing its own ELF header.
     *
     * AT_PHDR/AT_PHENT/AT_PHNUM describe the MAIN PROGRAM's phdrs, not
     * the interpreter's, even when one is loaded — `aux` is always
     * exec_image_build()'s first parse_and_load_binary() call, never
     * the interpreter's own. That's deliberate and matches real
     * ld.so/kernel behavior: the whole point of AT_PHDR is letting the
     * interpreter find the *main program's* PT_DYNAMIC segment (its
     * needed-library list, symbol tables) directly in memory without
     * re-reading that file from disk. The interpreter finds its own
     * phdrs a different way (a self-relocation trick every real ld.so
     * already does at startup, using its own program counter — nothing
     * this kernel needs to supply). `interp_base` is INTERP_BASE when
     * exec_image_build() loaded a PT_INTERP interpreter for this
     * program, 0 otherwise; that's what AT_BASE reports. */
    size_t auxv_base = 1 + argc + 1 + envc + 1;
    size_t a = 0;
    #define AUXV(t, v) do { \
        sp_words[auxv_base + 2 * a]     = (t); \
        sp_words[auxv_base + 2 * a + 1] = (uint64_t)(v); \
        a++; \
    } while (0)
    AUXV(AT_PHDR,   aux->phdr_va);
    AUXV(AT_PHENT,  aux->phentsize);
    AUXV(AT_PHNUM,  aux->phnum);
    AUXV(AT_PAGESZ, PAGE_SIZE);
    AUXV(AT_BASE,   interp_base);
    AUXV(AT_ENTRY,  aux->entry);
    AUXV(AT_UID,    uid);
    AUXV(AT_EUID,   euid);
    AUXV(AT_GID,    gid);
    AUXV(AT_EGID,   egid);
    AUXV(AT_RANDOM, random_va);
    AUXV(AT_EXECFN, execfn_va);
    AUXV(AT_NULL,   0);
    #undef AUXV

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
    if (strlen(binary_path) + 1 > EXEC_PATH_BYTES) {
        kprintf("[EXEC] executable path is too long for initial stack\n");
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

    struct elf_aux_info aux;
    
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)binary;
    uint64_t main_bias = 0;
    if (binary_size >= sizeof(Elf64_Ehdr) && 
        ehdr->e_ident[EI_MAG0] == ELFMAG0 && 
        ehdr->e_type == ET_DYN) {
        main_bias = ELF_USER_EXPECTED_BASE;
    }
    
    int load_result = parse_and_load_binary((virt_addr_t)binary, binary_size,
                                            ttbr0, main_bias, &aux, mm);
    kfree(binary);
    if (load_result != 0) {
        kprintf("[EXEC] ELF load failed for %s\n", binary_path);
        goto fail;
    }

    /* PT_INTERP: don't jump into the main image directly — load the
     * named interpreter too (at INTERP_BASE, since it's normally
     * ET_DYN) and start there instead. AT_ENTRY (in `aux`, already
     * populated above) keeps pointing at the real program's own entry
     * point regardless, exactly what the interpreter needs once it's
     * done bootstrapping itself to jump into the program it was
     * actually asked to run. */
    virt_addr_t real_start = aux.entry;
    virt_addr_t interp_base = 0;
    if (aux.has_interp) {
        size_t interp_size;
        void *interp_bin = load_binary_bytes(requester, aux.interp_path, &interp_size);
        if (!interp_bin) {
            kprintf("[EXEC] interpreter '%s' not found for %s\n",
                    aux.interp_path, binary_path);
            goto fail;
        }
        struct elf_aux_info interp_aux;
        int interp_result = parse_and_load_binary((virt_addr_t)interp_bin, interp_size,
                                                   ttbr0, INTERP_BASE, &interp_aux, mm);
        kfree(interp_bin);
        if (interp_result != 0) {
            kprintf("[EXEC] interpreter '%s' failed to load for %s\n",
                    aux.interp_path, binary_path);
            goto fail;
        }
        real_start  = interp_aux.entry;
        interp_base = INTERP_BASE;
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

    /* Same "NULL means kernel-initiated boot spawn, resolves as root"
     * convention load_binary_bytes() above already uses. execve()
     * doesn't process setuid/setgid bits (out of scope here), so the
     * new image simply reports whatever identity was already current —
     * exactly what a real kernel reports too when nothing changes it. */
    uint32_t aux_uid = 0, aux_euid = 0, aux_gid = 0, aux_egid = 0;
    if (requester) {
        irq_spin_lock(&requester->cred_lock);
        aux_uid  = requester->ruid;
        aux_euid = requester->euid;
        aux_gid  = requester->rgid;
        aux_egid = requester->egid;
        irq_spin_unlock(&requester->cred_lock);
    }

    out->entry   = real_start;
    out->user_sp = build_arg_stack(top_phys, binary_path, args, argc, envp, envc, &aux,
                                   interp_base,
                                   aux_uid, aux_euid, aux_gid, aux_egid,
                                   &out->argv);
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
