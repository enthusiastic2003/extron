#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <kernel/sync/spinlock.h>
#include <kernel/klibc/string.h>

static uint8_t* vmm_bitmap = (uint8_t*)VMM_BITMAP_VIRT_START;
static uint64_t last_allocated_index = 0;
static spinlock_t vmm_lock = SPINLOCK_INIT;

/* ---- bitmap ---- */

static inline void bitmap_set(uint64_t i) {
    vmm_bitmap[i >> 3] |= (1ULL << (i & 7));
}

static inline void bitmap_clear(uint64_t i) {
    vmm_bitmap[i >> 3] &= ~(1ULL << (i & 7));
}

static inline int bitmap_test(uint64_t i) {
    return vmm_bitmap[i >> 3] & (1ULL << (i & 7));
}

/* ---- init ---- */

void vmm_init() {
    uint64_t pages = (KERNEL_HEAP_BITMAP_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        if (!p) panic("VMM: bitmap alloc fail");

        kmap(VMM_BITMAP_VIRT_START + i * PAGE_SIZE,
             p,
             PAGE_PRESENT | PAGE_WRITE);
    }

    for (uint64_t i = 0; i < KERNEL_HEAP_BITMAP_SIZE; i++)
        vmm_bitmap[i] = 0;

    kprintf("VMM: Bitmap initialized (%llu bytes)\n", KERNEL_HEAP_BITMAP_SIZE);
}

/* ---- alloc ---- */

virt_addr_t vmm_alloc_pages(size_t n) {
    spin_lock(&vmm_lock);

    uint64_t total = KERNEL_HEAP_SIZE / PAGE_SIZE;
    uint64_t found = 0, start = 0;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t begin = (pass == 0) ? last_allocated_index : 0;
        uint64_t end   = (pass == 0) ? total : last_allocated_index;

        // FIX: Reset search counter if we wrap around to the beginning
        if (pass == 1) found = 0;

        for (uint64_t i = begin; i < end; i++) {
            if (!bitmap_test(i)) {
                if (found == 0) start = i;

                if (++found == n) {
                    for (uint64_t j = 0; j < n; j++) {
                        uint64_t idx = start + j;

                        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
                        if (!p) panic("VMM: out of physical memory");

                        if (kmap(KERNEL_HEAP_START + idx * PAGE_SIZE,
                                 p,
                                 PAGE_PRESENT | PAGE_WRITE) != 0) {
                            panic("VMM: kmap failed");
                        }

                        bitmap_set(idx);
                    }

                    last_allocated_index = start + n;
                    virt_addr_t result = KERNEL_HEAP_START + start * PAGE_SIZE;
                    spin_unlock(&vmm_lock);
                    return result;
                }
            } else {
                found = 0;
            }
        }
    }

    spin_unlock(&vmm_lock);
    panic("VMM: kernel heap exhausted");
    return 0;
}


virt_addr_t vmm_alloc_page() {
    return vmm_alloc_pages(1);
}

/* ---- free ---- */

int vmm_free_pages(virt_addr_t v, size_t n) {
    // Included the bounds check fix from earlier!
    if (v < KERNEL_HEAP_START ||
        v + (n * PAGE_SIZE) > KERNEL_HEAP_START + KERNEL_HEAP_SIZE)
        return -1;

    uint64_t start = (v - KERNEL_HEAP_START) / PAGE_SIZE;

    for (uint64_t i = 0; i < n; i++) {
        virt_addr_t addr = v + i * PAGE_SIZE;

        // Use kvirt_to_phys to find the address before unmapping
        phys_addr_t p = kvirt_to_phys(addr);
        
        if (p) {
            kunmap(addr);
            pmm_free_page((void*)p);
        }
        else {
            panic("VMM: Tried to free virtual address that was never mapped: %p\n", (void*)addr);
        }

        bitmap_clear(start + i);
    }

    return 0;
}

int vmm_free_page(virt_addr_t v) {
    return vmm_free_pages(v, 1);
}

/* ---- stack ---- */

virt_addr_t vmm_setup_stack() {
    virt_addr_t base = KERNEL_STACK_RANGE_START + KERNEL_STACK_GUARD;

    for (uint64_t i = 0; i < KERNEL_STACK_SIZE; i += PAGE_SIZE) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        if (!p) panic("VMM: stack alloc fail");

        if (kmap(base + i, p, PAGE_PRESENT | PAGE_WRITE) != 0) {
            panic("VMM: stack map fail");
        }
    }

    virt_addr_t top = base + KERNEL_STACK_SIZE;

    kprintf("VMM: stack at %p (top %p)\n",
            (void*)base, (void*)top);

    return top;
}


virt_addr_t vm_allocate_region(struct vm_space *mm, size_t size, int flags) {
    if (!mm || size == 0) return 0;

    size = align_up(size, PAGE_SIZE);

    spin_lock(&mm->lock);

    // Enforce the 1 GB heap cap
    if (mm->mmap_next + size > USER_HEAP_START + USER_HEAP_SIZE) {
        spin_unlock(&mm->lock);
        return 0;
    }

    // Bump the allocator cursor
    virt_addr_t start_addr = mm->mmap_next;
    mm->mmap_next += size;

    uint64_t hw_flags = arch_translate_vm_flags(flags);
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        phys_addr_t frame = (phys_addr_t)pmm_alloc_page();
        if (!frame) {
            // OOM: roll back the cursor (already-mapped pages are leaked —
            // a full rollback requires a VMA tree, noted as future work)
            mm->mmap_next = start_addr;
            spin_unlock(&mm->lock);
            return 0;
        }
        map_page(mm->cr3, start_addr + offset, frame, hw_flags);
    }

    spin_unlock(&mm->lock);

    // Anonymous memory must be zeroed (POSIX)
    memset((void*)start_addr, 0, size);

    return start_addr;
}

void vm_free_region(struct vm_space *mm, virt_addr_t addr, size_t size) {
    if (!mm || size == 0) return;

    size = align_up(size, PAGE_SIZE);

    spin_lock(&mm->lock);

    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        virt_addr_t va = addr + offset;

        // Walk the user page table via HHDM to find the backing physical frame
        uint64_t *pml4_v = (uint64_t*)phys_to_virt_hhdm(mm->cr3);
        uint64_t pml4e = pml4_v[PML4_IDX(va)];
        if (!(pml4e & PAGE_PRESENT)) continue;

        uint64_t *pdpt = (uint64_t*)phys_to_virt_hhdm(pml4e & PAGE_ADDR_MASK);
        uint64_t pdpte = pdpt[PDPT_IDX(va)];
        if (!(pdpte & PAGE_PRESENT)) continue;

        uint64_t *pd = (uint64_t*)phys_to_virt_hhdm(pdpte & PAGE_ADDR_MASK);
        uint64_t pde = pd[PD_IDX(va)];
        if (!(pde & PAGE_PRESENT)) continue;

        uint64_t *pt = (uint64_t*)phys_to_virt_hhdm(pde & PAGE_ADDR_MASK);
        uint64_t pte = pt[PT_IDX(va)];
        if (!(pte & PAGE_PRESENT)) continue;

        phys_addr_t frame = pte & PAGE_ADDR_MASK;
        unmap_page(mm->cr3, va);
        pmm_free_page((void*)frame);
    }

    flush_tlb();
    spin_unlock(&mm->lock);
}

struct vm_space *vm_space_create(phys_addr_t cr3) {
    struct vm_space *mm = kmalloc(sizeof(struct vm_space));
    if (!mm) return NULL;
    mm->lock.locked  = 0;
    mm->cr3          = cr3;
    mm->mmap_next    = USER_HEAP_START;
    return mm;
}

void vm_space_destroy(struct vm_space *mm) {
    if (!mm) return;
    kfree(mm);
}

