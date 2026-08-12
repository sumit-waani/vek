#ifndef VEK_CLI_H
#define VEK_CLI_H

#include "common.h"

// ANSI color macros
#define CLI_RED     "\033[31m"
#define CLI_GREEN   "\033[32m"
#define CLI_YELLOW  "\033[33m"
#define CLI_BLUE    "\033[34m"
#define CLI_CYAN    "\033[36m"
#define CLI_BOLD    "\033[1m"
#define CLI_DIM     "\033[2m"
#define CLI_RESET   "\033[0m"

// Command handler function pointer type
typedef int (*CommandHandler)(int argc, char** argv);

// Command struct
typedef struct {
    const char* name;
    const char* description;
    const char* usage;
    CommandHandler handler;
} Command;

// Check if color output is enabled (checks isatty + NO_COLOR env)
bool cli_color_enabled(void);

// Argument parsing helpers
bool cli_has_flag(int argc, char** argv, const char* flag);
const char* cli_get_option(int argc, char** argv, const char* key);

// Print help text (banner + all commands)
void cli_print_help(void);

// Dispatch command based on argv[1]
int cli_dispatch(int argc, char** argv);

#endif // VEK_CLI_H
