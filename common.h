#pragma once

#include "feature.h"

#define unlikely(x) __builtin_expect((x), 0)
#define likely(x) __builtin_expect((x), 1)

#define UNUSED __attribute__((unused))

#define MASK(n) (~((~0U << (n))))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

/* Ensure that __builtin_clz is never called with 0 argument */
static inline int ilog2(int x)
{
    return 31 - __builtin_clz(x | 1);
}

/* Range check
 * For any variable range checking:
 *     if (x >= minx && x <= maxx) ...
 * it is faster to use bit operation:
 *     if ((signed)((x - minx) | (maxx - x)) >= 0) ...
 */
#define RANGE_CHECK(x, minx, size) \
    ((int32_t) ((x - minx) | (minx + size - 1 - x)) >= 0)

/* Packed macro */
#if defined(__GNUC__) || defined(__clang__)
#define PACKED(name) name __attribute__((packed))
#elif defined(_MSC_VER)
#define PACKED(name) __pragma(pack(push, 1)) name __pragma(pack(pop))
#else /* unsupported compilers */
#define PACKED(name)
#endif
