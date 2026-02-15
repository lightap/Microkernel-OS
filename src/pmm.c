#include "pmm.h"
#include "vga.h"

#define MAX_PAGES   (1024 * 256)
#define BITMAP_SIZE (MAX_PAGES / 32)

static uint32_t bitmap[BITMAP_SIZE];
static uint32_t total_pages;
static uint32_t used_pages;

static inline void bm_set(uint32_t p)   { bitmap[p / 32] |= (1 << (p % 32)); }
static inline void bm_clear(uint32_t p) { bitmap[p / 32] &= ~(1 << (p % 32)); }
static inline bool bm_test(uint32_t p)  { return (bitmap[p / 32] >> (p % 32)) & 1; }

void pmm_init(uint32_t mem_size_kb) {
    total_pages = mem_size_kb / 4;
    if (total_pages > MAX_PAGES) total_pages = MAX_PAGES;

    for (uint32_t i = 0; i < BITMAP_SIZE; i++) bitmap[i] = 0xFFFFFFFF;
    used_pages = total_pages;

    /*
     * Reserve all memory up to _kernel_end (rounded up to next page).
     * The old hardcoded 2 MB limit was too low — the kernel BSS
     * (page tables, bitmap, etc.) extends well past 2 MB.
     */
    extern uint32_t _kernel_end;
    uint32_t kernel_end_addr = (uint32_t)&_kernel_end;
    uint32_t start = (kernel_end_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = start; i < total_pages; i++) {
        bm_clear(i);
        used_pages--;
    }
}

void* pmm_alloc_page(void) {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            used_pages++;
            void* addr = (void*)(i * PAGE_SIZE);
       //     memset(addr, 0, PAGE_SIZE);  /* Zero the page */
            return addr;
        }
    }
    return NULL;
}

void pmm_free_page(void* addr) {
    uint32_t page = (uint32_t)addr / PAGE_SIZE;
    if (page < total_pages && bm_test(page)) {
        bm_clear(page);
        used_pages--;
    }
}

void pmm_reserve_range(uint32_t start_addr, uint32_t size) {
    uint32_t first_page = start_addr / PAGE_SIZE;
    uint32_t last_page = (start_addr + size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (last_page > total_pages) last_page = total_pages;
    for (uint32_t i = first_page; i < last_page; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            used_pages++;
        }
    }
}

uint32_t pmm_get_free_pages(void)  { return total_pages - used_pages; }
uint32_t pmm_get_total_pages(void) { return total_pages; }
uint32_t pmm_get_used_pages(void)  { return used_pages; }
