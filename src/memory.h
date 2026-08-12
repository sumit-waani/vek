#ifndef VEK_MEMORY_H
#define VEK_MEMORY_H

#include "common.h"

// Page size for heap allocation
#define PAGE_SIZE 16384  // 16 KB

// Page structure for bump allocation
struct Page {
    struct Page* next;
    uint8_t*    data;       // start of usable memory
    size_t      used;       // bytes used (bump pointer offset)
    size_t      capacity;   // usable capacity
    bool        full;       // switched to free-list mode
};

// Memory statistics
typedef struct {
    size_t bytes_allocated;
    size_t bytes_alive_after_last_gc;
    size_t total_allocations;
    size_t total_frees;
} MemoryStats;

// Global memory stats
extern MemoryStats mem_stats;

// Page management
Page* page_new(void);
void  page_free(Page* page);
void* page_alloc(Page* page, size_t size);

// Page list management
typedef struct {
    Page* head;
    Page* current;  // page currently being bumped into
} Heap;

extern Heap heap;

void  heap_init(void);
void  heap_destroy(void);
void* heap_alloc(size_t size);

#endif // VEK_MEMORY_H
