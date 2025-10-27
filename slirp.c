#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netdev.h"

/* Slirp callback: invoked when Slirp wants to send a packet to the backend */
static ssize_t net_slirp_send_packet(const void *buf, size_t len, void *opaque)
{
    net_user_options_t *usr = (net_user_options_t *) opaque;

    return write(usr->guest_to_host_channel[SLIRP_WRITE_SIDE], buf, len);
}

/* Slirp callback: reports an error from the guest (current unused) */
static void net_slirp_guest_error(const char *msg UNUSED, void *opaque UNUSED)
{
    // Unused
}

/* Slirp callback: returns current time in nanoseconds for Slirp timers */
static int64_t net_slirp_clock_get_ns(void *opaque UNUSED)
{
    net_user_options_t *usr = (net_user_options_t *) opaque;

    return semu_timer_get(&usr->timer->timer);
}

/* Slirp callback: called when Slirp has finished initialization */
static void net_slirp_init_completed(Slirp *slirp, void *opaque)
{
    net_user_options_t *s = opaque;
    s->slirp = slirp;
}

static void slirp_timer_init(slirp_timer *t, void (*cb)(void *opaque))
{
    t->cb = cb;
    semu_timer_init(&t->timer, CLOCK_FREQ, 1);
}

static void net_slirp_timer_cb(void *opaque)
{
    slirp_timer *t = opaque;
    slirp_handle_timer(t->slirp, t->id, t->cb_opaque);
}

/* Slirp callback: allocated and initializes a new timer object */
static void *net_slirp_timer_new_opaque(SlirpTimerId id,
                                        void *cb_opaque,
                                        void *opaque)
{
    net_user_options_t *usr = (net_user_options_t *) opaque;
    slirp_timer *t = malloc(sizeof(slirp_timer));
    usr->timer = t;
    t->slirp = usr->slirp;
    t->id = id;
    t->cb_opaque = cb_opaque;
    t->expire_timer_msec = -1;
    slirp_timer_init(t, net_slirp_timer_cb);

    return t;
}

/* Slirp callback: releases resources associated with a timer */
static void net_slirp_timer_free(void *timer, void *opaque UNUSED)
{
    if (timer)
        free(timer);
}

/* Slirp callback: modifies the expiration time of an existing timer */
static void net_slirp_timer_mod(void *timer,
                                int64_t expire_time,
                                void *opaque UNUSED)
{
    slirp_timer *t = (slirp_timer *) timer;
    semu_timer_rebase(&t->timer, expire_time);
}

/* Slirp callback: registers a pollable socket (unused in this backend) */
static void net_slirp_register_poll_sock(int fd UNUSED, void *opaque UNUSED)
{
    // Unused
}

/* Slirp callback: unregisters a pollable socket (unused in this backend) */
static void net_slirp_unregister_poll_sock(int fd UNUSED, void *opaque UNUSED)
{
    // Unused
}

/* Slirp callback: notifies backend of pending activity (unused) */
static void net_slirp_notify(void *opaque UNUSED)
{
    // Unused
}

static const SlirpCb slirp_cb = {
    .send_packet = net_slirp_send_packet,
    .guest_error = net_slirp_guest_error,
    .clock_get_ns = net_slirp_clock_get_ns,
    .init_completed = net_slirp_init_completed,
    .timer_new_opaque = net_slirp_timer_new_opaque,
    .timer_free = net_slirp_timer_free,
    .timer_mod = net_slirp_timer_mod,
    .register_poll_socket = net_slirp_register_poll_sock,
    .unregister_poll_socket = net_slirp_unregister_poll_sock,
    .notify = net_slirp_notify,
};

static int poll_to_slirp_poll(int events)
{
    int ret = 0;
    if (events & POLLIN)
        ret |= SLIRP_POLL_IN;
    if (events & POLLOUT)
        ret |= SLIRP_POLL_OUT;
    if (events & POLLPRI)
        ret |= SLIRP_POLL_PRI;
    if (events & POLLERR)
        ret |= SLIRP_POLL_ERR;
    if (events & POLLHUP)
        ret |= SLIRP_POLL_HUP;
    return ret;
}

int semu_slirp_get_revents(int idx, void *opaque)
{
    net_user_options_t *usr = opaque;
    return poll_to_slirp_poll(usr->pfd[idx].revents);
}

int semu_slirp_add_poll_socket(slirp_os_socket fd, int events, void *opaque)
{
    net_user_options_t *usr = opaque;
    if (usr->pfd_len >= usr->pfd_size) {
        int newsize = usr->pfd_size + 16;
        struct pollfd *new = realloc(usr->pfd, newsize * sizeof(struct pollfd));
        if (new) {
            usr->pfd = new;
            usr->pfd_size = newsize;
        }
    }
    if (usr->pfd_len < usr->pfd_size) {
        int idx = usr->pfd_len++;
        usr->pfd[idx].fd = fd;

        usr->pfd[idx].events = poll_to_slirp_poll(events);
        return idx;
    } else {
        return -1;
    }
}

int net_slirp_read(net_user_options_t *usr)
{
    uint8_t pkt[1514];
    ssize_t plen =
        read(usr->host_to_guest_channel[SLIRP_READ_SIDE], pkt, sizeof(pkt));
    if (plen < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        fprintf(stderr, "[SLIRP] failed to read packet from virtio-net: %s\n",
                strerror(errno));
        return -1;
    }
    slirp_input(usr->slirp, pkt, plen);
    return plen;
}

Slirp *slirp_create(net_user_options_t *usr, SlirpConfig *cfg)
{
    /* Create a Slirp instance with special address. All
     * addresses of the form 10.0.2.xxx are special to
     * Slirp.
     */
    cfg->version = SLIRP_CHECK_VERSION(4, 8, 0)   ? 6
                   : SLIRP_CHECK_VERSION(4, 7, 0) ? 4
                                                  : 1;
    cfg->restricted = 0;
    cfg->in_enabled = 1;
    inet_pton(AF_INET, "10.0.2.0", &(cfg->vnetwork));
    inet_pton(AF_INET, "255.255.255.0", &(cfg->vnetmask));
    inet_pton(AF_INET, "10.0.2.2", &(cfg->vhost));
    cfg->in6_enabled = 1;
    inet_pton(AF_INET6, "fd00::", &cfg->vprefix_addr6);
    cfg->vhostname = "slirp";
    cfg->tftp_server_name = NULL;
    cfg->tftp_path = NULL;
    cfg->bootfile = NULL;
    inet_pton(AF_INET, "10.0.2.15", &(cfg->vdhcp_start));
    inet_pton(AF_INET, "10.0.2.3", &(cfg->vnameserver));
    inet_pton(AF_INET6, "fd00::3", &cfg->vnameserver6);
    cfg->vdnssearch = NULL;
    cfg->vdomainname = NULL;
    cfg->if_mtu = 1500;
    cfg->if_mru = 1500;
    cfg->outbound_addr = NULL;
    cfg->disable_host_loopback = 0;

    return slirp_new(cfg, &slirp_cb, usr);
}

int net_slirp_init(net_user_options_t *usr)
{
    SlirpConfig cfg;
    usr->slirp = slirp_create(usr, &cfg);
    if (usr->slirp == NULL) {
        fprintf(stderr, "create slirp failed\n");
    }

    if (pipe(usr->guest_to_host_channel) < 0)
        return -1;
    assert(
        fcntl(usr->guest_to_host_channel[SLIRP_READ_SIDE], F_SETFL,
              fcntl(usr->guest_to_host_channel[SLIRP_READ_SIDE], F_GETFL, 0) |
                  O_NONBLOCK) >= 0);

    if (pipe(usr->host_to_guest_channel) < 0)
        return -1;
    assert(
        fcntl(usr->host_to_guest_channel[SLIRP_READ_SIDE], F_SETFL,
              fcntl(usr->host_to_guest_channel[SLIRP_READ_SIDE], F_GETFL, 0) |
                  O_NONBLOCK) >= 0);
    assert(
        fcntl(usr->host_to_guest_channel[SLIRP_WRITE_SIDE], F_SETFL,
              fcntl(usr->host_to_guest_channel[SLIRP_WRITE_SIDE], F_GETFL, 0) |
                  O_NONBLOCK) >= 0);

    /* Register the read end of the internal pipe (channel[SLIRP_READ_SIDE])
     * with slirp's poll system. This allows slirp to monitor it for incoming
     * data (POLL_IN) or hang-up event (POLL_HUP).
     */
    semu_slirp_add_poll_socket(usr->guest_to_host_channel[SLIRP_READ_SIDE],
                               SLIRP_POLL_IN | SLIRP_POLL_HUP, usr);
    semu_slirp_add_poll_socket(usr->host_to_guest_channel[SLIRP_READ_SIDE],
                               SLIRP_POLL_IN | SLIRP_POLL_HUP, usr);
    return 0;
}
