#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

#define DISK_BLK_SIZE 512

#define VBLK_FEATURES_0 0
#define VBLK_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VBLK_QUEUE_NUM_MAX 1024
#define VBLK_QUEUE (vblk->queues[vblk->QueueSel])

struct vblk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t status;
} __attribute__((packed));

static void virtio_blk_set_fail(virtio_blk_state_t *vblk)
{
    vblk->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vblk->Status & VIRTIO_STATUS__DRIVER_OK)
        vblk->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vblk_preprocess(virtio_blk_state_t *vblk, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_blk_set_fail(vblk), 0;

    return addr >> 2;
}

static void virtio_blk_update_status(virtio_blk_state_t *vblk, uint32_t status)
{
    vblk->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vblk->ram;
    uint32_t *disk = vblk->disk;
    uint32_t capacity = vblk->capacity;
    memset(vblk, 0, sizeof(*vblk));
    vblk->ram = ram;
    vblk->disk = disk;
    vblk->capacity = capacity;
}

static void virtio_blk_write_handler(virtio_blk_state_t *vblk,
                                     uint64_t sector,
                                     uint32_t desc_addr,
                                     uint32_t len)
{
    char *dest = (char *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    char *src = (char *) ((uintptr_t) vblk->ram + desc_addr);
    memcpy(dest, src, len);
}

static void virtio_blk_read_handler(virtio_blk_state_t *vblk,
                                    uint64_t sector,
                                    uint32_t desc_addr,
                                    uint32_t len)
{
    char *dest = (char *) ((uintptr_t) vblk->ram + desc_addr);
    char *src = (char *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    memcpy(dest, src, len);
}

static int virtio_blk_desc_handler(virtio_blk_state_t *vblk,
                                   const virtio_blk_queue_t *queue,
                                   uint32_t desc_idx,
                                   uint32_t *plen)
{
    /*
     * A full virtio_blk_req is represented by 3 descriptors, where
     * the first descriptor contains:
     *   le32 type
     *   le32 reserved
     *   le64 sector
     * the second descriptor contains:
     *   u8 data[][512]
     * the third descriptor contains:
     *   u8 status
     */

    struct virtq_desc vq_desc[3];

    /* Collect the descriptors */
    for (int i = 0; i < 3; i++) {
        /* The size of the `struct virtq_desc` is 4 words */
        uint32_t *desc = &vblk->ram[queue->QueueDesc + desc_idx * 4];

        /* Retrieve the fields of current descriptor */
        vq_desc[i].addr = desc[0];
        vq_desc[i].len = desc[2];
        vq_desc[i].flags = desc[3];
        desc_idx = desc[3] >> 16; /* vq_desc[desc_cnt].next */
    }

    /*
     * The next flag for the first and second descriptors should be set,
     * whereas for the third descriptor is should not be set
     */
    if (!(vq_desc[0].flags & VIRTIO_DESC_F_NEXT) ||
        !(vq_desc[1].flags & VIRTIO_DESC_F_NEXT) ||
        (vq_desc[2].flags & VIRTIO_DESC_F_NEXT)) {
        /* since the descriptor list is abnormal, we don't write the status
         * back here */
        virtio_blk_set_fail(vblk);
        return -1;
    }

    /* Process the header */
    struct vblk_req_header *hdr =
        (struct vblk_req_header *) ((uintptr_t) vblk->ram + vq_desc[0].addr);
    uint32_t type = hdr->type;
    uint64_t sector = hdr->sector;
    uint8_t *status = (uint8_t *) ((uintptr_t) vblk->ram + vq_desc[2].addr);

    /* Check sector index is valid */
    if (sector > (vblk->capacity - 1)) {
        *status = VIRTIO_BLK_S_IOERR;
        return -1;
    }

    /* Process the data */
    switch (type) {
    case VIRTIO_BLK_T_IN:
        virtio_blk_read_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    case VIRTIO_BLK_T_OUT:
        virtio_blk_write_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    default:
        fprintf(stderr, "unsupported virtio-blk operation!\n");
        *status = VIRTIO_BLK_S_UNSUPP;
        return -1;
    }

    /* Return the device status */
    *status = VIRTIO_BLK_S_OK;
    *plen = vq_desc[1].len;

    return 0;
}

static void virtio_queue_notify_handler(virtio_blk_state_t *vblk, int index)
{
    uint32_t *ram = vblk->ram;
    virtio_blk_queue_t *queue = &vblk->queues[index];
    if (vblk->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;

    if (!((vblk->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        return virtio_blk_set_fail(vblk);

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)
        return (fprintf(stderr, "size check fail\n"),
                virtio_blk_set_fail(vblk));

    if (queue->last_avail == new_avail)
        return;

    /* Process them */
    uint16_t new_used = ram[queue->QueueUsed] >> 16; /* virtq_used.idx (le16) */
    while (queue->last_avail != new_avail) {
        /* Obtain the index in the ring buffer */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;

        /*
         * Since each buffer index occupies 2 bytes but the memory is aligned
         * with 4 bytes, and the first element of the available queue is stored
         * at ram[queue->QueueAvail + 1], to acquire the buffer index, it
         * requires the following array index calculation and bit shifting.
         * Check also the `struct virtq_avail` on the spec.
         */
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        /* Consume request from the available queue and process the data in the
         * descriptor list */
        uint32_t len = 0;
        int result = virtio_blk_desc_handler(vblk, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_blk_set_fail(vblk);

        /* Write used element information (`struct virtq_used_elem`) to the used
         * queue */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx; /* virtq_used_elem.id  (le32) */
        ram[vq_used_addr + 1] = len;    /* virtq_used_elem.len (le32) */
        queue->last_avail++;
        new_used++;
    }

    /* Check le32 len field of `struct virtq_used_elem` on the spec  */
    vblk->ram[queue->QueueUsed] &= MASK(16); /* Reset low 16 bits to zero */
    vblk->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16; /* len */

    /* Send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    if (!(ram[queue->QueueAvail] & 1))
        vblk->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool virtio_blk_reg_read(virtio_blk_state_t *vblk,
                                uint32_t addr,
                                uint32_t *value)
{
    /*
     * TODO: replace the register address with enum.
     * For the address after the configuration space, it should be
     * handled by structure pointer depend on the device.
     */

    switch (addr) {
    case 0: /* MagicValue (R) */
        *value = 0x74726976;
        return true;
    case 1: /* Version (R) */
        *value = 2;
        return true;
    case 2: /* DeviceID (R) */
        *value = 2;
        return true;
    case 3: /* VendorID (R) */
        *value = VIRTIO_VENDOR_ID;
        return true;
    case 4: /* DeviceFeatures (R) */
        *value = vblk->DeviceFeaturesSel == 0
                     ? VBLK_FEATURES_0
                     : (vblk->DeviceFeaturesSel == 1 ? VBLK_FEATURES_1 : 0);
        return true;
    case 13: /* QueueNumMax (R) */
        *value = VBLK_QUEUE_NUM_MAX;
        return true;
    case 17: /* QueueReady (RW) */
        *value = VBLK_QUEUE.ready ? 1 : 0;
        return true;
    case 24: /* InterruptStatus (R) */
        *value = vblk->InterruptStatus;
        return true;
    case 28: /* Status (RW) */
        *value = vblk->Status;
        return true;
    case 63: /* ConfigGeneration (R) */
        *value = 0;
        return true;
    case 64: /* CapacityLow (R) */
        *value = 0x00000000ffffffff & vblk->capacity;
        return true;
    case 65: /* CapacityHigh (R) */
        *value = vblk->capacity >> 32;
        return true;
    default:
        return false;
    }
}

static bool virtio_blk_reg_write(virtio_blk_state_t *vblk,
                                 uint32_t addr,
                                 uint32_t value)
{
    /*
     * TODO: replace the register address with enum.
     * For the address after the configuration space, it should be
     * handled by structure pointer depend on the device.
     */

    switch (addr) {
    case 5: /* DeviceFeaturesSel (W) */
        vblk->DeviceFeaturesSel = value;
        return true;
    case 8: /* DriverFeatures (W) */
        vblk->DriverFeaturesSel == 0 ? (vblk->DriverFeatures = value) : 0;
        return true;
    case 9: /* DriverFeaturesSel (W) */
        vblk->DriverFeaturesSel = value;
        return true;
    case 12: /* QueueSel (W) */
        if (value < ARRAY_SIZE(vblk->queues))
            vblk->QueueSel = value;
        else
            virtio_blk_set_fail(vblk);
        return true;
    case 14: /* QueueNum (W) */
        if (value > 0 && value <= VBLK_QUEUE_NUM_MAX)
            VBLK_QUEUE.QueueNum = value;
        else
            virtio_blk_set_fail(vblk);
        return true;
    case 17: /* QueueReady (RW) */
        VBLK_QUEUE.ready = value & 1;
        if (value & 1)
            VBLK_QUEUE.last_avail = vblk->ram[VBLK_QUEUE.QueueAvail] >> 16;
        return true;
    case 32: /* QueueDescLow (W) */
        VBLK_QUEUE.QueueDesc = vblk_preprocess(vblk, value);
        return true;
    case 33: /* QueueDescHigh (W) */
        if (value)
            virtio_blk_set_fail(vblk);
        return true;
    case 36: /* QueueAvailLow (W) */
        VBLK_QUEUE.QueueAvail = vblk_preprocess(vblk, value);
        return true;
    case 37: /* QueueAvailHigh (W) */
        if (value)
            virtio_blk_set_fail(vblk);
        return true;
    case 40: /* QueueUsedLow (W) */
        VBLK_QUEUE.QueueUsed = vblk_preprocess(vblk, value);
        return true;
    case 41: /* QueueUsedHigh (W) */
        if (value)
            virtio_blk_set_fail(vblk);
        return true;
    case 20: /* QueueNotify (W) */
        if (value < ARRAY_SIZE(vblk->queues))
            virtio_queue_notify_handler(vblk, value);
        else
            virtio_blk_set_fail(vblk);
        return true;
    case 25: /* InterruptACK (W) */
        vblk->InterruptStatus &= ~value;
        return true;
    case 28: /* Status (RW) */
        virtio_blk_update_status(vblk, value);
        return true;
    default:
        return false;
    }
}

void virtio_blk_read(vm_t *vm,
                     virtio_blk_state_t *vblk,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_blk_reg_read(vblk, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSTR, 0);
        return;
    }
}

void virtio_blk_write(vm_t *vm,
                      virtio_blk_state_t *vblk,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_blk_reg_write(vblk, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSTR, 0);
        return;
    }
}

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk, char *disk_file)
{
    /* No disk image is provided */
    if (!disk_file) {
        /* By setting the block capacity to zero, the kernel will
         * then not to touch the device after booting */
        vblk->capacity = 0;
        return NULL;
    }

    /* Open disk file */
    int disk_fd = open(disk_file, O_RDWR);
    if (disk_fd < 0) {
        fprintf(stderr, "could not open %s\n", disk_file);
        exit(2);
    }

    /* Get the disk image size */
    struct stat st;
    fstat(disk_fd, &st);
    size_t disk_size = st.st_size;

    /* Set up the disk memory */
    uint32_t *disk_mem =
        mmap(NULL, disk_size, PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd, 0);
    if (disk_mem == MAP_FAILED) {
        fprintf(stderr, "Could not map disk\n");
        return NULL;
    }
    assert(!(((uintptr_t) disk_mem) & 0b11));

    vblk->disk = disk_mem;
    vblk->capacity = (disk_size - 1) / DISK_BLK_SIZE + 1;

    return disk_mem;
}
