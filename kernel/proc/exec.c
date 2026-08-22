#include <kernel/proc/exec.h>
#include <kernel/proc/elf_loader.h>
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
 * proc_init(), which this calls). One page for now; no argv/envp. */
#define USER_STACK_VA 0x500000

/* One page was enough while every payload was hand-written assembly with
 * no call depth and no locals. C code blows through that immediately —
 * a single printf frame with a format buffer can approach it — and there
 * is no guard page, so overflow silently corrupts whatever sits below
 * rather than faulting. 128KB is cheap per process and leaves room for
 * the DOOM port's call depth. */
#define USER_STACK_PAGES 32

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

/* Maps the framebuffer and its descriptor into `pml4`. Returns 0 on
 * success, -1 if there is no framebuffer or a mapping fails. */
static int map_framebuffer_into(phys_addr_t pml4, const char *binary_path) {
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

    if (map_page(pml4, USER_FB_INFO_VA, info_phys,
                 PAGE_PRESENT | PAGE_USER | PAGE_NX) != 0) {
        pmm_free_page((void *)info_phys);
        return -1;
    }

    /* PAGE_NORMAL_NC for the same reason the kernel's own mapping uses
     * it (kernel/drivers/fb.c): the GPU scans this memory out
     * continuously so it must not sit dirty in a cache, but Device
     * memory would forbid the unaligned accesses a memcpy emits — and
     * DG_DrawFrame is precisely a memcpy. */
    phys_addr_t ring = kbd_ring_phys();
    if (ring && map_page(pml4, USER_INPUT_VA, ring,
                         PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX) != 0) {
        kprintf("[EXEC] failed mapping the input ring for %s\n", binary_path);
        return -1;
    }

    for (uint32_t off = 0; off < fb->size; off += PAGE_SIZE) {
        if (map_page(pml4, USER_FB_VA + off, fb->phys + off,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX
                     | PAGE_NORMAL_NC) != 0) {
            kprintf("[EXEC] failed mapping framebuffer for %s\n", binary_path);
            return -1;
        }
    }
    return 0;
}

struct proc *proc_create_from_binary(const char *binary_path, unsigned flags) {
    struct tar_file f;
    if (!tar_open(binary_path, &f)) {
        kprintf("[EXEC] %s not found in initrd\n", binary_path);
        return NULL;
    }

    phys_addr_t pml4 = create_user_pml4();
    virt_addr_t entry;
    if (parse_and_load_binary((virt_addr_t)f.data, f.size, pml4, &entry) != 0) {
        kprintf("[EXEC] ELF load failed for %s\n", binary_path);
        return NULL;
    }

    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        phys_addr_t stack_phys = (phys_addr_t)pmm_alloc_page();
        if (!stack_phys) {
            kprintf("[EXEC] out of memory allocating stack for %s\n", binary_path);
            return NULL;
        }
        map_page(pml4, USER_STACK_VA + i * PAGE_SIZE, stack_phys,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
    }

    if (flags & PROC_MAP_FRAMEBUFFER) {
        if (map_framebuffer_into(pml4, binary_path) != 0) {
            return NULL;
        }
    }

    /* No MMIO mapping here any more. The UART used to be identity-mapped
     * into every process purely so kernel kprintf()s would survive a
     * TTBR0 swap — uart.c now reaches it through the kernel's own
     * high-half Device mapping (serial_remap_to_hhdm(), called by
     * init_paging()), so a user table contains only what that process
     * actually owns: its ELF segments and its stack. */

    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p) {
        kprintf("[EXEC] out of memory allocating struct proc for %s\n", binary_path);
        return NULL;
    }

    /* proc_table_add() needs `p` to already exist (it stores the
     * pointer) but assigns the pid before proc_init() fills the struct
     * in — proc_init() is what actually writes p->pid, so it must run
     * with the pid proc_table_add() hands back, not before. */
    uint64_t pid = proc_table_add(p);
    proc_init(p, pid, entry, USER_STACK_VA + USER_STACK_PAGES * PAGE_SIZE, pml4);

    p->mm = vm_space_create(pml4);
    if (!p->mm) {
        kprintf("[EXEC] out of memory allocating vm_space for %s\n", binary_path);
        proc_table_remove(p);
        kfree(p);
        return NULL;
    }

    return p;
}
