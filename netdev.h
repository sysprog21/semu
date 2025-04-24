#pragma once

#include <poll.h>
#include <unistd.h>

#include "minislirp/src/libslirp.h"
#include "utils.h"


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

/* SLIRP */
#define SLIRP_POLL_INTERVAL 100000
#define SLIRP_READ_SIDE 0
#define SLIRP_WRITE_SIDE 1
typedef struct {
    semu_timer_t timer;
    Slirp *slirp;
    SlirpTimerId id;
    void *cb_opaque;
    void (*cb)(void *opaque);
    int64_t expire_timer_msec;
} slirp_timer;

typedef struct {
    Slirp *slirp;
    int channel[2];
    int pfd_len;
    int pfd_size;
    struct pollfd *pfd;
    slirp_timer *timer;
    void *peer;
} net_user_options_t;

Slirp *slirp_create(net_user_options_t *usr, SlirpConfig *cfg);
int net_slirp_init(net_user_options_t *usr);
int semu_slirp_add_poll_socket(slirp_os_socket fd, int events, void *opaque);
int semu_slirp_get_revents(int idx, void *opaque);

typedef struct {
    char *name;
    netdev_impl_t type;
    void *op;
} netdev_t;

bool netdev_init(netdev_t *nedtev, const char *net_type);
