#include "heap.h"
#include "vga.h"

#define HEAP_MAGIC 0xDEADBEEF

typedef struct block_header {
    uint32_t magic;
    size_t   size;         /* Size of data area (excludes header) */
    bool     free;
    struct block_header* next;
    struct block_header* prev;
} block_header_t;

static block_header_t* heap_start = NULL;
static size_t heap_total = 0;

void heap_init(void* start, size_t size) {
    heap_start = (block_header_t*)start;
    heap_start->magic = HEAP_MAGIC;
    heap_start->size  = size - sizeof(block_header_t);
    heap_start->free  = true;
    heap_start->next  = NULL;
    heap_start->prev  = NULL;
    heap_total = size;
}

static void split_block(block_header_t* block, size_t size) {
    /* Only split if remaining space is big enough for a new block */
    if (block->size < size + sizeof(block_header_t) + 16) return;

    block_header_t* new_block = (block_header_t*)((uint8_t*)block + sizeof(block_header_t) + size);
    new_block->magic = HEAP_MAGIC;
    new_block->size  = block->size - size - sizeof(block_header_t);
    new_block->free  = true;
    new_block->next  = block->next;
    new_block->prev  = block;

    if (block->next) block->next->prev = new_block;
    block->next = new_block;
    block->size = size;
}

static void merge_free(block_header_t* block) {
    /* Merge with next block if it's free */
    while (block->next && block->next->free) {
        block->size += sizeof(block_header_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }
}

void* kmalloc(size_t size) {
    if (!size) return NULL;
    /* Align to 8 bytes */
    size = (size + 7) & ~7;

    block_header_t* curr = heap_start;
    while (curr) {
        if (curr->free && curr->size >= size) {
            split_block(curr, size);
            curr->free = false;
            return (void*)((uint8_t*)curr + sizeof(block_header_t));
        }
        curr = curr->next;
    }
    return NULL;
}

void* kcalloc(size_t count, size_t size) {
    void* ptr = kmalloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_header_t* block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    if (block->magic != HEAP_MAGIC) {
        kprintf("kfree: corrupt block at %x!\n", (uint32_t)ptr);
        return;
    }
    block->free = true;

    /* Merge with neighbors */
    if (block->prev && block->prev->free) {
        block = block->prev;
    }
    merge_free(block);
}

size_t heap_free_space(void) {
    size_t total = 0;
    block_header_t* curr = heap_start;
    while (curr) {
        if (curr->free) total += curr->size;
        curr = curr->next;
    }
    return total;
}

size_t heap_used_space(void) {
    size_t total = 0;
    block_header_t* curr = heap_start;
    while (curr) {
        if (!curr->free) total += curr->size;
        curr = curr->next;
    }
    return total;
}

void heap_dump(void) {
    kprintf("  Heap blocks:\n");
    block_header_t* curr = heap_start;
    int i = 0;
    while (curr && i < 20) {
        kprintf("    [%d] addr=%x size=%u %s\n", i,
                (uint32_t)curr, curr->size,
                curr->free ? "FREE" : "USED");
        curr = curr->next;
        i++;
    }
    if (curr) kprintf("    ... (more blocks)\n");
    kprintf("  Free: %u bytes, Used: %u bytes\n", heap_free_space(), heap_used_space());
}
