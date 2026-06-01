#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

#define VCON_FEATURES_0 0
#define VCON_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */

#define VCON_QUEUE_NUM_MAX 1024
#define VCON_QUEUE (vcon->queues[vcon->QueueSel])

enum { VCON_QUEUE_RX = 0, VCON_QUEUE_TX = 1 };

static void virtio_console_set_fail(virtio_console_state_t *vcon)
{
    vcon->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vcon->Status & VIRTIO_STATUS__DRIVER_OK)
        vcon->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vcon_preprocess(virtio_console_state_t *vcon,
                                       uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_console_set_fail(vcon), 0;

    return addr >> 2;
}

static void virtio_console_update_status(virtio_console_state_t *vcon,
                                         uint32_t status)
{
    vcon->Status |= status;
    if (status)
        return;

    /* Reset while preserving host wiring and config space. */
    uint32_t *ram = vcon->ram;
    int in_fd = vcon->in_fd;
    int out_fd = vcon->out_fd;
    memset(vcon, 0, sizeof(*vcon));
    vcon->ram = ram;
    vcon->in_fd = in_fd;
    vcon->out_fd = out_fd;
}

static uint16_t vcon_read_le16(uint32_t *ram, uint32_t base, uint16_t idx)
{
    uint32_t word = ram[base + 1 + idx / 2];
    return (word >> (16 * (idx % 2))) & MASK(16);
}

static bool vcon_validate_guest_range(uint64_t addr,
                                      uint32_t len)
{
    return addr < RAM_SIZE && len <= (uint64_t) RAM_SIZE - addr;
}

static bool vcon_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *cursor = buf;

    while (len) {
        ssize_t written = write(fd, cursor, len);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }

        cursor += written;
        len -= written;
    }

    return true;
}

static void vcon_push_used(virtio_console_state_t *vcon,
                           virtio_console_queue_t *queue,
                           uint16_t head,
                           uint32_t written_len)
{
    uint32_t *ram = vcon->ram;
    uint16_t used = ram[queue->QueueUsed] >> 16;
    uint32_t used_addr = queue->QueueUsed + 1 + (used % queue->QueueNum) * 2;

    ram[used_addr] = head;
    ram[used_addr + 1] = written_len;
    used++;

    ram[queue->QueueUsed] &= MASK(16);
    ram[queue->QueueUsed] |= ((uint32_t) used) << 16;

    if (!(ram[queue->QueueAvail] & 1))
        vcon->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool vcon_fd_ready(int fd)
{
    struct pollfd pfd = {fd, POLLIN, 0};

    if (poll(&pfd, 1, 0) < 0)
        return false;
    return (pfd.revents & POLLIN) != 0;
}

static bool vcon_process_tx(virtio_console_state_t *vcon,
                            virtio_console_queue_t *queue)
{
    uint32_t *ram = vcon->ram;
    uint16_t avail = ram[queue->QueueAvail] >> 16;

    while (queue->last_avail != avail) {
        uint16_t slot = queue->last_avail % queue->QueueNum;
        uint16_t head = vcon_read_le16(ram, queue->QueueAvail, slot);
        uint16_t desc_idx = head;
        uint32_t total_len = 0;
        bool saw_end = false;

        queue->last_avail++;

        for (uint16_t steps = 0; steps < queue->QueueNum; steps++) {
            struct virtq_desc *desc;
            const void *buf;

            if (desc_idx >= queue->QueueNum) {
                virtio_console_set_fail(vcon);
                return false;
            }

            desc = (struct virtq_desc *) &ram[queue->QueueDesc + desc_idx * 4];
            if (desc->flags & VIRTIO_DESC_F_WRITE) {
                virtio_console_set_fail(vcon);
                return false;
            }

            if (!vcon_validate_guest_range(desc->addr, desc->len)) {
                virtio_console_set_fail(vcon);
                return false;
            }

            buf = (const void *) ((uintptr_t) vcon->ram + (uintptr_t) desc->addr);
            if (!vcon_write_all(vcon->out_fd, buf, desc->len)) {
                fprintf(stderr, "failed to write virtio-console output\n");
                virtio_console_set_fail(vcon);
                return false;
            }
            total_len += desc->len;

            if (!(desc->flags & VIRTIO_DESC_F_NEXT)) {
                saw_end = true;
                break;
            }
            desc_idx = desc->next;
        }

        if (!saw_end) {
            virtio_console_set_fail(vcon);
            return false;
        }

        vcon_push_used(vcon, queue, head, total_len);

        avail = ram[queue->QueueAvail] >> 16;
    }

    return true;
}

void virtio_console_refresh_rx(virtio_console_state_t *vcon)
{
    virtio_console_queue_t *queue = &vcon->queues[VCON_QUEUE_RX];
    uint32_t *ram = vcon->ram;
    uint16_t avail;

    if (!queue->ready || !(vcon->Status & VIRTIO_STATUS__DRIVER_OK))
        return;
    if (!vcon_fd_ready(vcon->in_fd))
        return;

    avail = ram[queue->QueueAvail] >> 16;
    while (queue->last_avail != avail && vcon_fd_ready(vcon->in_fd)) {
        uint16_t slot = queue->last_avail % queue->QueueNum;
        uint16_t head = vcon_read_le16(ram, queue->QueueAvail, slot);
        uint16_t desc_idx = head;
        uint32_t total_len = 0;

        for (uint16_t steps = 0; steps < queue->QueueNum; steps++) {
            struct virtq_desc *desc;
            uint8_t *buf;
            ssize_t nread;

            if (desc_idx >= queue->QueueNum) {
                virtio_console_set_fail(vcon);
                return;
            }

            desc = (struct virtq_desc *) &ram[queue->QueueDesc + desc_idx * 4];
            if (!(desc->flags & VIRTIO_DESC_F_WRITE)) {
                virtio_console_set_fail(vcon);
                return;
            }
            if (!vcon_validate_guest_range(desc->addr, desc->len)) {
                virtio_console_set_fail(vcon);
                return;
            }

            buf = (uint8_t *) ((uintptr_t) vcon->ram + (uintptr_t) desc->addr);
            nread = read(vcon->in_fd, buf, desc->len);
            if (nread < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                virtio_console_set_fail(vcon);
                return;
            }
            if (nread == 0)
                return;
            total_len += (uint32_t) nread;

            if (total_len || !(desc->flags & VIRTIO_DESC_F_NEXT))
                break;
            desc_idx = desc->next;
        }

        queue->last_avail++;
        vcon_push_used(vcon, queue, head, total_len);

        avail = ram[queue->QueueAvail] >> 16;
    }
}

static bool virtio_console_reg_read(virtio_console_state_t *vcon,
                                    uint32_t addr,
                                    uint32_t *value)
{
#define _(reg) VIRTIO_##reg
    switch (addr >> 2) {
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
        *value = vcon->QueueSel < ARRAY_SIZE(vcon->queues) ? VCON_QUEUE_NUM_MAX
                                                           : 0;
        return true;
    case _(QueueReady):
        *value = VCON_QUEUE.ready ? 1 : 0;
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
    switch (addr >> 2) {
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
        if (value > 0 && value <= VCON_QUEUE_NUM_MAX)
            VCON_QUEUE.QueueNum = value;
        else
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueReady):
        VCON_QUEUE.ready = value & 1;
        if (value & 1)
            VCON_QUEUE.last_avail = vcon->ram[VCON_QUEUE.QueueAvail] >> 16;
        return true;
    case _(QueueDescLow):
        VCON_QUEUE.QueueDesc = vcon_preprocess(vcon, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueDriverLow):
        VCON_QUEUE.QueueAvail = vcon_preprocess(vcon, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueDeviceLow):
        VCON_QUEUE.QueueUsed = vcon_preprocess(vcon, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_console_set_fail(vcon);
        return true;
    case _(QueueNotify):
        if (value == VCON_QUEUE_TX)
            return vcon_process_tx(vcon, &vcon->queues[VCON_QUEUE_TX]);
        if (value == VCON_QUEUE_RX)
            return true;
        virtio_console_set_fail(vcon);
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
        if (!virtio_console_reg_read(vcon, addr, value))
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
        if (!virtio_console_reg_write(vcon, addr, value))
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

void virtio_console_init(virtio_console_state_t *vcon)
{
    (void) vcon;
}
