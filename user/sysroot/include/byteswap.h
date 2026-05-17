#ifndef LAMP_LIBC_BYTESWAP_H
#define LAMP_LIBC_BYTESWAP_H

#include <stdint.h>

static inline uint16_t bswap_16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t bswap_32(uint32_t x) {
    return __builtin_bswap32(x);
}

static inline uint64_t bswap_64(uint64_t x) {
    return __builtin_bswap64(x);
}

#endif
