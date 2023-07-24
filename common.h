#pragma once

#include "feature.h"

#define unlikely(x) __builtin_expect((x), 0)
#define likely(x) __builtin_expect((x), 1)

#define UNUSED __attribute__((unused))

#define MASK(n) (~((~0U << (n))))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))
