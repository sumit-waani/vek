#ifndef VEK_VM_H
#define VEK_VM_H

#include "common.h"
#include "value.h"
#include "chunk.h"
#include "object.h"

// VM limits
#define FRAMES_MAX 256
#define STACK_MAX  65536

// Upvalue object for capturing variables by reference
struct ObjUpvalue {
    ObjHeader header;
    Value* location;    // pointer to the captured variable (on stack or closed)
    Value closed;       // when closed, variable is moved here
    struct ObjUpvalue* next; // linked list of open upvalues
};

// Native function type
typedef Value (*NativeFn)(int arg_count, Value* args);

// Bound method type (native method with a captured receiver)
typedef Value (*BoundMethodFn)(Value receiver, int arg_count, Value* args);

// Native function object
struct ObjNative {
    ObjHeader header;
    NativeFn function;
    const char* name;
    int arity; // -1 means variadic
};

// Bound method: a native method + its receiver
typedef struct {
    ObjHeader header;
    Value receiver;
    BoundMethodFn method;
    int arity; // -1 means variadic
} ObjBoundMethod;

// Call frame: one per active function call
typedef struct {
    ObjClosure* closure;
    uint8_t* ip;
    Value* slots;  // pointer into VM stack for this frame's window
} CallFrame;

// Error handler frame for begin/rescue
typedef struct {
    CallFrame* frame;
    uint8_t* ip;        // jump target (rescue block)
    Value* stack_top;   // stack pointer to restore
    int frame_count;    // frame count to restore
} ErrorHandler;

#define HANDLER_MAX 64

// The VM
typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frame_count;

    Value stack[STACK_MAX];
    Value* stack_top;

    ObjMap* globals;           // global variable table
    ObjUpvalue* open_upvalues; // linked list of open upvalues (sorted by location)

    ErrorHandler handlers[HANDLER_MAX];
    int handler_count;

    // Runtime error message
    char error_msg[512];
    bool had_error;

    // Call depth for vm_call (stop interpreting when frame_count drops to this)
    int call_depth;
} VM;

// VM result codes
typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

// Global VM instance
extern VM vm;

// VM lifecycle
void vm_init(void);
void vm_free(void);

// Interpret source code
InterpretResult vm_interpret(const char* source);

// Push/pop values on the stack (for native functions, etc.)
void vm_push(Value value);
Value vm_pop(void);
Value vm_peek(int distance);

// Call a vek closure or native function from C code.
// Pushes arguments, executes the function, and returns the result.
// callee must be on the stack already (push it before calling this).
// args should already be pushed after the callee.
// Returns the result value (or VAL_NIL on error).
Value vm_call(Value callee, int arg_count);

#endif // VEK_VM_H
