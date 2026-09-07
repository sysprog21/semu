#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "console.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

#define VCON_FEATURES_0 0
#define VCON_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VCON_QUEUE_NUM_MAX 1024
#define VCON_INPUT_CHUNK 256
#define VCON_QUEUE (vcon->queues[vcon->QueueSel])

enum { VCON_QUEUE_RX = 0, VCON_QUEUE_TX = 1 };

static void virtio_console_set_fail(virtio_console_state_t *vcon)
{
    vcon->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vcon->Status & VIRTIO_STATUS__DRIVER_OK)
        vcon->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static bool vcon_guest_range_valid(uint64_t addr, uint64_t len)
{
    return addr <= RAM_SIZE && len <= (uint64_t) RAM_SIZE - addr;
}

static inline uint32_t vcon_preprocess(virtio_console_state_t *vcon,
                                       uint32_t addr)
{
    if (addr >= RAM_SIZE || (addr & 0b11))
        return virtio_console_set_fail(vcon), 0;

    return addr >> 2;
}

static bool vcon_queue_layout_valid(const virtio_console_queue_t *queue)
{
    uint64_t num = queue->QueueNum;
    uint64_t desc = (uint64_t) queue->QueueDesc << 2;
    uint64_t avail = (uint64_t) queue->QueueAvail << 2;
    uint64_t used = (uint64_t) queue->QueueUsed << 2;

    if (!num || num > VCON_QUEUE_NUM_MAX || (num & (num - 1)))
        return false;
    if ((desc & 0xf) || (avail & 0x1) || (used & 0x3))
        return false;

    return vcon_guest_range_valid(desc, num * 16) &&
           vcon_guest_range_valid(avail, 4 + num * 2) &&
           vcon_guest_range_valid(used, 4 + num * 8);
}

static void virtio_console_update_status(virtio_console_state_t *vcon,
                                         uint32_t status)
{
    vcon->Status |= status;
    if (status)
        return;

    uint32_t *ram = vcon->ram;
    int in_fd = vcon->in_fd;
    int out_fd = vcon->out_fd;

    memset(vcon, 0, sizeof(*vcon));
    vcon->ram = ram;
    vcon->in_fd = in_fd;
    vcon->out_fd = out_fd;
}

static uint16_t vcon_avail_entry(const uint32_t *ram,
                                 const virtio_console_queue_t *queue,
                                 uint16_t slot)
{
    uint32_t word = ram[queue->QueueAvail + 1 + slot / 2];
    return word >> (16 * (slot % 2));
}

static bool vcon_get_chain(virtio_console_state_t *vcon,
                           const virtio_console_queue_t *queue,
                           uint16_t head,
                           bool device_writes,
                           struct virtq_desc *chain,
                           uint16_t *chain_len)
{
    uint16_t desc_idx = head;

    *chain_len = 0;
    for (uint32_t steps = 0; steps < queue->QueueNum; steps++) {
        struct virtq_desc desc;

        if (desc_idx >= queue->QueueNum) {
            virtio_console_set_fail(vcon);
            return false;
        }

        memcpy(&desc, &vcon->ram[queue->QueueDesc + desc_idx * 4],
               sizeof(desc));
        if ((!!(desc.flags & VIRTIO_DESC_F_WRITE)) != device_writes ||
            (desc.flags & ~(VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE)) ||
            !vcon_guest_range_valid(desc.addr, desc.len)) {
            virtio_console_set_fail(vcon);
            return false;
        }

        chain[(*chain_len)++] = desc;
        if (!(desc.flags & VIRTIO_DESC_F_NEXT))
            return true;
        desc_idx = desc.next;
    }

    /* A chain with more descriptors than the queue contains has a cycle. */
    virtio_console_set_fail(vcon);
    return false;
}

static void vcon_push_used(virtio_console_state_t *vcon,
                           virtio_console_queue_t *queue,
                           uint16_t head,
                           uint32_t len)
{
    uint16_t used = vcon->ram[queue->QueueUsed] >> 16;
    uint32_t elem = queue->QueueUsed + 1 + (used % queue->QueueNum) * 2;

    vcon->ram[elem] = head;
    vcon->ram[elem + 1] = len;
    used++;
    vcon->ram[queue->QueueUsed] &= MASK(16);
    vcon->ram[queue->QueueUsed] |= (uint32_t) used << 16;

    if (!(vcon->ram[queue->QueueAvail] & 1))
        vcon->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool vcon_queue_pending(virtio_console_state_t *vcon,
                               virtio_console_queue_t *queue,
                               uint16_t *avail)
{
    *avail = vcon->ram[queue->QueueAvail] >> 16;
    if ((uint16_t) (*avail - queue->last_avail) <= queue->QueueNum)
        return queue->last_avail != *avail;

    virtio_console_set_fail(vcon);
    return false;
}

static bool vcon_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *cursor = buf;

    while (len) {
        ssize_t written = write(fd, cursor, len);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        cursor += written;
        len -= written;
    }

    return true;
}

static void vcon_process_tx(virtio_console_state_t *vcon)
{
    virtio_console_queue_t *queue = &vcon->queues[VCON_QUEUE_TX];
    struct virtq_desc chain[VCON_QUEUE_NUM_MAX];
    uint16_t avail;

    if (vcon->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;
    if (!(vcon->Status & VIRTIO_STATUS__DRIVER_OK) || !queue->ready) {
        virtio_console_set_fail(vcon);
        return;
    }

    if (!vcon_queue_pending(vcon, queue, &avail))
        return;

    while (queue->last_avail != avail) {
        uint16_t slot = queue->last_avail % queue->QueueNum;
        uint16_t head = vcon_avail_entry(vcon->ram, queue, slot);
        uint16_t chain_len;
        uint32_t total = 0;

        if (!vcon_get_chain(vcon, queue, head, false, chain, &chain_len))
            return;

        for (uint16_t i = 0; i < chain_len; i++) {
            if (chain[i].len > UINT32_MAX - total) {
                virtio_console_set_fail(vcon);
                return;
            }
            const void *buf =
                (const uint8_t *) vcon->ram + (uintptr_t) chain[i].addr;
            if (!vcon_write_all(vcon->out_fd, buf, chain[i].len)) {
                fprintf(stderr, "virtio-console: output failed: %s\n",
                        strerror(errno));
                virtio_console_set_fail(vcon);
                return;
            }
            total += chain[i].len;
        }

        queue->last_avail++;
        vcon_push_used(vcon, queue, head, total);
        if (!vcon_queue_pending(vcon, queue, &avail))
            return;
    }
}

static bool vcon_input_ready(int fd)
{
    struct pollfd pfd = {fd, POLLIN, 0};

    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

void virtio_console_refresh(virtio_console_state_t *vcon)
{
    virtio_console_queue_t *queue = &vcon->queues[VCON_QUEUE_RX];
    struct virtq_desc chain[VCON_QUEUE_NUM_MAX];
    uint8_t input[VCON_INPUT_CHUNK];
    uint16_t avail;

    if ((vcon->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) ||
        !(vcon->Status & VIRTIO_STATUS__DRIVER_OK) || !queue->ready ||
        !vcon_input_ready(vcon->in_fd) ||
        !vcon_queue_pending(vcon, queue, &avail))
        return;

    while (queue->last_avail != avail && vcon_input_ready(vcon->in_fd)) {
        uint16_t slot = queue->last_avail % queue->QueueNum;
        uint16_t head = vcon_avail_entry(vcon->ram, queue, slot);
        uint16_t chain_len;
        size_t capacity = 0;

        if (!vcon_get_chain(vcon, queue, head, true, chain, &chain_len))
            return;

        for (uint16_t i = 0; i < chain_len && capacity < sizeof(input); i++)
            capacity += MIN((size_t) chain[i].len, sizeof(input) - capacity);
        if (!capacity) {
            virtio_console_set_fail(vcon);
            return;
        }

        ssize_t nread = host_console_read(vcon->in_fd, input, capacity);
        if (nread < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (nread < 0) {
            fprintf(stderr, "virtio-console: input failed: %s\n",
                    strerror(errno));
            virtio_console_set_fail(vcon);
            return;
        }
        if (!nread)
            return;

        size_t copied = 0;
        for (uint16_t i = 0; i < chain_len && copied < (size_t) nread; i++) {
            size_t len = MIN((size_t) chain[i].len, (size_t) nread - copied);
            void *buf = (uint8_t *) vcon->ram + (uintptr_t) chain[i].addr;
            memcpy(buf, input + copied, len);
            copied += len;
        }

        queue->last_avail++;
        vcon_push_used(vcon, queue, head, nread);
        if (!vcon_queue_pending(vcon, queue, &avail))
            return;
    }
}

static bool virtio_console_reg_read(virtio_console_state_t *vcon,
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
        *value = 3;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vcon->DeviceFeaturesSel == 0
                     ? VCON_FEATURES_0
                     : (vcon->DeviceFeaturesSel == 1 ? VCON_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VCON_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VCON_QUEUE.ready;
        return true;
    case _(InterruptStatus):
        *value = vcon->InterruptStatus;
        return true;
    case _(Status):
        *value = vcon->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    default:
        return false;
    }
#undef _
}

static bool virtio_console_reg_write(virtio_console_state_t *vcon,
                                     uint32_t addr,
                                     uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vcon->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        if (vcon->DriverFeaturesSel == 0)
            vcon->DriverFeatures = value;
        return true;
    case _(DriverFeaturesSel):
        vcon->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vcon->queues))
            vcon->QueueSel = value;
        else
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueNum):
        if (VCON_QUEUE.ready)
            virtio_console_set_fail(vcon);
        else if (value && value <= VCON_QUEUE_NUM_MAX && !(value & (value - 1)))
            VCON_QUEUE.QueueNum = value;
        else
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueReady):
        if (value & ~1U || (value && !vcon_queue_layout_valid(&VCON_QUEUE))) {
            virtio_console_set_fail(vcon);
            return true;
        }
        VCON_QUEUE.ready = value;
        return true;
    case _(QueueDescLow):
        if (VCON_QUEUE.ready)
            virtio_console_set_fail(vcon);
        else
            VCON_QUEUE.QueueDesc = vcon_preprocess(vcon, value);
        return true;
    case _(QueueDescHigh):
        if (VCON_QUEUE.ready || value)
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueDriverLow):
        if (VCON_QUEUE.ready)
            virtio_console_set_fail(vcon);
        else
            VCON_QUEUE.QueueAvail = vcon_preprocess(vcon, value);
        return true;
    case _(QueueDriverHigh):
        if (VCON_QUEUE.ready || value)
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueDeviceLow):
        if (VCON_QUEUE.ready)
            virtio_console_set_fail(vcon);
        else
            VCON_QUEUE.QueueUsed = vcon_preprocess(vcon, value);
        return true;
    case _(QueueDeviceHigh):
        if (VCON_QUEUE.ready || value)
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueNotify):
        if (value >= ARRAY_SIZE(vcon->queues))
            virtio_console_set_fail(vcon);
        else if (!(vcon->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) &&
                 (!(vcon->Status & VIRTIO_STATUS__DRIVER_OK) ||
                  !vcon->queues[value].ready))
            virtio_console_set_fail(vcon);
        else if (value == VCON_QUEUE_TX)
            vcon_process_tx(vcon);
        return true;
    case _(InterruptACK):
        vcon->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_console_update_status(vcon, value);
        return true;
    default:
        return false;
    }
#undef _
}

void virtio_console_read(hart_t *vm,
                         virtio_console_state_t *vcon,
                         uint32_t addr,
                         uint8_t width,
                         uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (addr & 0x3) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
        if (!virtio_console_reg_read(vcon, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        return;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    }
}

void virtio_console_write(hart_t *vm,
                          virtio_console_state_t *vcon,
                          uint32_t addr,
                          uint8_t width,
                          uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (addr & 0x3) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
        if (!virtio_console_reg_write(vcon, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        return;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    }
}

void virtio_console_init(virtio_console_state_t *vcon,
                         uint32_t *ram,
                         int in_fd,
                         int out_fd)
{
    vcon->ram = ram;
    vcon->in_fd = in_fd;
    vcon->out_fd = out_fd;
}
