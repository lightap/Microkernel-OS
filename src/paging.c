#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "vga.h"
#include "task.h"
#include "serial.h"

/* Master kernel page directory and static tables */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[1024][1024] __attribute__((aligned(4096))); 

/* High virtual addresses for mapping windows */
#define TEMP_PD_VA 0x3FE00000
#define TEMP_PT_VA 0x3FE01000

static void page_fault_handler(registers_t* regs) {
    uint32_t faulting_addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_addr));
    task_t* t = task_get_current();
    
    serial_printf("\n!!! PAGE FAULT !!! addr=%x eip=%x err=%x\n", faulting_addr, regs->eip, regs->err_code);
    
    if (t && t->id > 0 && (regs->err_code & 4)) {
        serial_printf("  Killing user task PID %u ('%s')\n", t->id, t->name);
        task_kill(t->id);
        task_yield();
        return;
    }

    kprintf("\n KERNEL PANIC: Page Fault at %x (EIP: %x)\n", faulting_addr, regs->eip);
    cli(); for (;;) hlt();
}

/* Force a full TLB flush by reloading CR3 */
static inline void flush_tlb_all(void) {
    uint32_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

void paging_init(uint32_t mem_kb) {
    register_interrupt_handler(14, page_fault_handler);
    uint32_t ram_pages = (mem_kb * 1024) / 4096;

    memset(page_directory, 0, sizeof(page_directory));



    for (uint32_t t = 0; t < 1024; t++) {
        memset(page_tables[t], 0, 4096);
        for (uint32_t p = 0; p < 1024; p++) {
            uint32_t page_idx = t * 1024 + p;
                if (page_idx < ram_pages) {
        // Identity map with PAGE_USER so the Shell can blit
        page_tables[t][p] = (page_idx * 4096) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
        }
        /* 
         * Link Directory to Table. 
         * Set PAGE_USER on the Directory entry so that Ring 3 tasks 
         * can access specific pages inside these tables if the PTE allows it.
         */
        page_directory[t] = (uint32_t)page_tables[t] | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    __asm__ volatile (
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"
        "mov %%eax, %%cr0" 
        : : "r"((uint32_t)page_directory) : "eax"
    );
}

// src/paging.c

// src/paging.c -> paging_map_page

// src/paging.c

void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    serial_printf("MAP_PAGE: v=%x p=%x f=%x\n", virt, phys, flags);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t* pde = &page_directory[pd_idx];

    /* Create page table if it doesn't exist yet */
    if (!(*pde & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_page();  // get a physical page
        if (!pt_phys) {
            serial_printf("OOM: cannot alloc PT for v=%x\n", virt);
            return;
        }

        memset((void*)pt_phys, 0, 4096);  // clear it (assumes low mem identity mapped)

        *pde = pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE;
        __asm__ volatile ("invlpg (%0)" : : "r"(pde) : "memory");

        serial_printf("  Created PT phys=%x for PD idx %u\n", pt_phys, pd_idx);
    }

    uint32_t* pt = (uint32_t*)(*pde & 0xFFFFF000);
    pt[pt_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;

    serial_printf("  PTE written: %x\n", pt[pt_idx]);

    __asm__ volatile ("invlpg (%0)" : : "r"((void*)virt) : "memory");
}
void paging_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags) {
    uint32_t pages = (size + 0xFFF) / 0x1000;
    for (uint32_t i = 0; i < pages; i++)
        paging_map_page(virt + i * 0x1000, phys + i * 0x1000, flags);
}

void paging_unmap_page(uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    page_tables[pd_idx][pt_idx] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

uint32_t* paging_get_kernel_pd(void) { return page_directory; }

uint32_t* paging_create_address_space(void) {
    uint32_t* pd = (uint32_t*)pmm_alloc_page();
    if (!pd) return NULL;

    /*
     * Clone each present kernel page table into a per-process copy
     * with PAGE_USER set on every present PTE.  This allows ring-3
     * servers (which are compiled into the kernel binary) to execute
     * the same code.  Each cloned PT is a separate physical page so
     * the kernel's own tables stay supervisor-only.
     */
    for (int i = 0; i < 1024; i++) {
        if (!(page_directory[i] & PAGE_PRESENT)) {
            pd[i] = 0;
            continue;
        }

        uint32_t* new_pt = (uint32_t*)pmm_alloc_page();
        if (!new_pt) {
            /* Out of memory — free everything allocated so far */
            for (int j = 0; j < i; j++) {
                if (pd[j] & PAGE_PRESENT)
                    pmm_free_page((void*)(pd[j] & 0xFFFFF000));
            }
            pmm_free_page(pd);
            return NULL;
        }

        /* Copy PTEs from the kernel's page table, adding PAGE_USER */
        uint32_t* src_pt = page_tables[i];
        for (int j = 0; j < 1024; j++) {
            if (src_pt[j] & PAGE_PRESENT)
                new_pt[j] = src_pt[j] | PAGE_USER;
            else
                new_pt[j] = 0;
        }

        pd[i] = (uint32_t)new_pt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    return pd;
}

uint32_t* paging_create_isolated_space(void) {
    uint32_t* pd = (uint32_t*)pmm_alloc_page();
    if (!pd) return NULL;
    
    /* 
     * Copy the entire kernel directory. 
     * This ensures hardware drivers, syscalls, and TEMP windows work 
     * while the user process is active. 
     */
    for (int i = 0; i < 1024; i++) {
        pd[i] = page_directory[i];
        /* 
         * Optional: Clear the specific range where ELFs are loaded 
         * (e.g., 0x40000000 - 0xBFFFFFFF) to ensure a clean start.
         */
        if (i >= 256 && i < 768) pd[i] = 0; 
    }
    return pd;
}

/**
 * paging_map_user
 * 
 * Maps a physical frame into a specific (target) Page Directory.
 * Used by the ELF loader and the GUI system to map memory into user processes.
 */
/*
void paging_map_user(uint32_t* target_pd_phys, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

   
    uint32_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint32_t kernel_cr3 = (uint32_t)page_directory; // Access global kernel PD
    
    if (saved_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

 
    paging_map_page(TEMP_PD_VA, (uint32_t)target_pd_phys, PAGE_PRESENT | PAGE_WRITE);
    uint32_t* pd_view = (uint32_t*)TEMP_PD_VA;


    if (!(pd_view[pd_idx] & PAGE_PRESENT)) {
        uint32_t new_pt_phys = (uint32_t)pmm_alloc_page();
        
        // Use the second window to zero out the new Page Table
        paging_map_page(TEMP_PT_VA, new_pt_phys, PAGE_PRESENT | PAGE_WRITE);
        memset((void*)TEMP_PT_VA, 0, 4096);
        
        // Link the new PT into the Page Directory
        // IMPORTANT: Must have PAGE_USER (0x04) or Ring 3 can't traverse this path
        pd_view[pd_idx] = new_pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        paging_unmap_page(TEMP_PT_VA);
    } else {
        // Ensure the existing directory entry has the USER bit set
        pd_view[pd_idx] |= PAGE_USER;
    }

  
    uint32_t pt_phys = pd_view[pd_idx] & 0xFFFFF000;
    paging_map_page(TEMP_PT_VA, pt_phys, PAGE_PRESENT | PAGE_WRITE);
    uint32_t* pt_view = (uint32_t*)TEMP_PT_VA;

 
    uint32_t final_flags = flags | PAGE_PRESENT | PAGE_USER;
    if (virt >= 0x30000000 && virt < 0x40000000) {
        final_flags |= 0x08; // PWT bit
    }

    pt_view[pt_idx] = (phys & 0xFFFFF000) | (final_flags & 0xFFF);

 
    paging_unmap_page(TEMP_PT_VA);
    paging_unmap_page(TEMP_PD_VA);

    if (saved_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
    }

    // Invalidate the TLB for the virtual address we just mapped 
    // in case it was cached as "non-present" in the active directory.
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}
*/

void paging_map_user(uint32_t* target_pd_phys, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    /* 
     * 1. Switch to Kernel Context:
     * We must ensure the Kernel's Page Directory is active to use the 
     * TEMP windows (0x3FE00000). Otherwise, we Page Fault.
     */
    uint32_t saved_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
    uint32_t kernel_cr3 = (uint32_t)page_directory; 
    
    if (saved_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    /* 
     * 2. Map Target Directory into View:
     */
    paging_map_page(TEMP_PD_VA, (uint32_t)target_pd_phys, PAGE_PRESENT | PAGE_WRITE);
    uint32_t* pd_view = (uint32_t*)TEMP_PD_VA;

    /* 
     * 3. Handle Page Table Allocation:
     */
    if (!(pd_view[pd_idx] & PAGE_PRESENT)) {
        uint32_t new_pt_phys = (uint32_t)pmm_alloc_page();
        
        // Zero out the new Page Table using the second window
        paging_map_page(TEMP_PT_VA, new_pt_phys, PAGE_PRESENT | PAGE_WRITE);
        memset((void*)TEMP_PT_VA, 0, 4096);
        
        // Link the new PT into the Page Directory
        // Set PAGE_USER (0x04) so User Mode can traverse this branch
        pd_view[pd_idx] = new_pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        paging_unmap_page(TEMP_PT_VA);
    } else {
        // Ensure existing Directory Entry allows User access
        pd_view[pd_idx] |= PAGE_USER;
    }

    /* 
     * 4. Map the Page Table itself:
     */
    uint32_t pt_phys = pd_view[pd_idx] & 0xFFFFF000;
    paging_map_page(TEMP_PT_VA, pt_phys, PAGE_PRESENT | PAGE_WRITE);
    uint32_t* pt_view = (uint32_t*)TEMP_PT_VA;

    /* 
     * 5. The GPU Cache Sync (Crucial Fix):
     * If mapping the GPU buffer area (0x30000000), add the 
     * PAGE_WRITE_THROUGH (0x08) bit. This forces the CPU to write 
     * commands to RAM immediately so the GPU (VirtIO) sees them correctly.
     */
    uint32_t final_flags = flags | PAGE_PRESENT | PAGE_USER;
    if (virt >= 0x30000000 && virt < 0x40000000) {
        final_flags |= 0x08; // PWT (Write-Through)
    }

    /* 
     * 6. Final Mapping:
     */
    pt_view[pt_idx] = (phys & 0xFFFFF000) | (final_flags & 0xFFF);

    /* 
     * 7. Cleanup and Restore:
     */
    paging_unmap_page(TEMP_PT_VA);
    paging_unmap_page(TEMP_PD_VA);

    if (saved_cr3 != kernel_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
    }

    // Invalidate the TLB for the target address
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}
void paging_destroy_address_space(uint32_t* pd) {
    if (!pd || pd == page_directory) return;

    /*
     * Free any page tables that are NOT the kernel's shared tables.
     * Per-process page tables (created by paging_create_address_space
     * or paging_map_user) have different physical addresses than the
     * kernel's static page_tables[] array.
     */
    for (int i = 0; i < 1024; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        uint32_t pt_phys = pd[i] & 0xFFFFF000;
        uint32_t kernel_pt_phys = page_directory[i] & 0xFFFFF000;
        if (pt_phys != kernel_pt_phys) {
            pmm_free_page((void*)pt_phys);
        }
    }

    pmm_free_page(pd);
}

void paging_switch(uint32_t* pd) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)pd) : "memory");
}

void paging_print_info(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    kprintf("  Paging: ENABLED | CR3: %x | Kernel PD: %x\n", cr3, (uint32_t)page_directory);
}

uint32_t paging_get_physical(uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    if (page_tables[pd_idx][pt_idx] & PAGE_PRESENT)
        return (page_tables[pd_idx][pt_idx] & 0xFFFFF000) | (virt & 0xFFF);
    return 0;
}

int copy_from_user(void* to, const void* from, uint32_t size) {
    if (!from || !to) return -1;
    memcpy(to, from, size);
    return 0;
}

uint32_t virt_to_phys(const void* va) {
    uint32_t v = (uint32_t)va;

    uint32_t pd_index = v >> 22;
    uint32_t pt_index = (v >> 12) & 0x3FF;

    uint32_t pde = page_directory[pd_index];
    if (!(pde & 0x1)) return 0;   // not present

    uint32_t* page_table = (uint32_t*)(pde & 0xFFFFF000);
    uint32_t pte = page_table[pt_index];
    if (!(pte & 0x1)) return 0;   // not present

    return (pte & 0xFFFFF000) | (v & 0xFFF);
}

