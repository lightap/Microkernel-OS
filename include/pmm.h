#ifndef PMM_H
#define PMM_H

#include "types.h"

#define PAGE_SIZE 4096

void     pmm_init(uint32_t mem_size_kb);
void*    pmm_alloc_page(void);
void     pmm_free_page(void* addr);
void     pmm_reserve_range(uint32_t start_addr, uint32_t size);
uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);

#endif
