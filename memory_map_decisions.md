# Kernel Memory Map and Layout Decisions

This document outlines the core memory management and virtual layout decisions made for the kernel.

## 1. Mapped Areas of the Virtual Space
The 64-bit higher half virtual address space is segmented into several specialized regions:
- **`0xFFFFFFFF80000000`** - **Kernel Virtual Base:** The location where the kernel ELF sections are loaded and mapped.
- **`0xFFFF800000000000`** - **Higher Half Direct Map (HHDM):** A direct identity mapping of all physical memory, optimized by using **2MB huge pages** for improved TLB efficiency.
- **`0xFFFFBF0000000000`** - **VMM Bitmap:** Fixed virtual space reserved for tracking the Virtual Memory Manager's allocation state (placed exactly 1TB below the kernel heap).
- **`0xFFFFC00000000000`** - **Kernel Heap:** The base address for dynamic kernel memory allocations.
- **`0xFFFFD00000000000`** - **Kernel Stack Range:** The dedicated region for mapping dynamic kernel and thread stacks.

## 2. Kernel Stack Configuration
- **Location:** The stack area begins at `0xFFFFD00000000000` (`KERNEL_STACK_RANGE_START`).
- **Size:** The new, expanded kernel stack is allocated as **4 MB** per stack (`KERNEL_STACK_SIZE`), significantly larger than standard defaults.
- **Guard Page:** A **4 KB** unmapped guard page (`KERNEL_STACK_GUARD`) is placed at the boundaries to safely catch stack overflow errors.

## 3. Location of Memory Management Bitmaps
- **PMM (Physical Memory Manager) Bitmap:** 
  Initially placed into an available region of physical memory discovered during early boot via Multiboot2 tags. Once paging is fully initialized, it is accessed virtually through the HHDM (`get_virtual_pmm_bitmap_location() + NEW_HDDM`).
- **VMM (Virtual Memory Manager) Bitmap:** 
  Allocated dynamically but mapped to a strictly fixed virtual address at `0xFFFFBF0000000000` (`VMM_BITMAP_VIRT_START`).

## 4. Areas Managed by the VMM
The Virtual Memory Manager (VMM) acts as the bridge between physical pages and virtual address allocation. It specifically manages:
- **The Kernel Heap Range:** Tracking allocations via its bitmap and allocating underlying physical pages as the kernel requests memory.
- **Kernel Stacks:** Dynamically allocating the 4MB physical backing for the stacks within the `KERNEL_STACK_RANGE_START` region.

## 5. Kernel Heap (kheap)
- **Location:** Begins at `0xFFFFC00000000000` (`KERNEL_HEAP_START`).
- **Size Limit:** The heap is configured to allow up to **128 GB** of total dynamic allocation space (`KERNEL_HEAP_SIZE`).
- **Implementation:** Serviced by the liballoc allocator, which calls down into the VMM (`vmm_alloc_pages` / `vmm_free_pages`) to fetch new pages behind the scenes as dynamic requests (`malloc`, `calloc`) are fulfilled.
