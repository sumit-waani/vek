#ifndef VEK_SHA256_H
#define VEK_SHA256_H

#include <stdint.h>
#include <stddef.h>

void sha256_compute(const uint8_t* data, size_t len, uint8_t out[32]);

#endif // VEK_SHA256_H
