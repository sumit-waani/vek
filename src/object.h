#ifndef VEK_OBJECT_H
#define VEK_OBJECT_H

#include "common.h"
#include "value.h"
#include "chunk.h"

// Object types
typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_BYTES,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
} ObjType;

// Object header flags
#define OBJ_FLAG_MARK  (1 << 0)
#define OBJ_FLAG_PIN   (1 << 1)
#define OBJ_FLAG_LARGE (1 << 2)

// Object header - all heap objects start with this
struct ObjHeader {
    uint8_t   type;    // ObjType
    uint8_t   flags;   // mark, pin, large bits
    uint32_t  size;    // total bytes including header
    uint32_t  hash;    // cached hash (used for strings)
    Page*     page;    // back-pointer for sweep
};

// ---- String ----
// Immutable UTF-8 string with flexible array member
struct ObjString {
    ObjHeader header;
    uint32_t  length;   // byte length (not including NUL)
    uint32_t  hash;     // FNV-1a hash
    char      data[];   // flexible array; NUL-terminated for convenience
};

// ---- List ----
// Dynamic array of Values
struct ObjList {
    ObjHeader header;
    uint32_t  length;
    uint32_t  capacity;
    Value*    data;
};

// ---- Map ----
// Open-addressing hash map with linear probing, string keys only
// Insertion-ordered via a separate entries array
typedef struct {
    ObjString* key;     // NULL = empty slot, tombstone uses special marker
    Value      value;
} MapEntry;

struct ObjMap {
    ObjHeader header;
    uint32_t  length;    // number of live entries
    uint32_t  capacity;  // size of entries array (power of 2)
    MapEntry* entries;
};

// ---- Bytes ----
// Raw byte buffer
struct ObjBytes {
    ObjHeader header;
    uint32_t  length;
    uint8_t   data[];
};

// ---- Function ----
// Compiled function with bytecode
struct ObjFunction {
    ObjHeader  header;
    uint8_t    arity;
    uint8_t    upvalue_count;
    Chunk      chunk;
    ObjString* name;        // function name (NULL for top-level script)
};

// ---- Closure ----
// Function + captured upvalues
struct ObjClosure {
    ObjHeader   header;
    ObjFunction* function;
    Value*       upvalues;
    uint8_t      upvalue_count;
};

// ---- String interning ----
#define INTERN_TABLE_INIT_CAP 64

typedef struct {
    ObjString** entries;
    uint32_t    count;
    uint32_t    capacity;
} InternTable;

extern InternTable intern_table;

void         intern_table_init(void);
void         intern_table_destroy(void);
ObjString*   intern_table_find(const char* chars, uint32_t length, uint32_t hash);
void         intern_table_insert(ObjString* str);
void         intern_table_remove_unmarked(void);

// ---- Object constructors ----
ObjString*   obj_string_new(const char* chars, uint32_t length);
ObjString*   obj_string_copy(const char* chars, uint32_t length);
ObjList*     obj_list_new(void);
void         obj_list_push(ObjList* list, Value value);
Value        obj_list_get(ObjList* list, uint32_t index);
ObjMap*      obj_map_new(void);
bool         obj_map_set(ObjMap* map, ObjString* key, Value value);
bool         obj_map_get(ObjMap* map, ObjString* key, Value* value);
bool         obj_map_delete(ObjMap* map, ObjString* key);
ObjBytes*    obj_bytes_new(const uint8_t* data, uint32_t length);
ObjFunction* obj_function_new(void);
ObjClosure*  obj_closure_new(ObjFunction* function);

// ---- Utility ----
uint32_t     hash_string(const char* key, uint32_t length);
void         obj_free(ObjHeader* obj);

// ---- Macros to cast Value to Object ----
#define OBJ_TYPE(value)   (((ObjHeader*)AS_PTR(value))->type)
#define IS_STRING(value)  (IS_PTR(value) && OBJ_TYPE(value) == OBJ_STRING)
#define IS_LIST(value)    (IS_PTR(value) && OBJ_TYPE(value) == OBJ_LIST)
#define IS_MAP(value)     (IS_PTR(value) && OBJ_TYPE(value) == OBJ_MAP)
#define AS_STRING(value)  ((ObjString*)AS_PTR(value))
#define AS_LIST(value)    ((ObjList*)AS_PTR(value))
#define AS_MAP(value)     ((ObjMap*)AS_PTR(value))
#define OBJ_VAL(obj)      PTR_VAL((void*)(obj))

#endif // VEK_OBJECT_H
