#include "object.h"
#include "memory.h"
#include "gc.h"
#include "chunk.h"

// ---- String Interning Table ----

InternTable intern_table = {0};

void intern_table_init(void) {
    intern_table.count = 0;
    intern_table.capacity = INTERN_TABLE_INIT_CAP;
    intern_table.entries = (ObjString**)calloc(intern_table.capacity, sizeof(ObjString*));
    if (!intern_table.entries) {
        fprintf(stderr, "vek: intern table out of memory\n");
        exit(1);
    }
}

void intern_table_destroy(void) {
    free(intern_table.entries);
    intern_table.entries = NULL;
    intern_table.count = 0;
    intern_table.capacity = 0;
}

// FNV-1a hash
uint32_t hash_string(const char* key, uint32_t length) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}

// Find an existing interned string
ObjString* intern_table_find(const char* chars, uint32_t length, uint32_t hash) {
    if (intern_table.capacity == 0) return NULL;

    uint32_t index = hash & (intern_table.capacity - 1);
    for (;;) {
        ObjString* entry = intern_table.entries[index];
        if (entry == NULL) {
            return NULL;
        }
        if (entry->length == length && entry->hash == hash &&
            memcmp(entry->data, chars, length) == 0) {
            return entry;
        }
        index = (index + 1) & (intern_table.capacity - 1);
    }
}

// Grow and rehash the intern table
static void intern_table_grow(void) {
    uint32_t new_cap = intern_table.capacity * 2;
    ObjString** new_entries = (ObjString**)calloc(new_cap, sizeof(ObjString*));
    if (!new_entries) {
        fprintf(stderr, "vek: intern table grow out of memory\n");
        exit(1);
    }

    // Rehash all entries
    for (uint32_t i = 0; i < intern_table.capacity; i++) {
        ObjString* entry = intern_table.entries[i];
        if (entry == NULL) continue;
        uint32_t idx = entry->hash & (new_cap - 1);
        while (new_entries[idx] != NULL) {
            idx = (idx + 1) & (new_cap - 1);
        }
        new_entries[idx] = entry;
    }

    free(intern_table.entries);
    intern_table.entries = new_entries;
    intern_table.capacity = new_cap;
}

// Insert a string into the intern table
void intern_table_insert(ObjString* str) {
    // Grow if load factor > 0.75
    if (intern_table.count + 1 > intern_table.capacity * 3 / 4) {
        intern_table_grow();
    }

    uint32_t index = str->hash & (intern_table.capacity - 1);
    while (intern_table.entries[index] != NULL) {
        index = (index + 1) & (intern_table.capacity - 1);
    }
    intern_table.entries[index] = str;
    intern_table.count++;
}

// Remove unmarked strings from the intern table (called during GC sweep)
void intern_table_remove_unmarked(void) {
    for (uint32_t i = 0; i < intern_table.capacity; i++) {
        ObjString* entry = intern_table.entries[i];
        if (entry != NULL && !(entry->header.flags & OBJ_FLAG_MARK)) {
            intern_table.entries[i] = NULL;
            intern_table.count--;
        }
    }
}

// ---- Object Constructors ----

// Allocate a raw object of given size
static ObjHeader* alloc_object(size_t size, ObjType type) {
    ObjHeader* obj = (ObjHeader*)vek_alloc(size);
    obj->type = (uint8_t)type;
    obj->flags = 0;
    obj->size = (uint32_t)size;
    obj->hash = 0;
    obj->page = NULL;  // Not using page-based alloc for v1 simplicity

    gc_track_object(obj);
    return obj;
}

// Create a new string (interned)
ObjString* obj_string_new(const char* chars, uint32_t length) {
    uint32_t hash = hash_string(chars, length);

    // Check if already interned
    ObjString* interned = intern_table_find(chars, length, hash);
    if (interned) return interned;

    // Allocate new string
    size_t size = sizeof(ObjString) + length + 1;
    ObjString* str = (ObjString*)alloc_object(size, OBJ_STRING);
    str->length = length;
    str->hash = hash;
    memcpy(str->data, chars, length);
    str->data[length] = '\0';
    str->header.hash = hash;

    // Intern it
    intern_table_insert(str);
    return str;
}

// Copy a string (same as new for interned strings)
ObjString* obj_string_copy(const char* chars, uint32_t length) {
    return obj_string_new(chars, length);
}

// Create a new empty list
ObjList* obj_list_new(void) {
    ObjList* list = (ObjList*)alloc_object(sizeof(ObjList), OBJ_LIST);
    list->length = 0;
    list->capacity = 0;
    list->data = NULL;
    return list;
}

// Push a value onto a list
void obj_list_push(ObjList* list, Value value) {
    if (list->length >= list->capacity) {
        uint32_t new_cap = GROW_CAPACITY(list->capacity);
        list->data = (Value*)vek_realloc(list->data,
                     sizeof(Value) * list->capacity,
                     sizeof(Value) * new_cap);
        list->capacity = new_cap;
    }
    list->data[list->length++] = value;
}

// Get a value from a list by index
Value obj_list_get(ObjList* list, uint32_t index) {
    assert(index < list->length);
    return list->data[index];
}

// Create a new empty map
ObjMap* obj_map_new(void) {
    ObjMap* map = (ObjMap*)alloc_object(sizeof(ObjMap), OBJ_MAP);
    map->length = 0;
    map->capacity = 0;
    map->entries = NULL;
    return map;
}

// Grow and rehash the map
static void map_grow(ObjMap* map) {
    uint32_t new_cap = map->capacity < 8 ? 8 : map->capacity * 2;
    MapEntry* new_entries = (MapEntry*)vek_alloc(sizeof(MapEntry) * new_cap);
    memset(new_entries, 0, sizeof(MapEntry) * new_cap);

    // Rehash existing entries
    if (map->entries) {
        for (uint32_t i = 0; i < map->capacity; i++) {
            MapEntry* entry = &map->entries[i];
            if (entry->key == NULL) continue;

            uint32_t idx = entry->key->hash & (new_cap - 1);
            while (new_entries[idx].key != NULL) {
                idx = (idx + 1) & (new_cap - 1);
            }
            new_entries[idx] = *entry;
        }
        vek_free(map->entries, sizeof(MapEntry) * map->capacity);
    }

    map->entries = new_entries;
    map->capacity = new_cap;
}

// Set a key-value pair in the map. Returns true if key was new.
bool obj_map_set(ObjMap* map, ObjString* key, Value value) {
    // Grow if load factor > 0.75
    if (map->length + 1 > map->capacity * 3 / 4) {
        map_grow(map);
    }

    uint32_t index = key->hash & (map->capacity - 1);
    for (;;) {
        MapEntry* entry = &map->entries[index];
        if (entry->key == NULL) {
            // Empty slot - insert new entry
            entry->key = key;
            entry->value = value;
            map->length++;
            return true;
        }
        if (entry->key == key) {
            // Same interned string - update value
            entry->value = value;
            return false;
        }
        index = (index + 1) & (map->capacity - 1);
    }
}

// Get a value by key. Returns true if found.
bool obj_map_get(ObjMap* map, ObjString* key, Value* value) {
    if (map->capacity == 0) return false;

    uint32_t index = key->hash & (map->capacity - 1);
    for (;;) {
        MapEntry* entry = &map->entries[index];
        if (entry->key == NULL) {
            return false;
        }
        if (entry->key == key) {
            *value = entry->value;
            return true;
        }
        index = (index + 1) & (map->capacity - 1);
    }
}

// Delete a key from the map. Returns true if found and deleted.
// Uses tombstone approach (set key to NULL but don't break probe chain).
bool obj_map_delete(ObjMap* map, ObjString* key) {
    if (map->capacity == 0) return false;

    uint32_t index = key->hash & (map->capacity - 1);
    for (;;) {
        MapEntry* entry = &map->entries[index];
        if (entry->key == NULL) {
            return false;
        }
        if (entry->key == key) {
            // Tombstone: clear the key but leave a marker
            // For simplicity in v1, we just NULL the key and decrement length
            entry->key = NULL;
            entry->value = VAL_NIL;
            map->length--;
            return true;
        }
        index = (index + 1) & (map->capacity - 1);
    }
}

// Create a new bytes object
ObjBytes* obj_bytes_new(const uint8_t* data, uint32_t length) {
    size_t size = sizeof(ObjBytes) + length;
    ObjBytes* bytes = (ObjBytes*)alloc_object(size, OBJ_BYTES);
    bytes->length = length;
    if (data && length > 0) {
        memcpy(bytes->data, data, length);
    }
    return bytes;
}

// Create a new function object
ObjFunction* obj_function_new(void) {
    ObjFunction* func = (ObjFunction*)alloc_object(sizeof(ObjFunction), OBJ_FUNCTION);
    func->arity = 0;
    func->upvalue_count = 0;
    func->name = NULL;
    chunk_init(&func->chunk);
    return func;
}

// Create a new closure
ObjClosure* obj_closure_new(ObjFunction* function) {
    uint8_t count = function->upvalue_count;
    Value* upvalues = NULL;
    if (count > 0) {
        upvalues = (Value*)vek_alloc(sizeof(Value) * count);
        for (uint8_t i = 0; i < count; i++) {
            upvalues[i] = VAL_NIL;
        }
    }

    ObjClosure* closure = (ObjClosure*)alloc_object(sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalue_count = count;
    return closure;
}

// Free an object and its associated memory
void obj_free(ObjHeader* obj) {
    switch (obj->type) {
        case OBJ_STRING: {
            ObjString* str = (ObjString*)obj;
            vek_free(obj, sizeof(ObjString) + str->length + 1);
            return;
        }
        case OBJ_LIST: {
            ObjList* list = (ObjList*)obj;
            if (list->data) {
                vek_free(list->data, sizeof(Value) * list->capacity);
            }
            vek_free(obj, sizeof(ObjList));
            return;
        }
        case OBJ_MAP: {
            ObjMap* map = (ObjMap*)obj;
            if (map->entries) {
                vek_free(map->entries, sizeof(MapEntry) * map->capacity);
            }
            vek_free(obj, sizeof(ObjMap));
            return;
        }
        case OBJ_BYTES: {
            ObjBytes* bytes = (ObjBytes*)obj;
            vek_free(obj, sizeof(ObjBytes) + bytes->length);
            return;
        }
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            chunk_free(&func->chunk);
            vek_free(obj, sizeof(ObjFunction));
            return;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            if (closure->upvalues) {
                vek_free(closure->upvalues, sizeof(Value) * closure->upvalue_count);
            }
            vek_free(obj, sizeof(ObjClosure));
            return;
        }
    }
}
