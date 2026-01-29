#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-gpu.h"
#include "virtio.h"
#include "window.h"

#define VGPU_CMD_TRACE_ENABLED 0

#define VIRTIO_F_VERSION_1 1

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)
#define VIRTIO_GPU_F_EDID (1 << 1)
#define VIRTIO_GPU_F_CONTEXT_INIT (1 << 4)
#define VIRTIO_GPU_FLAG_FENCE (1 << 0)

#define VGPU_QUEUE_NUM_MAX 1024
#define VGPU_QUEUE (vgpu->queues[vgpu->QueueSel])

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

#if VGPU_CMD_TRACE_ENABLED == 0
#define VGPU_CMD(cmd, fn)                       \
    case VIRTIO_GPU_CMD_##cmd:                  \
        g_vgpu_backend.fn(vgpu, vq_desc, plen); \
        break;
#else
#define VGPU_CMD(cmd, fn)                                            \
    case VIRTIO_GPU_CMD_##cmd:                                       \
        printf("(*) semu/virtio-gpu: %s\n", "VIRTIO_GPU_CMD_" #cmd); \
        g_vgpu_backend.fn(vgpu, vq_desc, plen);                      \
        break;
#endif

extern const struct vgpu_cmd_backend g_vgpu_backend;
extern const struct window_backend g_window;

static virtio_gpu_data_t virtio_gpu_data;
static struct vgpu_config vgpu_configs;
static LIST_HEAD(vgpu_res_2d_list);

size_t iov_to_buf(const struct iovec *iov,
                  const unsigned int iov_cnt,
                  size_t offset,
                  void *buf,
                  size_t bytes)
{
    size_t done = 0;

    for (unsigned int i = 0; i < iov_cnt; i++) {
        /* Skip empty pages */
        if (iov[i].iov_base == 0 || iov[i].iov_len == 0)
            continue;

        if (offset < iov[i].iov_len) {
            /* Take as much as data of current page can provide */
            size_t remained = bytes - done;
            size_t page_avail = iov[i].iov_len - offset;
            size_t len = (remained < page_avail) ? remained : page_avail;

            /* Copy to buffer */
            void *src = (void *) ((uintptr_t) iov[i].iov_base + offset);
            void *dest = (void *) ((uintptr_t) buf + done);
            memcpy(dest, src, len);

            /* If there is still data left to read, but current page is
             * exhausted, we need to read from the beginning of the next
             * page, where its offset should be 0 */
            offset = 0;

            /* Count the total received bytes so far */
            done += len;

            /* Data transfering of current scanline is complete */
            if (done >= bytes)
                break;
        } else {
            offset -= iov[i].iov_len;
        }
    }

    return done;
}

struct vgpu_resource_2d *vgpu_create_resource_2d(int resource_id)
{
    struct vgpu_resource_2d *res = malloc(sizeof(struct vgpu_resource_2d));
    if (!res)
        return NULL;

    memset(res, 0, sizeof(*res));
    res->resource_id = resource_id;
    list_push(&res->list, &vgpu_res_2d_list);
    return res;
}

struct vgpu_resource_2d *vgpu_get_resource_2d(uint32_t resource_id)
{
    struct vgpu_resource_2d *res_2d;
    list_for_each_entry (res_2d, &vgpu_res_2d_list, list) {
        if (res_2d->resource_id == resource_id)
            return res_2d;
    }

    return NULL;
}

int vgpu_destroy_resource_2d(uint32_t resource_id)
{
    struct vgpu_resource_2d *res_2d = vgpu_get_resource_2d(resource_id);

    /* Failed to find the resource */
    if (!res_2d)
        return -1;

    /* Release the resource */
    free(res_2d->image);
    list_del(&res_2d->list);
    free(res_2d->iovec);
    free(res_2d);

    return 0;
}

void *vgpu_mem_guest_to_host(virtio_gpu_state_t *vgpu, uint32_t addr)
{
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "virtio-gpu: guest address 0x%x out of bounds\n", addr);
        return NULL;
    }
    return (void *) ((uintptr_t) vgpu->ram + addr);
}

uint32_t virtio_gpu_write_response(virtio_gpu_state_t *vgpu,
                                   uint64_t addr,
                                   uint32_t type)
{
    struct vgpu_ctrl_hdr *response = vgpu_mem_guest_to_host(vgpu, addr);
    if (!response)
        return 0;

    memset(response, 0, sizeof(*response));
    response->type = type;

    return sizeof(*response);
}

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
{
    vgpu->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vgpu->Status & VIRTIO_STATUS__DRIVER_OK)
        vgpu->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vgpu_preprocess(virtio_gpu_state_t *vgpu, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_gpu_set_fail(vgpu), 0;

    return addr >> 2;
}

static void virtio_gpu_update_status(virtio_gpu_state_t *vgpu, uint32_t status)
{
    vgpu->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vgpu->ram;
    void *priv = vgpu->priv;
    uint32_t scanout_num = vgpu_configs.num_scanouts;
    memset(vgpu->priv, 0, sizeof(*vgpu->priv));
    memset(vgpu, 0, sizeof(*vgpu));
    vgpu->ram = ram;
    vgpu->priv = priv;
    vgpu_configs.num_scanouts = scanout_num;

    /* Release all 2D resources */
    struct list_head *curr, *next;
    list_for_each_safe (curr, next, &vgpu_res_2d_list) {
        struct vgpu_resource_2d *res_2d =
            list_entry(curr, struct vgpu_resource_2d, list);

        list_del(&res_2d->list);
        free(res_2d->image);
        free(res_2d->iovec);
        free(res_2d);
    }
}

void virtio_gpu_set_response_fencing(virtio_gpu_state_t *vgpu,
                                     struct vgpu_ctrl_hdr *request,
                                     uint64_t addr)
{
    struct vgpu_ctrl_hdr *response = vgpu_mem_guest_to_host(vgpu, addr);

    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }
}

void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    /* Write display information */
    struct vgpu_resp_disp_info *response =
        vgpu_mem_guest_to_host(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;

    int scanout_num = vgpu_configs.num_scanouts;
    for (int i = 0; i < scanout_num; i++) {
        response->pmodes[i].r.width = PRIV(vgpu)->scanouts[i].width;
        response->pmodes[i].r.height = PRIV(vgpu)->scanouts[i].height;
        response->pmodes[i].enabled = PRIV(vgpu)->scanouts[i].enabled;
    }

    /* Update write length */
    *plen = sizeof(*response);
}

static uint8_t virtio_gpu_generate_edid_checksum(uint8_t *edid, size_t size)
{
    uint8_t sum = 0;

    for (size_t i = 0; i < size; i++)
        sum += edid[i];

    return 0x100 - sum;
}

static void virtio_gpu_generate_edid(uint8_t *edid, int width_cm, int height_cm)
{
    /* Check:
     * "VESA ENHANCED EXTENDED DISPLAY IDENTIFICATION DATA STANDARD"
     * (Defines EDID Structure Version 1, Revision 4)
     */

    memset(edid, 0, 128);

    /* EDID header */
    edid[0] = 0x00;
    edid[1] = 0xff;
    edid[2] = 0xff;
    edid[3] = 0xff;
    edid[4] = 0xff;
    edid[5] = 0xff;
    edid[6] = 0xff;
    edid[7] = 0x00;

    /* ISA (Industry Standard Architecture)
     * Plug and Play Device Identifier (PNPID) */
    char manufacture[3] = {'T', 'W', 'N'};

    /* Vendor ID uses 2 bytes to store 3 characters, where 'A' starts as 1 */
    uint16_t vendor_id = ((((manufacture[0] - '@') & 0b11111) << 10) |
                          (((manufacture[1] - '@') & 0b11111) << 5) |
                          (((manufacture[2] - '@') & 0b11111) << 0));
    /* Convert vendor ID to big-endian order */
    edid[8] = vendor_id >> 8;
    edid[9] = vendor_id & 0xff;

    /* Product code (all zeros if unused) */
    memset(&edid[10], 0, 6);

    /* Week of manufacture (1-54) */
    edid[16] = 0;
    /* Year of manufacture (starts from 1990) */
    edid[17] = 2023 - 1990;

    /* EDID 1.4 (Version 1, Revision 4) */
    edid[18] = 1; /* Version number */
    edid[19] = 4; /* Revision number */

    /* Video input definition */
    uint8_t signal_interface = 0b1 << 7;  /* digital */
    uint8_t color_bit_depth = 0b010 << 4; /* 8 bits per primary color */
    uint8_t interface_type = 0b101;       /* DisplayPort is supported */
    edid[20] = signal_interface | color_bit_depth | interface_type;

    /* Screen size or aspect ratio */
    edid[21] = width_cm;  /* Horizontal screen size (1cm - 255cm) */
    edid[22] = height_cm; /* Vertical screen size (1cm - 255cm) */

    /* Gamma value */
    edid[23] = 1; /* Assigned with the minimum value */

    /* Feature support */
    uint8_t power_management = 0 << 4; /* standby, suspend and active-off
                                        * modes are not supported */
    uint8_t color_type = 0 << 2; /* ignored as it is for the analog display */
    uint8_t other_flags = 0b110; /* [2]: sRGB as default color space
                                  * [1]: Prefered timing mode with native format
                                  * [0]: Non-continuys frequency */
    edid[24] = power_management | color_type | other_flags;

    /* Established timmings: These are the default timmings defined by the
     * VESA. Each bit represents 1 configuration. For now, we enable the
     * timming configurations of 1024x768@60Hz only */
    edid[35] = 0b00000000;
    edid[36] = 0b00001000;
    edid[37] = 0b00000000;

    /* Standard timmings: 16 bytes data start from edid[38] to edid[54] as
     * additional timming configurations with 2 bytes for each to define
     * the horizontal pixel number, aspect ratio, and refresh rate. */

    /* Extension block count number */
    edid[126] = 0; /* No other extension blocks are defined */

    /* Checksum of the first (and the only) extension block */
    edid[127] = virtio_gpu_generate_edid_checksum(edid, 127);
}

void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen)
{
    /* Generate the display EDID */
    struct vgpu_resp_edid edid = {
        .hdr = {.type = VIRTIO_GPU_RESP_OK_EDID},
        .size = 128 /* One EDID extension block only */
    };
    virtio_gpu_generate_edid((uint8_t *) edid.edid, 0, 0);

    /* Write EDID response */
    struct vgpu_resp_edid *response =
        vgpu_mem_guest_to_host(vgpu, vq_desc[1].addr);
    memcpy(response, &edid, sizeof(*response));

    /* return write length */
    *plen = sizeof(*response);
}

void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    struct vgpu_ctrl_hdr *header =
        vgpu_mem_guest_to_host(vgpu, vq_desc[0].addr);

    fprintf(stderr, "%s(): unsupported VirtIO-GPU command %d.", __func__,
            header->type);

    virtio_gpu_set_fail(vgpu);
    *plen = 0;
}

static int virtio_gpu_desc_handler(virtio_gpu_state_t *vgpu,
                                   const virtio_gpu_queue_t *queue,
                                   uint32_t desc_idx,
                                   uint32_t *plen)
{
    /* virtio-gpu uses 3 virtqueue descriptors at most */
    struct virtq_desc vq_desc[3];

    /* Collect descriptors */
    for (int i = 0; i < 3; i++) {
        /* The size of the `struct virtq_desc` is 4 words */
        uint32_t *desc = &vgpu->ram[queue->QueueDesc + desc_idx * 4];

        /* Retrieve the fields of current descriptor */
        vq_desc[i].addr = desc[0];
        vq_desc[i].len = desc[2];
        vq_desc[i].flags = desc[3];
        desc_idx = desc[3] >> 16; /* vq_desc[desc_cnt].next */

        /* Leave the loop if next-flag is not set */
        if (!(vq_desc[i].flags & VIRTIO_DESC_F_NEXT))
            break;
    }

    /* Process the header */
    struct vgpu_ctrl_hdr *header =
        vgpu_mem_guest_to_host(vgpu, vq_desc[0].addr);

    /* Process the command */
    switch (header->type) {
        /* 2D commands */
        VGPU_CMD(GET_DISPLAY_INFO, get_display_info)
        VGPU_CMD(RESOURCE_CREATE_2D, resource_create_2d)
        VGPU_CMD(RESOURCE_UNREF, resource_unref)
        VGPU_CMD(SET_SCANOUT, set_scanout)
        VGPU_CMD(RESOURCE_FLUSH, resource_flush)
        VGPU_CMD(TRANSFER_TO_HOST_2D, transfer_to_host_2d)
        VGPU_CMD(RESOURCE_ATTACH_BACKING, resource_attach_backing)
        VGPU_CMD(RESOURCE_DETACH_BACKING, resource_detach_backing)
        VGPU_CMD(GET_CAPSET_INFO, get_capset_info)
        VGPU_CMD(GET_CAPSET, get_capset)
        VGPU_CMD(GET_EDID, get_edid)
        VGPU_CMD(RESOURCE_ASSIGN_UUID, resource_assign_uuid)
        VGPU_CMD(RESOURCE_CREATE_BLOB, resource_create_blob)
        VGPU_CMD(SET_SCANOUT_BLOB, set_scanout_blob)
        /* 3D commands */
        VGPU_CMD(CTX_CREATE, ctx_create)
        VGPU_CMD(CTX_DESTROY, ctx_destroy)
        VGPU_CMD(CTX_ATTACH_RESOURCE, ctx_attach_resource)
        VGPU_CMD(CTX_DETACH_RESOURCE, ctx_detach_resource)
        VGPU_CMD(RESOURCE_CREATE_3D, resource_create_3d)
        VGPU_CMD(TRANSFER_TO_HOST_3D, transfer_to_host_3d)
        VGPU_CMD(TRANSFER_FROM_HOST_3D, transfer_from_host_3d)
        VGPU_CMD(SUBMIT_3D, submit_3d)
        VGPU_CMD(RESOURCE_MAP_BLOB, resource_map_blob)
        VGPU_CMD(RESOURCE_UNMAP_BLOB, resource_unmap_blob)
        VGPU_CMD(UPDATE_CURSOR, update_cursor)
        VGPU_CMD(MOVE_CURSOR, move_cursor)
    default:
        virtio_gpu_cmd_undefined_handler(vgpu, vq_desc, plen);
        break;
    }

    return 0;
}

static void virtio_queue_notify_handler(virtio_gpu_state_t *vgpu, int index)
{
    uint32_t *ram = vgpu->ram;
    virtio_gpu_queue_t *queue = &vgpu->queues[index];
    if (vgpu->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;

    if (!((vgpu->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        return virtio_gpu_set_fail(vgpu);

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)
        return (fprintf(stderr, "%s(): size check failed\n", __func__),
                virtio_gpu_set_fail(vgpu));

    if (queue->last_avail == new_avail)
        return;

    /* Process them */
    uint16_t new_used = ram[queue->QueueUsed] >> 16; /* virtq_used.idx (le16) */
    while (queue->last_avail != new_avail) {
        /* Obtain the index in the ring buffer */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;

        /* Since each buffer index occupies 2 bytes but the memory is aligned
         * with 4 bytes, and the first element of the available queue is stored
         * at ram[queue->QueueAvail + 1], to acquire the buffer index, it
         * requires the following array index calculation and bit shifting.
         * Check also the `struct virtq_avail` on the spec.
         */
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        /* Consume request from the available queue and process the data in the
         * descriptor list.
         */
        uint32_t len = 0;
        int result = virtio_gpu_desc_handler(vgpu, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_gpu_set_fail(vgpu);

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
    vgpu->ram[queue->QueueUsed] &= MASK(16); /* Reset low 16 bits to zero */
    vgpu->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16; /* len */

    /* Send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    if (!(ram[queue->QueueAvail] & 1))
        vgpu->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool virtio_gpu_reg_read(virtio_gpu_state_t *vgpu,
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
        *value = 16;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        if (vgpu->DeviceFeaturesSel) { /* [63:32] */
            *value = VIRTIO_F_VERSION_1;
        } else { /* [31:0] */
            *value = VIRTIO_GPU_F_EDID;
        }
        return true;
    case _(QueueNumMax):
        *value = VGPU_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VGPU_QUEUE.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vgpu->InterruptStatus;
        return true;
    case _(Status):
        *value = vgpu->Status;
        return true;
    case _(SHMLenLow):
    case _(SHMLenHigh):
        /* shared memory is unimplemented */
        *value = -1;
        return true;
    case _(SHMBaseLow):
        *value = 0;
        return true;
    case _(SHMBaseHigh):
        *value = 0;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct vgpu_config)))
            return false;

        /* Read configuration from the corresponding register */
        uint32_t offset = (addr - _(Config)) << 2;
        switch (offset) {
        case offsetof(struct vgpu_config, events_read): {
            *value = 0; /* No event is implemented currently */
            return true;
        }
        case offsetof(struct vgpu_config, num_scanouts): {
            *value = vgpu_configs.num_scanouts;
            return true;
        }
        case offsetof(struct vgpu_config, num_capsets): {
            *value = 0;
            return true;
        }
        default:
            return false;
        }
    }
#undef _
}

static bool virtio_gpu_reg_write(virtio_gpu_state_t *vgpu,
                                 uint32_t addr,
                                 uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vgpu->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        if (vgpu->DriverFeaturesSel == 0)
            vgpu->DriverFeatures = value;
        return true;
    case _(DriverFeaturesSel):
        vgpu->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vgpu->queues))
            vgpu->QueueSel = value;
        else
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VGPU_QUEUE_NUM_MAX)
            VGPU_QUEUE.QueueNum = value;
        else
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueReady):
        VGPU_QUEUE.ready = value & 1;
        if (value & 1)
            VGPU_QUEUE.last_avail = vgpu->ram[VGPU_QUEUE.QueueAvail] >> 16;
        return true;
    case _(QueueDescLow):
        VGPU_QUEUE.QueueDesc = vgpu_preprocess(vgpu, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueDriverLow):
        VGPU_QUEUE.QueueAvail = vgpu_preprocess(vgpu, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueDeviceLow):
        VGPU_QUEUE.QueueUsed = vgpu_preprocess(vgpu, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vgpu->queues))
            virtio_queue_notify_handler(vgpu, value);
        else
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(InterruptACK):
        vgpu->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_gpu_update_status(vgpu, value);
        return true;
    case _(SHMSel):
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct vgpu_config)))
            return false;

        /* Write configuration to the corresponding register */
        uint32_t offset = (addr - _(Config)) << 2;
        switch (offset) {
        case offsetof(struct vgpu_config, events_clear): {
            /* Ignored, no event is implemented currently */
            return true;
        }
        default:
            return false;
        }
    }
#undef _
}

void virtio_gpu_read(hart_t *vm,
                     virtio_gpu_state_t *vgpu,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_gpu_reg_read(vgpu, addr >> 2, value))
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

void virtio_gpu_write(hart_t *vm,
                      virtio_gpu_state_t *vgpu,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_gpu_reg_write(vgpu, addr >> 2, value))
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

void virtio_gpu_init(virtio_gpu_state_t *vgpu)
{
    memset(&virtio_gpu_data, 0, sizeof(virtio_gpu_data_t));
    vgpu->priv = &virtio_gpu_data;
}

void virtio_gpu_add_scanout(virtio_gpu_state_t *vgpu,
                            uint32_t width,
                            uint32_t height)
{
    int scanout_num = vgpu_configs.num_scanouts;

    if (scanout_num >= VIRTIO_GPU_MAX_SCANOUTS) {
        fprintf(stderr, "%s(): exceeded scanout maximum number\n", __func__);
        exit(2);
    }

    PRIV(vgpu)->scanouts[scanout_num].width = width;
    PRIV(vgpu)->scanouts[scanout_num].height = height;
    PRIV(vgpu)->scanouts[scanout_num].enabled = 1;

    g_window.window_add(width, height);

    vgpu_configs.num_scanouts++;
}
