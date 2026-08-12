#include "memory.h"
#include "gc.h"

// Global memory statistics
MemoryStats mem_stats = {0};

// Global heap
Heap heap = {0};

// ---- Low-level allocator wrappers ----

void* vek_alloc(size_t size) {
    if (size == 0) return NULL;
    mem_stats.bytes_allocated += size;
    mem_stats.total_allocations++;
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "vek: out of memory (requested %zu bytes)\n", size);
        exit(1);
    }
    return ptr;
}

void* vek_realloc(void* pointer, size_t old_size, size_t new_size) {
    mem_stats.bytes_allocated -= old_size;
    mem_stats.bytes_allocated += new_size;

    if (new_size == 0) {
        free(pointer);
        mem_stats.total_frees++;
        return NULL;
    }

    if (pointer == NULL) {
        mem_stats.total_allocations++;
    }

    void* result = realloc(pointer, new_size);
    if (!result) {
        fprintf(stderr, "vek: out of memory (realloc %zu bytes)\n", new_size);
        exit(1);
    }
    return result;
}

void vek_free(void* pointer, size_t size) {
    if (pointer == NULL) return;
    mem_stats.bytes_allocated -= size;
    mem_stats.total_frees++;
    free(pointer);
}

// ---- Page-based heap ----

Page* page_new(void) {
    Page* page = (Page*)malloc(sizeof(Page) + PAGE_SIZE);
    if (!page) {
        fprintf(stderr, "vek: out of memory (page allocation)\n");
        exit(1);
    }
    page->next = NULL;
    page->data = (uint8_t*)(page + 1);  // data follows the Page struct
    page->used = 0;
    page->capacity = PAGE_SIZE;
    page->full = false;
    return page;
}

void page_free(Page* page) {
    free(page);
}

void* page_alloc(Page* page, size_t size) {
    // Align to 16 bytes
    size_t aligned = (size + 15) & ~(size_t)15;

    if (page->used + aligned > page->capacity) {
        page->full = true;
        return NULL;
    }

    void* ptr = page->data + page->used;
    page->used += aligned;
    return ptr;
}

void heap_init(void) {
    heap.head = page_new();
    heap.current = heap.head;
}

void heap_destroy(void) {
    Page* page = heap.head;
    while (page) {
        Page* next = page->next;
        page_free(page);
        page = next;
    }
    heap.head = NULL;
    heap.current = NULL;
}

void* heap_alloc(size_t size) {
    // Try to trigger GC if threshold exceeded
    gc_maybe_collect();

    // Align to 16 bytes
    size_t aligned = (size + 15) & ~(size_t)15;

    // Try current page
    void* ptr = page_alloc(heap.current, aligned);
    if (ptr) {
        mem_stats.bytes_allocated += aligned;
        return ptr;
    }

    // Current page is full, allocate a new page
    Page* new_page = page_new();
    new_page->next = NULL;
    heap.current->next = new_page;
    heap.current = new_page;

    ptr = page_alloc(new_page, aligned);
    if (!ptr) {
        fprintf(stderr, "vek: object too large for page (%zu bytes)\n", size);
        exit(1);
    }
    mem_stats.bytes_allocated += aligned;
    return ptr;
}
