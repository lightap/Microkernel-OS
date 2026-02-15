#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define PAGE_PRESENT   0x001
#define PAGE_WRITE     0x002
#define PAGE_USER      0x004
#define PAGE_4MB       0x080
#define PAGE_CACHE_DISABLE   (1U << 4)

/* Initialize paging with identity-mapped kernel space */
void paging_init(uint32_t mem_kb);

/* Single-page operations in the kernel page directory */
void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void paging_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);
void paging_unmap_page(uint32_t virt);
uint32_t paging_get_physical(uint32_t virt);
void paging_print_info(void);

/* --- Per-process address spaces (microkernel support) --- */

/* Create a new page directory for a process.
 * Clones kernel mappings (supervisor-only) into the new PD.
 * Returns physical address of the new page directory. */
uint32_t* paging_create_address_space(void);
uint32_t* paging_create_isolated_space(void);

/* Destroy a process's page directory and free its page tables.
 * Does NOT free user-mapped physical frames — caller must do that. */
void paging_destroy_address_space(uint32_t* pd);

/* Switch the active address space (load CR3) */
void paging_switch(uint32_t* pd);

/* Map a page in a specific page directory as user-accessible */
void paging_map_user(uint32_t* page_dir, uint32_t virt, uint32_t phys, uint32_t flags);

/* Unmap a page in a specific page directory */
void paging_unmap_user(uint32_t* pd, uint32_t virt);

/* Get the kernel's page directory (for kernel tasks) */
uint32_t* paging_get_kernel_pd(void);


uint32_t virt_to_phys(const void* va);

#endif
