#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <fcntl.h>
#include <unistd.h>

// CSRF token key stored in session
#define CSRF_SESSION_KEY "_csrf_token"
#define CSRF_SESSION_KEY_LEN 11

// Forward declaration of HMAC (from stdlib_session.c, not needed here)
// CSRF uses random bytes for tokens.

// Generate random bytes from /dev/urandom
static bool csrf_random_bytes(uint8_t* buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;

    size_t total = 0;
    while (total < len) {
        ssize_t r = read(fd, buf + total, len - total);
        if (r <= 0) { close(fd); return false; }
        total += (size_t)r;
    }
    close(fd);
    return true;
}

// Helper: look up session package and call session.set/get
static ObjMap* get_session_package(void) {
    ObjString* session_key = obj_string_new("session", 7);
    Value session_val;
    if (!obj_map_get(vm.globals, session_key, &session_val)) return NULL;
    if (!IS_MAP(session_val)) return NULL;
    return AS_MAP(session_val);
}

static Value call_session_fn(const char* name, int argc, Value* args) {
    ObjMap* session_pkg = get_session_package();
    if (!session_pkg) return VAL_NIL;

    ObjString* fn_key = obj_string_new(name, (uint32_t)strlen(name));
    Value fn_val;
    if (!obj_map_get(session_pkg, fn_key, &fn_val)) return VAL_NIL;
    if (!IS_NATIVE(fn_val)) return VAL_NIL;

    ObjNative* native = AS_NATIVE(fn_val);
    return native->function(argc, args);
}

// csrf.generate() - generates a random token, stores in session, returns token string
static Value native_csrf_generate(int argc, Value* args) {
    (void)argc;
    (void)args;

    // Generate 32 random bytes
    uint8_t random[32];
    if (!csrf_random_bytes(random, 32)) return VAL_NIL;

    // Hex encode
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i*2, 3, "%02x", random[i]);
    }
    hex[64] = '\0';

    ObjString* token = obj_string_new(hex, 64);
    gc_push_root(OBJ_VAL(token));

    // Store in session
    Value set_args[2];
    set_args[0] = OBJ_VAL(obj_string_new(CSRF_SESSION_KEY, CSRF_SESSION_KEY_LEN));
    set_args[1] = OBJ_VAL(token);
    call_session_fn("set", 2, set_args);

    gc_pop_root();
    return OBJ_VAL(token);
}

// csrf.validate(token_string) - checks if token matches what is in session
static Value native_csrf_validate(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);

    ObjString* provided = AS_STRING(args[0]);

    // Get stored token from session
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new(CSRF_SESSION_KEY, CSRF_SESSION_KEY_LEN));
    Value stored = call_session_fn("get", 1, get_args);

    if (!IS_STRING(stored)) return BOOL_VAL(false);
    ObjString* stored_str = AS_STRING(stored);

    // Constant-time comparison
    if (provided->length != stored_str->length) return BOOL_VAL(false);

    int diff = 0;
    for (uint32_t i = 0; i < provided->length; i++) {
        diff |= provided->data[i] ^ stored_str->data[i];
    }
    return BOOL_VAL(diff == 0);
}

// csrf.tag() - returns HTML hidden input: <input type="hidden" name="_csrf" value="TOKEN">
static Value native_csrf_tag(int argc, Value* args) {
    (void)argc;
    (void)args;

    // Get current token from session (or generate one)
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new(CSRF_SESSION_KEY, CSRF_SESSION_KEY_LEN));
    Value token_val = call_session_fn("get", 1, get_args);

    // If no token exists yet, generate one
    if (!IS_STRING(token_val)) {
        token_val = native_csrf_generate(0, NULL);
        if (!IS_STRING(token_val)) return VAL_NIL;
    }

    ObjString* token = AS_STRING(token_val);

    // Build HTML: <input type="hidden" name="_csrf" value="TOKEN">
    const char* prefix = "<input type=\"hidden\" name=\"_csrf\" value=\"";
    const char* suffix = "\">";
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    size_t total_len = prefix_len + token->length + suffix_len;

    char* html = (char*)malloc(total_len + 1);
    memcpy(html, prefix, prefix_len);
    memcpy(html + prefix_len, token->data, token->length);
    memcpy(html + prefix_len + token->length, suffix, suffix_len);
    html[total_len] = '\0';

    ObjString* result = obj_string_new(html, (uint32_t)total_len);
    free(html);
    return OBJ_VAL(result);
}

void stdlib_csrf_init(ObjMap* pkg) {
    stdlib_register(pkg, "generate", native_csrf_generate, 0);
    stdlib_register(pkg, "validate", native_csrf_validate, 1);
    stdlib_register(pkg, "tag", native_csrf_tag, 0);
}
