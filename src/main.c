#include "common.h"
#include "cli.h"

int main(int argc, char* argv[]) {
    // Handle --version flag (before anything else)
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("vek %s\n", VEK_VERSION_STRING);
        return 0;
    }

    // Handle --help flag
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        cli_print_help();
        return 0;
    }

    // Dispatch to the appropriate subcommand
    return cli_dispatch(argc, argv);
}
