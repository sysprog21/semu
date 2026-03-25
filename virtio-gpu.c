#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-gpu.h"
#include "virtio.h"

#define VIRTIO_GPU_CMD_TRACE_ENABLED 0

#define VIRTIO_F_VERSION_1 1

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)
#define VIRTIO_GPU_F_EDID (1 << 1)
#define VIRTIO_GPU_F_CONTEXT_INIT (1 << 4)

#define VIRTIO_GPU_QUEUE_NUM_MAX 1024
#define VIRTIO_GPU_QUEUE (vgpu->queues[vgpu->QueueSel])

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

#if VIRTIO_GPU_CMD_TRACE_ENABLED
#define VIRTIO_GPU_CMD_CASE(cmd, fn)                                 \
    case VIRTIO_GPU_CMD_##cmd:                                       \
        printf("(*) semu/virtio-gpu: %s\n", "VIRTIO_GPU_CMD_" #cmd); \
        g_virtio_gpu_backend.fn(vgpu, vq_desc, plen);                \
        break;
#else
#define VIRTIO_GPU_CMD_CASE(cmd, fn)                  \
    case VIRTIO_GPU_CMD_##cmd:                        \
        g_virtio_gpu_backend.fn(vgpu, vq_desc, plen); \
        break;
#endif

extern const struct virtio_gpu_cmd_backend g_virtio_gpu_backend;
static virtio_gpu_data_t virtio_gpu_data;

void *virtio_gpu_mem_guest_to_host(virtio_gpu_state_t *vgpu,
                                   uint32_t addr,
                                   uint32_t size)
{
    if (addr >= RAM_SIZE || size > RAM_SIZE || addr + size > RAM_SIZE) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): guest address 0x%x size 0x%x out of bounds\n",
                __func__, addr, size);
        return NULL;
    }
    return (void *) ((uintptr_t) vgpu->ram + addr);
}

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
{
    vgpu->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vgpu->Status & VIRTIO_STATUS__DRIVER_OK)
        vgpu->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

void *virtio_gpu_get_request(virtio_gpu_state_t *vgpu,
                             struct virtq_desc *vq_desc,
                             size_t request_size)
{
    if ((vq_desc[0].flags & VIRTIO_DESC_F_WRITE) ||
        vq_desc[0].len < request_size || request_size > UINT32_MAX)
        return NULL;

    return virtio_gpu_mem_guest_to_host(vgpu, vq_desc[0].addr,
                                        (uint32_t) request_size);
}

int virtio_gpu_get_response_desc(struct virtq_desc *vq_desc,
                                 int max_desc,
                                 size_t response_size)
{
    if (response_size > UINT32_MAX)
        return -1;

    /* This helper works with the current fixed-shape descriptor parser:
     * 'vq_desc[0]' is the request, optional command data follows, and the
     * first writable descriptor is the response buffer. A writable descriptor
     * that is too small therefore means the expected response buffer is
     * malformed; this helper does not skip it and search for a later writable
     * descriptor.
     *
     * TODO: Support generic descriptor-chain parsing.
     */
    for (int i = 1; i < max_desc; i++) {
        if (!(vq_desc[i].flags & VIRTIO_DESC_F_WRITE))
            continue;

        if (vq_desc[i].len < response_size)
            return -1;

        return i;
    }

    return -1;
}

uint32_t virtio_gpu_write_ctrl_response(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t type)
{
    if (response_desc->len < sizeof(struct virtio_gpu_ctrl_hdr))
        return 0;

    struct virtio_gpu_ctrl_hdr *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response)
        return 0;

    memset(response, 0, sizeof(*response));
    response->type = type;

    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }

    return sizeof(*response);
}

/* 'virtio_gpu' protocol handlers */
void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_resp_disp_info));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    struct virtio_gpu_resp_disp_info *response = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[resp_idx].addr, sizeof(struct virtio_gpu_resp_disp_info));
    if (!response) {
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;

    /* 'GET_DISPLAY_INFO' exposes scanouts as the 'pmodes[]' array, so the array
     * index is the guest-visible 'scanout_id' used by later requests such as
     * 'SET_SCANOUT' and 'GET_EDID'.
     *
     * The spec describes 'pmodes[]' as per-scanout information but does not
     * spell out this mapping as a separate rule. semu follows the implicit
     * model where 'pmodes[i]' describes scanout ID 'i' because later requests
     * only carry a 'scanout_id', and Linux does the same when it copies
     * 'resp->pmodes[i]' into 'outputs[i]' and later sends 'output->index' in
     * 'SET_SCANOUT'. See 'virtgpu_vq.c' and 'virtgpu_display.c' for more
     * details.
     */
    int scanout_num = PRIV(vgpu)->num_scanouts;
    for (int i = 0; i < scanout_num; i++) {
        response->pmodes[i].r.width = PRIV(vgpu)->scanouts[i].width;
        response->pmodes[i].r.height = PRIV(vgpu)->scanouts[i].height;
        response->pmodes[i].enabled = PRIV(vgpu)->scanouts[i].enabled;
    }

    *plen = sizeof(*response);
    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->fence_id;
    }
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
     * Plug and Play Device Identifier (PNPID)
     */
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
                                        * modes are not supported
                                        */
    uint8_t color_type = 0 << 2; /* ignored as it is for the analog display */
    uint8_t other_flags = 0b110; /* [2]: sRGB as default color space
                                  * [1]: Prefered timing mode with native format
                                  * [0]: Non-continuys frequency
                                  */
    edid[24] = power_management | color_type | other_flags;

    /* Established timings: These are the default timings defined by the
     * VESA. Each bit represents 1 configuration. For now, we enable the
     * timing configurations of 1024x768@60Hz only
     */
    edid[35] = 0b00000000;
    edid[36] = 0b00001000;
    edid[37] = 0b00000000;

    /* Standard timings: 16 bytes data start from 'edid[38]' to 'edid[54]' as
     * additional timing configurations with 2 bytes for each to define
     * the horizontal pixel number, aspect ratio, and refresh rate.
     */

    /* Extension block count number */
    edid[126] = 0; /* No other extension blocks are defined */

    /* Checksum of the first (and the only) extension block */
    edid[127] = virtio_gpu_generate_edid_checksum(edid, 127);
}

void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen)
{
    struct virtio_gpu_cmd_get_edid *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_get_edid));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_resp_edid));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    if (request->scanout >= PRIV(vgpu)->num_scanouts ||
        !PRIV(vgpu)->scanouts[request->scanout].enabled) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid scanout id %u\n",
                __func__, request->scanout);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        return;
    }

    /* Generate the display EDID */
    struct virtio_gpu_resp_edid edid = {
        .hdr = {.type = VIRTIO_GPU_RESP_OK_EDID},
        .size = 128 /* One EDID extension block only */
    };
    virtio_gpu_generate_edid((uint8_t *) edid.edid, 0, 0);

    /* Write EDID response */
    struct virtio_gpu_resp_edid *response = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[resp_idx].addr, sizeof(struct virtio_gpu_resp_edid));
    if (!response) {
        *plen = 0;
        return;
    }

    memcpy(response, &edid, sizeof(struct virtio_gpu_resp_edid));

    /* return write length */
    *plen = sizeof(*response);
    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }
}

void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *header = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!header) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    fprintf(stderr,
            VIRTIO_GPU_LOG_PREFIX "%s(): unsupported VirtIO-GPU command %d.",
            __func__, header->type);

    virtio_gpu_set_fail(vgpu);
    *plen = 0;
}

static int virtio_gpu_desc_handler(virtio_gpu_state_t *vgpu,
                                   const virtio_gpu_queue_t *queue,
                                   uint32_t desc_idx,
                                   uint32_t *plen)
{
    struct virtq_desc vq_desc[VIRTIO_GPU_MAX_DESC] = {0};

    /* Collect descriptors */
    for (int i = 0; i < VIRTIO_GPU_MAX_DESC; i++) {
        if (desc_idx >= queue->QueueNum) {
            virtio_gpu_set_fail(vgpu);
            return -1;
        }

        /* The size of 'struct virtq_desc' is 4 words. */
        uint32_t desc_offset = queue->QueueDesc + desc_idx * 4;
        uint32_t *desc = &vgpu->ram[desc_offset];

        /* The guest is riscv32, so the upper 32 bits of every descriptor
         * address must be zero. Reject any descriptor whose 'addr_high' is set
         * before later code truncates it via 'virtio_gpu_mem_guest_to_host()',
         * which would otherwise silently mask a guest bug.
         */
        if (desc[1] != 0) {
            virtio_gpu_set_fail(vgpu);
            return -1;
        }

        /* Retrieve the fields of the current descriptor. */
        vq_desc[i].addr = desc[0];
        vq_desc[i].len = desc[2];
        vq_desc[i].flags = desc[3];
        desc_idx = desc[3] >> 16; /* 'vq_desc[desc_cnt].next' */

        /* Leave the loop if 'VIRTIO_DESC_F_NEXT' is not set. */
        if (!(vq_desc[i].flags & VIRTIO_DESC_F_NEXT))
            break;
    }

    struct virtio_gpu_ctrl_hdr *header = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!header) {
        virtio_gpu_set_fail(vgpu);
        return -1;
    }

    /* Keep the fixed 3-descriptor contract explicit. Longer chains need
     * multi-SG parsing, so reject them before command dispatch.
     *
     * TODO: Support generic descriptor-chain parsing.
     */
    if (vq_desc[VIRTIO_GPU_MAX_DESC - 1].flags & VIRTIO_DESC_F_NEXT) {
        int resp_idx = virtio_gpu_get_response_desc(
            vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_ctrl_hdr));
        if (resp_idx < 0) {
            fprintf(stderr,
                    VIRTIO_GPU_LOG_PREFIX
                    "%s(): descriptor chain exceeds supported length and has "
                    "no usable response descriptor\n",
                    __func__);
            virtio_gpu_set_fail(vgpu);
            return -1;
        }

        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): descriptor chain exceeds supported length\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(vgpu, header, &vq_desc[resp_idx],
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen) {
            virtio_gpu_set_fail(vgpu);
            return -1;
        }

        return 0;
    }

    /* Process the command */
    switch (header->type) {
        /* 2D commands */
        VIRTIO_GPU_CMD_CASE(GET_DISPLAY_INFO, get_display_info)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_2D, resource_create_2d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_UNREF, resource_unref)
        VIRTIO_GPU_CMD_CASE(SET_SCANOUT, set_scanout)
        VIRTIO_GPU_CMD_CASE(RESOURCE_FLUSH, resource_flush)
        VIRTIO_GPU_CMD_CASE(TRANSFER_TO_HOST_2D, transfer_to_host_2d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_ATTACH_BACKING, resource_attach_backing)
        VIRTIO_GPU_CMD_CASE(RESOURCE_DETACH_BACKING, resource_detach_backing)
        VIRTIO_GPU_CMD_CASE(GET_CAPSET_INFO, get_capset_info)
        VIRTIO_GPU_CMD_CASE(GET_CAPSET, get_capset)
        VIRTIO_GPU_CMD_CASE(GET_EDID, get_edid)
        VIRTIO_GPU_CMD_CASE(RESOURCE_ASSIGN_UUID, resource_assign_uuid)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_BLOB, resource_create_blob)
        VIRTIO_GPU_CMD_CASE(SET_SCANOUT_BLOB, set_scanout_blob)
        /* 3D commands */
        VIRTIO_GPU_CMD_CASE(CTX_CREATE, ctx_create)
        VIRTIO_GPU_CMD_CASE(CTX_DESTROY, ctx_destroy)
        VIRTIO_GPU_CMD_CASE(CTX_ATTACH_RESOURCE, ctx_attach_resource)
        VIRTIO_GPU_CMD_CASE(CTX_DETACH_RESOURCE, ctx_detach_resource)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_3D, resource_create_3d)
        VIRTIO_GPU_CMD_CASE(TRANSFER_TO_HOST_3D, transfer_to_host_3d)
        VIRTIO_GPU_CMD_CASE(TRANSFER_FROM_HOST_3D, transfer_from_host_3d)
        VIRTIO_GPU_CMD_CASE(SUBMIT_3D, submit_3d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_MAP_BLOB, resource_map_blob)
        VIRTIO_GPU_CMD_CASE(RESOURCE_UNMAP_BLOB, resource_unmap_blob)
        VIRTIO_GPU_CMD_CASE(UPDATE_CURSOR, update_cursor)
        VIRTIO_GPU_CMD_CASE(MOVE_CURSOR, move_cursor)
    default:
        virtio_gpu_cmd_undefined_handler(vgpu, vq_desc, plen);
        return -1;
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
    uint16_t avail_delta = (uint16_t) (new_avail - queue->last_avail);
    if (avail_delta > (uint16_t) queue->QueueNum)
        return (
            fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): size check failed\n",
                    __func__),
            virtio_gpu_set_fail(vgpu));

    if (queue->last_avail == new_avail)
        return;

    /* Process them */
    uint16_t new_used =
        ram[queue->QueueUsed] >> 16; /* 'virtq_used.idx' (le16) */
    while (queue->last_avail != new_avail) {
        /* Obtain the index in the ring buffer */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;

        /* Since each buffer index occupies 2 bytes but the memory is aligned
         * with 4 bytes, and the first element of the available queue is stored
         * at 'ram[queue->QueueAvail + 1]', to acquire the buffer index, it
         * requires the following array index calculation and bit shifting.
         * Check also 'struct virtq_avail' in the spec.
         */
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        /* Consume request from the available queue and process the data in the
         * descriptor list.
         */
        uint32_t len = 0;
        int result = virtio_gpu_desc_handler(vgpu, queue, buffer_idx, &len);
        if (result != 0)
            return;

        /* Write used element information ('struct virtq_used_elem') to the used
         * queue
         */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx; /* 'virtq_used_elem.id'  (le32) */
        ram[vq_used_addr + 1] = len;    /* 'virtq_used_elem.len' (le32) */
        queue->last_avail++;
        new_used++;
    }

    /* Update 'virtq_used.idx' (keep 'virtq_used.flags' in low 16 bits). */
    ram[queue->QueueUsed] &= MASK(16); /* clear high 16 bits (idx) */
    ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16; /* set idx */

    /* Send interrupt, unless 'VIRTQ_AVAIL_F_NO_INTERRUPT' is set. */
    if (!(ram[queue->QueueAvail] & 1))
        vgpu->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static inline uint32_t virtio_gpu_preprocess(virtio_gpu_state_t *vgpu,
                                             uint32_t addr)
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

    if (g_virtio_gpu_backend.reset)
        g_virtio_gpu_backend.reset(vgpu);

    /* Reset VirtIO device state (feature negotiation, queue descriptors,
     * avail/used rings, status and interrupt registers). 'ram' and 'priv' are
     * infrastructure pointers provided by the host, not device state, so
     * they are saved and restored across the 'memset()'.
     *
     * 'vgpu->priv' ('virtio_gpu_data_t') is intentionally NOT reset here.
     * It holds host-configured scanout info (display dimensions / enabled
     * flags) set up before the guest driver probes the device. The guest
     * re-queries this via 'CMD_GET_DISPLAY_INFO' after each reset, so it must
     * survive. Renderer-specific bindings and resources live behind the
     * backend hook and are reset before the generic device state is cleared.
     */
    uint32_t *ram = vgpu->ram;
    void *priv = vgpu->priv;
    memset(vgpu, 0, sizeof(*vgpu));
    vgpu->ram = ram;
    vgpu->priv = priv;
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
        *value = vgpu->DeviceFeaturesSel == 0
                     ? VIRTIO_GPU_F_EDID
                     : (vgpu->DeviceFeaturesSel == 1 ? VIRTIO_F_VERSION_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VIRTIO_GPU_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VIRTIO_GPU_QUEUE.ready ? 1 : 0;
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
    case _(SHMBaseHigh):
        *value = 0;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_gpu_config)))
            return false;

        /* Read configuration from the corresponding register */
        uint32_t offset = (addr - _(Config)) << 2;
        switch (offset) {
        case offsetof(struct virtio_gpu_config, events_read): {
            *value = 0; /* No event is implemented currently */
            return true;
        }
        case offsetof(struct virtio_gpu_config, num_scanouts): {
            *value = PRIV(vgpu)->num_scanouts;
            return true;
        }
        case offsetof(struct virtio_gpu_config, num_capsets): {
            *value = 0;
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
    /* The VGPU device exposes its MMIO registers as aligned 32-bit words
     * only. It rejects byte and halfword accesses instead of emulating
     * partial register reads.
     */
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

/* After 'QueueReady' is set, 'QueueNum' and the ring address registers have
 * already been validated and may be consumed by the device. Reject later
 * writes to that virtqueue configuration instead of letting the guest change
 * it under the running queue.
 */
static bool virtio_gpu_vq_config_after_ready(virtio_gpu_state_t *vgpu,
                                             uint32_t addr)
{
    if (!VIRTIO_GPU_QUEUE.ready)
        return false;

#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(QueueNum):
    case _(QueueDescLow):
    case _(QueueDescHigh):
    case _(QueueDriverLow):
    case _(QueueDriverHigh):
    case _(QueueDeviceLow):
    case _(QueueDeviceHigh):
        return true;
    default:
        return false;
    }
#undef _
}

static bool virtio_gpu_reg_write(virtio_gpu_state_t *vgpu,
                                 uint32_t addr,
                                 uint32_t value)
{
#define _(reg) VIRTIO_##reg
    if (virtio_gpu_vq_config_after_ready(vgpu, addr)) {
        virtio_gpu_set_fail(vgpu);
        return true;
    }

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
        if (value > 0 && value <= VIRTIO_GPU_QUEUE_NUM_MAX)
            VIRTIO_GPU_QUEUE.QueueNum = value;
        else
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueReady):
        VIRTIO_GPU_QUEUE.ready = value & 1;
        if (value & 1) {
            /* Validate that the full rings fit in guest RAM before allowing
             * the queue to go live. 'virtio_gpu_preprocess()' only checked the
             * base addresses. Here we verify the end of each ring region.
             * All addresses are word indices (byte address >> 2).
             *
             * These sizes assume 'VIRTIO_F_EVENT_IDX' is not negotiated. We
             * never advertise it (see 'DeviceFeatures'), so neither
             * 'avail.used_event' nor 'used.avail_event' exist. If that flag is
             * ever added, both end calculations need an extra word for the
             * trailing '*_event' field.
             */
            uint32_t qnum = VIRTIO_GPU_QUEUE.QueueNum;
            uint32_t ram_words = RAM_SIZE / sizeof(uint32_t);

            /* Desc table: 'QueueNum' entries * 4 words each. */
            uint32_t desc_end = VIRTIO_GPU_QUEUE.QueueDesc + qnum * 4;
            /* Avail ring: one word for 'flags' + 'idx', then
             * ceil('QueueNum' / 2) words for 16-bit descriptor indexes.
             */
            uint32_t avail_end =
                VIRTIO_GPU_QUEUE.QueueAvail + 1 + (qnum + 1) / 2;
            /* Used ring: one word for 'flags' + 'idx', then 'QueueNum'
             * entries of 'struct virtq_used_elem' (2 words each).
             */
            uint32_t used_end = VIRTIO_GPU_QUEUE.QueueUsed + 1 + qnum * 2;

            if (!qnum || desc_end > ram_words || avail_end > ram_words ||
                used_end > ram_words) {
                VIRTIO_GPU_QUEUE.ready = false;
                return virtio_gpu_set_fail(vgpu), true;
            }
            VIRTIO_GPU_QUEUE.last_avail =
                vgpu->ram[VIRTIO_GPU_QUEUE.QueueAvail] >> 16;
        }
        return true;
    case _(QueueDescLow):
        VIRTIO_GPU_QUEUE.QueueDesc = virtio_gpu_preprocess(vgpu, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueDriverLow):
        VIRTIO_GPU_QUEUE.QueueAvail = virtio_gpu_preprocess(vgpu, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_gpu_set_fail(vgpu);
        return true;
    case _(QueueDeviceLow):
        VIRTIO_GPU_QUEUE.QueueUsed = virtio_gpu_preprocess(vgpu, value);
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
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_gpu_config)))
            return false;

        /* Write configuration to the corresponding register */
        uint32_t offset = (addr - _(Config)) << 2;
        switch (offset) {
        case offsetof(struct virtio_gpu_config, events_clear): {
            /* Ignored, no event is implemented currently */
            return true;
        }
        default:
            return false;
        }
    }
#undef _
}

void virtio_gpu_write(hart_t *vm,
                      virtio_gpu_state_t *vgpu,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    /* The VGPU device applies the same rule to writes: only aligned 32-bit
     * stores are accepted for the MMIO register block, and narrower accesses
     * fault.
     */
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
    static bool initialized = false;

    if (initialized) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): only one virtio-gpu instance is supported\n",
                __func__);
        exit(2);
    }
    initialized = true;

    vgpu->priv = &virtio_gpu_data;
}

uint32_t virtio_gpu_register_scanout(virtio_gpu_state_t *vgpu,
                                     uint32_t width,
                                     uint32_t height)
{
    int scanout_num = PRIV(vgpu)->num_scanouts;
    if (scanout_num >= VIRTIO_GPU_MAX_SCANOUTS) {
        /* Registration is init-only today. Return an error instead if scanout
         * creation becomes dynamic or guest-triggered.
         */
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): exceeded scanout maximum number\n",
                __func__);
        exit(2);
    }

    PRIV(vgpu)->scanouts[scanout_num].width = width;
    PRIV(vgpu)->scanouts[scanout_num].height = height;
    PRIV(vgpu)->scanouts[scanout_num].enabled = 1;
    PRIV(vgpu)->scanouts[scanout_num].primary_resource_id = 0;
    PRIV(vgpu)->scanouts[scanout_num].cursor_resource_id = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_x = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_y = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_w = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_h = 0;

    /* 'scanout_num' will match the guest-visible 'scanout_id'. See
     * 'virtio_gpu_get_display_info_handler()' above for how that index is
     * exposed to the guest and later reused in 'SET_SCANOUT'/'GET_EDID'.
     */
    PRIV(vgpu)->num_scanouts++;

    return (uint32_t) scanout_num;
}
