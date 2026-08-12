#include "common.h"
#include "memory.h"
#include "gc.h"
#include "object.h"

int main(int argc, char* argv[]) {
    // Handle --version flag
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("vek %s\n", VEK_VERSION_STRING);
        return 0;
    }

    // Initialize subsystems
    gc_init();
    intern_table_init();
    heap_init();

    // For now, just print usage
    printf("vek %s\n", VEK_VERSION_STRING);
    printf("Usage: vek [command] [file]\n");
    printf("Commands:\n");
    printf("  --version    Print version information\n");

    // Cleanup
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return 0;
}
