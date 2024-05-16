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
    return __builtin_clz(x | 1);
}

/*
 * Note: only applicable to 32-bit number
 *
 * Example output:
 * GET_INTR_IDX(1) = 0
 * GET_INTR_IDX(2) = 1
 * GET_INTR_IDX(4) = 2
 * GET_INTR_IDX(8) = 3
 */
#define GET_INTR_IDX(x) (31 - ilog2(x))

/* Range check
 * For any variable range checking:
 *     if (x >= minx && x <= maxx) ...
 * it is faster to use bit operation:
 *     if ((signed)((x - minx) | (maxx - x)) >= 0) ...
 */
#define RANGE_CHECK(x, minx, size) \
    ((int32_t) ((x - minx) | (minx + size - 1 - x)) >= 0)
