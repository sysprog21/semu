#pragma once

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>
#include <unistd.h>

#include "minislirp/src/libslirp.h"
#include "utils.h"

/* Forward declarations */
typedef struct netdev netdev_t;

/* clang-format off */
#if defined(__APPLE__)
#define SUPPORTED_DEVICES   \
        _(vmnet)            \
        _(user)
#else
#define SUPPORTED_DEVICES   \
        _(tap)              \
        _(user)
#endif
/* clang-format on */

typedef enum {
#define _(dev) NETDEV_IMPL_##dev,
    SUPPORTED_DEVICES
#undef _
} netdev_impl_t;

typedef struct {
    int tap_fd;
} net_tap_options_t;

/* vmnet (macOS) */
#if defined(__APPLE__)
#include <pthread.h>

typedef struct {
    void *iface;     /* interface_ref (opaque) */
    void *queue;     /* dispatch_queue_t (opaque) */
    void *sem;       /* dispatch_semaphore_t (opaque) */
    int pipe_fds[2]; /* pipe for integration with poll() */
    uint8_t mac[6];
    pthread_mutex_t lock;
    bool running;
} net_vmnet_state_t;

typedef net_vmnet_state_t net_vmnet_options_t;

/* Use vmnet framework's mode constants */
typedef enum {
    SEMU_VMNET_SHARED = 0,
    SEMU_VMNET_HOST = 1,
    SEMU_VMNET_BRIDGED = 2,
} semu_vmnet_mode_t;

int net_vmnet_init(netdev_t *netdev,
                   semu_vmnet_mode_t mode,
                   const char *iface_name);
ssize_t net_vmnet_read(net_vmnet_state_t *state, uint8_t *buf, size_t len);
ssize_t net_vmnet_write(net_vmnet_state_t *state,
                        const uint8_t *buf,
                        size_t len);
ssize_t net_vmnet_writev(net_vmnet_state_t *state,
                         const struct iovec *iov,
                         size_t iovcnt);
int net_vmnet_get_fd(net_vmnet_state_t *state);
void net_vmnet_cleanup(net_vmnet_state_t *state);
#endif

/* SLIRP (cross-platform userspace network) */
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
    int guest_to_host_channel[2];
    int host_to_guest_channel[2];
    int pfd_len;
    int pfd_size;
    struct pollfd *pfd;
    slirp_timer *timer;
    void *peer;
} net_user_options_t;

Slirp *slirp_create(net_user_options_t *usr, SlirpConfig *cfg);
int net_slirp_init(net_user_options_t *usr);
int net_slirp_read(net_user_options_t *usr);
int semu_slirp_add_poll_socket(slirp_os_socket fd, int events, void *opaque);
int semu_slirp_get_revents(int idx, void *opaque);

struct netdev {
    char *name;
    netdev_impl_t type;
    void *op;
};

bool netdev_init(netdev_t *nedtev, const char *net_type);
