#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <kernel/sync/spinlock.h>
#include <kernel/klibc/string.h>

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

struct vm_space *vm_space_clone(struct vm_space *src) {
    if (!src) return NULL;
    
    spin_lock(&src->lock);
    
    phys_addr_t new_cr3 = deep_copy_pml4(src->cr3);
    if (!new_cr3) {
        spin_unlock(&src->lock);
        return NULL;
    }
    
    struct vm_space *new_mm = kmalloc(sizeof(struct vm_space));
    if (!new_mm) {
        spin_unlock(&src->lock);
        return NULL;
    }
    
    new_mm->lock.locked = 0;
    new_mm->cr3 = new_cr3;
    new_mm->mmap_next = src->mmap_next;
    
    spin_unlock(&src->lock);
    
    return new_mm;
}

static phys_addr_t alloc_page_zeroed(void) {
    phys_addr_t p = (phys_addr_t)pmm_alloc_page();
    if (p) {
        memset(phys_to_virt_hhdm(p), 0, PAGE_SIZE);
    }
    return p;
}

static phys_addr_t deep_copy_pt(phys_addr_t src_phys) {
    uint64_t *src = phys_to_virt_hhdm(src_phys);
    phys_addr_t dst_phys = alloc_page_zeroed();
    if (!dst_phys) panic("OOM in deep_copy_pt");
    uint64_t *dst = phys_to_virt_hhdm(dst_phys);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PAGE_PRESENT)) { dst[i] = 0; continue; }

        phys_addr_t page = alloc_page_zeroed();
        if (!page) panic("OOM in deep_copy_pt");
        memcpy(phys_to_virt_hhdm(page), phys_to_virt_hhdm(src[i] & PAGE_ADDR_MASK), PAGE_SIZE);
        dst[i] = page | (src[i] & ~PAGE_ADDR_MASK);
    }
    return dst_phys;
}

static phys_addr_t deep_copy_pd(phys_addr_t src_phys) {
    uint64_t *src = phys_to_virt_hhdm(src_phys);
    phys_addr_t dst_phys = alloc_page_zeroed();
    if (!dst_phys) panic("OOM in deep_copy_pd");
    uint64_t *dst = phys_to_virt_hhdm(dst_phys);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PAGE_PRESENT)) { dst[i] = 0; continue; }

        if (src[i] & PAGE_HUGE) {
            panic("Huge pages in userspace not supported for deep copy");
        } else {
            dst[i] = deep_copy_pt(src[i] & PAGE_ADDR_MASK) | (src[i] & ~PAGE_ADDR_MASK);
        }
    }
    return dst_phys;
}

static phys_addr_t deep_copy_pdpt(phys_addr_t src_phys) {
    uint64_t *src = phys_to_virt_hhdm(src_phys);
    phys_addr_t dst_phys = alloc_page_zeroed();
    if (!dst_phys) panic("OOM in deep_copy_pdpt");
    uint64_t *dst = phys_to_virt_hhdm(dst_phys);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PAGE_PRESENT)) { dst[i] = 0; continue; }

        if (src[i] & PAGE_HUGE) {
            panic("Huge pages in userspace not supported for deep copy");
        } else {
            dst[i] = deep_copy_pd(src[i] & PAGE_ADDR_MASK) | (src[i] & ~PAGE_ADDR_MASK);
        }
    }
    return dst_phys;
}

phys_addr_t deep_copy_pml4(phys_addr_t src_pml4_phys) {
    uint64_t *src = phys_to_virt_hhdm(src_pml4_phys);
    phys_addr_t dst_phys = create_user_pml4();
    if (!dst_phys) panic("OOM in deep_copy_pml4");
    uint64_t *dst = phys_to_virt_hhdm(dst_phys);

    // Entries 0-255: userspace — deep copy
    for (int i = 0; i < 256; i++) {
        if (!(src[i] & PAGE_PRESENT)) { dst[i] = 0; continue; }
        dst[i] = deep_copy_pdpt(src[i] & PAGE_ADDR_MASK) | (src[i] & ~PAGE_ADDR_MASK);
    }

    // Kernel half (256-511) is already copied by create_user_pml4()

    return dst_phys;
}