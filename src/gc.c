#include "gc.h"
#include "memory.h"
#include "object.h"

// Global GC state
GC gc = {0};

// File-local object tracking array
static ObjHeader** tracked_objects = NULL;
static uint32_t tracked_count = 0;
static uint32_t tracked_cap = 0;

void gc_init(void) {
    gc.objects = NULL;
    gc.gray_stack = NULL;
    gc.gray_count = 0;
    gc.gray_capacity = 0;
    gc.roots = NULL;
    gc.root_count = 0;
    gc.root_capacity = 0;
    gc.next_gc = GC_INITIAL_THRESHOLD;
    gc.enabled = true;

    tracked_objects = NULL;
    tracked_count = 0;
    tracked_cap = 0;
}

void gc_destroy(void) {
    // Free all tracked objects
    for (uint32_t i = 0; i < tracked_count; i++) {
        if (tracked_objects[i]) {
            obj_free(tracked_objects[i]);
        }
    }
    free(tracked_objects);
    tracked_objects = NULL;
    tracked_count = 0;
    tracked_cap = 0;

    free(gc.gray_stack);
    gc.gray_stack = NULL;
    gc.gray_count = 0;
    gc.gray_capacity = 0;

    free(gc.roots);
    gc.roots = NULL;
    gc.root_count = 0;
    gc.root_capacity = 0;
}

// Track a newly allocated object for GC
void gc_track_object(ObjHeader* obj) {
    if (tracked_count >= tracked_cap) {
        tracked_cap = tracked_cap < 256 ? 256 : tracked_cap * 2;
        tracked_objects = (ObjHeader**)realloc(tracked_objects,
                          sizeof(ObjHeader*) * tracked_cap);
        if (!tracked_objects) {
            fprintf(stderr, "vek: gc tracking out of memory\n");
            exit(1);
        }
    }
    tracked_objects[tracked_count++] = obj;
}

// Accessors for tracked objects (used by tests)
uint32_t gc_tracked_count(void) {
    return tracked_count;
}

ObjHeader** gc_tracked_objects_array(void) {
    return tracked_objects;
}

// Mark a value (if it is a pointer, mark the object)
void gc_mark_value(Value value) {
    if (IS_PTR(value)) {
        gc_mark_object((ObjHeader*)AS_PTR(value));
    }
}

// Mark an object (set mark bit, push to gray stack for tracing)
void gc_mark_object(ObjHeader* obj) {
    if (obj == NULL) return;
    if (obj->flags & OBJ_FLAG_MARK) return;  // already marked

    obj->flags |= OBJ_FLAG_MARK;

    // Push to gray stack for transitive marking
    if (gc.gray_count >= gc.gray_capacity) {
        gc.gray_capacity = gc.gray_capacity < 64 ? 64 : gc.gray_capacity * 2;
        gc.gray_stack = (ObjHeader**)realloc(gc.gray_stack,
                        sizeof(ObjHeader*) * gc.gray_capacity);
        if (!gc.gray_stack) {
            fprintf(stderr, "vek: gc gray stack out of memory\n");
            exit(1);
        }
    }
    gc.gray_stack[gc.gray_count++] = obj;
}

// Trace references from a gray object (follow its pointers)
static void gc_trace_object(ObjHeader* obj) {
    switch (obj->type) {
        case OBJ_STRING:
        case OBJ_BYTES:
        case OBJ_FUNCTION:
            // Leaf objects: no outgoing heap references
            break;

        case OBJ_LIST: {
            ObjList* list = (ObjList*)obj;
            for (uint32_t i = 0; i < list->length; i++) {
                gc_mark_value(list->data[i]);
            }
            break;
        }

        case OBJ_MAP: {
            ObjMap* map = (ObjMap*)obj;
            for (uint32_t i = 0; i < map->capacity; i++) {
                if (map->entries[i].key != NULL) {
                    gc_mark_object((ObjHeader*)map->entries[i].key);
                    gc_mark_value(map->entries[i].value);
                }
            }
            break;
        }

        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            gc_mark_object((ObjHeader*)closure->function);
            for (uint8_t i = 0; i < closure->upvalue_count; i++) {
                gc_mark_value(closure->upvalues[i]);
            }
            break;
        }

        case OBJ_UPVALUE: {
            // Mark the closed-over value
            // We need to include vm.h for this but ObjUpvalue is defined there.
            // For now, the upvalue's closed value is marked via its Value stored in closure.
            break;
        }

        case OBJ_NATIVE:
            // Leaf object
            break;

        case OBJ_BOUND_METHOD:
            // Bound method has a receiver value that may be a heap object
            break;
    }
}

// Trace all gray objects until gray stack is empty
static void gc_trace_references(void) {
    while (gc.gray_count > 0) {
        ObjHeader* obj = gc.gray_stack[--gc.gray_count];
        gc_trace_object(obj);
    }
}

// Mark all roots
static void gc_mark_roots(void) {
    // Mark pinned objects
    for (uint32_t i = 0; i < tracked_count; i++) {
        if (tracked_objects[i] && (tracked_objects[i]->flags & OBJ_FLAG_PIN)) {
            gc_mark_object(tracked_objects[i]);
        }
    }

    // Mark C-held root values
    for (uint32_t i = 0; i < gc.root_count; i++) {
        gc_mark_value(gc.roots[i]);
    }
}

// Sweep: free all unmarked objects, compact tracked array
static void gc_sweep(void) {
    uint32_t write = 0;
    for (uint32_t i = 0; i < tracked_count; i++) {
        ObjHeader* obj = tracked_objects[i];
        if (obj == NULL) continue;

        if (obj->flags & OBJ_FLAG_MARK) {
            // Object is alive - unmark for next cycle
            obj->flags &= ~OBJ_FLAG_MARK;
            tracked_objects[write++] = obj;
        } else {
            // Object is unreachable - free it
            obj_free(obj);
        }
    }
    tracked_count = write;
}

// Run a full GC cycle
void gc_collect(void) {
    if (!gc.enabled) return;

    // Mark phase
    gc_mark_roots();
    gc_trace_references();

    // Remove unmarked interned strings
    intern_table_remove_unmarked();

    // Sweep phase
    gc_sweep();

    // Update threshold for next collection
    mem_stats.bytes_alive_after_last_gc = mem_stats.bytes_allocated;
    gc.next_gc = mem_stats.bytes_alive_after_last_gc * GC_HEAP_GROW_FACTOR;
    if (gc.next_gc < GC_INITIAL_THRESHOLD) {
        gc.next_gc = GC_INITIAL_THRESHOLD;
    }
}

// Check if collection threshold is exceeded
void gc_maybe_collect(void) {
    if (!gc.enabled) return;
    if (mem_stats.bytes_allocated > gc.next_gc) {
        gc_collect();
    }
}

// Root management for C code holding heap references
void gc_push_root(Value value) {
    if (gc.root_count >= gc.root_capacity) {
        gc.root_capacity = gc.root_capacity < 16 ? 16 : gc.root_capacity * 2;
        gc.roots = (Value*)realloc(gc.roots, sizeof(Value) * gc.root_capacity);
        if (!gc.roots) {
            fprintf(stderr, "vek: gc root stack out of memory\n");
            exit(1);
        }
    }
    gc.roots[gc.root_count++] = value;
}

void gc_pop_root(void) {
    assert(gc.root_count > 0);
    gc.root_count--;
}

// Pin/unpin: prevent an object from being collected
void vm_pin(ObjHeader* obj) {
    if (obj) obj->flags |= OBJ_FLAG_PIN;
}

void vm_unpin(ObjHeader* obj) {
    if (obj) obj->flags &= ~OBJ_FLAG_PIN;
}
