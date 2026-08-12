#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <fcntl.h>
#include <unistd.h>

// uuid.v4() - generates a random UUID v4 string
// Format: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" where y is 8, 9, a, or b
static Value native_uuid_v4(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    uint8_t bytes[16];

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return VAL_NIL;

    size_t total = 0;
    while (total < 16) {
        ssize_t r = read(fd, bytes + total, 16 - total);
        if (r <= 0) { close(fd); return VAL_NIL; }
        total += (size_t)r;
    }
    close(fd);

    // Set version (4) in byte 6: clear top 4 bits, set 0100
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set variant (10xx) in byte 8: clear top 2 bits, set 10
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as UUID string: 8-4-4-4-12
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return OBJ_VAL(obj_string_new(uuid, 36));
}

void stdlib_uuid_init(ObjMap* pkg) {
    stdlib_register(pkg, "v4", native_uuid_v4, 0);
}
