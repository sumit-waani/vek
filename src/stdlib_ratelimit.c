#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <time.h>

// Token bucket rate limiter
// Each key stores: tokens (int), last_refill (timestamp as int)

static ObjMap* ratelimit_store = NULL;

static void ensure_ratelimit_state(void) {
    if (!ratelimit_store) {
        ratelimit_store = obj_map_new();
        vm_pin((ObjHeader*)ratelimit_store);
    }
}

// ratelimit.check(key, max_tokens, refill_rate)
// refill_rate = tokens added per second
// Returns map: {allowed: bool, remaining: int}
static Value native_ratelimit_check(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_INT(args[1]) || !IS_INT(args[2])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);
    int64_t max_tokens = AS_INT(args[1]);
    int64_t refill_rate = AS_INT(args[2]);

    if (max_tokens <= 0 || refill_rate <= 0) return VAL_NIL;

    ensure_ratelimit_state();

    int64_t now = (int64_t)time(NULL);
    int64_t tokens;
    int64_t last_refill;

    // Look up existing bucket
    Value bucket_val;
    if (obj_map_get(ratelimit_store, key, &bucket_val) && IS_MAP(bucket_val)) {
        ObjMap* bucket = AS_MAP(bucket_val);

        // Get tokens
        ObjString* tokens_key = obj_string_new("tokens", 6);
        Value tokens_val;
        if (obj_map_get(bucket, tokens_key, &tokens_val) && IS_INT(tokens_val)) {
            tokens = AS_INT(tokens_val);
        } else {
            tokens = max_tokens;
        }

        // Get last_refill
        ObjString* last_key = obj_string_new("last_refill", 11);
        Value last_val;
        if (obj_map_get(bucket, last_key, &last_val) && IS_INT(last_val)) {
            last_refill = AS_INT(last_val);
        } else {
            last_refill = now;
        }

        // Refill tokens based on elapsed time
        int64_t elapsed = now - last_refill;
        if (elapsed > 0) {
            int64_t refill = elapsed * refill_rate;
            tokens = tokens + refill;
            if (tokens > max_tokens) tokens = max_tokens;
            last_refill = now;
        }
    } else {
        // New bucket - start full
        tokens = max_tokens;
        last_refill = now;
    }

    // Try to consume a token
    bool allowed = false;
    if (tokens > 0) {
        tokens--;
        allowed = true;
    }

    // Store updated bucket
    ObjMap* bucket = obj_map_new();
    gc_push_root(OBJ_VAL(bucket));

    ObjString* tokens_key = obj_string_new("tokens", 6);
    obj_map_set(bucket, tokens_key, INT_VAL(tokens));

    ObjString* last_key = obj_string_new("last_refill", 11);
    obj_map_set(bucket, last_key, INT_VAL(last_refill));

    obj_map_set(ratelimit_store, key, OBJ_VAL(bucket));

    gc_pop_root();

    // Build result map
    ObjMap* result = obj_map_new();
    gc_push_root(OBJ_VAL(result));

    ObjString* allowed_key = obj_string_new("allowed", 7);
    obj_map_set(result, allowed_key, allowed ? VAL_TRUE : VAL_FALSE);

    ObjString* remaining_key = obj_string_new("remaining", 9);
    obj_map_set(result, remaining_key, INT_VAL(tokens));

    gc_pop_root();
    return OBJ_VAL(result);
}

void stdlib_ratelimit_init(ObjMap* pkg) {
    stdlib_register(pkg, "check", native_ratelimit_check, 3);
}
