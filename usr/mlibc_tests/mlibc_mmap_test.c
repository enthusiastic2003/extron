/*
 * Real mmap()/munmap() (kernel/proc/syscall.c's sys_mmap()/sys_munmap()),
 * reachable through the actual mlibc entry points now instead of the
 * ENOSYS stub sys_vm_map() used to be. Two backings, both going through
 * the exact same syscall:
 *
 *  - MAP_ANONYMOUS: vm_allocate_region() picks a free VA and hands back
 *    fresh, zeroed pages — the same call SYS_ANON_ALLOC already made for
 *    malloc, just reachable through mmap() now too.
 *  - /dev/fb0 (kernel/fs/devfs.c): vfs_node_ops.mmap hands back the
 *    framebuffer's OWN existing physical memory — proving a real device
 *    can be mapped, not just fresh anonymous pages, and that unmapping
 *    it doesn't try to free VideoCore memory back to the PMM.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

/* usr/include/extron/fb.h isn't reachable from here — it's included via
 * a -I flag DOOM's own Makefile rule passes, not through the mlibc
 * sysroot every usr/mlibc_tests/ binary actually builds against (same
 * reason mlibc_mem_stress.c/mlibc_fork_stress.c duplicate raw syscall
 * numbers locally rather than including a kernel-side header). Must
 * match struct extron_fb_geometry there byte for byte. */
#define EXTRON_FB_PATH "/dev/fb0"
struct extron_fb_geometry {
    uint32_t width, height, pitch, depth, rgb_order, size;
};

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[mmap_test] %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void test_anonymous(void) {
    size_t length = 3 * 4096; /* multiple pages, not just one */
    void *region = mmap(NULL, length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap(MAP_ANONYMOUS) succeeds", region != MAP_FAILED);
    if (region == MAP_FAILED) return;

    unsigned char *bytes = region;
    int zeroed = 1;
    for (size_t i = 0; i < length; i++)
        if (bytes[i] != 0) zeroed = 0;
    check("fresh anonymous mapping is zeroed", zeroed);

    memset(bytes, 0xA5, length);
    int all_set = 1;
    for (size_t i = 0; i < length; i++)
        if (bytes[i] != 0xA5) all_set = 0;
    check("writes to an anonymous mapping are visible immediately", all_set);

    check("munmap() succeeds", munmap(region, length) == 0);
}

static void test_framebuffer_device(void) {
    int fd = open(EXTRON_FB_PATH, O_RDWR);
    check("open /dev/fb0", fd >= 0);
    if (fd < 0) return;

    struct extron_fb_geometry geo;
    long got = read(fd, &geo, sizeof(geo));
    check("read() returns the geometry struct whole",
          got == (long)sizeof(geo));
    check("reported geometry is non-degenerate",
          geo.width > 0 && geo.height > 0 && geo.pitch > 0 && geo.size > 0);

    void *fb = mmap(NULL, geo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap() /dev/fb0 succeeds", fb != MAP_FAILED);
    close(fd); /* the mapping must outlive the fd, same as any other mmap() */

    if (fb != MAP_FAILED) {
        volatile unsigned char *pixels = fb;
        unsigned char before = pixels[0];
        pixels[0] = (unsigned char)(before ^ 0xFF);
        unsigned char after = pixels[0];
        pixels[0] = before; /* leave the display as we found it */
        check("a write through the mapping reads back immediately",
              after == (unsigned char)(before ^ 0xFF));

        check("munmap() of the framebuffer succeeds",
              munmap(fb, geo.size) == 0);
    }
}

/*
 * MAP_FIXED (kernel/mm/uvm.c's vm_allocate_region_at()/vm_map_region_at(),
 * dynamic-linking groundwork stage 2 — stage 1 was the auxiliary
 * vector). Placement at a caller-chosen address is real now, but real
 * MAP_FIXED's "silently discard whatever already overlaps" behavior
 * isn't: this version requires the target range to already be
 * completely free, and fails instead of clobbering something a
 * caller might not have expected to lose.
 */
static void test_map_fixed(void) {
    /* Address 0 (NULL) sits below this allocator's window
     * ([0x10000000, 0x20000000) today) no matter what — MAP_FIXED
     * there fails for being out of range, not because MAP_FIXED
     * itself is unsupported. */
    void *out_of_range = mmap(NULL, 4096, PROT_READ,
                              MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED at an out-of-range address fails", out_of_range == MAP_FAILED);

    void *misaligned = mmap((void *)0x10001001, 4096, PROT_READ,
                            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED with a misaligned hint fails", misaligned == MAP_FAILED);

    /* Probe for a real, currently-free address the ordinary way first
     * — mirroring how a dynamic linker actually uses MAP_FIXED: reserve
     * space with a plain mmap() to learn where it's free, then place
     * things precisely inside it. */
    size_t length = 3 * 4096;
    void *probe = mmap(NULL, length, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("probe mmap() to find a real free address succeeds", probe != MAP_FAILED);
    if (probe == MAP_FAILED) return;
    check("munmap() the probe so its address is free again",
          munmap(probe, length) == 0);

    void *fixed = mmap(probe, 4096, PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED at a genuinely free address succeeds", fixed != MAP_FAILED);
    check("MAP_FIXED returns exactly the requested address", fixed == probe);
    if (fixed == MAP_FAILED) return;

    unsigned char *bytes = fixed;
    int zeroed = 1;
    for (size_t i = 0; i < 4096; i++)
        if (bytes[i] != 0) zeroed = 0;
    check("MAP_FIXED anonymous mapping is zeroed", zeroed);

    bytes[0] = 0x5A;
    check("a write through a MAP_FIXED mapping reads back", bytes[0] == 0x5A);

    /* A second MAP_FIXED request landing on the live mapping above
     * must fail rather than silently clobber it — see this function's
     * own header comment. */
    void *overlap = mmap(fixed, 4096, PROT_READ,
                         MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED onto an already-mapped range fails (no clobber support yet)",
          overlap == MAP_FAILED);

    check("munmap() of the MAP_FIXED region succeeds", munmap(fixed, 4096) == 0);
}

static void test_bad_arguments(void) {
    int fd = open("/opt/tests/hello.txt", O_RDONLY);
    check("open a plain ramfs file for the ENODEV check", fd >= 0);
    void *r2 = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap() on a file with no mmap op fails, doesn't corrupt state",
          r2 == MAP_FAILED);
    close(fd);
}

int main(void) {
    printf("\n[mmap_test] === real mmap()/munmap(), two backings ===\n");

    test_anonymous();
    test_framebuffer_device();
    test_map_fixed();
    test_bad_arguments();

    printf("[mmap_test] === %d failure(s) ===\n", failures);
    return failures;
}
