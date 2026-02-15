#ifndef HEAP_H
#define HEAP_H

#include "types.h"

void  heap_init(void* start, size_t size);
void* kmalloc(size_t size);
void* kcalloc(size_t count, size_t size);
void  kfree(void* ptr);
void  heap_dump(void);
size_t heap_free_space(void);
size_t heap_used_space(void);

#endif
