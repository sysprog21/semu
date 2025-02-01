#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

#define VIRTIO_F_VERSION_1 1

#define VRNG_FEATURES_0 0
#define VRNG_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */

#define VRNG_QUEUE_NUM_MAX 1024
#define VRNG_QUEUE (vrng->queues[vrng->QueueSel])

static int rng_fd = -1;

static void virtio_rng_set_fail(virtio_rng_state_t *vrng)
{
    vrng->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vrng->Status & VIRTIO_STATUS__DRIVER_OK)
        vrng->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vrng_preprocess(virtio_rng_state_t *vrng, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_rng_set_fail(vrng), 0;

    return addr >> 2;
}

static void virtio_rng_update_status(virtio_rng_state_t *vrng, uint32_t status)
{
    vrng->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vrng->ram;
    memset(vrng, 0, sizeof(*vrng));
    vrng->ram = ram;
}

static void virtio_queue_notify_handler(virtio_rng_state_t *vrng,
                                        virtio_rng_queue_t *queue)
{
    uint32_t *ram = vrng->ram;

    /* Calculate available ring index */
    uint16_t queue_idx = queue->last_avail % queue->QueueNum;
    uint16_t buffer_idx =
        ram[queue->QueueAvail + 1 + queue_idx / 2] >> (16 * (queue_idx % 2));

    /* Update available ring pointer */
    VRNG_QUEUE.last_avail++;

    /* Read descriptor */
    struct virtq_desc *vq_desc =
        (struct virtq_desc *) &vrng->ram[queue->QueueDesc + buffer_idx * 4];

    /* Write entropy buffer */
    void *entropy_buf =
        (void *) ((uintptr_t) vrng->ram + (uintptr_t) vq_desc->addr);
    ssize_t total = read(rng_fd, entropy_buf, vq_desc->len);

    /* Clear write flag */
    vq_desc->flags = 0;

    /* Get virtq_used.idx (le16) */
    uint16_t used = ram[queue->QueueUsed] >> 16;

    /* Update used ring information */
    uint32_t vq_used_addr =
        VRNG_QUEUE.QueueUsed + 1 + (used % queue->QueueNum) * 2;
    ram[vq_used_addr] = buffer_idx;
    ram[vq_used_addr + 1] = total;
    used++;

    /* Reset used ring flag to zero (virtq_used.flags) */
    vrng->ram[VRNG_QUEUE.QueueUsed] &= MASK(16);

    /* Update the used ring pointer (virtq_used.idx) */
    vrng->ram[VRNG_QUEUE.QueueUsed] |= ((uint32_t) used) << 16;

    /* Send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    if (!(ram[VRNG_QUEUE.QueueAvail] & 1))
        vrng->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool virtio_rng_reg_read(virtio_rng_state_t *vrng,
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
        *value = 4;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vrng->DeviceFeaturesSel == 0
                     ? VRNG_FEATURES_0
                     : (vrng->DeviceFeaturesSel == 1 ? VRNG_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VRNG_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VRNG_QUEUE.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vrng->InterruptStatus;
        return true;
    case _(Status):
        *value = vrng->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    default:
        /* No other readable registers */
        return false;
    }
#undef _
}

static bool virtio_rng_reg_write(virtio_rng_state_t *vrng,
                                 uint32_t addr,
                                 uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vrng->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        vrng->DriverFeaturesSel == 0 ? (vrng->DriverFeatures = value) : 0;
        return true;
    case _(DriverFeaturesSel):
        vrng->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vrng->queues))
            vrng->QueueSel = value;
        else
            virtio_rng_set_fail(vrng);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VRNG_QUEUE_NUM_MAX)
            VRNG_QUEUE.QueueNum = value;
        else
            virtio_rng_set_fail(vrng);
        return true;
    case _(QueueReady):
        VRNG_QUEUE.ready = value & 1;
        if (value & 1)
            VRNG_QUEUE.last_avail = vrng->ram[VRNG_QUEUE.QueueAvail] >> 16;
        return true;
    case _(QueueDescLow):
        VRNG_QUEUE.QueueDesc = vrng_preprocess(vrng, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_rng_set_fail(vrng);
        return true;
    case _(QueueDriverLow):
        VRNG_QUEUE.QueueAvail = vrng_preprocess(vrng, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_rng_set_fail(vrng);
        return true;
    case _(QueueDeviceLow):
        VRNG_QUEUE.QueueUsed = vrng_preprocess(vrng, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_rng_set_fail(vrng);
        return true;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vrng->queues))
            virtio_queue_notify_handler(vrng, &VRNG_QUEUE);
        else
            virtio_rng_set_fail(vrng);
        return true;
    case _(InterruptACK):
        vrng->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_rng_update_status(vrng, value);
        return true;
    default:
        /* No other writable registers */
        return false;
    }
#undef _
}

void virtio_rng_read(hart_t *vm,
                     virtio_rng_state_t *vrng,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_rng_reg_read(vrng, addr >> 2, value))
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

void virtio_rng_write(hart_t *vm,
                      virtio_rng_state_t *vrng,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_rng_reg_write(vrng, addr >> 2, value))
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

void virtio_rng_init(void)
{
    rng_fd = open("/dev/random", O_RDONLY);
    if (rng_fd < 0) {
        fprintf(stderr, "Could not open /dev/random\n");
        exit(2);
    }
}
