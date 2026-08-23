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

static void test_bad_arguments(void) {
    void *r = mmap(NULL, 4096, PROT_READ, MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED is refused, not silently ignored", r == MAP_FAILED);

    int fd = open("/hello.txt", O_RDONLY);
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
    test_bad_arguments();

    printf("[mmap_test] === %d failure(s) ===\n", failures);
    return failures;
}
