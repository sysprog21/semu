#ifndef __SEMU_MUL128_H__
#define __SEMU_MUL128_H__

#include <stdint.h>

static inline uint64_t mulhu(uint64_t u, uint64_t v)
{
    uint64_t a = u >> 32;
    uint64_t b = u & 0xffffffff;
    uint64_t c = v >> 32;
    uint64_t d = v & 0xffffffff;

    uint64_t ac = a * c;
    uint64_t bc = b * c;
    uint64_t ad = a * d;
    uint64_t bd = b * d;

    uint64_t mid34 = (bd >> 32) + (bc & 0xffffffff) + (ad & 0xffffffff);

    uint64_t hi64 = ac + (bc >> 32) + (ad >> 32) + (mid34 >> 32);

    return hi64;
}

static inline int64_t mulh(int64_t u, int64_t v)
{
    return mulhu((uint64_t) u, (uint64_t) v) - ((u < 0) ? v : 0) -
           ((v < 0) ? u : 0);
}

static inline uint64_t mulhsu(int64_t u, uint64_t v)
{
    return mulhu((uint64_t) u, (uint64_t) v) - ((u < 0) ? v : 0);
}

#endif
