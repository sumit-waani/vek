#ifndef VEK_FORMATTER_H
#define VEK_FORMATTER_H

#include "common.h"

// Format a .ve source string and return newly allocated formatted output.
// Returns NULL on error. Caller must free the result.
char* fmt_format(const char* source, size_t length, size_t* out_length);

#endif // VEK_FORMATTER_H
