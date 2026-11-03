/*
 * Extron UEFI loader (BOOTX64.EFI).
 *
 *   1. read \extron\kernel.elf and \extron\initrd.ext2 from the ESP
 *   2. copy the kernel's PT_LOAD segments to their physical load addresses
 *   3. build a multiboot2-shaped info block (MMAP + MODULE tags) — the same
 *      format the kernel's PMM/ramdisk code already consumes on every arch
 *   4. ExitBootServices(), jump to the kernel entry with rdi = info block
 *
 * One contiguous allocation sits directly after the kernel image:
 *   [ initrd | boot-info block | slack ]
 * The kernel places its PMM bitmap right above the highest of
 * {kernel end, info block, module}, so that slack is guaranteed to be
 * ordinary RAM rather than whatever firmware parked there.
 */
#include "efi.h"

#define KERNEL_VMA        0xFFFFFFFF80000000ULL
#define PAGE              4096ULL
#define MAX_PHYS_LIMIT    0x3FFFFFFFULL      /* kernel's PMM wants everything < 1 GiB */
#define INFO_SIZE         (64 * 1024)
#define SLACK_SIZE        (2 * 1024 * 1024)

#define MB2_TAG_END    0
#define MB2_TAG_MODULE 3
#define MB2_TAG_MMAP   6

static const EFI_GUID LoadedImageGuid = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID SimpleFsGuid    = {0x964E5B22, 0x6459, 0x11D2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* freestanding helpers — gcc may emit calls to these */
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d; }

static void print(const CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }

static void print_hex(uint64_t v) {
    CHAR16 buf[19]; buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) { int d = (v >> (60 - 4 * i)) & 0xF; buf[2 + i] = d < 10 ? '0' + d : 'a' + d - 10; }
    buf[18] = 0; print(buf);
}

static void fail(const CHAR16 *msg, EFI_STATUS st) {
    print(L"\r\nextron loader: "); print(msg); print(L" status="); print_hex(st); print(L"\r\n");
    for (;;) __asm__ volatile ("hlt");
}

static EFI_FILE *root;

/* Open a file and return its size (position the cursor at 0). */
static EFI_FILE *open_file(const CHAR16 *path, uint64_t *size) {
    EFI_FILE *f;
    EFI_STATUS s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) { print(path); fail(L" not found on the ESP", s); }
    s = f->SetPosition(f, ~0ULL);
    if (!EFI_ERROR(s)) s = f->GetPosition(f, size);
    if (!EFI_ERROR(s)) s = f->SetPosition(f, 0);
    if (EFI_ERROR(s)) fail(L"cannot size file", s);
    return f;
}

static void read_all(EFI_FILE *f, void *dst, uint64_t size) {
    uint8_t *p = dst;
    while (size) {
        UINTN chunk = size > (1u << 20) ? (1u << 20) : (UINTN)size;
        EFI_STATUS s = f->Read(f, &chunk, p);
        if (EFI_ERROR(s) || chunk == 0) fail(L"read error", s);
        p += chunk; size -= chunk;
    }
}

typedef struct {
    uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

static uint8_t memmap_buf[256 * 1024] __attribute__((aligned(16)));

typedef void (__attribute__((sysv_abi)) *kernel_entry_t)(uint64_t boot_info);

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    print(L"Extron UEFI loader\r\n");

    EFI_LOADED_IMAGE *li; EFI_SIMPLE_FILE_SYSTEM *fs;
    EFI_STATUS s = BS->HandleProtocol(image, &LoadedImageGuid, (void **)&li);
    if (EFI_ERROR(s)) fail(L"LoadedImage protocol", s);
    s = BS->HandleProtocol(li->DeviceHandle, &SimpleFsGuid, (void **)&fs);
    if (EFI_ERROR(s)) fail(L"SimpleFileSystem protocol", s);
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s)) fail(L"OpenVolume", s);

    /* ---- kernel ---- */
    uint64_t ksize;
    EFI_FILE *kf = open_file(L"\\extron\\kernel.elf", &ksize);
    void *kbuf;
    s = BS->AllocatePool(EfiLoaderData, ksize, &kbuf);
    if (EFI_ERROR(s)) fail(L"alloc kernel buffer", s);
    read_all(kf, kbuf, ksize);
    kf->Close(kf);

    Elf64_Ehdr *eh = kbuf;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_machine != 62)
        fail(L"kernel.elf is not an x86-64 ELF", 0);
    Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)kbuf + eh->e_phoff);

    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue;
        if (ph[i].p_paddr < lo) lo = ph[i].p_paddr;
        if (ph[i].p_paddr + ph[i].p_memsz > hi) hi = ph[i].p_paddr + ph[i].p_memsz;
    }
    if (lo == ~0ULL) fail(L"kernel.elf has no PT_LOAD", 0);
    lo &= ~(PAGE - 1);
    hi = (hi + PAGE - 1) & ~(PAGE - 1);

    /* ---- initrd ---- */
    uint64_t isize;
    EFI_FILE *rf = open_file(L"\\extron\\initrd.ext2", &isize);

    /* kernel image, then [initrd | info | slack] right behind it, one block */
    uint64_t initrd_off = hi;
    uint64_t initrd_len = (isize + PAGE - 1) & ~(PAGE - 1);
    uint64_t info_off = initrd_off + initrd_len;
    uint64_t total = (info_off + INFO_SIZE + SLACK_SIZE) - lo;
    if (lo + total > MAX_PHYS_LIMIT) fail(L"kernel+initrd do not fit below 1 GiB", 0);

    EFI_PHYSICAL_ADDRESS base = lo;
    s = BS->AllocatePages(AllocateAddress, EfiLoaderData, total / PAGE, &base);
    if (EFI_ERROR(s)) {
        /* Show what the firmware has there, to pick a better KERNEL_LMA. */
        UINTN ms = sizeof(memmap_buf), mk, ds; uint32_t dv;
        if (!EFI_ERROR(BS->GetMemoryMap(&ms, (EFI_MEMORY_DESCRIPTOR *)memmap_buf, &mk, &ds, &dv))) {
            for (UINTN off = 0; off < ms; off += ds) {
                EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(memmap_buf + off);
                if (d->PhysicalStart > MAX_PHYS_LIMIT) break;
                print(L"  type "); print_hex(d->Type); print(L" @ "); print_hex(d->PhysicalStart);
                print(L" pages "); print_hex(d->NumberOfPages); print(L"\r\n");
            }
        }
        print(L"wanted "); print_hex(lo); print(L" pages "); print_hex(total / PAGE); print(L"\r\n");
        fail(L"cannot allocate kernel region at its fixed load address", s);
    }
    memset((void *)lo, 0, total);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue;
        memcpy((void *)ph[i].p_paddr, (uint8_t *)kbuf + ph[i].p_offset, ph[i].p_filesz);
    }
    uint64_t entry_phys = eh->e_entry - KERNEL_VMA;

    read_all(rf, (void *)initrd_off, isize);
    rf->Close(rf);
    BS->FreePool(kbuf);

    print(L"kernel entry "); print_hex(entry_phys);
    print(L"  initrd "); print_hex(initrd_off); print(L" +"); print_hex(isize);
    print(L"\r\nexiting boot services\r\n");

    /* ---- leave boot services ---- */
    UINTN map_size, map_key, desc_size; uint32_t desc_ver;
    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = sizeof(memmap_buf);
        s = BS->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)memmap_buf, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(s)) fail(L"GetMemoryMap", s);
        s = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(s)) break;
    }
    if (EFI_ERROR(s)) fail(L"ExitBootServices", s);

    /* ---- multiboot2-shaped info block (no firmware services from here on) ---- */
    uint8_t *info = (uint8_t *)info_off;
    uint8_t *mmap_tag = info + 8;
    uint8_t *entries = mmap_tag + 16;
    uint32_t n = 0;
    for (UINTN off = 0; off < map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(memmap_buf + off);
        int usable = d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode
                  || d->Type == EfiLoaderData || d->Type == EfiBootServicesCode
                  || d->Type == EfiBootServicesData;
        if (!usable) continue;
        uint64_t start = d->PhysicalStart, len = d->NumberOfPages * PAGE;
        /* coalesce with the previous entry when contiguous */
        if (n) {
            uint8_t *prev = entries + (n - 1) * 24;
            uint64_t pa, pl; memcpy(&pa, prev, 8); memcpy(&pl, prev + 8, 8);
            if (pa + pl == start) { put64(prev + 8, pl + len); continue; }
        }
        if ((16 + (n + 1) * 24 + 8 + 32 + 16) > INFO_SIZE) break;
        uint8_t *e = entries + n * 24;
        put64(e, start); put64(e + 8, len); put32(e + 16, 1); put32(e + 20, 0);
        n++;
    }
    uint32_t mmap_size = 16 + n * 24;
    uint32_t mmap_aligned = (mmap_size + 7) & ~7u;

    uint8_t *mod = mmap_tag + mmap_aligned;
    put32(mod, MB2_TAG_MODULE);
    put32(mod + 4, 16 + 7);                    /* header + "initrd\0" */
    put32(mod + 8, (uint32_t)initrd_off);
    put32(mod + 12, (uint32_t)(initrd_off + isize));
    memcpy(mod + 16, "initrd", 7);
    uint32_t mod_aligned = (16 + 7 + 7) & ~7u;

    uint8_t *end = mod + mod_aligned;
    put32(end, MB2_TAG_END); put32(end + 4, 8);

    put32(mmap_tag, MB2_TAG_MMAP);
    put32(mmap_tag + 4, mmap_size);
    put32(mmap_tag + 8, 24);
    put32(mmap_tag + 12, 0);

    put32(info, (uint32_t)((end + 8) - info));
    put32(info + 4, 0);

    __asm__ volatile ("cli");
    ((kernel_entry_t)entry_phys)(info_off);
    for (;;) __asm__ volatile ("hlt");
}
