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
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
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

static void test_anonymous_shared(void) {
    unsigned char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("anonymous MAP_SHARED succeeds", shared != MAP_FAILED);
    if (shared == MAP_FAILED) return;
    shared[0] = 0x11;
    pid_t child = fork();
    check("fork() with anonymous shared memory succeeds", child >= 0);
    if (child == 0) {
        shared[0] = 0x7B;
        _exit(0);
    }
    if (child > 0) {
        int status = 0;
        check("waitpid() for shared-memory child succeeds",
              waitpid(child, &status, 0) == child && WIFEXITED(status));
        check("parent observes child's MAP_SHARED write", shared[0] == 0x7B);
    }
    check("munmap() anonymous shared memory succeeds",
          munmap(shared, 4096) == 0);
}

static void test_mprotect(void) {
    unsigned char *pages = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap() for mprotect test succeeds", pages != MAP_FAILED);
    if (pages == MAP_FAILED) return;
    pages[0] = 1; pages[4096] = 2; pages[8192] = 3;
    check("mprotect() can protect only the middle page",
          mprotect(pages + 4096, 4096, PROT_NONE) == 0);
    check("mprotect split leaves neighboring pages accessible",
          pages[0] == 1 && pages[8192] == 3);

    pid_t child = fork();
    if (child == 0) {
        volatile unsigned char value = pages[4096];
        (void)value;
        _exit(99);
    }
    int status = 0;
    check("PROT_NONE faults only the child process",
          child > 0 && waitpid(child, &status, 0) == child
          && WIFSIGNALED(status) && WTERMSIG(status) == 11);
    check("mprotect() restores middle-page read/write access",
          mprotect(pages + 4096, 4096, PROT_READ | PROT_WRITE) == 0);
    pages[4096] = 0x44;
    check("restored middle page is writable", pages[4096] == 0x44);

    errno = 0;
    check("mprotect() rejects an unmapped range with ENOMEM",
          mprotect((void *)0x1f000000, 4096, PROT_READ) == -1
          && errno == ENOMEM);
    errno = 0;
    check("mprotect() rejects unknown protection bits",
          mprotect(pages, 4096, 0x4000) == -1 && errno == EINVAL);
    check("munmap() mprotect test region succeeds",
          munmap(pages, 3 * 4096) == 0);
}

static void test_file_shared(void) {
    const char *path = "/mmap-shared-test.tmp";
    unsigned char initial[4096];
    memset(initial, 0, sizeof(initial));
    memcpy(initial, "before", 7);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    check("create writable file for MAP_SHARED", fd >= 0);
    if (fd < 0) return;
    check("seed MAP_SHARED test file", write(fd, initial, sizeof(initial)) == 4096);

    unsigned char *first = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
    unsigned char *second = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
    check("two file MAP_SHARED mappings succeed",
          first != MAP_FAILED && second != MAP_FAILED);
    if (first == MAP_FAILED || second == MAP_FAILED) {
        close(fd);
        if (first != MAP_FAILED) munmap(first, 4096);
        if (second != MAP_FAILED) munmap(second, 4096);
        unlink(path);
        return;
    }
    memcpy(first, "shared", 7);
    check("second mapping immediately observes first mapping",
          memcmp(second, "shared", 7) == 0);
    char through_read[8] = {0};
    check("seek fd while shared mappings are active", lseek(fd, 0, SEEK_SET) == 0);
    check("ordinary read() observes a shared mapping write",
          read(fd, through_read, 7) == 7 && !memcmp(through_read, "shared", 7));
    check("seek fd for ordinary write() coherence", lseek(fd, 32, SEEK_SET) == 32);
    check("ordinary write() succeeds while mappings are active",
          write(fd, "ordinary", 8) == 8);
    check("shared mapping observes ordinary write() immediately",
          !memcmp(second + 32, "ordinary", 8));

    pid_t child = fork();
    if (child == 0) {
        memcpy(second + 64, "child", 6);
        _exit(0);
    }
    int status = 0;
    check("file-shared child exits normally",
          child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status));
    check("parent observes child's file MAP_SHARED write",
          !memcmp(first + 64, "child", 6));
    close(fd);
    check("msync(MS_SYNC) writes shared pages through VFS",
          msync(first, 4096, MS_SYNC) == 0);
    check("shared mappings outlive their original fd",
          memcmp(second, "shared", 7) == 0);
    check("unmap both shared views", munmap(first, 4096) == 0
          && munmap(second, 4096) == 0);

    fd = open(path, O_RDONLY);
    char persisted[8] = {0};
    check("reopen MAP_SHARED test file", fd >= 0);
    if (fd >= 0) {
        check("msync data persists through ordinary read()",
              read(fd, persisted, 7) == 7 && !memcmp(persisted, "shared", 7));
        unsigned char *readonly = mmap(NULL, 4096, PROT_READ,
                                       MAP_SHARED, fd, 0);
        check("read-only MAP_SHARED mapping succeeds", readonly != MAP_FAILED);
        if (readonly != MAP_FAILED) {
            errno = 0;
            check("mprotect cannot add shared write access to O_RDONLY fd",
                  mprotect(readonly, 4096, PROT_READ | PROT_WRITE) == -1
                  && errno == EACCES);
            munmap(readonly, 4096);
        }
        close(fd);
    }

    fd = open(path, O_RDWR);
    unsigned char *private = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE, fd, 0);
    check("file MAP_PRIVATE mapping succeeds", private != MAP_FAILED);
    if (private != MAP_FAILED) {
        memcpy(private, "private", 8);
        check("MAP_PRIVATE change is visible in its own mapping",
              !memcmp(private, "private", 8));
        check("msync on MAP_PRIVATE is harmless", msync(private, 4096, MS_SYNC) == 0);
        munmap(private, 4096);
        lseek(fd, 0, SEEK_SET);
        memset(persisted, 0, sizeof(persisted));
        check("MAP_PRIVATE change was not written to the file",
              read(fd, persisted, 7) == 7 && !memcmp(persisted, "shared", 7));
    }
    if (fd >= 0) close(fd);
    unlink(path);
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
 * vector). Placement at a caller-chosen address is real, and so now is
 * real MAP_FIXED's "silently discard whatever already overlaps"
 * behavior: carve_range_locked() (kernel/mm/uvm.c) trims or splits
 * whatever VMA(s) the target range overlaps rather than requiring the
 * range to already be free. vm_free_region() (munmap()'s backend)
 * shares that same primitive, so an ordinary munmap() of an arbitrary
 * sub-range works now too, not just a whole VMA at its exact base.
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

    /* A second MAP_FIXED request landing exactly on the live mapping
     * above must now succeed and discard it — real Linux semantics,
     * see this function's own header comment. A fresh, zeroed page
     * where the 0x5A byte used to be is the actual proof: a bug that
     * left the request "succeeding" without really replacing the old
     * mapping (e.g. just returning the same address without doing
     * anything) would still read back 0x5A. */
    void *overlap = mmap(fixed, 4096, PROT_READ | PROT_WRITE,
                         MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED onto an already-mapped range now succeeds",
          overlap == fixed);
    check("...and the old mapping's contents are really gone",
          overlap != MAP_FAILED && ((unsigned char *)overlap)[0] == 0);

    check("munmap() of the MAP_FIXED region succeeds", munmap(fixed, 4096) == 0);

    /* The harder case: MAP_FIXED landing in the MIDDLE of a larger live
     * VMA, which carve_range_locked() must split into a surviving head
     * and a surviving tail rather than just deleting the whole thing.
     * Three untouched pages, each given a distinct sentinel byte, then
     * MAP_FIXED replaces only the middle one. */
    size_t triple_len = 3 * 4096;
    void *triple = mmap(NULL, triple_len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap() a 3-page region to test splitting", triple != MAP_FAILED);
    if (triple == MAP_FAILED) return;

    unsigned char *pages = triple;
    pages[0 * 4096] = 0xAA;
    pages[1 * 4096] = 0xBB;
    pages[2 * 4096] = 0xCC;

    void *middle = (unsigned char *)triple + 4096;
    void *split = mmap(middle, 4096, PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("MAP_FIXED into the middle of a live VMA succeeds", split == middle);

    check("the head page survives the split, untouched",
          pages[0 * 4096] == 0xAA);
    check("the middle page is a fresh, zeroed replacement",
          pages[1 * 4096] == 0);
    check("the tail page survives the split, untouched",
          pages[2 * 4096] == 0xCC);

    /* munmap() of just the (post-split) middle page exercises the same
     * carve/split path from vm_free_region()'s side. */
    check("munmap() of the middle page alone succeeds",
          munmap(middle, 4096) == 0);
    check("the head page still survives after that munmap()",
          pages[0 * 4096] == 0xAA);
    check("the tail page still survives after that munmap()",
          pages[2 * 4096] == 0xCC);

    check("munmap() of the head page succeeds", munmap(triple, 4096) == 0);
    check("munmap() of the tail page succeeds",
          munmap((unsigned char *)triple + 2 * 4096, 4096) == 0);
}

static void test_bad_arguments(void) {
    errno = 0;
    check("mmap rejects both MAP_SHARED and MAP_PRIVATE",
          mmap(NULL, 4096, PROT_READ,
               MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED
          && errno == EINVAL);
    errno = 0;
    check("mmap rejects a missing sharing mode",
          mmap(NULL, 4096, PROT_READ, MAP_ANONYMOUS, -1, 0) == MAP_FAILED
          && errno == EINVAL);
    errno = 0;
    check("mmap rejects a zero-length mapping",
          mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
              == MAP_FAILED && errno == EINVAL);
    errno = 0;
    check("anonymous mmap rejects a nonzero offset",
          mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 4096)
              == MAP_FAILED && errno == EINVAL);
    errno = 0;
    check("munmap rejects a misaligned address",
          munmap((void *)0x10000001, 4096) == -1 && errno == EINVAL);
    errno = 0;
    check("munmap rejects a zero-length range",
          munmap((void *)0x10000000, 0) == -1 && errno == EINVAL);

    int fd = open("/opt/tests/hello.txt", O_RDONLY);
    check("open a plain file for file-backed mmap check", fd >= 0);

    errno = 0;
    check("file mmap rejects a misaligned offset",
          mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 1) == MAP_FAILED
          && errno == EINVAL);
    
    void *r2 = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap() on a regular file succeeds", r2 != MAP_FAILED);
    
    /* Verify we actually read the file's contents into the memory.
     * hello.txt starts with "Hello from Extron" or similar.
     * We just check if the first character is 'H'. */
    if (r2 != MAP_FAILED) {
        check("file-backed mmap populated the data", ((char *)r2)[0] == 'H');
        check("munmap() of the file-backed region succeeds", munmap(r2, 4096) == 0);
    }
    close(fd);
}

int main(void) {
    printf("\n[mmap_test] === real mmap()/munmap(), two backings ===\n");

    test_anonymous();
    test_anonymous_shared();
    test_mprotect();
    test_file_shared();
    test_framebuffer_device();
    test_map_fixed();
    test_bad_arguments();

    printf("[mmap_test] === %d failure(s) ===\n", failures);
    return failures;
}
