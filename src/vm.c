/*
 * vek VM - Stack-based bytecode interpreter with computed GOTO dispatch.
 *
 * Key design decisions:
 * - Computed GOTO (GCC/Clang extension) for fast opcode dispatch
 * - Stack-based: locals accessed by slot index relative to frame base
 * - NaN-boxed values on the stack
 * - int / int always returns float (7 / 2 = 3.5)
 * - Only nil and false are falsy
 * - Closures capture by reference via ObjUpvalue
 */

#include "vm.h"
#include "compiler.h"
#include "memory.h"
#include "gc.h"
#include "debug.h"

#include <math.h>
#include <ctype.h>

// Global VM instance
VM vm;

// ---- Forward declarations ----
static InterpretResult run(void);
static void define_native(const char* name, NativeFn function, int arity);
static void runtime_error(const char* format, ...);
static bool call_value(Value callee, int arg_count);
static bool call_closure(ObjClosure* closure, int arg_count);
static ObjUpvalue* capture_upvalue(Value* local);
static void close_upvalues(Value* last);
static ObjString* value_to_string(Value value);
static ObjString* value_to_string_depth(Value value, int depth);

// ---- Native function implementations ----

static Value native_print(int arg_count, Value* args) {
    for (int i = 0; i < arg_count; i++) {
        if (i > 0) printf(" ");
        ObjString* s = value_to_string(args[i]);
        printf("%.*s", (int)s->length, s->data);
    }
    printf("\n");
    return VAL_NIL;
}

static Value native_puts(int arg_count, Value* args) {
    return native_print(arg_count, args);
}

static Value native_type_of(int arg_count, Value* args) {
    (void)arg_count;
    Value v = args[0];
    const char* type_name;
    if (IS_NIL(v)) type_name = "nil";
    else if (IS_BOOL(v)) type_name = "bool";
    else if (IS_INT(v)) type_name = "int";
    else if (IS_FLOAT(v)) type_name = "float";
    else if (IS_PTR(v)) {
        switch (OBJ_TYPE(v)) {
            case OBJ_STRING:   type_name = "string"; break;
            case OBJ_LIST:     type_name = "list"; break;
            case OBJ_MAP:      type_name = "map"; break;
            case OBJ_FUNCTION:
            case OBJ_CLOSURE:
            case OBJ_NATIVE:   type_name = "function"; break;
            default:           type_name = "object"; break;
        }
    } else {
        type_name = "unknown";
    }
    return OBJ_VAL(obj_string_new(type_name, (uint32_t)strlen(type_name)));
}

static Value native_to_s(int arg_count, Value* args) {
    (void)arg_count;
    ObjString* s = value_to_string(args[0]);
    return OBJ_VAL(s);
}

static Value native_to_i(int arg_count, Value* args) {
    (void)arg_count;
    Value v = args[0];
    if (IS_INT(v)) return v;
    if (IS_FLOAT(v)) return INT_VAL((int64_t)AS_DOUBLE(v));
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        char* end;
        long long val = strtoll(s->data, &end, 10);
        if (end == s->data) return INT_VAL(0);
        return INT_VAL((int64_t)val);
    }
    if (IS_BOOL(v)) return INT_VAL(AS_BOOL(v) ? 1 : 0);
    return INT_VAL(0);
}

static Value native_to_f(int arg_count, Value* args) {
    (void)arg_count;
    Value v = args[0];
    if (IS_FLOAT(v)) return v;
    if (IS_INT(v)) return FLOAT_VAL((double)AS_INT(v));
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        char* end;
        double val = strtod(s->data, &end);
        if (end == s->data) return FLOAT_VAL(0.0);
        return FLOAT_VAL(val);
    }
    if (IS_BOOL(v)) return FLOAT_VAL(AS_BOOL(v) ? 1.0 : 0.0);
    return FLOAT_VAL(0.0);
}

static Value native_len(int arg_count, Value* args) {
    (void)arg_count;
    Value v = args[0];
    if (IS_STRING(v)) return INT_VAL((int64_t)AS_STRING(v)->length);
    if (IS_LIST(v)) return INT_VAL((int64_t)AS_LIST(v)->length);
    if (IS_MAP(v)) return INT_VAL((int64_t)AS_MAP(v)->length);
    return INT_VAL(0);
}

// ---- VM Initialization ----

void vm_init(void) {
    vm.stack_top = vm.stack;
    vm.frame_count = 0;
    vm.open_upvalues = NULL;
    vm.handler_count = 0;
    vm.had_error = false;
    vm.error_msg[0] = '\0';

    // Create globals table
    vm.globals = obj_map_new();
    vm_pin((ObjHeader*)vm.globals);

    // Register native functions
    define_native("print", native_print, -1);
    define_native("puts", native_puts, -1);
    define_native("type_of", native_type_of, 1);
    define_native("to_s", native_to_s, 1);
    define_native("to_i", native_to_i, 1);
    define_native("to_f", native_to_f, 1);
    define_native("len", native_len, 1);
}

void vm_free(void) {
    if (vm.globals) {
        vm_unpin((ObjHeader*)vm.globals);
    }
    vm.globals = NULL;
    vm.open_upvalues = NULL;
    vm.stack_top = vm.stack;
    vm.frame_count = 0;
    vm.handler_count = 0;
}

// ---- Stack operations ----

void vm_push(Value value) {
    if (vm.stack_top >= vm.stack + STACK_MAX) {
        fprintf(stderr, "Runtime error: Stack overflow.\n");
        vm.had_error = true;
        return;
    }
    *vm.stack_top = value;
    vm.stack_top++;
}

Value vm_pop(void) {
    vm.stack_top--;
    return *vm.stack_top;
}

Value vm_peek(int distance) {
    return vm.stack_top[-1 - distance];
}

// ---- Helper: define a native function ----

static void define_native(const char* name, NativeFn function, int arity) {
    ObjNative* native = (ObjNative*)vek_alloc(sizeof(ObjNative));
    native->header.type = OBJ_NATIVE;
    native->header.flags = OBJ_FLAG_PIN;
    native->header.size = sizeof(ObjNative);
    native->header.hash = 0;
    native->header.page = NULL;
    native->function = function;
    native->name = name;
    native->arity = arity;
    gc_track_object((ObjHeader*)native);

    ObjString* name_str = obj_string_new(name, (uint32_t)strlen(name));
    obj_map_set(vm.globals, name_str, OBJ_VAL(native));
}

// ---- Helper: convert value to string ----

#define VALUE_TO_STRING_MAX_DEPTH 16

static ObjString* value_to_string_depth(Value value, int depth) {
    if (IS_STRING(value)) return AS_STRING(value);

    char buffer[256];
    int len = 0;

    if (depth > VALUE_TO_STRING_MAX_DEPTH) {
        len = snprintf(buffer, sizeof(buffer), "...");
        return obj_string_new(buffer, (uint32_t)len);
    }

    if (IS_NIL(value)) {
        len = snprintf(buffer, sizeof(buffer), "nil");
    } else if (IS_BOOL(value)) {
        len = snprintf(buffer, sizeof(buffer), "%s", AS_BOOL(value) ? "true" : "false");
    } else if (IS_INT(value)) {
        len = snprintf(buffer, sizeof(buffer), "%lld", (long long)AS_INT(value));
    } else if (IS_FLOAT(value)) {
        double d = AS_DOUBLE(value);
        // Print with minimal necessary decimal places
        if (d == (double)(long long)d && fabs(d) < 1e15) {
            len = snprintf(buffer, sizeof(buffer), "%.1f", d);
        } else {
            len = snprintf(buffer, sizeof(buffer), "%g", d);
        }
    } else if (IS_PTR(value)) {
        ObjHeader* obj = (ObjHeader*)AS_PTR(value);
        switch (obj->type) {
            case OBJ_FUNCTION: {
                ObjFunction* fn = (ObjFunction*)obj;
                if (fn->name) {
                    len = snprintf(buffer, sizeof(buffer), "<fn %.*s>",
                                   (int)fn->name->length, fn->name->data);
                } else {
                    len = snprintf(buffer, sizeof(buffer), "<script>");
                }
                break;
            }
            case OBJ_CLOSURE: {
                ObjClosure* cl = (ObjClosure*)obj;
                if (cl->function->name) {
                    len = snprintf(buffer, sizeof(buffer), "<fn %.*s>",
                                   (int)cl->function->name->length,
                                   cl->function->name->data);
                } else {
                    len = snprintf(buffer, sizeof(buffer), "<script>");
                }
                break;
            }
            case OBJ_NATIVE: {
                ObjNative* nat = (ObjNative*)obj;
                len = snprintf(buffer, sizeof(buffer), "<native %s>", nat->name);
                break;
            }
            case OBJ_LIST: {
                ObjList* list = (ObjList*)obj;
                // Build a simple representation
                len = snprintf(buffer, sizeof(buffer), "[");
                for (uint32_t i = 0; i < list->length && len < 240; i++) {
                    if (i > 0) len += snprintf(buffer + len, sizeof(buffer) - (size_t)len, ", ");
                    ObjString* elem = value_to_string_depth(list->data[i], depth + 1);
                    if (IS_STRING(list->data[i])) {
                        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                                       "\"%.*s\"", (int)elem->length, elem->data);
                    } else {
                        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                                       "%.*s", (int)elem->length, elem->data);
                    }
                }
                len += snprintf(buffer + len, sizeof(buffer) - (size_t)len, "]");
                break;
            }
            case OBJ_MAP: {
                ObjMap* map = (ObjMap*)obj;
                len = snprintf(buffer, sizeof(buffer), "{");
                bool first = true;
                for (uint32_t i = 0; i < map->capacity && len < 240; i++) {
                    if (map->entries[i].key == NULL || map->entries[i].key == MAP_TOMBSTONE) continue;
                    if (!first) len += snprintf(buffer + len, sizeof(buffer) - (size_t)len, ", ");
                    first = false;
                    len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                                   "%.*s: ", (int)map->entries[i].key->length,
                                   map->entries[i].key->data);
                    ObjString* val = value_to_string_depth(map->entries[i].value, depth + 1);
                    len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                                   "%.*s", (int)val->length, val->data);
                }
                len += snprintf(buffer + len, sizeof(buffer) - (size_t)len, "}");
                break;
            }
            default:
                len = snprintf(buffer, sizeof(buffer), "<object>");
                break;
        }
    } else {
        len = snprintf(buffer, sizeof(buffer), "???");
    }

    return obj_string_new(buffer, (uint32_t)len);
}

static ObjString* value_to_string(Value value) {
    return value_to_string_depth(value, 0);
}

// ---- Runtime errors ----

#include <stdarg.h>

static void runtime_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(vm.error_msg, sizeof(vm.error_msg), format, args);
    va_end(args);

    vm.had_error = true;

    // Print error with stack trace
    fprintf(stderr, "Runtime error: %s\n", vm.error_msg);
    for (int i = vm.frame_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* function = frame->closure->function;
        size_t instruction = (size_t)(frame->ip - function->chunk.code - 1);
        int line = function->chunk.lines[instruction];
        fprintf(stderr, "  [line %d] in ", line);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%.*s()\n", (int)function->name->length, function->name->data);
        }
    }
}

// ---- Upvalue management ----

static ObjUpvalue* new_upvalue(Value* slot) {
    ObjUpvalue* upvalue = (ObjUpvalue*)vek_alloc(sizeof(ObjUpvalue));
    upvalue->header.type = OBJ_UPVALUE;
    upvalue->header.flags = 0;
    upvalue->header.size = sizeof(ObjUpvalue);
    upvalue->header.hash = 0;
    upvalue->header.page = NULL;
    upvalue->location = slot;
    upvalue->closed = VAL_NIL;
    upvalue->next = NULL;
    gc_track_object((ObjHeader*)upvalue);
    return upvalue;
}

static ObjUpvalue* capture_upvalue(Value* local) {
    ObjUpvalue* prev_upvalue = NULL;
    ObjUpvalue* upvalue = vm.open_upvalues;

    // Walk the list looking for an existing upvalue for this slot.
    // Open upvalues are sorted by stack location (descending).
    while (upvalue != NULL && upvalue->location > local) {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }

    // If we found an existing upvalue for this slot, reuse it
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    // Create a new upvalue and insert it into the sorted list
    ObjUpvalue* created = new_upvalue(local);
    created->next = upvalue;

    if (prev_upvalue == NULL) {
        vm.open_upvalues = created;
    } else {
        prev_upvalue->next = created;
    }

    return created;
}

static void close_upvalues(Value* last) {
    while (vm.open_upvalues != NULL && vm.open_upvalues->location >= last) {
        ObjUpvalue* upvalue = vm.open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.open_upvalues = upvalue->next;
    }
}

// ---- Function calls ----

static bool call_closure(ObjClosure* closure, int arg_count) {
    if (arg_count != closure->function->arity) {
        runtime_error("Expected %d arguments but got %d.",
                     closure->function->arity, arg_count);
        return false;
    }

    if (vm.frame_count == FRAMES_MAX) {
        runtime_error("Stack overflow.");
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frame_count++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stack_top - arg_count - 1;
    return true;
}

static bool call_value(Value callee, int arg_count) {
    if (IS_PTR(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_CLOSURE:
                return call_closure(AS_CLOSURE(callee), arg_count);
            case OBJ_NATIVE: {
                ObjNative* native = AS_NATIVE(callee);
                if (native->arity != -1 && arg_count != native->arity) {
                    runtime_error("Expected %d arguments but got %d.",
                                 native->arity, arg_count);
                    return false;
                }
                Value result = native->function(arg_count, vm.stack_top - arg_count);
                vm.stack_top -= arg_count + 1;
                vm_push(result);
                return true;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = (ObjBoundMethod*)AS_PTR(callee);
                if (bound->arity != -1 && arg_count != bound->arity) {
                    runtime_error("Expected %d arguments but got %d.",
                                 bound->arity, arg_count);
                    return false;
                }
                Value result = bound->method(bound->receiver, arg_count, vm.stack_top - arg_count);
                vm.stack_top -= arg_count + 1;
                vm_push(result);
                return true;
            }
            default:
                break;
        }
    }
    runtime_error("Can only call functions and closures.");
    return false;
}

// ---- String concatenation helper ----

static ObjString* concatenate_strings(ObjString* a, ObjString* b) {
    uint32_t length = a->length + b->length;
    char* chars = (char*)malloc(length + 1);
    memcpy(chars, a->data, a->length);
    memcpy(chars + a->length, b->data, b->length);
    chars[length] = '\0';
    ObjString* result = obj_string_new(chars, length);
    free(chars);
    return result;
}

// ---- List/Map iterator support ----

// We use a simple ObjList with 3 elements as an iterator:
// [0] = the collection being iterated
// [1] = current index (as int)
// [2] = marker to identify it as an iterator (special int value)
#define ITER_MARKER_VAL INT_VAL(0x1TERA)

static Value make_iterator(Value iterable) {
    ObjList* iter = obj_list_new();
    gc_push_root(OBJ_VAL(iter));
    obj_list_push(iter, iterable);        // [0] = collection
    obj_list_push(iter, INT_VAL(0));      // [1] = index
    obj_list_push(iter, INT_VAL(-9999));  // [2] = marker
    gc_pop_root();
    return OBJ_VAL(iter);
}

// Advance iterator. Returns true if there is a next value (pushed on stack).
// Returns false if iteration is done.
static bool iterator_next(Value iter_val, Value* out) {
    if (!IS_LIST(iter_val)) return false;
    ObjList* iter = AS_LIST(iter_val);
    if (iter->length < 3) return false;

    Value collection = iter->data[0];
    int64_t index = AS_INT(iter->data[1]);

    if (IS_LIST(collection)) {
        ObjList* list = AS_LIST(collection);
        if (index >= (int64_t)list->length) return false;
        *out = list->data[index];
        iter->data[1] = INT_VAL(index + 1);
        return true;
    } else if (IS_MAP(collection)) {
        ObjMap* map = AS_MAP(collection);
        // Iterate through map entries
        while (index < (int64_t)map->capacity) {
            if (map->entries[index].key != NULL && map->entries[index].key != MAP_TOMBSTONE) {
                *out = OBJ_VAL(map->entries[index].key);
                iter->data[1] = INT_VAL(index + 1);
                return true;
            }
            index++;
        }
        return false;
    } else if (IS_STRING(collection)) {
        ObjString* str = AS_STRING(collection);
        if (index >= (int64_t)str->length) return false;
        *out = OBJ_VAL(obj_string_new(&str->data[index], 1));
        iter->data[1] = INT_VAL(index + 1);
        return true;
    }

    return false;
}

// ---- Built-in method dispatch ----

// ---- Bound method creation helper ----

static Value make_bound_method(Value receiver, BoundMethodFn method, int arity) {
    ObjBoundMethod* bound = (ObjBoundMethod*)vek_alloc(sizeof(ObjBoundMethod));
    bound->header.type = OBJ_BOUND_METHOD;
    bound->header.flags = 0;
    bound->header.size = sizeof(ObjBoundMethod);
    bound->header.hash = 0;
    bound->header.page = NULL;
    bound->receiver = receiver;
    bound->method = method;
    bound->arity = arity;
    gc_track_object((ObjHeader*)bound);
    return OBJ_VAL(bound);
}

// ---- List methods that take arguments ----

static Value list_push_method(Value receiver, int arg_count, Value* args) {
    ObjList* list = AS_LIST(receiver);
    for (int i = 0; i < arg_count; i++) {
        obj_list_push(list, args[i]);
    }
    return receiver;
}

static Value list_contains_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjList* list = AS_LIST(receiver);
    Value target = args[0];
    for (uint32_t i = 0; i < list->length; i++) {
        if (value_equal(list->data[i], target)) return VAL_TRUE;
    }
    return VAL_FALSE;
}

static Value list_join_method(Value receiver, int arg_count, Value* args) {
    ObjList* list = AS_LIST(receiver);
    ObjString* sep;
    if (arg_count > 0 && IS_STRING(args[0])) {
        sep = AS_STRING(args[0]);
    } else {
        sep = obj_string_new("", 0);
    }

    // Calculate total length
    uint32_t total = 0;
    for (uint32_t i = 0; i < list->length; i++) {
        ObjString* s = value_to_string(list->data[i]);
        total += s->length;
        if (i > 0) total += sep->length;
    }

    char* buf = (char*)malloc(total + 1);
    uint32_t pos = 0;
    for (uint32_t i = 0; i < list->length; i++) {
        if (i > 0) {
            memcpy(buf + pos, sep->data, sep->length);
            pos += sep->length;
        }
        ObjString* s = value_to_string(list->data[i]);
        memcpy(buf + pos, s->data, s->length);
        pos += s->length;
    }
    buf[total] = '\0';
    ObjString* result = obj_string_new(buf, total);
    free(buf);
    return OBJ_VAL(result);
}

// ---- String methods that take arguments ----

static Value string_contains_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjString* str = AS_STRING(receiver);
    if (!IS_STRING(args[0])) return VAL_FALSE;
    ObjString* needle = AS_STRING(args[0]);
    if (needle->length == 0) return VAL_TRUE;
    if (needle->length > str->length) return VAL_FALSE;
    bool found = (strstr(str->data, needle->data) != NULL);
    return BOOL_VAL(found);
}

static Value string_split_method(Value receiver, int arg_count, Value* args) {
    ObjString* str = AS_STRING(receiver);
    ObjString* sep;
    if (arg_count > 0 && IS_STRING(args[0])) {
        sep = AS_STRING(args[0]);
    } else {
        sep = obj_string_new(" ", 1);
    }

    ObjList* result = obj_list_new();
    gc_push_root(OBJ_VAL(result));

    if (sep->length == 0) {
        // Split into individual characters
        for (uint32_t i = 0; i < str->length; i++) {
            ObjString* ch = obj_string_new(&str->data[i], 1);
            obj_list_push(result, OBJ_VAL(ch));
        }
    } else {
        const char* start = str->data;
        const char* end = str->data + str->length;
        while (start <= end) {
            const char* found = strstr(start, sep->data);
            if (found == NULL) {
                ObjString* part = obj_string_new(start, (uint32_t)(end - start));
                obj_list_push(result, OBJ_VAL(part));
                break;
            }
            ObjString* part = obj_string_new(start, (uint32_t)(found - start));
            obj_list_push(result, OBJ_VAL(part));
            start = found + sep->length;
        }
    }

    gc_pop_root();
    return OBJ_VAL(result);
}

static Value string_replace_method(Value receiver, int arg_count, Value* args) {
    if (arg_count < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return receiver;
    ObjString* str = AS_STRING(receiver);
    ObjString* old = AS_STRING(args[0]);
    ObjString* new_str = AS_STRING(args[1]);

    if (old->length == 0) return receiver;

    // Count occurrences
    int count = 0;
    const char* pos = str->data;
    while ((pos = strstr(pos, old->data)) != NULL) {
        count++;
        pos += old->length;
    }
    if (count == 0) return receiver;

    uint32_t new_len = str->length + (uint32_t)count * (new_str->length - old->length);
    char* buf = (char*)malloc(new_len + 1);
    char* dst = buf;
    const char* src = str->data;
    while ((pos = strstr(src, old->data)) != NULL) {
        size_t chunk = (size_t)(pos - src);
        memcpy(dst, src, chunk);
        dst += chunk;
        memcpy(dst, new_str->data, new_str->length);
        dst += new_str->length;
        src = pos + old->length;
    }
    strcpy(dst, src);
    ObjString* result = obj_string_new(buf, new_len);
    free(buf);
    return OBJ_VAL(result);
}

static Value string_starts_with_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjString* str = AS_STRING(receiver);
    if (!IS_STRING(args[0])) return VAL_FALSE;
    ObjString* prefix = AS_STRING(args[0]);
    if (prefix->length > str->length) return VAL_FALSE;
    return BOOL_VAL(memcmp(str->data, prefix->data, prefix->length) == 0);
}

static Value string_ends_with_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjString* str = AS_STRING(receiver);
    if (!IS_STRING(args[0])) return VAL_FALSE;
    ObjString* suffix = AS_STRING(args[0]);
    if (suffix->length > str->length) return VAL_FALSE;
    return BOOL_VAL(memcmp(str->data + str->length - suffix->length, suffix->data, suffix->length) == 0);
}

static Value string_slice_method(Value receiver, int arg_count, Value* args) {
    ObjString* str = AS_STRING(receiver);
    int64_t start = 0, end_pos = (int64_t)str->length;
    if (arg_count >= 1 && IS_INT(args[0])) start = AS_INT(args[0]);
    if (arg_count >= 2 && IS_INT(args[1])) end_pos = AS_INT(args[1]);
    if (start < 0) start += (int64_t)str->length;
    if (end_pos < 0) end_pos += (int64_t)str->length;
    if (start < 0) start = 0;
    if (end_pos > (int64_t)str->length) end_pos = (int64_t)str->length;
    if (start >= end_pos) return OBJ_VAL(obj_string_new("", 0));
    return OBJ_VAL(obj_string_new(str->data + start, (uint32_t)(end_pos - start)));
}

static Value string_index_of_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjString* str = AS_STRING(receiver);
    if (!IS_STRING(args[0])) return INT_VAL(-1);
    ObjString* needle = AS_STRING(args[0]);
    const char* found = strstr(str->data, needle->data);
    if (found == NULL) return INT_VAL(-1);
    return INT_VAL((int64_t)(found - str->data));
}

// ---- Map methods that take arguments ----

static Value map_has_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjMap* map = AS_MAP(receiver);
    if (!IS_STRING(args[0])) return VAL_FALSE;
    Value v;
    return BOOL_VAL(obj_map_get(map, AS_STRING(args[0]), &v));
}

static Value map_delete_method(Value receiver, int arg_count, Value* args) {
    (void)arg_count;
    ObjMap* map = AS_MAP(receiver);
    if (!IS_STRING(args[0])) return VAL_NIL;
    Value v;
    if (obj_map_get(map, AS_STRING(args[0]), &v)) {
        obj_map_delete(map, AS_STRING(args[0]));
        return v;
    }
    return VAL_NIL;
}

// ---- Updated field access for methods ----

static bool string_method(ObjString* str, ObjString* method_name, Value* result) {
    const char* m = method_name->data;
    uint32_t mlen = method_name->length;

    if (mlen == 6 && memcmp(m, "length", 6) == 0) {
        *result = INT_VAL((int64_t)str->length);
        return true;
    }
    if (mlen == 5 && memcmp(m, "upper", 5) == 0) {
        char* buf = (char*)malloc(str->length + 1);
        for (uint32_t i = 0; i < str->length; i++) buf[i] = (char)toupper((unsigned char)str->data[i]);
        buf[str->length] = '\0';
        *result = OBJ_VAL(obj_string_new(buf, str->length));
        free(buf);
        return true;
    }
    if (mlen == 5 && memcmp(m, "lower", 5) == 0) {
        char* buf = (char*)malloc(str->length + 1);
        for (uint32_t i = 0; i < str->length; i++) buf[i] = (char)tolower((unsigned char)str->data[i]);
        buf[str->length] = '\0';
        *result = OBJ_VAL(obj_string_new(buf, str->length));
        free(buf);
        return true;
    }
    if (mlen == 4 && memcmp(m, "trim", 4) == 0) {
        const char* start = str->data;
        const char* end = str->data + str->length;
        while (start < end && isspace((unsigned char)*start)) start++;
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
        *result = OBJ_VAL(obj_string_new(start, (uint32_t)(end - start)));
        return true;
    }
    if (mlen == 8 && memcmp(m, "is_empty", 8) == 0) {
        *result = BOOL_VAL(str->length == 0);
        return true;
    }
    if (mlen == 7 && memcmp(m, "reverse", 7) == 0) {
        char* buf = (char*)malloc(str->length + 1);
        for (uint32_t i = 0; i < str->length; i++) buf[i] = str->data[str->length - 1 - i];
        buf[str->length] = '\0';
        *result = OBJ_VAL(obj_string_new(buf, str->length));
        free(buf);
        return true;
    }
    if (mlen == 4 && memcmp(m, "to_i", 4) == 0) {
        int64_t val = strtoll(str->data, NULL, 10);
        *result = INT_VAL(val);
        return true;
    }
    if (mlen == 4 && memcmp(m, "to_f", 4) == 0) {
        double val = strtod(str->data, NULL);
        *result = FLOAT_VAL(val);
        return true;
    }
    // Methods that need arguments - return bound methods
    Value recv = OBJ_VAL(str);
    if (mlen == 8 && memcmp(m, "contains", 8) == 0) {
        *result = make_bound_method(recv, string_contains_method, 1);
        return true;
    }
    if (mlen == 5 && memcmp(m, "split", 5) == 0) {
        *result = make_bound_method(recv, string_split_method, -1);
        return true;
    }
    if (mlen == 7 && memcmp(m, "replace", 7) == 0) {
        *result = make_bound_method(recv, string_replace_method, 2);
        return true;
    }
    if (mlen == 11 && memcmp(m, "starts_with", 11) == 0) {
        *result = make_bound_method(recv, string_starts_with_method, 1);
        return true;
    }
    if (mlen == 9 && memcmp(m, "ends_with", 9) == 0) {
        *result = make_bound_method(recv, string_ends_with_method, 1);
        return true;
    }
    if (mlen == 5 && memcmp(m, "slice", 5) == 0) {
        *result = make_bound_method(recv, string_slice_method, -1);
        return true;
    }
    if (mlen == 8 && memcmp(m, "index_of", 8) == 0) {
        *result = make_bound_method(recv, string_index_of_method, 1);
        return true;
    }
    return false;
}

static bool list_method(ObjList* list, ObjString* method_name, Value* result) {
    const char* m = method_name->data;
    uint32_t mlen = method_name->length;

    if (mlen == 6 && memcmp(m, "length", 6) == 0) {
        *result = INT_VAL((int64_t)list->length);
        return true;
    }
    if (mlen == 3 && memcmp(m, "pop", 3) == 0) {
        if (list->length == 0) {
            *result = VAL_NIL;
        } else {
            *result = list->data[--list->length];
        }
        return true;
    }
    if (mlen == 5 && memcmp(m, "first", 5) == 0) {
        *result = list->length > 0 ? list->data[0] : VAL_NIL;
        return true;
    }
    if (mlen == 4 && memcmp(m, "last", 4) == 0) {
        *result = list->length > 0 ? list->data[list->length - 1] : VAL_NIL;
        return true;
    }
    if (mlen == 8 && memcmp(m, "is_empty", 8) == 0) {
        *result = BOOL_VAL(list->length == 0);
        return true;
    }
    if (mlen == 7 && memcmp(m, "reverse", 7) == 0) {
        ObjList* rev = obj_list_new();
        gc_push_root(OBJ_VAL(rev));
        for (int64_t i = (int64_t)list->length - 1; i >= 0; i--) {
            obj_list_push(rev, list->data[i]);
        }
        gc_pop_root();
        *result = OBJ_VAL(rev);
        return true;
    }
    // Methods that take arguments - return bound methods
    Value recv = OBJ_VAL(list);
    if (mlen == 4 && memcmp(m, "push", 4) == 0) {
        *result = make_bound_method(recv, list_push_method, -1);
        return true;
    }
    if (mlen == 8 && memcmp(m, "contains", 8) == 0) {
        *result = make_bound_method(recv, list_contains_method, 1);
        return true;
    }
    if (mlen == 4 && memcmp(m, "join", 4) == 0) {
        *result = make_bound_method(recv, list_join_method, -1);
        return true;
    }
    return false;
}

static bool map_method(ObjMap* map, ObjString* method_name, Value* result) {
    const char* m = method_name->data;
    uint32_t mlen = method_name->length;

    if (mlen == 6 && memcmp(m, "length", 6) == 0) {
        *result = INT_VAL((int64_t)map->length);
        return true;
    }
    if (mlen == 4 && memcmp(m, "keys", 4) == 0) {
        ObjList* keys = obj_list_new();
        gc_push_root(OBJ_VAL(keys));
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key != NULL && map->entries[i].key != MAP_TOMBSTONE) {
                obj_list_push(keys, OBJ_VAL(map->entries[i].key));
            }
        }
        gc_pop_root();
        *result = OBJ_VAL(keys);
        return true;
    }
    if (mlen == 6 && memcmp(m, "values", 6) == 0) {
        ObjList* values = obj_list_new();
        gc_push_root(OBJ_VAL(values));
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key != NULL && map->entries[i].key != MAP_TOMBSTONE) {
                obj_list_push(values, map->entries[i].value);
            }
        }
        gc_pop_root();
        *result = OBJ_VAL(values);
        return true;
    }
    if (mlen == 8 && memcmp(m, "is_empty", 8) == 0) {
        *result = BOOL_VAL(map->length == 0);
        return true;
    }
    // Methods that take arguments
    Value recv = OBJ_VAL(map);
    if (mlen == 3 && memcmp(m, "has", 3) == 0) {
        *result = make_bound_method(recv, map_has_method, 1);
        return true;
    }
    if (mlen == 6 && memcmp(m, "delete", 6) == 0) {
        *result = make_bound_method(recv, map_delete_method, 1);
        return true;
    }
    return false;
}

// ---- Main interpret entry point ----

InterpretResult vm_interpret(const char* source) {
    ObjFunction* function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    vm_pin((ObjHeader*)function);
    ObjClosure* closure = obj_closure_new(function);
    vm_pin((ObjHeader*)closure);

    vm_push(OBJ_VAL(closure));
    call_closure(closure, 0);

    InterpretResult result = run();

    vm_unpin((ObjHeader*)closure);
    vm_unpin((ObjHeader*)function);

    return result;
}

// ---- The dispatch loop ----

static InterpretResult run(void) {
    CallFrame* frame = &vm.frames[vm.frame_count - 1];

    // Read macros - little-endian 16-bit
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
    (frame->ip += 2, (uint16_t)((frame->ip[-2]) | (frame->ip[-1] << 8)))
#define READ_CONSTANT() (frame->closure->function->chunk.constants[READ_SHORT()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

    // Computed GOTO dispatch table
#if defined(__GNUC__) || defined(__clang__)
#define USE_COMPUTED_GOTO
#endif

#ifdef USE_COMPUTED_GOTO
    static void* dispatch_table[] = {
        [OP_CONSTANT]      = &&op_constant,
        [OP_NIL]           = &&op_nil,
        [OP_TRUE]          = &&op_true,
        [OP_FALSE]         = &&op_false,
        [OP_ADD]           = &&op_add,
        [OP_SUB]           = &&op_sub,
        [OP_MUL]           = &&op_mul,
        [OP_DIV]           = &&op_div,
        [OP_MOD]           = &&op_mod,
        [OP_NEG]           = &&op_neg,
        [OP_NOT]           = &&op_not,
        [OP_BAND]          = &&op_band,
        [OP_BOR]           = &&op_bor,
        [OP_BXOR]          = &&op_bxor,
        [OP_BNOT]          = &&op_bnot,
        [OP_SHL]           = &&op_shl,
        [OP_SHR]           = &&op_shr,
        [OP_EQUAL]         = &&op_equal,
        [OP_NOT_EQUAL]     = &&op_not_equal,
        [OP_LESS]          = &&op_less,
        [OP_LESS_EQUAL]    = &&op_less_equal,
        [OP_GREATER]       = &&op_greater,
        [OP_GREATER_EQUAL] = &&op_greater_equal,
        [OP_CONCAT]        = &&op_concat,
        [OP_POP]           = &&op_pop,
        [OP_PRINT]         = &&op_print,
        [OP_GET_LOCAL]     = &&op_get_local,
        [OP_SET_LOCAL]     = &&op_set_local,
        [OP_GET_GLOBAL]    = &&op_get_global,
        [OP_SET_GLOBAL]    = &&op_set_global,
        [OP_DEFINE_GLOBAL] = &&op_define_global,
        [OP_GET_UPVALUE]   = &&op_get_upvalue,
        [OP_SET_UPVALUE]   = &&op_set_upvalue,
        [OP_CLOSE_UPVALUE] = &&op_close_upvalue,
        [OP_JUMP]          = &&op_jump,
        [OP_JUMP_IF_FALSE] = &&op_jump_if_false,
        [OP_LOOP]          = &&op_loop,
        [OP_CALL]          = &&op_call,
        [OP_RETURN]        = &&op_return,
        [OP_CLOSURE]       = &&op_closure,
        [OP_NEW_LIST]      = &&op_new_list,
        [OP_NEW_MAP]       = &&op_new_map,
        [OP_GET_INDEX]     = &&op_get_index,
        [OP_SET_INDEX]     = &&op_set_index,
        [OP_GET_FIELD]     = &&op_get_field,
        [OP_SET_FIELD]     = &&op_set_field,
        [OP_ITER_INIT]     = &&op_iter_init,
        [OP_ITER_NEXT]     = &&op_iter_next,
        [OP_POWER]         = &&op_power,
    };

#define DISPATCH() goto *dispatch_table[READ_BYTE()]
#else
#define DISPATCH() goto loop_top
#endif

#ifdef USE_COMPUTED_GOTO
    DISPATCH();
#endif

    for (;;) {
#ifndef USE_COMPUTED_GOTO
    loop_top: ;
        uint8_t instruction = READ_BYTE();
        switch (instruction) {
#define CASE(op) case op
#define END_CASE break
#else
#define CASE(op) op_##op
// Not needed with computed goto, but for syntactic balance:
#define END_CASE DISPATCH()
#endif

// ---- Constants and literals ----

#ifdef USE_COMPUTED_GOTO
    op_constant:
#else
    case OP_CONSTANT:
#endif
    {
        Value constant = READ_CONSTANT();
        vm_push(constant);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_nil:
#else
    case OP_NIL:
#endif
    {
        vm_push(VAL_NIL);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_true:
#else
    case OP_TRUE:
#endif
    {
        vm_push(VAL_TRUE);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_false:
#else
    case OP_FALSE:
#endif
    {
        vm_push(VAL_FALSE);
        DISPATCH();
    }

// ---- Arithmetic ----

#ifdef USE_COMPUTED_GOTO
    op_add:
#else
    case OP_ADD:
#endif
    {
        Value b = vm_pop();
        Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) {
            vm_push(INT_VAL(AS_INT(a) + AS_INT(b)));
        } else if (IS_FLOAT(a) && IS_FLOAT(b)) {
            vm_push(FLOAT_VAL(AS_DOUBLE(a) + AS_DOUBLE(b)));
        } else if (IS_INT(a) && IS_FLOAT(b)) {
            vm_push(FLOAT_VAL((double)AS_INT(a) + AS_DOUBLE(b)));
        } else if (IS_FLOAT(a) && IS_INT(b)) {
            vm_push(FLOAT_VAL(AS_DOUBLE(a) + (double)AS_INT(b)));
        } else if (IS_STRING(a) && IS_STRING(b)) {
            ObjString* result = concatenate_strings(AS_STRING(a), AS_STRING(b));
            vm_push(OBJ_VAL(result));
        } else {
            runtime_error("Operands must be numbers or strings for '+'.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_sub:
#else
    case OP_SUB:
#endif
    {
        Value b = vm_pop();
        Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) {
            vm_push(INT_VAL(AS_INT(a) - AS_INT(b)));
        } else if (IS_FLOAT(a) && IS_FLOAT(b)) {
            vm_push(FLOAT_VAL(AS_DOUBLE(a) - AS_DOUBLE(b)));
        } else if (IS_INT(a) && IS_FLOAT(b)) {
            vm_push(FLOAT_VAL((double)AS_INT(a) - AS_DOUBLE(b)));
        } else if (IS_FLOAT(a) && IS_INT(b)) {
            vm_push(FLOAT_VAL(AS_DOUBLE(a) - (double)AS_INT(b)));
        } else {
            runtime_error("Operands must be numbers for '-'.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_mul:
#else
    case OP_MUL:
#endif
    {
        Value b = vm_pop();
        Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) {
            vm_push(INT_VAL(AS_INT(a) * AS_INT(b)));
        } else if (IS_FLOAT(a) && IS_FLOAT(b)) {
            vm_push(FLOAT_VAL(AS_DOUBLE(a) * AS_DOUBLE(b)));
        } else if (IS_INT(a) && IS_FLOAT(b)) {
            vm_push(FLOAT_VAL((double)AS_INT(a) * AS_DOUBLE(b)));
        } else if (IS_FLOAT(a) && IS_INT(b)) {
            vm_push(FLOAT_VAL(AS_DOUBLE(a) * (double)AS_INT(b)));
        } else {
            runtime_error("Operands must be numbers for '*'.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_div:
#else
    case OP_DIV:
#endif
    {
        // Division ALWAYS returns float (even int/int)
        Value b = vm_pop();
        Value a = vm_pop();
        double da, db;
        if (IS_INT(a)) da = (double)AS_INT(a);
        else if (IS_FLOAT(a)) da = AS_DOUBLE(a);
        else { runtime_error("Operands must be numbers for '/'."); return INTERPRET_RUNTIME_ERROR; }
        if (IS_INT(b)) db = (double)AS_INT(b);
        else if (IS_FLOAT(b)) db = AS_DOUBLE(b);
        else { runtime_error("Operands must be numbers for '/'."); return INTERPRET_RUNTIME_ERROR; }
        if (db == 0.0) { runtime_error("Division by zero."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(FLOAT_VAL(da / db));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_mod:
#else
    case OP_MOD:
#endif
    {
        Value b = vm_pop();
        Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) {
            int64_t bv = AS_INT(b);
            if (bv == 0) { runtime_error("Modulo by zero."); return INTERPRET_RUNTIME_ERROR; }
            vm_push(INT_VAL(AS_INT(a) % bv));
        } else {
            double da, db;
            if (IS_INT(a)) da = (double)AS_INT(a); else if (IS_FLOAT(a)) da = AS_DOUBLE(a);
            else { runtime_error("Operands must be numbers for '%%'."); return INTERPRET_RUNTIME_ERROR; }
            if (IS_INT(b)) db = (double)AS_INT(b); else if (IS_FLOAT(b)) db = AS_DOUBLE(b);
            else { runtime_error("Operands must be numbers for '%%'."); return INTERPRET_RUNTIME_ERROR; }
            if (db == 0.0) { runtime_error("Modulo by zero."); return INTERPRET_RUNTIME_ERROR; }
            vm_push(FLOAT_VAL(fmod(da, db)));
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_power:
#else
    case OP_POWER:
#endif
    {
        Value b = vm_pop();
        Value a = vm_pop();
        double da, db;
        if (IS_INT(a)) da = (double)AS_INT(a); else if (IS_FLOAT(a)) da = AS_DOUBLE(a);
        else { runtime_error("Operands must be numbers for '**'."); return INTERPRET_RUNTIME_ERROR; }
        if (IS_INT(b)) db = (double)AS_INT(b); else if (IS_FLOAT(b)) db = AS_DOUBLE(b);
        else { runtime_error("Operands must be numbers for '**'."); return INTERPRET_RUNTIME_ERROR; }
        double result = pow(da, db);
        // If both operands were int and exponent >= 0, return int
        if (IS_INT(a) && IS_INT(b) && AS_INT(b) >= 0) {
            vm_push(INT_VAL((int64_t)result));
        } else {
            vm_push(FLOAT_VAL(result));
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_neg:
#else
    case OP_NEG:
#endif
    {
        Value v = vm_pop();
        if (IS_INT(v)) {
            vm_push(INT_VAL(-AS_INT(v)));
        } else if (IS_FLOAT(v)) {
            vm_push(FLOAT_VAL(-AS_DOUBLE(v)));
        } else {
            runtime_error("Operand must be a number for unary '-'.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_not:
#else
    case OP_NOT:
#endif
    {
        Value v = vm_pop();
        vm_push(BOOL_VAL(IS_FALSY(v)));
        DISPATCH();
    }

// ---- Bitwise ----

#ifdef USE_COMPUTED_GOTO
    op_band:
#else
    case OP_BAND:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (!IS_INT(a) || !IS_INT(b)) { runtime_error("Bitwise operations require integers."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(INT_VAL(AS_INT(a) & AS_INT(b)));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_bor:
#else
    case OP_BOR:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (!IS_INT(a) || !IS_INT(b)) { runtime_error("Bitwise operations require integers."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(INT_VAL(AS_INT(a) | AS_INT(b)));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_bxor:
#else
    case OP_BXOR:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (!IS_INT(a) || !IS_INT(b)) { runtime_error("Bitwise operations require integers."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(INT_VAL(AS_INT(a) ^ AS_INT(b)));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_bnot:
#else
    case OP_BNOT:
#endif
    {
        Value v = vm_pop();
        if (!IS_INT(v)) { runtime_error("Bitwise NOT requires an integer."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(INT_VAL(~AS_INT(v)));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_shl:
#else
    case OP_SHL:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (!IS_INT(a) || !IS_INT(b)) { runtime_error("Shift operations require integers."); return INTERPRET_RUNTIME_ERROR; }
        int64_t shift = AS_INT(b);
        if (shift < 0 || shift >= 64) { runtime_error("Shift count must be between 0 and 63."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(INT_VAL(AS_INT(a) << shift));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_shr:
#else
    case OP_SHR:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (!IS_INT(a) || !IS_INT(b)) { runtime_error("Shift operations require integers."); return INTERPRET_RUNTIME_ERROR; }
        int64_t shift = AS_INT(b);
        if (shift < 0 || shift >= 64) { runtime_error("Shift count must be between 0 and 63."); return INTERPRET_RUNTIME_ERROR; }
        vm_push(INT_VAL(AS_INT(a) >> shift));
        DISPATCH();
    }

// ---- Comparison ----

#ifdef USE_COMPUTED_GOTO
    op_equal:
#else
    case OP_EQUAL:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        // Handle cross-type numeric equality
        if (IS_INT(a) && IS_FLOAT(b)) {
            vm_push(BOOL_VAL((double)AS_INT(a) == AS_DOUBLE(b)));
        } else if (IS_FLOAT(a) && IS_INT(b)) {
            vm_push(BOOL_VAL(AS_DOUBLE(a) == (double)AS_INT(b)));
        } else {
            vm_push(BOOL_VAL(value_equal(a, b)));
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_not_equal:
#else
    case OP_NOT_EQUAL:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (IS_INT(a) && IS_FLOAT(b)) {
            vm_push(BOOL_VAL((double)AS_INT(a) != AS_DOUBLE(b)));
        } else if (IS_FLOAT(a) && IS_INT(b)) {
            vm_push(BOOL_VAL(AS_DOUBLE(a) != (double)AS_INT(b)));
        } else {
            vm_push(BOOL_VAL(!value_equal(a, b)));
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_less:
#else
    case OP_LESS:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_INT(a) < AS_INT(b))); }
        else if (IS_FLOAT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) < AS_DOUBLE(b))); }
        else if (IS_INT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL((double)AS_INT(a) < AS_DOUBLE(b))); }
        else if (IS_FLOAT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) < (double)AS_INT(b))); }
        else if (IS_STRING(a) && IS_STRING(b)) {
            int cmp = strcmp(AS_STRING(a)->data, AS_STRING(b)->data);
            vm_push(BOOL_VAL(cmp < 0));
        }
        else { runtime_error("Operands must be numbers or strings for '<'."); return INTERPRET_RUNTIME_ERROR; }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_less_equal:
#else
    case OP_LESS_EQUAL:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_INT(a) <= AS_INT(b))); }
        else if (IS_FLOAT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) <= AS_DOUBLE(b))); }
        else if (IS_INT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL((double)AS_INT(a) <= AS_DOUBLE(b))); }
        else if (IS_FLOAT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) <= (double)AS_INT(b))); }
        else if (IS_STRING(a) && IS_STRING(b)) {
            int cmp = strcmp(AS_STRING(a)->data, AS_STRING(b)->data);
            vm_push(BOOL_VAL(cmp <= 0));
        }
        else { runtime_error("Operands must be numbers or strings for '<='."); return INTERPRET_RUNTIME_ERROR; }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_greater:
#else
    case OP_GREATER:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_INT(a) > AS_INT(b))); }
        else if (IS_FLOAT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) > AS_DOUBLE(b))); }
        else if (IS_INT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL((double)AS_INT(a) > AS_DOUBLE(b))); }
        else if (IS_FLOAT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) > (double)AS_INT(b))); }
        else if (IS_STRING(a) && IS_STRING(b)) {
            int cmp = strcmp(AS_STRING(a)->data, AS_STRING(b)->data);
            vm_push(BOOL_VAL(cmp > 0));
        }
        else { runtime_error("Operands must be numbers or strings for '>'."); return INTERPRET_RUNTIME_ERROR; }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_greater_equal:
#else
    case OP_GREATER_EQUAL:
#endif
    {
        Value b = vm_pop(); Value a = vm_pop();
        if (IS_INT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_INT(a) >= AS_INT(b))); }
        else if (IS_FLOAT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) >= AS_DOUBLE(b))); }
        else if (IS_INT(a) && IS_FLOAT(b)) { vm_push(BOOL_VAL((double)AS_INT(a) >= AS_DOUBLE(b))); }
        else if (IS_FLOAT(a) && IS_INT(b)) { vm_push(BOOL_VAL(AS_DOUBLE(a) >= (double)AS_INT(b))); }
        else if (IS_STRING(a) && IS_STRING(b)) {
            int cmp = strcmp(AS_STRING(a)->data, AS_STRING(b)->data);
            vm_push(BOOL_VAL(cmp >= 0));
        }
        else { runtime_error("Operands must be numbers or strings for '>='."); return INTERPRET_RUNTIME_ERROR; }
        DISPATCH();
    }

// ---- String concatenation ----

#ifdef USE_COMPUTED_GOTO
    op_concat:
#else
    case OP_CONCAT:
#endif
    {
        uint8_t count = READ_BYTE();
        // Concatenate 'count' values from the stack into one string
        if (count == 0) {
            vm_push(OBJ_VAL(obj_string_new("", 0)));
        } else if (count == 1) {
            Value v = vm_pop();
            ObjString* s = value_to_string(v);
            vm_push(OBJ_VAL(s));
        } else {
            // Collect strings
            ObjString* parts[256];
            uint32_t total_len = 0;
            for (int i = count - 1; i >= 0; i--) {
                Value v = vm_pop();
                parts[i] = value_to_string(v);
                total_len += parts[i]->length;
            }
            char* buf = (char*)malloc(total_len + 1);
            uint32_t pos = 0;
            for (int i = 0; i < count; i++) {
                memcpy(buf + pos, parts[i]->data, parts[i]->length);
                pos += parts[i]->length;
            }
            buf[total_len] = '\0';
            ObjString* result = obj_string_new(buf, total_len);
            free(buf);
            vm_push(OBJ_VAL(result));
        }
        DISPATCH();
    }

// ---- Stack manipulation ----

#ifdef USE_COMPUTED_GOTO
    op_pop:
#else
    case OP_POP:
#endif
    {
        vm_pop();
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_print:
#else
    case OP_PRINT:
#endif
    {
        Value v = vm_pop();
        ObjString* s = value_to_string(v);
        printf("%.*s\n", (int)s->length, s->data);
        DISPATCH();
    }

// ---- Variables ----

#ifdef USE_COMPUTED_GOTO
    op_get_local:
#else
    case OP_GET_LOCAL:
#endif
    {
        uint16_t slot = READ_SHORT();
        vm_push(frame->slots[slot]);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_set_local:
#else
    case OP_SET_LOCAL:
#endif
    {
        uint16_t slot = READ_SHORT();
        frame->slots[slot] = vm_peek(0);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_get_global:
#else
    case OP_GET_GLOBAL:
#endif
    {
        ObjString* name = READ_STRING();
        Value value;
        if (!obj_map_get(vm.globals, name, &value)) {
            runtime_error("Undefined variable '%.*s'.", (int)name->length, name->data);
            return INTERPRET_RUNTIME_ERROR;
        }
        vm_push(value);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_set_global:
#else
    case OP_SET_GLOBAL:
#endif
    {
        ObjString* name = READ_STRING();
        Value value;
        if (!obj_map_get(vm.globals, name, &value)) {
            // In vek, assignment to undefined global creates it
            obj_map_set(vm.globals, name, vm_peek(0));
        } else {
            obj_map_set(vm.globals, name, vm_peek(0));
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_define_global:
#else
    case OP_DEFINE_GLOBAL:
#endif
    {
        ObjString* name = READ_STRING();
        obj_map_set(vm.globals, name, vm_peek(0));
        vm_pop();
        DISPATCH();
    }

// ---- Upvalues ----

#ifdef USE_COMPUTED_GOTO
    op_get_upvalue:
#else
    case OP_GET_UPVALUE:
#endif
    {
        uint8_t slot = READ_BYTE();
        vm_push(*((ObjUpvalue*)AS_PTR(frame->closure->upvalues[slot]))->location);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_set_upvalue:
#else
    case OP_SET_UPVALUE:
#endif
    {
        uint8_t slot = READ_BYTE();
        *((ObjUpvalue*)AS_PTR(frame->closure->upvalues[slot]))->location = vm_peek(0);
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_close_upvalue:
#else
    case OP_CLOSE_UPVALUE:
#endif
    {
        close_upvalues(vm.stack_top - 1);
        vm_pop();
        DISPATCH();
    }

// ---- Control flow ----

#ifdef USE_COMPUTED_GOTO
    op_jump:
#else
    case OP_JUMP:
#endif
    {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_jump_if_false:
#else
    case OP_JUMP_IF_FALSE:
#endif
    {
        uint16_t offset = READ_SHORT();
        if (IS_FALSY(vm_peek(0))) {
            frame->ip += offset;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_loop:
#else
    case OP_LOOP:
#endif
    {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        DISPATCH();
    }

// ---- Functions ----

#ifdef USE_COMPUTED_GOTO
    op_call:
#else
    case OP_CALL:
#endif
    {
        int arg_count = READ_BYTE();
        if (!call_value(vm_peek(arg_count), arg_count)) {
            return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frame_count - 1];
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_return:
#else
    case OP_RETURN:
#endif
    {
        Value result = vm_pop();
        close_upvalues(frame->slots);
        vm.frame_count--;
        if (vm.frame_count == 0) {
            vm_pop(); // pop the script function
            return INTERPRET_OK;
        }
        vm.stack_top = frame->slots;
        vm_push(result);
        frame = &vm.frames[vm.frame_count - 1];
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_closure:
#else
    case OP_CLOSURE:
#endif
    {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = obj_closure_new(function);
        vm_push(OBJ_VAL(closure));

        // Capture upvalues
        for (int i = 0; i < closure->upvalue_count; i++) {
            uint8_t is_local = READ_BYTE();
            uint8_t index = READ_BYTE();
            if (is_local) {
                ObjUpvalue* uv = capture_upvalue(frame->slots + index);
                closure->upvalues[i] = OBJ_VAL(uv);
            } else {
                closure->upvalues[i] = frame->closure->upvalues[index];
            }
        }
        DISPATCH();
    }

// ---- Collections ----

#ifdef USE_COMPUTED_GOTO
    op_new_list:
#else
    case OP_NEW_LIST:
#endif
    {
        uint16_t count = READ_SHORT();
        ObjList* list = obj_list_new();
        gc_push_root(OBJ_VAL(list));
        // Items are on the stack in order, with the first pushed deepest
        Value* start = vm.stack_top - count;
        for (uint16_t i = 0; i < count; i++) {
            obj_list_push(list, start[i]);
        }
        vm.stack_top -= count;
        gc_pop_root();
        vm_push(OBJ_VAL(list));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_new_map:
#else
    case OP_NEW_MAP:
#endif
    {
        uint16_t count = READ_SHORT(); // number of key-value pairs
        ObjMap* map = obj_map_new();
        gc_push_root(OBJ_VAL(map));
        // Stack has: key1, val1, key2, val2, ... (2*count values)
        Value* start = vm.stack_top - count * 2;
        for (uint16_t i = 0; i < count; i++) {
            Value key = start[i * 2];
            Value val = start[i * 2 + 1];
            if (!IS_STRING(key)) {
                gc_pop_root();
                runtime_error("Map keys must be strings.");
                return INTERPRET_RUNTIME_ERROR;
            }
            obj_map_set(map, AS_STRING(key), val);
        }
        vm.stack_top -= count * 2;
        gc_pop_root();
        vm_push(OBJ_VAL(map));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_get_index:
#else
    case OP_GET_INDEX:
#endif
    {
        Value index = vm_pop();
        Value obj = vm_pop();
        if (IS_LIST(obj)) {
            if (!IS_INT(index)) { runtime_error("List index must be an integer."); return INTERPRET_RUNTIME_ERROR; }
            ObjList* list = AS_LIST(obj);
            int64_t idx = AS_INT(index);
            if (idx < 0) idx += (int64_t)list->length;
            if (idx < 0 || idx >= (int64_t)list->length) { runtime_error("List index out of bounds."); return INTERPRET_RUNTIME_ERROR; }
            vm_push(list->data[idx]);
        } else if (IS_MAP(obj)) {
            if (!IS_STRING(index)) { runtime_error("Map key must be a string."); return INTERPRET_RUNTIME_ERROR; }
            Value val;
            if (obj_map_get(AS_MAP(obj), AS_STRING(index), &val)) {
                vm_push(val);
            } else {
                vm_push(VAL_NIL);
            }
        } else if (IS_STRING(obj)) {
            if (!IS_INT(index)) { runtime_error("String index must be an integer."); return INTERPRET_RUNTIME_ERROR; }
            ObjString* str = AS_STRING(obj);
            int64_t idx = AS_INT(index);
            if (idx < 0) idx += (int64_t)str->length;
            if (idx < 0 || idx >= (int64_t)str->length) { runtime_error("String index out of bounds."); return INTERPRET_RUNTIME_ERROR; }
            vm_push(OBJ_VAL(obj_string_new(&str->data[idx], 1)));
        } else {
            runtime_error("Only lists, maps, and strings support indexing.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_set_index:
#else
    case OP_SET_INDEX:
#endif
    {
        Value value = vm_pop();
        Value index = vm_pop();
        Value obj = vm_pop();
        if (IS_LIST(obj)) {
            if (!IS_INT(index)) { runtime_error("List index must be an integer."); return INTERPRET_RUNTIME_ERROR; }
            ObjList* list = AS_LIST(obj);
            int64_t idx = AS_INT(index);
            if (idx < 0) idx += (int64_t)list->length;
            if (idx < 0 || idx >= (int64_t)list->length) { runtime_error("List index out of bounds."); return INTERPRET_RUNTIME_ERROR; }
            list->data[idx] = value;
            vm_push(value);
        } else if (IS_MAP(obj)) {
            if (!IS_STRING(index)) { runtime_error("Map key must be a string."); return INTERPRET_RUNTIME_ERROR; }
            obj_map_set(AS_MAP(obj), AS_STRING(index), value);
            vm_push(value);
        } else {
            runtime_error("Only lists and maps support index assignment.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_get_field:
#else
    case OP_GET_FIELD:
#endif
    {
        ObjString* name = READ_STRING();
        Value obj = vm_pop();
        if (IS_MAP(obj)) {
            Value val;
            if (obj_map_get(AS_MAP(obj), name, &val)) {
                vm_push(val);
            } else {
                // Try map methods
                Value result;
                if (map_method(AS_MAP(obj), name, &result)) {
                    vm_push(result);
                } else {
                    vm_push(VAL_NIL);
                }
            }
        } else if (IS_STRING(obj)) {
            Value result;
            if (string_method(AS_STRING(obj), name, &result)) {
                vm_push(result);
            } else {
                runtime_error("String has no property '%.*s'.", (int)name->length, name->data);
                return INTERPRET_RUNTIME_ERROR;
            }
        } else if (IS_LIST(obj)) {
            Value result;
            if (list_method(AS_LIST(obj), name, &result)) {
                vm_push(result);
            } else {
                runtime_error("List has no property '%.*s'.", (int)name->length, name->data);
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            runtime_error("Only maps, strings, and lists have fields/methods.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_set_field:
#else
    case OP_SET_FIELD:
#endif
    {
        ObjString* name = READ_STRING();
        Value value = vm_pop();
        Value obj = vm_pop();
        if (IS_MAP(obj)) {
            obj_map_set(AS_MAP(obj), name, value);
            vm_push(value);
        } else {
            runtime_error("Only maps support field assignment.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

// ---- Iteration ----

#ifdef USE_COMPUTED_GOTO
    op_iter_init:
#else
    case OP_ITER_INIT:
#endif
    {
        Value iterable = vm_pop();
        vm_push(make_iterator(iterable));
        DISPATCH();
    }

#ifdef USE_COMPUTED_GOTO
    op_iter_next:
#else
    case OP_ITER_NEXT:
#endif
    {
        uint16_t offset = READ_SHORT();
        Value iter_val = vm_peek(0); // iterator is on top of stack
        Value next_val;
        if (iterator_next(iter_val, &next_val)) {
            vm_push(next_val);
        } else {
            // Iteration done - jump past the loop body
            frame->ip += offset;
        }
        DISPATCH();
    }

// ---- End of dispatch ----

#ifndef USE_COMPUTED_GOTO
        default:
            runtime_error("Unknown opcode %d.", instruction);
            return INTERPRET_RUNTIME_ERROR;
        }
#endif
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef DISPATCH
#undef CASE
#undef END_CASE
}
