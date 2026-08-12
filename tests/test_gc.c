/*
 * Unit tests for the garbage collector.
 * Tests mark-and-sweep, pinning, and root management.
 */
#include "common.h"
#include "value.h"
#include "memory.h"
#include "gc.h"
#include "object.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  test: %s ... ", #name); \
    if (test_##name()) { tests_passed++; printf("ok\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    ASSERT FAILED: %s (line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

// Helper to count tracked objects
extern uint32_t gc_tracked_count(void);

// ---- GC tests ----

static bool test_gc_collect_unreachable(void) {
    // Create some objects without any roots
    gc.enabled = false;

    uint32_t before = gc_tracked_count();
    // Use unique strings to avoid interning collisions with other tests
    obj_string_new("gc_garbage_alpha", 16);
    obj_string_new("gc_garbage_beta", 15);
    obj_string_new("gc_garbage_gamma", 16);
    uint32_t after_alloc = gc_tracked_count();
    ASSERT(after_alloc == before + 3);

    // Now collect - nothing is rooted, so all new objects should be freed
    gc.enabled = true;
    gc_collect();

    uint32_t after_gc = gc_tracked_count();
    // All 3 new unreachable strings should be collected
    ASSERT(after_gc == before);

    return true;
}

static bool test_gc_preserve_pinned(void) {
    gc.enabled = false;

    uint32_t before = gc_tracked_count();
    ObjString* pinned = obj_string_new("gc_pinned_unique_str", 20);
    obj_string_new("gc_discard_pin_test", 19);

    vm_pin((ObjHeader*)pinned);

    gc.enabled = true;
    gc_collect();

    uint32_t after_gc = gc_tracked_count();
    // Only the pinned object should survive
    ASSERT(after_gc == before + 1);

    // Verify the pinned object is still valid
    ASSERT(strcmp(pinned->data, "gc_pinned_unique_str") == 0);

    vm_unpin((ObjHeader*)pinned);
    return true;
}

static bool test_gc_preserve_rooted(void) {
    // Clean up any leftover unreachable objects from previous tests
    gc.enabled = true;
    gc_collect();
    gc.enabled = false;

    uint32_t before = gc_tracked_count();
    ObjString* rooted = obj_string_new("gc_rooted_unique_x", 18);
    obj_string_new("gc_unrooted_unique_x", 20);

    // Push as a GC root
    gc_push_root(OBJ_VAL(rooted));

    gc.enabled = true;
    gc_collect();

    uint32_t after_gc = gc_tracked_count();
    // Only the rooted object should survive
    ASSERT(after_gc == before + 1);
    ASSERT(strcmp(rooted->data, "gc_rooted_unique_x") == 0);

    gc_pop_root();
    return true;
}

static bool test_gc_transitive_marking(void) {
    // Clean up any leftover unreachable objects from previous tests
    gc.enabled = true;
    gc_collect();
    gc.enabled = false;

    uint32_t before = gc_tracked_count();

    // Create a list that holds a string
    ObjList* list = obj_list_new();
    ObjString* str = obj_string_new("gc_in_list_trans_v2", 19);
    obj_list_push(list, OBJ_VAL(str));

    // Also create an unreachable object
    obj_string_new("gc_unreachable_tr_v2", 20);

    // Root only the list - the string should survive transitively
    gc_push_root(OBJ_VAL(list));

    gc.enabled = true;
    gc_collect();

    uint32_t after_gc = gc_tracked_count();
    // List + string should survive (2 objects), unreachable should be freed
    ASSERT(after_gc == before + 2);

    // Verify objects are still valid
    ASSERT(list->length == 1);
    ObjString* recovered = AS_STRING(obj_list_get(list, 0));
    ASSERT(strcmp(recovered->data, "gc_in_list_trans_v2") == 0);

    gc_pop_root();
    return true;
}

static bool test_gc_map_marking(void) {
    // Clean up any leftover unreachable objects from previous tests
    gc.enabled = true;
    gc_collect();
    gc.enabled = false;

    uint32_t before = gc_tracked_count();

    ObjMap* map = obj_map_new();
    ObjString* key = obj_string_new("gc_mapkey_v2", 12);
    ObjString* val_str = obj_string_new("gc_mapval_v2", 12);
    obj_map_set(map, key, OBJ_VAL(val_str));

    // Create unreachable
    obj_string_new("gc_unreachable_map_v2", 21);

    gc_push_root(OBJ_VAL(map));

    gc.enabled = true;
    gc_collect();

    uint32_t after_gc = gc_tracked_count();
    // Map + key string + value string = 3 objects should survive
    ASSERT(after_gc == before + 3);

    // Verify
    Value result;
    ASSERT(obj_map_get(map, key, &result));
    ASSERT(strcmp(AS_STRING(result)->data, "gc_mapval_v2") == 0);

    gc_pop_root();
    return true;
}

static bool test_gc_multiple_collections(void) {
    // Run multiple GC cycles and verify stability
    for (int cycle = 0; cycle < 5; cycle++) {
        gc.enabled = false;

        char buf[32];
        snprintf(buf, sizeof(buf), "gc_keep_multi_%d", cycle);
        ObjString* keep = obj_string_new(buf, (uint32_t)strlen(buf));

        snprintf(buf, sizeof(buf), "gc_disc_a_%d", cycle);
        obj_string_new(buf, (uint32_t)strlen(buf));
        snprintf(buf, sizeof(buf), "gc_disc_b_%d", cycle);
        obj_string_new(buf, (uint32_t)strlen(buf));

        vm_pin((ObjHeader*)keep);

        gc.enabled = true;
        gc_collect();

        // Verify pinned object survived
        snprintf(buf, sizeof(buf), "gc_keep_multi_%d", cycle);
        ASSERT(strcmp(keep->data, buf) == 0);

        vm_unpin((ObjHeader*)keep);

        // Collect again - now everything is unreachable
        gc_collect();
    }

    return true;
}

int main(void) {
    // Initialize subsystems
    gc_init();
    intern_table_init();
    heap_init();

    printf("=== Garbage Collector Tests ===\n");

    TEST(gc_collect_unreachable);
    TEST(gc_preserve_pinned);
    TEST(gc_preserve_rooted);
    TEST(gc_transitive_marking);
    TEST(gc_map_marking);
    TEST(gc_multiple_collections);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    // Cleanup
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
