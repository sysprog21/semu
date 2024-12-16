#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "netdev.h"

static int net_init_tap();
static int net_init_user();

static const char *netdev_impl_lookup[] = {
#define _(dev) [NETDEV_IMPL_##dev] = #dev,
    SUPPORTED_DEVICES
#undef _
        NULL,
};

static int find_net_dev_idx(const char *net_type, const char **netlookup)
{
    if (!net_type)
        return -1;

    for (int i = 0; netlookup[i]; i++) {
        if (!strcmp(net_type, netlookup[i]))
            return i;
    }
    return -1;
}

static int net_init_tap(netdev_t *netdev)
{
    net_tap_options_t *tap = (net_tap_options_t *) netdev->op;
    tap->tap_fd = open("/dev/net/tun", O_RDWR);
    if (tap->tap_fd < 0) {
        fprintf(stderr, "failed to open TAP device: %s\n", strerror(errno));
        return false;
    }

    /* Specify persistent tap device */
    struct ifreq ifreq = {.ifr_flags = IFF_TAP | IFF_NO_PI};
    strncpy(ifreq.ifr_name, "tap%d", sizeof(ifreq.ifr_name));
    if (ioctl(tap->tap_fd, TUNSETIFF, &ifreq) < 0) {
        fprintf(stderr, "failed to allocate TAP device: %s\n", strerror(errno));
        return false;
    }

    fprintf(stderr, "allocated TAP interface: %s\n", ifreq.ifr_name);
    assert(fcntl(tap->tap_fd, F_SETFL,
                 fcntl(tap->tap_fd, F_GETFL, 0) | O_NONBLOCK) >= 0);
    return 0;
}

static int net_init_user(netdev_t *netdev UNUSED)
{
    /* TODO: create slirp dev */
    return 0;
}

bool netdev_init(netdev_t *netdev, const char *net_type)
{
    int dev_idx = find_net_dev_idx(net_type, netdev_impl_lookup);
    if (dev_idx == -1)
        return false;
    netdev->type = dev_idx;

    switch (dev_idx) {
#define _(dev)                                                           \
    case NETDEV_IMPL_##dev:                                              \
        netdev->op = malloc(sizeof(net_##dev##_options_t));              \
        if (!netdev->op) {                                               \
            fprintf(stderr, "Failed to allocate memory for %s device\n", \
                    #dev);                                               \
            return false;                                                \
        }                                                                \
        net_init_##dev(netdev);                                          \
        break;
        SUPPORTED_DEVICES
#undef _
    default:
        fprintf(stderr, "unknown network device\n");
        break;
    }

    return true;
}
