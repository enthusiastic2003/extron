// kernel/mm/pmm.c
#include <stdint.h>
#include <stddef.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <boot/multiboot2.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <kernel/sync/spinlock.h>
#include <stdint.h>
#include <kernel/klibc/builtins.h>
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* Rolling next-fit hint, a page index. Purely an optimisation — every
 * scan wraps, so a stale value costs a few extra words, never
 * correctness. */
static uint64_t pmm_cursor = 0;



/* --- Limit to where grub can place structures ---*/

/* --- linker --- */
extern char _kernel_start;
extern char _kernel_end;

struct phys_mem_info
{
    virt_addr_t bmp_phys;
    uint64_t total_mem;
    
} global_phys_mem_info;


/* Regions registered before init_pmm() by arch boot code — memory that
 * IS inside an available range but is occupied. See
 * pmm_reserve_boot_region(). */
static struct { uint64_t start, size; } boot_reservations[PMM_MAX_BOOT_RESERVATIONS];
static size_t boot_reservation_count = 0;

void pmm_reserve_boot_region(uint64_t start, uint64_t size) {
    if (size == 0 || boot_reservation_count >= PMM_MAX_BOOT_RESERVATIONS) {
        return;
    }
    boot_reservations[boot_reservation_count].start = start;
    boot_reservations[boot_reservation_count].size  = size;
    boot_reservation_count++;
}

static inline uint64_t addr_to_idx(uint64_t a) { return a / PAGE_SIZE; }
static inline uint64_t idx_to_addr(uint64_t i) { return i * PAGE_SIZE; }

// Add this near the top of pmm.c, below your other helpers
static inline void bitmap_set(uint64_t bit_idx) {
    uint8_t* bmp = (uint8_t*)global_phys_mem_info.bmp_phys;
    bmp[bit_idx / 8] |= (1 << (bit_idx % 8));
}

static inline void bitmap_clear(uint64_t bit_idx) {
    uint8_t* bmp = (uint8_t*)global_phys_mem_info.bmp_phys;
    bmp[bit_idx / 8] &= ~(1 << (bit_idx % 8));
}

static inline int bitmap_test(uint64_t bit_idx) {
    uint8_t* bmp = (uint8_t*)global_phys_mem_info.bmp_phys;
    return bmp[bit_idx / 8] & (1 << (bit_idx % 8));
}


// Mark a region as used (Aligns outward to safely cover partial pages)
static void pmm_mark_used_region(uint64_t start, uint64_t length) {
    uint64_t start_idx = addr_to_idx(align_down(start, PAGE_SIZE));
    uint64_t end_idx = addr_to_idx(align_up(start + length, PAGE_SIZE));
    for (uint64_t i = start_idx; i < end_idx; i++) {
        bitmap_set(i);
    }
}

// Mark a region as free (Aligns inward to ensure we don't accidentally free reserved data)
static void pmm_mark_free_region(uint64_t start, uint64_t length) {
    uint64_t start_idx = addr_to_idx(align_up(start, PAGE_SIZE)); 
    uint64_t end_idx = addr_to_idx(align_down(start + length, PAGE_SIZE));
    for (uint64_t i = start_idx; i < end_idx; i++) {
        bitmap_clear(i);
    }
}

static inline struct multiboot_tag* mb2_next(struct multiboot_tag* tag) {
    struct multiboot_tag* tag2 = (struct multiboot_tag*)((uint64_t)tag + (uint64_t)((tag->size + 7) & ~7));
    return tag2;
}


void get_bitmap_location(uint64_t mb2_addr){
    uint64_t highest_reserved_addr = (uint64_t)&_kernel_end - KERNEL_VMA;

    if(mb2_addr >= ONE_GIB){
        panic("GRUB Loaded its data at : %lx > %lx", mb2_addr, ONE_GIB);
    }

    /* We check if the mb2 structure is after the kernel*/
    uint32_t mb2_size = *(uint32_t*)mb2_addr; 
    uint64_t mb2_end = mb2_addr + mb2_size;
    if(mb2_end > highest_reserved_addr){
        highest_reserved_addr = mb2_end;
    }

    // The first 8 bytes are the total size and reserved field, tags start after that.
    struct multiboot_tag* tag = (struct multiboot_tag*)(mb2_addr + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        
        switch (tag->type) {
            
            // --- 1. MODULE INFO ---
            case MULTIBOOT_TAG_TYPE_MODULE: {
                struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;
                
                // cmdline is a null-terminated string included at the end of the struct
                kprintf("Module Found: Start=0x%x, End=0x%x, Cmdline=\"%s\"\n", 
                       mod->mod_start, mod->mod_end, mod->cmdline);
                
                highest_reserved_addr = (mod->mod_end > highest_reserved_addr)? mod->mod_end: highest_reserved_addr;
                
                break;
            }

            // --- 2. BASIC MEMORY INFO ---
            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: {
                struct multiboot_tag_basic_meminfo* mem = (struct multiboot_tag_basic_meminfo*)tag;
                
                kprintf("Basic Meminfo: Lower Memory = %u KB, Upper Memory = %u KB\n", 
                       mem->mem_lower, mem->mem_upper);
                break;
            }

            // --- 3. MEMORY MAP (Essential for Physical Memory Management) ---
            case MULTIBOOT_TAG_TYPE_MMAP: {
                struct multiboot_tag_mmap* mmap = (struct multiboot_tag_mmap*)tag;
                kprintf("Memory Map:\n");
                
                // Iterate through the array of memory map entries
                // We use mmap->entry_size to step forward because the spec allows 
                // the entry size to change in future Multiboot versions.
                struct multiboot_mmap_entry* entry;
                for (entry = mmap->entries; 
                     (uint8_t*)entry < (uint8_t*)tag + tag->size; 

                     entry = (struct multiboot_mmap_entry*)((uint64_t)entry + mmap->entry_size)) {

                        // Inside get_bitmap_location's MMAP loop:
                        uint64_t region_end = entry->addr + entry->len;
                        if (region_end > global_phys_mem_info.total_mem && entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                            global_phys_mem_info.total_mem = region_end; // Tracks highest available address space needed
                        }
                        
                    // Type 1 is available RAM. Other types are reserved/ACPI/etc.
                    kprintf("  Region: Addr=0x%lx, Length=0x%lx bytes, Type=%d\n", 
                           entry->addr, entry->len, entry->type);
                }
                break;
            }
        }

        // Advance to the next tag using your 8-byte aligned helper function
        tag = mb2_next(tag);
    }

    /* Keep the bitmap clear of anything registered via
     * pmm_reserve_boot_region(). Reserving a region and then writing the
     * bitmap over it would be self-defeating — and the DTB in particular
     * can sit above the kernel and initrd, exactly where the bitmap
     * would otherwise land. */
    for (size_t i = 0; i < boot_reservation_count; i++) {
        uint64_t end = boot_reservations[i].start + boot_reservations[i].size;
        if (end > highest_reserved_addr) {
            highest_reserved_addr = end;
        }
    }

    global_phys_mem_info.bmp_phys = (virt_addr_t) highest_reserved_addr;

}

/*
 * Free pages, as a number rather than as a printout.
 *
 * Exists to make leaks measurable. Address-space teardown has a lot of
 * pieces that each have to hand memory back — ELF segments, stack,
 * heap, the page tables underneath all three — and every one of them
 * fails silently when it forgets: the system keeps working perfectly
 * and just has less memory than it did. Comparing this across a
 * fork/exec/wait cycle turns that into something a test can assert on.
 */
uint64_t pmm_free_pages(void) {
    spin_lock(&pmm_lock);

    uint64_t max_pages = addr_to_idx(global_phys_mem_info.total_mem);
    uint64_t* bmp64 = (uint64_t*)global_phys_mem_info.bmp_phys;

    uint64_t used_pages = 0;
    uint64_t full_words = max_pages / 64;
    uint64_t leftover_bits = max_pages % 64;

    for (uint64_t i = 0; i < full_words; i++)
        used_pages += __builtin_popcountll(bmp64[i]);
    if (leftover_bits) {
        uint64_t mask = (1ULL << leftover_bits) - 1;
        used_pages += __builtin_popcountll(bmp64[full_words] & mask);
    }

    spin_unlock(&pmm_lock);
    return max_pages - used_pages;
}

void pmm_print_stats(void) {
    spin_lock(&pmm_lock);

    uint64_t max_pages = addr_to_idx(global_phys_mem_info.total_mem);
    uint64_t* bmp64 = (uint64_t*)global_phys_mem_info.bmp_phys;

    uint64_t used_pages = 0;
    uint64_t full_words = max_pages / 64;
    uint64_t leftover_bits = max_pages % 64;

    // Fast path: count set bits 64 at a time with popcount.
    for (uint64_t i = 0; i < full_words; i++) {
        used_pages += __builtin_popcountll(bmp64[i]);
    }

    // Handle the trailing partial word (mask off bits beyond max_pages,
    // since the bitmap is rounded up to PAGE_SIZE and those padding bits
    // were initialized to 1 / "used" but don't represent real pages).
    if (leftover_bits) {
        uint64_t mask = (1ULL << leftover_bits) - 1;
        used_pages += __builtin_popcountll(bmp64[full_words] & mask);
    }

    uint64_t free_pages = max_pages - used_pages;

    spin_unlock(&pmm_lock);

    uint64_t total_bytes = max_pages * PAGE_SIZE;
    uint64_t free_bytes  = free_pages * PAGE_SIZE;
    uint64_t used_bytes  = used_pages * PAGE_SIZE;

    kprintf("PMM Stats:\n");
    kprintf("  Total managed:    %lu pages  (%lu MiB,  %lu GiB)\n",
            max_pages, total_bytes >> 20, total_bytes >> 30);
    kprintf("  Free (handout):   %lu pages  (%lu MiB)\n",
            free_pages, free_bytes >> 20);
    kprintf("  Used / reserved:  %lu pages  (%lu MiB)\n",
            used_pages, used_bytes >> 20);
}

void init_pmm(uint64_t mb2_addr) {
    
    // 1. Locate highest reserved boot address and total physical address space
    get_bitmap_location(mb2_addr);

    // Page-align the start of our bitmap
    global_phys_mem_info.bmp_phys = (virt_addr_t)align_up((uint64_t)global_phys_mem_info.bmp_phys, PAGE_SIZE);

    // Calculate how big the bitmap needs to be (1 bit per page)
    uint64_t max_pages = addr_to_idx(global_phys_mem_info.total_mem);
    uint64_t bitmap_size_bytes = align_up(max_pages / 8, PAGE_SIZE);

    uint64_t bitmap_end_address = (uint64_t)global_phys_mem_info.bmp_phys + bitmap_size_bytes;

    if(bitmap_end_address > ONE_GIB){
        panic("Bitmap Allocation Exceeds 1GB initially mapped memory");
    }

    // 2. Deny by Default: Mark everything as USED (1)
    uint8_t* bmp = (uint8_t*)global_phys_mem_info.bmp_phys;
    for (uint64_t i = 0; i < bitmap_size_bytes; i++) {
        bmp[i] = 0xFF; 
    }


    // 3. Second Pass: Loop through MMAP and free AVAILABLE regions
    struct multiboot_tag* tag = (struct multiboot_tag*)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) { 
            struct multiboot_tag_mmap* mmap = (struct multiboot_tag_mmap*)tag;
            struct multiboot_mmap_entry* entry;
            
            for (entry = mmap->entries; 
                 (uint8_t*)entry < (uint8_t*)tag + tag->size; 
                 entry = (struct multiboot_mmap_entry*)((uint64_t)entry + mmap->entry_size)) {
                
                if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    pmm_mark_free_region(entry->addr, entry->len);
                }
            }
        }
        tag = mb2_next(tag);
    }


    // 4. Re-reserve Critical Boot Structures
    
    // Reserve the first 1MB (BIOS data, VGA buffer, IVT, etc.)
    pmm_mark_used_region(0, 0x100000);


    // Reserve the Kernel
    uint64_t kernel_start = (uint64_t)&_kernel_start;
    uint64_t kernel_len = (uint64_t)&_kernel_end - kernel_start;
    kernel_start -= KERNEL_VMA;
    pmm_mark_used_region(kernel_start, kernel_len);


    // Reserve the Multiboot2 Data Structure itself
    uint32_t mb2_size = *(uint32_t*)mb2_addr;
    pmm_mark_used_region(mb2_addr, mb2_size);


    // Reserve all loaded modules (Initrd, etc.)
    tag = (struct multiboot_tag*)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;
            pmm_mark_used_region(mod->mod_start, mod->mod_end - mod->mod_start);
        }
        tag = mb2_next(tag);
    }

    // Reserve the memory holding the Bitmap itself
    pmm_mark_used_region((uint64_t)global_phys_mem_info.bmp_phys, bitmap_size_bytes);

    // Anything arch boot code registered before we got here (the DTB).
    for (size_t i = 0; i < boot_reservation_count; i++) {
        pmm_mark_used_region(boot_reservations[i].start, boot_reservations[i].size);
        kprintf("PMM: reserved boot region 0x%lx - 0x%lx\n",
                boot_reservations[i].start,
                boot_reservations[i].start + boot_reservations[i].size);
    }


    kprintf("PMM Initialized! Bitmap at 0x%lx, Size: %u bytes\n", 
            (uint64_t)global_phys_mem_info.bmp_phys, bitmap_size_bytes);
}

uint64_t get_virtual_pmm_bitmap_location(){
    return (uint64_t)global_phys_mem_info.bmp_phys;
}

uint64_t get_pmm_total_manage(){
    return global_phys_mem_info.total_mem;
}

void set_virtual_pmm_bitmap_location(uint64_t new_bitmap_virt_loc){
    global_phys_mem_info.bmp_phys = (virt_addr_t) new_bitmap_virt_loc;
}

/*
 * Find and claim one free page. Caller holds pmm_lock (or is genuinely
 * single-threaded pre-MMU — see pmm_alloc_page_nolock).
 *
 * Scans 64 bits at a time from a rolling cursor, rather than bit by bit
 * from index 0. The old version restarted at zero on every call, so with
 * ~19,700 pages already used at boot each allocation burned at least
 * that many iterations before reaching free space, degrading further as
 * memory filled. A DOOM-sized 8MB zone is ~2048 allocations, which made
 * that roughly 40 million iterations of pure scan.
 *
 * The cursor makes the common case (allocating into fresh space) hit on
 * the first word, and ctzll finds the free bit within a word in one
 * instruction. The wrap-around second pass is what keeps freed pages
 * reachable — without it the cursor would strand everything behind it.
 */
static void* pmm_take_free_page_locked(void) {
    uint64_t max_pages = addr_to_idx(global_phys_mem_info.total_mem);
    if (max_pages == 0)
        return NULL;

    uint64_t  words = (max_pages + 63) / 64;
    uint64_t* bmp64 = (uint64_t*)global_phys_mem_info.bmp_phys;

    uint64_t start_word = pmm_cursor / 64;
    if (start_word >= words)
        start_word = 0;

    for (uint64_t n = 0; n < words; n++) {
        uint64_t w = start_word + n;
        if (w >= words) w -= words;          /* wrap */

        uint64_t word = bmp64[w];
        if (word == ~0ULL)
            continue;                        /* all 64 pages taken */

        uint64_t bit = (uint64_t)__builtin_ctzll(~word);
        uint64_t idx = w * 64 + bit;
        /* Only reachable in the final partial word: bits past max_pages
         * are initialised used and never freed, but check anyway rather
         * than hand out a page that doesn't exist. */
        if (idx >= max_pages)
            continue;

        bitmap_set(idx);
        pmm_cursor = idx + 1;
        return (void*)idx_to_addr(idx);
    }

    return NULL;
}

/*
 * Returns the physical address of a free 4KB page, or NULL if out of
 * memory.
 *
 * NULL, not panic. This used to panic, which made every OOM path in
 * every caller dead code — kernel/mm/uvm.c's careful unwind-and-free on
 * partial allocation had never once executed — and, worse, handed
 * userspace a way to halt the machine: SYS_ANON_ALLOC in a loop until
 * physical memory ran out. A process asking for too much must get a
 * failed allocation, not take the kernel down with it. Boot-critical
 * callers that genuinely cannot continue panic at their own call site,
 * where they can say something useful about what failed.
 */
void* pmm_alloc_page(void) {
    spin_lock(&pmm_lock);
    void* page = pmm_take_free_page_locked();
    spin_unlock(&pmm_lock);
    return page;
}

// Same as pmm_alloc_page(), but without taking pmm_lock. For genuinely
// single-threaded, pre-MMU bootstrap code only (e.g. aarch64's own page
// table allocation before the MMU exists): AArch64 exclusive-access
// atomics (ldxr/stxr, what spin_lock compiles to) require Normal memory,
// which isn't available until paging is up — with the MMU off, memory
// defaults to Device semantics on real hardware, and stxr never
// succeeds, spinning forever. QEMU's TCG doesn't model this restriction,
// so it's invisible there.
void* pmm_alloc_page_nolock(void) {
    return pmm_take_free_page_locked();
}

/* Takes pmm_lock, which it previously did not while pmm_alloc_page()
 * did — an asymmetry that would corrupt the bitmap (one physical page
 * handed out twice) the moment anything freed from an IRQ handler or
 * syscalls became preemptible. Not reachable today on a single core
 * with the callers all running DAIF-masked, which is exactly why it
 * would have gone unnoticed until it wasn't. */
void pmm_free_page(void* phys_addr) {
    uint64_t addr = (uint64_t)phys_addr;

    // Sanity check: ensure the address is exactly on a 4KB boundary
    if (addr % PAGE_SIZE != 0) {
        panic("PMM: Tried to free an unaligned address: 0x%lx", addr);
    }

    uint64_t idx = addr_to_idx(addr);

    spin_lock(&pmm_lock);
    if (!bitmap_test(idx)) {
        spin_unlock(&pmm_lock);
        panic("PMM: Double free or freeing unallocated page at 0x%lx", addr);
    }
    bitmap_clear(idx);
    /* Reuse freed space promptly rather than letting the cursor run
     * ahead of it: without this a free-heavy workload would keep pushing
     * the cursor forward and only revisit this page on the wrap pass. */
    if (idx < pmm_cursor)
        pmm_cursor = idx;
    spin_unlock(&pmm_lock);
}