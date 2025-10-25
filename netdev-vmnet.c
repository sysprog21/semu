/*
 * vmnet.framework based network backend for macOS
 *
 * Supports three modes:
 * - shared: NAT + DHCP (default)
 * - host: isolated network for VM-to-VM communication
 * - bridged: bridge with physical network interface
 *
 * Requires macOS 11.0+ and root privileges or com.apple.vm.networking
 * entitlement
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dispatch/dispatch.h>
#include <vmnet/vmnet.h>

#include "device.h"
#include "netdev.h"

#define VMNET_BUF_SIZE 2048

static void vmnet_packet_handler(net_vmnet_state_t *state,
                                 uint8_t *buf,
                                 ssize_t len)
{
    if (len <= 0)
        return;

    pthread_mutex_lock(&state->lock);

    /* Write packet size first (4 bytes) */
    uint32_t pkt_len = (uint32_t) len;
    if (write(state->pipe_fds[1], &pkt_len, sizeof(pkt_len)) !=
        sizeof(pkt_len)) {
        fprintf(stderr, "[vmnet] failed to write packet size to pipe\n");
        pthread_mutex_unlock(&state->lock);
        return;
    }

    /* Write packet data */
    ssize_t written = write(state->pipe_fds[1], buf, len);
    if (written != len) {
        fprintf(stderr, "[vmnet] failed to write packet to pipe: %zd/%zd\n",
                written, len);
    }

    pthread_mutex_unlock(&state->lock);
}

static int vmnet_init_shared(net_vmnet_state_t *state)
{
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
                              VMNET_SHARED_MODE);

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_queue_t queue =
        dispatch_queue_create("org.semu.vmnet.shared", DISPATCH_QUEUE_SERIAL);
    state->sem = sem;
    state->queue = queue;

    __block interface_ref iface = NULL;
    __block vmnet_return_t status = VMNET_FAILURE;

    iface = vmnet_start_interface(
        iface_desc, queue, ^(vmnet_return_t ret, xpc_object_t param) {
          status = ret;
          if (ret == VMNET_SUCCESS) {
              /* Extract MAC address */
              const char *mac_str =
                  xpc_dictionary_get_string(param, vmnet_mac_address_key);
              if (mac_str) {
                  sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                         &state->mac[0], &state->mac[1], &state->mac[2],
                         &state->mac[3], &state->mac[4], &state->mac[5]);
              }

              fprintf(stderr, "[vmnet] shared mode started\n");
              fprintf(stderr, "[vmnet] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                      state->mac[0], state->mac[1], state->mac[2],
                      state->mac[3], state->mac[4], state->mac[5]);

              /* Set up packet receive handler */
              vmnet_interface_set_event_callback(
                  iface, VMNET_INTERFACE_PACKETS_AVAILABLE, state->queue,
                  ^(interface_event_t event_id, xpc_object_t event) {
                    (void) event_id;
                    (void) event;

                    struct vmpktdesc pkts[32];
                    uint8_t bufs[32][VMNET_BUF_SIZE];
                    struct iovec iovs[32];
                    int pkt_cnt = sizeof(pkts) / sizeof(pkts[0]);

                    for (int i = 0; i < pkt_cnt; i++) {
                        iovs[i].iov_base = bufs[i];
                        iovs[i].iov_len = VMNET_BUF_SIZE;
                        pkts[i].vm_pkt_size = VMNET_BUF_SIZE;
                        pkts[i].vm_pkt_iov = &iovs[i];
                        pkts[i].vm_pkt_iovcnt = 1;
                        pkts[i].vm_flags = 0;
                    }

                    int received = pkt_cnt;
                    vmnet_return_t ret = vmnet_read(iface, pkts, &received);
                    if (ret != VMNET_SUCCESS) {
                        fprintf(stderr, "[vmnet] read failed: %d\n", ret);
                        return;
                    }

                    for (int i = 0; i < received; i++) {
                        vmnet_packet_handler((net_vmnet_state_t *) state,
                                             bufs[i], pkts[i].vm_pkt_size);
                    }
                  });
          }
          dispatch_semaphore_signal(state->sem);
        });

    /* Wait for interface creation */
    dispatch_semaphore_wait(state->sem, DISPATCH_TIME_FOREVER);

    if (status != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] failed to create interface: %d\n", status);
        xpc_release(iface_desc);
        return -1;
    }

    state->iface = iface;
    xpc_release(iface_desc);
    return 0;
}

static int vmnet_init_host(net_vmnet_state_t *state)
{
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
                              VMNET_HOST_MODE);

    state->sem = dispatch_semaphore_create(0);
    state->queue =
        dispatch_queue_create("org.semu.vmnet.host", DISPATCH_QUEUE_SERIAL);

    __block interface_ref iface = NULL;
    __block vmnet_return_t status = VMNET_FAILURE;

    iface = vmnet_start_interface(
        iface_desc, state->queue, ^(vmnet_return_t ret, xpc_object_t param) {
          status = ret;
          if (ret == VMNET_SUCCESS) {
              const char *mac_str =
                  xpc_dictionary_get_string(param, vmnet_mac_address_key);
              if (mac_str) {
                  sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                         &state->mac[0], &state->mac[1], &state->mac[2],
                         &state->mac[3], &state->mac[4], &state->mac[5]);
              }

              fprintf(stderr, "[vmnet] host mode started\n");
              fprintf(stderr, "[vmnet] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                      state->mac[0], state->mac[1], state->mac[2],
                      state->mac[3], state->mac[4], state->mac[5]);

              vmnet_interface_set_event_callback(
                  iface, VMNET_INTERFACE_PACKETS_AVAILABLE, state->queue,
                  ^(interface_event_t event_id, xpc_object_t event) {
                    (void) event_id;
                    (void) event;

                    struct vmpktdesc pkts[32];
                    uint8_t bufs[32][VMNET_BUF_SIZE];
                    struct iovec iovs[32];
                    int pkt_cnt = sizeof(pkts) / sizeof(pkts[0]);

                    for (int i = 0; i < pkt_cnt; i++) {
                        iovs[i].iov_base = bufs[i];
                        iovs[i].iov_len = VMNET_BUF_SIZE;
                        pkts[i].vm_pkt_size = VMNET_BUF_SIZE;
                        pkts[i].vm_pkt_iov = &iovs[i];
                        pkts[i].vm_pkt_iovcnt = 1;
                        pkts[i].vm_flags = 0;
                    }

                    int received = pkt_cnt;
                    vmnet_return_t ret = vmnet_read(iface, pkts, &received);
                    if (ret != VMNET_SUCCESS) {
                        fprintf(stderr, "[vmnet] read failed: %d\n", ret);
                        return;
                    }

                    for (int i = 0; i < received; i++) {
                        vmnet_packet_handler((net_vmnet_state_t *) state,
                                             bufs[i], pkts[i].vm_pkt_size);
                    }
                  });
          }
          dispatch_semaphore_signal(state->sem);
        });

    dispatch_semaphore_wait(state->sem, DISPATCH_TIME_FOREVER);

    if (status != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] failed to create interface: %d\n", status);
        xpc_release(iface_desc);
        return -1;
    }

    state->iface = iface;
    xpc_release(iface_desc);
    return 0;
}

static int vmnet_init_bridged(net_vmnet_state_t *state, const char *iface_name)
{
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key,
                              VMNET_BRIDGED_MODE);

    if (iface_name && strlen(iface_name) > 0) {
        xpc_dictionary_set_string(iface_desc, vmnet_shared_interface_name_key,
                                  iface_name);
    }

    state->sem = dispatch_semaphore_create(0);
    state->queue =
        dispatch_queue_create("org.semu.vmnet.bridged", DISPATCH_QUEUE_SERIAL);

    __block interface_ref iface = NULL;
    __block vmnet_return_t status = VMNET_FAILURE;

    iface = vmnet_start_interface(
        iface_desc, state->queue, ^(vmnet_return_t ret, xpc_object_t param) {
          status = ret;
          if (ret == VMNET_SUCCESS) {
              const char *mac_str =
                  xpc_dictionary_get_string(param, vmnet_mac_address_key);
              if (mac_str) {
                  sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                         &state->mac[0], &state->mac[1], &state->mac[2],
                         &state->mac[3], &state->mac[4], &state->mac[5]);
              }

              fprintf(stderr, "[vmnet] bridged mode started\n");
              fprintf(stderr, "[vmnet] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                      state->mac[0], state->mac[1], state->mac[2],
                      state->mac[3], state->mac[4], state->mac[5]);

              vmnet_interface_set_event_callback(
                  iface, VMNET_INTERFACE_PACKETS_AVAILABLE, state->queue,
                  ^(interface_event_t event_id, xpc_object_t event) {
                    (void) event_id;
                    (void) event;

                    struct vmpktdesc pkts[32];
                    uint8_t bufs[32][VMNET_BUF_SIZE];
                    struct iovec iovs[32];
                    int pkt_cnt = sizeof(pkts) / sizeof(pkts[0]);

                    for (int i = 0; i < pkt_cnt; i++) {
                        iovs[i].iov_base = bufs[i];
                        iovs[i].iov_len = VMNET_BUF_SIZE;
                        pkts[i].vm_pkt_size = VMNET_BUF_SIZE;
                        pkts[i].vm_pkt_iov = &iovs[i];
                        pkts[i].vm_pkt_iovcnt = 1;
                        pkts[i].vm_flags = 0;
                    }

                    int received = pkt_cnt;
                    vmnet_return_t ret = vmnet_read(iface, pkts, &received);
                    if (ret != VMNET_SUCCESS) {
                        fprintf(stderr, "[vmnet] read failed: %d\n", ret);
                        return;
                    }

                    for (int i = 0; i < received; i++) {
                        vmnet_packet_handler((net_vmnet_state_t *) state,
                                             bufs[i], pkts[i].vm_pkt_size);
                    }
                  });
          }
          dispatch_semaphore_signal(state->sem);
        });

    dispatch_semaphore_wait(state->sem, DISPATCH_TIME_FOREVER);

    if (status != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] failed to create interface: %d\n", status);
        xpc_release(iface_desc);
        return -1;
    }

    state->iface = iface;
    xpc_release(iface_desc);
    return 0;
}

int net_vmnet_init(netdev_t *netdev,
                   semu_vmnet_mode_t mode,
                   const char *iface_name)
{
    net_vmnet_state_t *state = (net_vmnet_state_t *) netdev->op;

    /* Create pipe for poll() integration */
    if (pipe(state->pipe_fds) < 0) {
        fprintf(stderr, "[vmnet] failed to create pipe: %s\n", strerror(errno));
        return -1;
    }

    /* Set read end non-blocking */
    int flags = fcntl(state->pipe_fds[0], F_GETFL, 0);
    fcntl(state->pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

    pthread_mutex_init(&state->lock, NULL);
    state->running = true;

    int ret = -1;
    switch (mode) {
    case SEMU_VMNET_SHARED:
        ret = vmnet_init_shared(state);
        break;
    case SEMU_VMNET_HOST:
        ret = vmnet_init_host(state);
        break;
    case SEMU_VMNET_BRIDGED:
        ret = vmnet_init_bridged(state, iface_name);
        break;
    default:
        fprintf(stderr, "[vmnet] unknown mode: %d\n", mode);
        return -1;
    }

    if (ret < 0) {
        close(state->pipe_fds[0]);
        close(state->pipe_fds[1]);
        pthread_mutex_destroy(&state->lock);
        return -1;
    }

    return 0;
}

ssize_t net_vmnet_read(net_vmnet_state_t *state, uint8_t *buf, size_t len)
{
    /* Read packet size first */
    uint32_t pkt_len;
    ssize_t n = read(state->pipe_fds[0], &pkt_len, sizeof(pkt_len));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;
        fprintf(stderr, "[vmnet] failed to read packet size: %s\n",
                strerror(errno));
        return -1;
    }

    if (n != sizeof(pkt_len)) {
        fprintf(stderr, "[vmnet] partial read of packet size\n");
        return -1;
    }

    if (pkt_len > len) {
        fprintf(stderr, "[vmnet] packet too large: %u > %zu\n", pkt_len, len);
        /* Drain the packet safely in chunks */
        uint8_t tmp[VMNET_BUF_SIZE];
        size_t remaining = pkt_len;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
            ssize_t n = read(state->pipe_fds[0], tmp, chunk);
            if (n <= 0)
                break;
            remaining -= n;
        }
        return -1;
    }

    /* Read packet data */
    ssize_t total = 0;
    while (total < pkt_len) {
        n = read(state->pipe_fds[0], buf + total, pkt_len - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            fprintf(stderr, "[vmnet] failed to read packet data: %s\n",
                    strerror(errno));
            return -1;
        }
        total += n;
    }

    return total;
}

ssize_t net_vmnet_write(net_vmnet_state_t *state,
                        const uint8_t *buf,
                        size_t len)
{
    if (!state->running || !state->iface)
        return -1;

    struct iovec iov;
    iov.iov_base = (void *) buf;
    iov.iov_len = len;

    struct vmpktdesc pkt;
    pkt.vm_pkt_size = len;
    pkt.vm_pkt_iov = &iov;
    pkt.vm_pkt_iovcnt = 1;
    pkt.vm_flags = 0;

    int pkt_cnt = 1;
    vmnet_return_t ret = vmnet_write(state->iface, &pkt, &pkt_cnt);

    if (ret != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] write failed: %d\n", ret);
        return -1;
    }

    return pkt_cnt > 0 ? len : -1;
}

ssize_t net_vmnet_writev(net_vmnet_state_t *state,
                         const struct iovec *iov,
                         size_t iovcnt)
{
    if (!state->running || !state->iface)
        return -1;

    /* Calculate total size */
    size_t total_len = 0;
    for (size_t i = 0; i < iovcnt; i++)
        total_len += iov[i].iov_len;

    /* Prepare vmpktdesc with the iovec array */
    struct vmpktdesc pkt;
    pkt.vm_pkt_size = total_len;
    pkt.vm_pkt_iov = (struct iovec *) iov;
    pkt.vm_pkt_iovcnt = iovcnt;
    pkt.vm_flags = 0;

    int pkt_cnt = 1;
    vmnet_return_t ret = vmnet_write(state->iface, &pkt, &pkt_cnt);

    if (ret != VMNET_SUCCESS) {
        fprintf(stderr, "[vmnet] writev failed: %d\n", ret);
        return -1;
    }

    return pkt_cnt > 0 ? total_len : -1;
}

int net_vmnet_get_fd(net_vmnet_state_t *state)
{
    return state->pipe_fds[0];
}

void net_vmnet_cleanup(net_vmnet_state_t *state)
{
    if (!state)
        return;

    state->running = false;

    if (state->iface) {
        vmnet_stop_interface(state->iface, state->queue, ^(vmnet_return_t ret) {
          (void) ret;
        });
        state->iface = NULL;
    }

    if (state->queue) {
        dispatch_release(state->queue);
        state->queue = NULL;
    }

    if (state->sem) {
        dispatch_release(state->sem);
        state->sem = NULL;
    }

    close(state->pipe_fds[0]);
    close(state->pipe_fds[1]);
    pthread_mutex_destroy(&state->lock);
}
