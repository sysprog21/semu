#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

#include "common.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

#define TAP_INTERFACE "tap%d"

#define VNET_DEV_CNT_MAX 1

#define VNET_FEATURES_0 0
#define VNET_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VNET_QUEUE_NUM_MAX 1024
#define VNET_QUEUE (vnet->queues[vnet->QueueSel])

#define PRIV(x) ((struct virtio_net_config *) x->priv)

enum { VNET_QUEUE_RX = 0, VNET_QUEUE_TX = 1 };

struct virtio_net_config {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} __attribute__((packed));

static struct virtio_net_config vnet_configs[VNET_DEV_CNT_MAX];
static int vnet_dev_cnt = 0;

static void virtio_net_set_fail(virtio_net_state_t *vnet)
{
    vnet->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vnet->Status & VIRTIO_STATUS__DRIVER_OK)
        vnet->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vnet_preprocess(virtio_net_state_t *vnet, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_net_set_fail(vnet), 0;

    return addr >> 2;
}

static void virtio_net_update_status(virtio_net_state_t *vnet, uint32_t status)
{
    vnet->Status |= status;
    if (status)
        return;

    /* Reset */
    int tap_fd = vnet->tap_fd;
    uint32_t *ram = vnet->ram;
    void *priv = vnet->priv;
    memset(vnet, 0, sizeof(*vnet));
    vnet->tap_fd = tap_fd, vnet->ram = ram;
    vnet->priv = priv;
}

static bool vnet_iovec_write(struct iovec **vecs,
                             size_t *nvecs,
                             const uint8_t *src,
                             size_t n)
{
    while (n && *nvecs) {
        if (n < (*vecs)->iov_len) {
            memcpy((*vecs)->iov_base, src, n);
            (*vecs)->iov_base = (void *) ((uintptr_t) (*vecs)->iov_base + n);
            (*vecs)->iov_len -= n;
            return true;
        }

        memcpy((*vecs)->iov_base, src, (*vecs)->iov_len);
        src += (*vecs)->iov_len;
        n -= (*vecs)->iov_len;
        (*vecs)++;
        (*nvecs)--;
    }
    return n && !*nvecs;
}

static bool vnet_iovec_read(struct iovec **vecs,
                            size_t *nvecs,
                            uint8_t *dst,
                            size_t n)
{
    while (n && *nvecs) {
        if (n < (*vecs)->iov_len) {
            memcpy(dst, (*vecs)->iov_base, n);
            (*vecs)->iov_base = (void *) ((uintptr_t) (*vecs)->iov_base + n);
            (*vecs)->iov_len -= n;
            return true;
        }
        memcpy(dst, (*vecs)->iov_base, (*vecs)->iov_len);
        dst += (*vecs)->iov_len;
        n -= (*vecs)->iov_len;
        (*vecs)++;
        (*nvecs)--;
    }
    return n && !*nvecs;
}

/* Require existing 'desc_idx' to use as iteration variable, and input
 * 'buffer_idx'.
 */
#define VNET_ITERATE_BUFFER(checked, body)                            \
    desc_idx = buffer_idx;                                            \
    while (1) {                                                       \
        if (checked && desc_idx >= queue->QueueNum)                   \
            return virtio_net_set_fail(vnet);                         \
        const uint32_t *desc = &ram[queue->QueueDesc + desc_idx * 4]; \
        uint16_t desc_flags = desc[3];                                \
        body if (!(desc_flags & VIRTIO_DESC_F_NEXT)) break;           \
        desc_idx = desc[3] >> 16;                                     \
    }

/* Input: 'buffer_idx'.
 * Output: 'buffer_niovs' and 'buffer_iovs'
 */
#define VNET_BUFFER_TO_IOV(expect_readable)                               \
    uint16_t desc_idx;                                                    \
    /* do a first pass to validate flags and count buffers */             \
    size_t buffer_niovs = 0;                                              \
    VNET_ITERATE_BUFFER(                                                  \
        true, if ((!!(desc_flags & VIRTIO_DESC_F_WRITE)) !=               \
                  (expect_readable)) return virtio_net_set_fail(vnet);    \
        buffer_niovs++;)                                                  \
    /* convert to iov */                                                  \
    struct iovec buffer_iovs[buffer_niovs];                               \
    buffer_niovs = 0;                                                     \
    VNET_ITERATE_BUFFER(                                                  \
        false, uint32_t desc_addr = desc[0]; uint32_t desc_len = desc[2]; \
        buffer_iovs[buffer_niovs].iov_base =                              \
            (void *) ((uintptr_t) ram + desc_addr);                       \
        buffer_iovs[buffer_niovs].iov_len = desc_len; buffer_niovs++;)

#define VNET_GENERATE_QUEUE_HANDLER(NAME_SUFFIX, VERB, QUEUE_IDX, READ)        \
    static void virtio_net_try_##NAME_SUFFIX(virtio_net_state_t *vnet)         \
    {                                                                          \
        uint32_t *ram = vnet->ram;                                             \
        virtio_net_queue_t *queue = &vnet->queues[QUEUE_IDX];                  \
        if ((vnet->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) ||              \
            !queue->fd_ready)                                                  \
            return;                                                            \
        if (!((vnet->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))      \
            return virtio_net_set_fail(vnet);                                  \
                                                                               \
        /* check for new buffers */                                            \
        uint16_t new_avail = ram[queue->QueueAvail] >> 16;                     \
        if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)        \
            return (fprintf(stderr, "size check fail\n"),                      \
                    virtio_net_set_fail(vnet));                                \
        if (queue->last_avail == new_avail)                                    \
            return;                                                            \
                                                                               \
        /* process them */                                                     \
        uint16_t new_used = ram[queue->QueueUsed] >> 16;                       \
        while (queue->last_avail != new_avail) {                               \
            uint16_t queue_idx = queue->last_avail % queue->QueueNum;          \
            uint16_t buffer_idx =                                              \
                ram[queue->QueueAvail + 1 + queue_idx / 2] >>                  \
                (16 * (queue_idx % 2));                                        \
            VNET_BUFFER_TO_IOV(READ)                                           \
            struct iovec *buffer_iovs_cursor = buffer_iovs;                    \
            uint8_t virtio_header[12];                                         \
            if (READ) {                                                        \
                memset(virtio_header, 0, sizeof(virtio_header));               \
                virtio_header[10] = 1;                                         \
                vnet_iovec_write(&buffer_iovs_cursor, &buffer_niovs,           \
                                 virtio_header, sizeof(virtio_header));        \
            } else {                                                           \
                vnet_iovec_read(&buffer_iovs_cursor, &buffer_niovs,            \
                                virtio_header, sizeof(virtio_header));         \
            }                                                                  \
                                                                               \
            ssize_t plen =                                                     \
                VERB##v(vnet->tap_fd, buffer_iovs_cursor, buffer_niovs);       \
            if (plen < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {       \
                queue->fd_ready = false;                                       \
                break;                                                         \
            }                                                                  \
            if (plen < 0) {                                                    \
                plen = 0;                                                      \
                fprintf(stderr, "[VNET] could not " #VERB " packet: %s\n",     \
                        strerror(errno));                                      \
            }                                                                  \
                                                                               \
            /* consume from available queue, write to used queue */            \
            queue->last_avail++;                                               \
            ram[queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2] =     \
                buffer_idx;                                                    \
            ram[queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2 + 1] = \
                READ ? (plen + sizeof(virtio_header)) : 0;                     \
            new_used++;                                                        \
        }                                                                      \
        vnet->ram[queue->QueueUsed] &= MASK(16);                               \
        vnet->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16;            \
                                                                               \
        /* send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */         \
        if (!(ram[queue->QueueAvail] & 1))                                     \
            vnet->InterruptStatus |= VIRTIO_INT__USED_RING;                    \
    }

VNET_GENERATE_QUEUE_HANDLER(rx, read, VNET_QUEUE_RX, true)
VNET_GENERATE_QUEUE_HANDLER(tx, write, VNET_QUEUE_TX, false)

void virtio_net_refresh_queue(virtio_net_state_t *vnet)
{
    if (!(vnet->Status & VIRTIO_STATUS__DRIVER_OK) ||
        (vnet->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET))
        return;

    struct pollfd pfd = {vnet->tap_fd, POLLIN | POLLOUT, 0};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN) {
        vnet->queues[VNET_QUEUE_RX].fd_ready = true;
        virtio_net_try_rx(vnet);
    }
    if (pfd.revents & POLLOUT) {
        vnet->queues[VNET_QUEUE_TX].fd_ready = true;
        virtio_net_try_tx(vnet);
    }
}

static bool virtio_net_reg_read(virtio_net_state_t *vnet,
                                uint32_t addr,
                                uint32_t *value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        *value = 0x74726976;
        return true;
    case _(Version):
        *value = 2;
        return true;
    case _(DeviceID):
        *value = 1;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;

    case _(DeviceFeatures):
        *value = vnet->DeviceFeaturesSel == 0
                     ? VNET_FEATURES_0
                     : (vnet->DeviceFeaturesSel == 1 ? VNET_FEATURES_1 : 0);
        return true;

    case _(QueueNumMax):
        *value = VNET_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VNET_QUEUE.ready ? 1 : 0;
        return true;

    case _(InterruptStatus):
        *value = vnet->InterruptStatus;
        return true;
    case _(Status):
        *value = vnet->Status;
        return true;

    case _(ConfigGeneration):
        *value = 0;
        return true;

    /* TODO: May want to check the occasion that the Linux kernel
     * touches the MAC address of the virtio-net under 8-bit accesses
     */
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_net_config)))
            return false;

        /* Read configuration from the corresponding register */
        *value = ((uint32_t *) PRIV(vnet))[addr - _(Config)];

        return true;
    }
#undef _
}

static bool virtio_net_reg_write(virtio_net_state_t *vnet,
                                 uint32_t addr,
                                 uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vnet->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        vnet->DriverFeaturesSel == 0 ? (vnet->DriverFeatures = value) : 0;
        return true;
    case _(DriverFeaturesSel):
        vnet->DriverFeaturesSel = value;
        return true;

    case _(QueueSel):
        if (value < ARRAY_SIZE(vnet->queues))
            vnet->QueueSel = value;
        else
            virtio_net_set_fail(vnet);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VNET_QUEUE_NUM_MAX)
            VNET_QUEUE.QueueNum = value;
        else
            virtio_net_set_fail(vnet);
        return true;
    case _(QueueReady):
        VNET_QUEUE.ready = value & 1;
        if (value & 1)
            VNET_QUEUE.last_avail = vnet->ram[VNET_QUEUE.QueueAvail] >> 16;
        if (vnet->QueueSel == VNET_QUEUE_RX)
            vnet->ram[VNET_QUEUE.QueueAvail] |=
                1; /* set VIRTQ_AVAIL_F_NO_INTERRUPT */
        return true;
    case _(QueueDescLow):
        VNET_QUEUE.QueueDesc = vnet_preprocess(vnet, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_net_set_fail(vnet);
        return true;
    case _(QueueDriverLow):
        VNET_QUEUE.QueueAvail = vnet_preprocess(vnet, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_net_set_fail(vnet);
        return true;
    case _(QueueDeviceLow):
        VNET_QUEUE.QueueUsed = vnet_preprocess(vnet, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_net_set_fail(vnet);
        return true;

    case _(QueueNotify):
        if (value < ARRAY_SIZE(vnet->queues)) {
            switch (value) {
            case VNET_QUEUE_RX:
                virtio_net_try_rx(vnet);
                break;
            case VNET_QUEUE_TX:
                virtio_net_try_tx(vnet);
                break;
            }
        } else {
            virtio_net_set_fail(vnet);
        }
        return true;
    case _(InterruptACK):
        vnet->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_net_update_status(vnet, value);
        return true;

    /* TODO: May want to check the occasion that the Linux kernel
     * touches the MAC address of the virtio-net under 8-bit accesses
     */
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_net_config)))
            return false;

        /* Write configuration to the corresponding register */
        ((uint32_t *) PRIV(vnet))[addr - _(Config)] = value;

        return true;
    }
#undef _
}

void virtio_net_read(hart_t *vm,
                     virtio_net_state_t *vnet,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_net_reg_read(vnet, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void virtio_net_write(hart_t *vm,
                      virtio_net_state_t *vnet,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_net_reg_write(vnet, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

bool virtio_net_init(virtio_net_state_t *vnet)
{
    if (vnet_dev_cnt >= VNET_DEV_CNT_MAX) {
        fprintf(stderr,
                "Excedded the number of virtio-net device can be allocated.\n");
        exit(2);
    }

    /* Allocate memory for the private member */
    vnet->priv = &vnet_configs[vnet_dev_cnt++];

    vnet->tap_fd = open("/dev/net/tun", O_RDWR);
    if (vnet->tap_fd < 0) {
        fprintf(stderr, "failed to open TAP device: %s\n", strerror(errno));
        return false;
    }

    /* Specify persistent tap device */
    struct ifreq ifreq = {.ifr_flags = IFF_TAP | IFF_NO_PI};
    strncpy(ifreq.ifr_name, TAP_INTERFACE, sizeof(ifreq.ifr_name));
    if (ioctl(vnet->tap_fd, TUNSETIFF, &ifreq) < 0) {
        fprintf(stderr, "failed to allocate TAP device: %s\n", strerror(errno));
        return false;
    }

    fprintf(stderr, "allocated TAP interface: %s\n", ifreq.ifr_name);
    assert(fcntl(vnet->tap_fd, F_SETFL,
                 fcntl(vnet->tap_fd, F_GETFL, 0) | O_NONBLOCK) >= 0);

    return true;
}
