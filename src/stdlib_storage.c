#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

// Storage configuration
static char storage_dir[512] = "storage";
static char storage_endpoint[256] = "";
static char storage_bucket[128] = "";
static char storage_access_key[128] = "";
static char storage_secret_key[128] = "";
static char storage_region[64] = "";

// ---- Helpers ----

// Create directory recursively
static void mkdirs(const char* path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

// Build full file path for a key
static void build_path(const char* key, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", storage_dir, key);
}

// Ensure parent directory exists for a path
static void ensure_parent_dir(const char* filepath) {
    char dir[512];
    strncpy(dir, filepath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    // Find last /
    char* last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdirs(dir);
    }
}

// Get string value from config map
static const char* storage_config_get(ObjMap* config, const char* key) {
    ObjString* k = obj_string_new(key, (uint32_t)strlen(key));
    Value val;
    if (obj_map_get(config, k, &val) && IS_STRING(val)) {
        return AS_STRING(val)->data;
    }
    return NULL;
}

// ---- Native functions ----

// storage.configure(config_map)
// config: {endpoint, bucket, access_key, secret_key, region, dir}
static Value native_storage_configure(int argc, Value* args) {
    (void)argc;
    if (!IS_MAP(args[0])) return VAL_NIL;

    ObjMap* config = AS_MAP(args[0]);

    const char* val;

    val = storage_config_get(config, "endpoint");
    if (val) strncpy(storage_endpoint, val, sizeof(storage_endpoint) - 1);

    val = storage_config_get(config, "bucket");
    if (val) strncpy(storage_bucket, val, sizeof(storage_bucket) - 1);

    val = storage_config_get(config, "access_key");
    if (val) strncpy(storage_access_key, val, sizeof(storage_access_key) - 1);

    val = storage_config_get(config, "secret_key");
    if (val) strncpy(storage_secret_key, val, sizeof(storage_secret_key) - 1);

    val = storage_config_get(config, "region");
    if (val) strncpy(storage_region, val, sizeof(storage_region) - 1);

    val = storage_config_get(config, "dir");
    if (val) strncpy(storage_dir, val, sizeof(storage_dir) - 1);

    return VAL_TRUE;
}

// storage.put(key, data_string) - store to local filesystem
static Value native_storage_put(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);
    ObjString* data = AS_STRING(args[1]);

    char filepath[512];
    build_path(key->data, filepath, sizeof(filepath));
    ensure_parent_dir(filepath);

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return VAL_NIL;

    ssize_t written = write(fd, data->data, data->length);
    close(fd);

    if (written < 0 || (size_t)written != data->length) return VAL_NIL;

    return VAL_TRUE;
}

// storage.get(key) - read from local filesystem
static Value native_storage_get(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);

    char filepath[512];
    build_path(key->data, filepath, sizeof(filepath));

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return VAL_NIL;

    // Get file size
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return VAL_NIL;
    }

    size_t file_size = (size_t)st.st_size;
    char* buf = (char*)malloc(file_size + 1);
    if (!buf) {
        close(fd);
        return VAL_NIL;
    }

    ssize_t nread = read(fd, buf, file_size);
    close(fd);

    if (nread < 0) {
        free(buf);
        return VAL_NIL;
    }

    buf[nread] = '\0';
    ObjString* result = obj_string_new(buf, (uint32_t)nread);
    free(buf);
    return OBJ_VAL(result);
}

// storage.delete(key) - remove file
static Value native_storage_delete(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);

    char filepath[512];
    build_path(key->data, filepath, sizeof(filepath));

    if (unlink(filepath) == 0) {
        return VAL_TRUE;
    }
    return VAL_NIL;
}

// storage.url(key, expires_seconds) - generate a URL path
static Value native_storage_url(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);
    int expires = 3600; // default 1 hour
    if (argc >= 2 && IS_INT(args[1])) {
        expires = (int)AS_INT(args[1]);
    }

    time_t expiry_time = time(NULL) + expires;

    char url[1024];
    int n = snprintf(url, sizeof(url), "/storage/%.*s?expires=%ld",
        (int)key->length, key->data, (long)expiry_time);

    ObjString* result = obj_string_new(url, (uint32_t)n);
    return OBJ_VAL(result);
}

void stdlib_storage_init(ObjMap* pkg) {
    stdlib_register(pkg, "configure", native_storage_configure, 1);
    stdlib_register(pkg, "put", native_storage_put, 2);
    stdlib_register(pkg, "get", native_storage_get, 1);
    stdlib_register(pkg, "delete", native_storage_delete, 1);
    stdlib_register(pkg, "url", native_storage_url, -1);
}
