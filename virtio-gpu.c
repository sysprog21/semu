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
#include "virtio.h"
#include "window.h"

#define VIRTIO_F_VERSION_1 1

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)
#define VIRTIO_GPU_F_EDID (1 << 1)
#define VIRTIO_GPU_FLAG_FENCE (1 << 0)

#define VGPU_QUEUE_NUM_MAX 1024
#define VGPU_QUEUE (vgpu->queues[vgpu->QueueSel])

#define PRIV(x) ((struct vgpu_scanout_info *) x->priv)

#define STRIDE_SIZE 4096

struct vgpu_scanout_info {
    uint32_t width;
    uint32_t height;
    uint32_t enabled;
};

struct vgpu_resource_2d {
    /* Public: */
    uint32_t scanout_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint32_t *image;
    bool scanout_attached;
    /* Private: */
    uint32_t resource_id;
    size_t page_cnt;
    struct iovec *iovec;
    struct list_head list;
};

PACKED(struct vgpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
});

PACKED(struct vgpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
});

PACKED(struct vgpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
});

PACKED(struct vgpu_resp_disp_info {
    struct vgpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one {
        struct vgpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
});

PACKED(struct vgpu_res_create_2d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
});

PACKED(struct vgpu_res_unref {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct vgpu_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
});

PACKED(struct vgpu_res_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct vgpu_trans_to_host_2d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct vgpu_res_attach_backing {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
});

PACKED(struct vgpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
});

PACKED(struct vgpu_resp_edid {
    struct vgpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
    char edid[1024];
});

PACKED(struct vgpu_get_capset_info {
    struct vgpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
});

PACKED(struct vgpu_resp_capset_info {
    struct vgpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
});

PACKED(struct virtio_gpu_resp_capset {
    struct vgpu_ctrl_hdr hdr;
    uint8_t *capset_data;
});

PACKED(struct virtio_gpu_ctx_create {
    struct vgpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
});

PACKED(struct virtio_gpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
});

PACKED(struct virtio_gpu_update_cursor {
    struct vgpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
});

/* clang-format off */
PACKED(struct virtio_gpu_ctx_destroy {
    struct vgpu_ctrl_hdr hdr;
});
/* clang-format on */

PACKED(struct virtio_gpu_resource_create_3d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
});

PACKED(struct virtio_gpu_ctx_resource {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct virtio_gpu_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
});

PACKED(struct virtio_gpu_transfer_host_3d {
    struct vgpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
});

PACKED(struct virtio_gpu_cmd_submit {
    struct vgpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t num_in_fences;
});

PACKED(struct virtio_gpu_resp_map_info {
    struct vgpu_ctrl_hdr hdr;
    uint32_t map_info;
    uint32_t padding;
});

static struct vgpu_config vgpu_configs;
static LIST_HEAD(vgpu_res_2d_list);

typedef void (*vgpu_cmd_func)(virtio_gpu_state_t *vgpu,
                              struct virtq_desc *vq_desc,
                              uint32_t *plen);

struct vgpu_cmd_backend {
    /* 2D commands */
    vgpu_cmd_func get_display_info;
    vgpu_cmd_func resource_create_2d;
    vgpu_cmd_func resource_unref;
    vgpu_cmd_func set_scanout;
    vgpu_cmd_func resource_flush;
    vgpu_cmd_func trasfer_to_host_2d;
    vgpu_cmd_func resource_attach_backing;
    vgpu_cmd_func resource_detach_backing;
    vgpu_cmd_func get_capset_info;
    vgpu_cmd_func get_capset;
    vgpu_cmd_func get_edid;
    vgpu_cmd_func resource_assign_uuid;
    vgpu_cmd_func resource_create_blob;
    vgpu_cmd_func set_scanout_blob;
    /* 3D commands */
    vgpu_cmd_func ctx_create;
    vgpu_cmd_func ctx_destroy;
    vgpu_cmd_func ctx_attach_resource;
    vgpu_cmd_func ctx_detach_resource;
    vgpu_cmd_func resource_create_3d;
    vgpu_cmd_func transfer_to_host_3d;
    vgpu_cmd_func transfer_from_host_3d;
    vgpu_cmd_func submit_3d;
    vgpu_cmd_func resource_map_blob;
    vgpu_cmd_func resource_unmap_blob;
    /* Cursor commands */
    vgpu_cmd_func update_cursor;
    vgpu_cmd_func move_cursor;
};

static inline void *vgpu_mem_host_to_guest(virtio_gpu_state_t *vgpu,
                                           uint32_t addr)
{
    return (void *) ((uintptr_t) vgpu->ram + addr);
}

static struct vgpu_resource_2d *create_vgpu_resource_2d(int resource_id)
{
    struct vgpu_resource_2d *res = malloc(sizeof(struct vgpu_resource_2d));
    if (!res)
        return NULL;

    res->resource_id = resource_id;
    res->scanout_attached = false;
    list_push(&res->list, &vgpu_res_2d_list);
    return res;
}

static struct vgpu_resource_2d *acquire_vgpu_resource_2d(uint32_t resource_id)
{
    struct vgpu_resource_2d *res_2d;
    list_for_each_entry (res_2d, &vgpu_res_2d_list, list) {
        if (res_2d->resource_id == resource_id)
            return res_2d;
    }

    return NULL;
}

static int destroy_vgpu_resource_2d(uint32_t resource_id)
{
    struct vgpu_resource_2d *res_2d = acquire_vgpu_resource_2d(resource_id);

    /* Failed to find the resource */
    if (!res_2d)
        return -1;

    int scanout_id = res_2d->scanout_id;
    if (res_2d->scanout_attached)
        window_lock(scanout_id);

    /* Release the resource */
    free(res_2d->image);
    list_del(&res_2d->list);
    free(res_2d->iovec);
    free(res_2d);

    if (res_2d->scanout_attached)
        window_unlock(scanout_id);

    return 0;
}

static void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
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

static void virtio_gpu_set_response_fencing(struct vgpu_ctrl_hdr *request,
                                            struct vgpu_ctrl_hdr *response)
{
    if (request->flags &= VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }
}

static void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                                struct virtq_desc *vq_desc,
                                                uint32_t *plen)
{
    /* Write display infomation */
    struct vgpu_resp_disp_info *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;

    int scanout_num = vgpu_configs.num_scanouts;
    for (int i = 0; i < scanout_num; i++) {
        response->pmodes[i].r.width = PRIV(vgpu)[i].width;
        response->pmodes[i].r.height = PRIV(vgpu)[i].height;
        response->pmodes[i].enabled = PRIV(vgpu)[i].enabled;
    }

    /* Update write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_create_2d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Create 2D resource */
    struct vgpu_resource_2d *res_2d =
        create_vgpu_resource_2d(request->resource_id);

    if (!res_2d) {
        fprintf(stderr, "%s(): Failed to allocate 2D resource\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Select image formats */
    uint32_t bits_per_pixel;

    switch (request->format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        bits_per_pixel = 32;
        break;
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        bits_per_pixel = 32;
        break;
    default:
        fprintf(stderr, "%s(): Unsupported format %d\n", __func__,
                request->format);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    uint32_t bytes_per_pixel = bits_per_pixel / 8;

    /* Set 2D resource */
    res_2d->width = request->width;
    res_2d->height = request->height;
    res_2d->format = request->format;
    res_2d->bits_per_pixel = bits_per_pixel;
    res_2d->stride = STRIDE_SIZE;
    res_2d->image = malloc(bytes_per_pixel * (request->width + res_2d->stride) *
                           request->height);

    /* Failed to create image buffer */
    if (!res_2d->image) {
        fprintf(stderr, "%s(): Failed to allocate image buffer\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Response with fencing flag if needed */
    virtio_gpu_set_response_fencing(&request->hdr, response);

    /* Return write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_unref *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Destroy 2D resource */
    int result = destroy_vgpu_resource_2d(request->resource_id);

    if (result != 0) {
        fprintf(stderr, "%s(): Failed to destroy 2D resource\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(struct vgpu_ctrl_hdr));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
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
    edid[9] = vendor_id && 0xff;

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

static void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
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
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);
    memcpy(response, &edid, sizeof(*response));

    /* return write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    /* Read request */
    struct vgpu_set_scanout *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Acquire 2D resource */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(request->resource_id);

    /* Linux's virtio-gpu driver may send scanout command
     * even if the resource does not exist */
    if (res_2d) {
        /* Set scanout ID to proper 2D resource */
        res_2d->scanout_id = request->scanout_id;
        res_2d->scanout_attached = true;
    }

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_flush *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Acquire 2D resource */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(request->resource_id);

    /* Trigger display window rendering */
    window_lock(res_2d->scanout_id);
    window_render((struct gpu_resource *) res_2d);
    window_unlock(res_2d->scanout_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_copy_image_from_pages(struct vgpu_trans_to_host_2d *req,
                                             struct vgpu_resource_2d *res_2d)
{
    uint32_t stride = res_2d->stride;
    uint32_t bpp = res_2d->bits_per_pixel / 8; /* Bytes per pixel */
    uint32_t width =
        (req->r.width < res_2d->width) ? req->r.width : res_2d->width;
    uint32_t height =
        (req->r.height < res_2d->height) ? req->r.height : res_2d->height;
    void *img_data = (void *) res_2d->image;

    for (uint32_t h = 0; h < height; h++) {
        size_t src_offset = req->offset + stride * h;
        size_t dest_offset = (req->r.y + h) * stride + (req->r.x * bpp);
        void *dest = (void *) ((uintptr_t) img_data + dest_offset);
        size_t done = 0;
        size_t total = width * bpp;

        for (uint32_t i = 0; i < res_2d->page_cnt; i++) {
            /* Skip empty pages */
            if (res_2d->iovec[i].iov_base == 0 || res_2d->iovec[i].iov_len == 0)
                continue;

            if (src_offset < res_2d->iovec[i].iov_len) {
                /* Source offset is in the image coordinate. The address to
                 * copy from is the page base address plus with the offset
                 */
                void *src = (void *) ((uintptr_t) res_2d->iovec[i].iov_base +
                                      src_offset);

                /* Take as much as data of current page can provide */
                size_t remained = total - done;
                size_t page_avail = res_2d->iovec[i].iov_len - src_offset;
                size_t nbytes = (remained < page_avail) ? remained : page_avail;

                /* Copy to 2D resource buffer */
                memcpy((void *) ((uintptr_t) dest + done), src, nbytes);

                /* If there is still data left to read, but current page is
                 * exhausted, we need to read from the beginning of the next
                 * page, where its offset should be 0 */
                src_offset = 0;

                /* Count the total received bytes so far */
                done += nbytes;

                /* Data transfering of current scanline is complete */
                if (done >= total)
                    break;
            } else {
                /* Keep substracting until reaching the page */
                src_offset -= res_2d->iovec[i].iov_len;
            }
        }
    }
}

static void virtio_gpu_cursor_image_copy(struct vgpu_resource_2d *res_2d)
{
    /* The cursor resource is tightly packed and contiguous */
    memcpy(res_2d->image, res_2d->iovec[0].iov_base,
           sizeof(uint32_t) * CURSOR_WIDTH * CURSOR_HEIGHT);
}

static void virtio_gpu_cmd_transfer_to_host_2d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Read request */
    struct vgpu_trans_to_host_2d *req =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Acquire 2D resource */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(req->resource_id);

    if (!res_2d) {
        fprintf(stderr, "%s(): Failed to find 2D resource\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Check image boundary */
    if (req->r.x > res_2d->width || req->r.y > res_2d->height ||
        req->r.width > res_2d->width || req->r.height > res_2d->height ||
        req->r.x + req->r.width > res_2d->width ||
        req->r.y + req->r.height > res_2d->height) {
        fprintf(stderr, "%s(): Invalid image size\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    uint32_t width =
        (req->r.width < res_2d->width) ? req->r.width : res_2d->width;
    uint32_t height =
        (req->r.height < res_2d->height) ? req->r.height : res_2d->height;

    /* Transfer frame data from guest to host */
    if (width == CURSOR_WIDTH && height == CURSOR_HEIGHT)
        virtio_gpu_cursor_image_copy(res_2d);
    else
        virtio_gpu_copy_image_from_pages(req, res_2d);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    struct vgpu_ctrl_hdr res_no_data = {.type = VIRTIO_GPU_RESP_OK_NODATA};
    memcpy(response, &res_no_data, sizeof(struct vgpu_ctrl_hdr));

    /* Response with fencing flag if needed */
    virtio_gpu_set_response_fencing(&req->hdr, response);

    /* Update write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_cmd_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_attach_backing *backing_info =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);
    struct vgpu_mem_entry *pages =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    /* Acquire 2D resource */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(backing_info->resource_id);

    /* Dispatch page memories to the 2D resource */
    res_2d->page_cnt = backing_info->nr_entries;
    res_2d->iovec = malloc(sizeof(struct iovec) * backing_info->nr_entries);
    struct vgpu_mem_entry *mem_entries = (struct vgpu_mem_entry *) pages;

    for (size_t i = 0; i < backing_info->nr_entries; i++) {
        /* Attach address and length of i-th page to the 2D resource */
        res_2d->iovec[i].iov_base =
            vgpu_mem_host_to_guest(vgpu, mem_entries[i].addr);
        res_2d->iovec[i].iov_len = mem_entries[i].length;

        /* Corrupted page address */
        if (!res_2d->iovec[i].iov_base) {
            fprintf(stderr, "%s(): Invalid page address\n", __func__);
            virtio_gpu_set_fail(vgpu);
            return;
        }
    }

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[2].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Response with fencing flag if needed */
    virtio_gpu_set_response_fencing(&backing_info->hdr, response);

    /* Return write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                                 struct virtq_desc *vq_desc,
                                                 uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_update_cursor *cursor =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Update cursor image */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(cursor->resource_id);

    if (res_2d != NULL) {
        window_lock(cursor->pos.scanout_id);
        cursor_update((struct gpu_resource *) res_2d, cursor->pos.scanout_id,
                      cursor->pos.x, cursor->pos.y);
        window_unlock(cursor->pos.scanout_id);
    } else if (cursor->resource_id == 0) {
        window_lock(cursor->pos.scanout_id);
        cursor_clear(cursor->pos.scanout_id);
        window_unlock(cursor->pos.scanout_id);
    } else {
        fprintf(stderr, "Invalid resource ID %d.\n", cursor->resource_id);
    }

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

static void virtio_gpu_cmd_move_cursor_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_update_cursor *cursor =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Move cursor to new position */
    window_lock(cursor->pos.scanout_id);
    cursor_move(cursor->pos.scanout_id, cursor->pos.x, cursor->pos.y);
    window_unlock(cursor->pos.scanout_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

static struct vgpu_cmd_backend vgpu_cmd_table = {
    .get_display_info = virtio_gpu_get_display_info_handler,
    .resource_create_2d = virtio_gpu_resource_create_2d_handler,
    .resource_unref = virtio_gpu_cmd_resource_unref_handler,
    .set_scanout = virtio_gpu_cmd_set_scanout_handler,
    .resource_flush = virtio_gpu_cmd_resource_flush_handler,
    .trasfer_to_host_2d = virtio_gpu_cmd_transfer_to_host_2d_handler,
    .resource_attach_backing = virtio_gpu_cmd_resource_attach_backing_handler,
    .resource_detach_backing = NULL,
    .get_capset_info = NULL,
    .get_capset = NULL,
    .get_edid = virtio_gpu_get_edid_handler,
    .resource_assign_uuid = NULL,
    .resource_create_blob = NULL,
    .set_scanout_blob = NULL,
    .ctx_create = NULL,
    .ctx_destroy = NULL,
    .ctx_attach_resource = NULL,
    .ctx_detach_resource = NULL,
    .resource_create_3d = NULL,
    .transfer_to_host_3d = NULL,
    .transfer_from_host_3d = NULL,
    .submit_3d = NULL,
    .resource_map_blob = NULL,
    .resource_unmap_blob = NULL,
    .update_cursor = virtio_gpu_cmd_update_cursor_handler,
    .move_cursor = virtio_gpu_cmd_move_cursor_handler,
};

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
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Process the command */
    vgpu_cmd_func vgpu_cmd = NULL;
    switch (header->type) {
    /* 2D commands */
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        vgpu_cmd = vgpu_cmd_table.get_display_info;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        vgpu_cmd = vgpu_cmd_table.resource_create_2d;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        vgpu_cmd = vgpu_cmd_table.resource_unref;
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        vgpu_cmd = vgpu_cmd_table.set_scanout;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        vgpu_cmd = vgpu_cmd_table.resource_flush;
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        vgpu_cmd = vgpu_cmd_table.trasfer_to_host_2d;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        vgpu_cmd = vgpu_cmd_table.resource_attach_backing;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        vgpu_cmd = vgpu_cmd_table.resource_detach_backing;
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        vgpu_cmd = vgpu_cmd_table.get_capset_info;
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        vgpu_cmd = vgpu_cmd_table.get_capset;
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        vgpu_cmd = vgpu_cmd_table.get_edid;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
        vgpu_cmd = vgpu_cmd_table.resource_assign_uuid;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        vgpu_cmd = vgpu_cmd_table.resource_create_blob;
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        vgpu_cmd = vgpu_cmd_table.set_scanout_blob;
        break;
    /* 3D commands */
    case VIRTIO_GPU_CMD_CTX_CREATE:
        vgpu_cmd = vgpu_cmd_table.ctx_create;
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        vgpu_cmd = vgpu_cmd_table.ctx_destroy;
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        vgpu_cmd = vgpu_cmd_table.ctx_attach_resource;
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        vgpu_cmd = vgpu_cmd_table.ctx_detach_resource;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        vgpu_cmd = vgpu_cmd_table.resource_create_3d;
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        vgpu_cmd = vgpu_cmd_table.transfer_to_host_3d;
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        vgpu_cmd = vgpu_cmd_table.transfer_from_host_3d;
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        vgpu_cmd = vgpu_cmd_table.submit_3d;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        vgpu_cmd = vgpu_cmd_table.resource_map_blob;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        vgpu_cmd = vgpu_cmd_table.resource_unmap_blob;
        break;
    case VIRTIO_GPU_CMD_UPDATE_CURSOR:
        vgpu_cmd = vgpu_cmd_table.update_cursor;
        break;
    case VIRTIO_GPU_CMD_MOVE_CURSOR:
        vgpu_cmd = vgpu_cmd_table.move_cursor;
        break;
    default:
        fprintf(stderr, "%s(): unknown command %d\n", __func__, header->type);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    if (vgpu_cmd)
        vgpu_cmd(vgpu, vq_desc, plen);
    else
        fprintf(stderr, "%s(): unsupported VirtIO-GPU command %d.", __func__,
                header->type);

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
            *value = 0; /* TODO: Add at least one capset to support VirGl */
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
        vgpu->DriverFeaturesSel == 0 ? (vgpu->DriverFeatures = value) : 0;
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
    vgpu->priv =
        calloc(sizeof(struct vgpu_scanout_info), VIRTIO_GPU_MAX_SCANOUTS);
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

    PRIV(vgpu)[scanout_num].width = width;
    PRIV(vgpu)[scanout_num].height = height;
    PRIV(vgpu)[scanout_num].enabled = 1;

    window_add(width, height);

    vgpu_configs.num_scanouts++;
}
