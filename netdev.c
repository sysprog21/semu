#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__APPLE__)
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#endif

#include "device.h"
#include "netdev.h"

#if !defined(__APPLE__)
static int net_init_tap(netdev_t *netdev);

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
#endif

static int net_init_user(netdev_t *netdev);

#if !defined(__APPLE__)
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

#endif

static int net_init_user(netdev_t *netdev)
{
    net_user_options_t *usr = (net_user_options_t *) netdev->op;
    memset(usr, 0, sizeof(*usr));
    usr->peer = container_of(netdev, virtio_net_state_t, peer);
    net_slirp_init(usr);

    return 0;
}

bool netdev_init(netdev_t *netdev, const char *net_type)
{
#if defined(__APPLE__)
    /* macOS: support vmnet (kernel, requires sudo) and user (slirp, no sudo) */
    if (!net_type || strcmp(net_type, "vmnet") == 0) {
        netdev->type = NETDEV_IMPL_vmnet;
        netdev->op = calloc(1, sizeof(net_vmnet_options_t));
        if (!netdev->op) {
            fprintf(stderr, "Failed to allocate memory for vmnet device\n");
            return false;
        }
        if (net_vmnet_init(netdev, SEMU_VMNET_SHARED, NULL) != 0) {
            free(netdev->op);
            netdev->op = NULL;
            /* If net_type was explicitly "vmnet", fail */
            if (net_type && strcmp(net_type, "vmnet") == 0) {
                fprintf(stderr,
                        "vmnet init failed. Run with sudo or use -n user\n");
                return false;
            }
            /* Auto-fallback to user mode when no explicit backend specified */
            fprintf(stderr,
                    "vmnet init failed (need sudo), falling back to user "
                    "mode...\n");
            /* Continue to user mode initialization below */
        } else {
            /* vmnet init succeeded */
            return true;
        }
    }

    /* Initialize user mode (SLIRP) - either explicitly requested or fallback */
    if (!net_type || strcmp(net_type, "user") == 0) {
        netdev->type = NETDEV_IMPL_user;
        /* If we already allocated for vmnet, netdev->op is NULL here */
        if (!netdev->op) {
            netdev->op = malloc(sizeof(net_user_options_t));
            if (!netdev->op) {
                fprintf(stderr, "Failed to allocate memory for user device\n");
                return false;
            }
        }
        if (net_init_user(netdev) != 0) {
            free(netdev->op);
            netdev->op = NULL;
            return false;
        }
        return true;
    }

    fprintf(stderr,
            "unsupported network type on macOS: %s (use 'vmnet' or 'user')\n",
            net_type);
    return false;
#else
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
#endif
}
