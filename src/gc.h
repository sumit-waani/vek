#ifndef VEK_GC_H
#define VEK_GC_H

#include "common.h"
#include "value.h"

// GC configuration
#define GC_HEAP_GROW_FACTOR 4  // Balanced: moderate frequency, moderate peak memory
#define GC_INITIAL_THRESHOLD (1024 * 256)  // 256 KB initial threshold

// GC state
struct GC {
    // Linked list of all allocated objects (for sweep)
    ObjHeader* objects;

    // Gray stack for mark phase
    ObjHeader** gray_stack;
    uint32_t    gray_count;
    uint32_t    gray_capacity;

    // Roots registered by C code
    Value*      roots;
    uint32_t    root_count;
    uint32_t    root_capacity;

    // Pinned objects (separate list for fast root marking)
    ObjHeader** pinned_objects;
    uint32_t    pinned_count;
    uint32_t    pinned_cap;

    // Threshold for triggering collection
    size_t      next_gc;
    bool        enabled;
};

extern GC gc;

// GC lifecycle
void gc_init(void);
void gc_destroy(void);

// Collection
void gc_collect(void);
void gc_maybe_collect(void);

// Object tracking
void gc_track_object(ObjHeader* obj);

// Mark phase helpers
void gc_mark_value(Value value);
void gc_mark_object(ObjHeader* obj);

// Root management (for C code holding references)
void gc_push_root(Value value);
void gc_pop_root(void);

// Pin/unpin (prevent collection while C code holds a reference)
void vm_pin(ObjHeader* obj);
void vm_unpin(ObjHeader* obj);

#endif // VEK_GC_H
