#pragma once

#include <stdbool.h>

/* clang-format off */
#define SUPPORTED_DEVICES   \
        _(tap)              \
        _(user)
/* clang-format on */

typedef enum {
#define _(dev) NETDEV_IMPL_##dev,
    SUPPORTED_DEVICES
#undef _
} netdev_impl_t;

typedef struct {
    int tap_fd;
} net_tap_options_t;

typedef struct {
    /* TODO: Implement user option */
} net_user_options_t;

typedef struct {
    char *name;
    netdev_impl_t type;
    void *op;
} netdev_t;

bool netdev_init(netdev_t *nedtev, const char *net_type);
