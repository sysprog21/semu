#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "device.h"
#include "utils.h"
#include "vgpu-display.h"
#include "virtio-gpu.h"
#include "virtio.h"

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

/* Host-side images are allocated per resource with 'calloc()'. Track their
 * aggregate size and cap it at 256 MiB.
 *
 * Backing entries describe guest RAM ranges. Use 4 KiB as the expected minimum
 * page granularity, so 512 MiB guest RAM needs at most 'RAM_SIZE / 4096'
 * entries plus one extra entry for an unaligned tail.
 */
#define VGPU_SW_MAX_HOSTMEM (256U * 1024U * 1024U)
#define VGPU_SW_BACKING_ENTRY_PAGE_SIZE 4096U
#define VGPU_SW_MAX_BACKING_ENTRIES \
    (RAM_SIZE / VGPU_SW_BACKING_ENTRY_PAGE_SIZE + 1U)

/* Host-side 2D resource owned by the software backend. It keeps the copied
 * 'image' plus any attached guest backing metadata needed by transfers.
 */
struct vgpu_sw_resource_2d {
    uint32_t resource_id;
    uint32_t format;
    uint32_t width, height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint32_t *image;
    size_t image_size;
    size_t page_cnt;
    struct iovec *iovec;
    struct list_head list;
};

/* Process-wide singleton: semu currently assumes at most one software
 * virtio-gpu backend instance per process.
 */
static LIST_HEAD(g_vgpu_sw_res_2d_list);
static size_t g_vgpu_sw_hostmem;

static size_t vgpu_sw_iov_to_buf(const struct iovec *iov,
                                 unsigned int iov_cnt,
                                 size_t offset,
                                 void *buf,
                                 size_t bytes)
{
    size_t done = 0;

    if (bytes == 0)
        return 0;

    /* Each non-empty 'iovec' entry is validated by 'RESOURCE_ATTACH_BACKING'
     * before it is stored here. Treat the array as one long byte stream: skip
     * whole entries until reaching the starting offset, then copy chunks into
     * 'buf'.
     */
    for (unsigned int i = 0; i < iov_cnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        assert(iov[i].iov_base != NULL);

        if (offset < iov[i].iov_len) {
            size_t remained = bytes - done;
            size_t page_avail = iov[i].iov_len - offset;
            size_t len = (remained < page_avail) ? remained : page_avail;
            void *src = (void *) ((uintptr_t) iov[i].iov_base + offset);
            void *dest = (void *) ((uintptr_t) buf + done);

            memcpy(dest, src, len);
            offset = 0;
            done += len;

            if (done >= bytes)
                break;
        } else {
            offset -= iov[i].iov_len;
        }
    }

    return done;
}

static bool vgpu_sw_u64_add_overflow(uint64_t a, uint64_t b, uint64_t *out)
{
    *out = a + b;
    return *out < a;
}

static bool vgpu_sw_u64_mul_overflow(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return true;

    *out = a * b;
    return false;
}

static bool vgpu_sw_rect_fits(uint32_t width,
                              uint32_t height,
                              const struct virtio_gpu_rect *rect)
{
    if (rect->width == 0 || rect->height == 0)
        return false;
    if (rect->x >= width || rect->y >= height)
        return false;

    return rect->width <= width - rect->x && rect->height <= height - rect->y;
}

static bool vgpu_sw_transfer_source_fits(
    const struct virtio_gpu_trans_to_host_2d *req,
    const struct vgpu_sw_resource_2d *res_2d)
{
    uint64_t bpp = res_2d->bits_per_pixel / 8;
    uint64_t row_bytes, row_stride, last_row, last_row_offset, end_offset;
    uint64_t required_bytes, backing_size = 0, backing_end;

    if (req->r.height == 0 || req->offset > SIZE_MAX)
        return false;
    if (vgpu_sw_u64_mul_overflow(req->r.width, bpp, &row_bytes) ||
        row_bytes == 0)
        return false;
    if (vgpu_sw_u64_mul_overflow((uint64_t) res_2d->stride, req->r.height - 1,
                                 &last_row))
        return false;
    if (vgpu_sw_u64_add_overflow(req->offset, last_row, &last_row_offset))
        return false;
    if (vgpu_sw_u64_add_overflow(last_row_offset, row_bytes, &end_offset))
        return false;
    if (vgpu_sw_u64_mul_overflow((uint64_t) res_2d->stride, req->r.height,
                                 &row_stride))
        return false;

    required_bytes =
        row_bytes == res_2d->stride ? row_stride : end_offset - req->offset;
    for (size_t i = 0; i < res_2d->page_cnt; i++) {
        if (vgpu_sw_u64_add_overflow(backing_size, res_2d->iovec[i].iov_len,
                                     &backing_size))
            return false;
    }

    return !vgpu_sw_u64_add_overflow(req->offset, required_bytes,
                                     &backing_end) &&
           backing_end <= backing_size;
}

static bool vgpu_sw_copy_image_from_pages(
    struct virtio_gpu_trans_to_host_2d *req,
    struct vgpu_sw_resource_2d *res_2d)
{
    uint32_t stride = res_2d->stride;
    uint32_t bpp = res_2d->bits_per_pixel / 8; /* Bytes per pixel */
    uint32_t width = req->r.width;
    uint32_t height = req->r.height;

    /* When the transfer spans full-width rows with no padding, both source
     * ('iovec' at 'req->offset') and destination ('image' at 'r.y') are
     * contiguous, so the entire rectangle can be copied in a single helper
     * call. This covers all cursor transfers, full-frame updates, and
     * full-width dirty bands.
     */
    if (req->r.x == 0 && (size_t) width * bpp == stride) {
        void *dest =
            (void *) ((uintptr_t) res_2d->image + (size_t) req->r.y * stride);
        size_t bytes = (size_t) stride * height;
        return vgpu_sw_iov_to_buf(res_2d->iovec, res_2d->page_cnt,
                                  (size_t) req->offset, dest, bytes) == bytes;
    }

    /* Partial-width sub-rect: copy row by row */
    for (uint32_t h = 0; h < height; h++) {
        /* Source offset is in the image coordinate. The address to copy from
         * is the page base address plus the offset.
         */
        size_t src_offset = req->offset + (size_t) stride * h;
        size_t dest_offset =
            ((size_t) req->r.y + h) * stride + (size_t) req->r.x * bpp;
        void *dest = (void *) ((uintptr_t) res_2d->image + dest_offset);
        size_t total = (size_t) width * bpp;

        if (vgpu_sw_iov_to_buf(res_2d->iovec, res_2d->page_cnt, src_offset,
                               dest, total) != total)
            return false;
    }

    return true;
}

static void vgpu_sw_destroy_resource_2d(struct vgpu_sw_resource_2d *res_2d)
{
    list_del(&res_2d->list);
    g_vgpu_sw_hostmem -= res_2d->image_size;
    free(res_2d->image);
    free(res_2d->iovec);
    free(res_2d);
}

static struct vgpu_sw_resource_2d *vgpu_sw_get_resource_2d(uint32_t resource_id)
{
    struct vgpu_sw_resource_2d *res_2d;
    list_for_each_entry (res_2d, &g_vgpu_sw_res_2d_list, list) {
        if (res_2d->resource_id == resource_id)
            return res_2d;
    }

    return NULL;
}

static const struct virtq_desc *vgpu_sw_get_response_desc(
    struct virtq_desc *vq_desc,
    size_t response_size,
    uint32_t *plen)
{
    int resp_idx = virtio_gpu_get_response_desc(vq_desc, VIRTIO_GPU_MAX_DESC,
                                                response_size);
    if (resp_idx >= 0)
        return &vq_desc[resp_idx];

    *plen = 0;
    return NULL;
}

static struct virtio_gpu_scanout_info *vgpu_sw_get_scanout(
    virtio_gpu_state_t *vgpu,
    uint32_t scanout_id)
{
    if (scanout_id >= PRIV(vgpu)->num_scanouts)
        return NULL;

    struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[scanout_id];
    return scanout->enabled ? scanout : NULL;
}

static struct vgpu_display_payload *vgpu_sw_create_window_payload(
    const struct vgpu_sw_resource_2d *res_2d,
    const struct virtio_gpu_scanout_info *scanout,
    const char *plane_name)
{
    if (!res_2d || !res_2d->image) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): missing %s image\n",
                __func__, plane_name);
        return NULL;
    }

    if (res_2d->bits_per_pixel == 0 || (res_2d->bits_per_pixel % 8) != 0) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid %s bpp %u\n",
                __func__, plane_name, res_2d->bits_per_pixel);
        return NULL;
    }

    size_t bytes_per_pixel = res_2d->bits_per_pixel / 8;
    uint32_t src_x = 0;
    uint32_t src_y = 0;
    uint32_t width = res_2d->width;
    uint32_t height = res_2d->height;
    if (scanout) {
        /* Primary scanouts can expose only a sub-rectangle of the resource.
         * Record that view before snapshotting it.
         */
        src_x = scanout->src_x;
        src_y = scanout->src_y;
        width = scanout->src_w;
        height = scanout->src_h;
    }

    if (width == 0 || height == 0) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid %s size %ux%u\n",
                __func__, plane_name, width, height);
        return NULL;
    }

    size_t row_bytes = (size_t) width * bytes_per_pixel;
    if (row_bytes / width != bytes_per_pixel) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): %s row size overflow\n",
                __func__, plane_name);
        return NULL;
    }
    if (row_bytes > UINT32_MAX) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): %s row size exceeds uint32_t\n",
                __func__, plane_name);
        return NULL;
    }
    if (res_2d->stride < row_bytes) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): invalid %s stride %u for row size %zu\n",
                __func__, plane_name, res_2d->stride, row_bytes);
        return NULL;
    }

    size_t pixels_size = row_bytes * height;
    if (pixels_size / height != row_bytes) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): %s image size overflow\n",
                __func__, plane_name);
        return NULL;
    }

    size_t alloc_size = sizeof(struct vgpu_display_payload) + pixels_size;
    if (alloc_size < pixels_size) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): %s allocation overflow\n",
                __func__, plane_name);
        return NULL;
    }

    struct vgpu_display_payload *payload = malloc(alloc_size);
    if (!payload) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): failed to allocate %s snapshot\n",
                __func__, plane_name);
        return NULL;
    }

    payload->cpu.format = res_2d->format;
    payload->cpu.width = width;
    payload->cpu.height = height;
    payload->cpu.stride = (uint32_t) row_bytes;
    payload->cpu.bits_per_pixel = res_2d->bits_per_pixel;
    payload->cpu.pixels = (uint8_t *) (payload + 1);

    /* The cropped view is contiguous only when the source stride matches this
     * snapshot's row size. Otherwise each source row still carries padding or
     * untouched pixels outside the requested view, so the snapshot must be
     * packed row by row.
     */
    const uint8_t *src_pixels = (const uint8_t *) res_2d->image +
                                (size_t) src_y * res_2d->stride +
                                (size_t) src_x * bytes_per_pixel;
    if (res_2d->stride == row_bytes) {
        memcpy(payload->cpu.pixels, src_pixels, pixels_size);
    } else {
        for (uint32_t y = 0; y < height; y++) {
            memcpy(payload->cpu.pixels + (size_t) y * row_bytes,
                   src_pixels + (size_t) y * res_2d->stride, row_bytes);
        }
    }

    return payload;
}

/* Backend Implementation */
static void vgpu_sw_reset(virtio_gpu_state_t *vgpu)
{
    /* The display queue may still hold older 'PRIMARY_SET' / 'CURSOR_SET'
     * frames published before this reset. Publishing 'CLEAR' advances the
     * per-plane generation; 'vgpu_display_pop_cmd()' consumes those clears
     * first, then drops older queued frame commands as stale.
     *
     * Queued frame payloads are deep copies, so destroying resources after the
     * clear publication cannot dangle any display payload still in the bridge.
     * The display queue is SPSC and consumer-owned, so reset does not drain it
     * from the producer side. The bounded queue releases stale payloads when
     * the SDL consumer pops them.
     */
    for (uint32_t i = 0; i < PRIV(vgpu)->num_scanouts; i++) {
        PRIV(vgpu)->scanouts[i].primary_resource_id = 0;
        PRIV(vgpu)->scanouts[i].cursor_resource_id = 0;
        PRIV(vgpu)->scanouts[i].src_x = 0;
        PRIV(vgpu)->scanouts[i].src_y = 0;
        PRIV(vgpu)->scanouts[i].src_w = 0;
        PRIV(vgpu)->scanouts[i].src_h = 0;
        vgpu_display_publish_primary_clear(i);
        vgpu_display_publish_cursor_clear(i);
    }

    struct list_head *curr, *next;
    list_for_each_safe (curr, next, &g_vgpu_sw_res_2d_list) {
        struct vgpu_sw_resource_2d *res_2d =
            list_entry(curr, struct vgpu_sw_resource_2d, list);

        vgpu_sw_destroy_resource_2d(res_2d);
    }
}

static void vgpu_sw_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_res_create_2d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_create_2d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Keep 'resource_id' 0 unavailable for real resources. The virtio spec
     * explicitly documents 'resource_id = 0' as the 'SET_SCANOUT' disable
     * sentinel.
     * The Linux virtio-gpu driver also allocates guest-generated resource IDs
     * as 'handle + 1', so they are always greater than 0. See
     * 'virtgpu_object.c' for details.
     */
    if (request->resource_id == 0) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): resource id should not be 0\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    if (request->width == 0 || request->height == 0) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource size %ux%u\n",
                __func__, request->width, request->height);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* Reject re-use of an already-live resource id. Without this check the
     * guest could orphan the previous resource (its 'image' and 'iovec' would
     * leak because 'vgpu_sw_get_resource_2d()' returns the first match) and
     * confuse later 'TRANSFER' / 'FLUSH' / 'UNREF' requests that target the
     * same id. Spec explicitly allows the device to fail this.
     */
    if (vgpu_sw_get_resource_2d(request->resource_id)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): resource id %u already in use\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Create 2D resource */
    struct vgpu_sw_resource_2d *res_2d = calloc(1, sizeof(*res_2d));
    if (!res_2d) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): failed to allocate new resource\n",
                __func__);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }
    res_2d->resource_id = request->resource_id;
    list_push(&res_2d->list, &g_vgpu_sw_res_2d_list);

    /* The software backend currently supports only 32bpp packed formats. */
    uint32_t bits_per_pixel;
    switch (request->format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        bits_per_pixel = 32;
        break;
    default:
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): unsupported format %d\n",
                __func__, request->format);
        vgpu_sw_destroy_resource_2d(res_2d);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* Set 2D resource */
    res_2d->width = request->width;
    res_2d->height = request->height;
    res_2d->format = request->format;
    res_2d->bits_per_pixel = bits_per_pixel;

    /* Compute the row stride in a wider type first, then narrow it only after
     * checking the final byte count still fits in 'uint32_t'. Otherwise a large
     * guest width could wrap during the intermediate multiplication and leave
     * a truncated stride in the resource.
     */
    size_t stride =
        (((size_t) res_2d->width * res_2d->bits_per_pixel + 0x1f) >> 5) *
        sizeof(uint32_t);
    if (stride > UINT32_MAX) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): stride overflow (%u x %u bpp)\n",
                __func__, res_2d->width, res_2d->bits_per_pixel);
        vgpu_sw_destroy_resource_2d(res_2d);
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        return;
    }
    res_2d->stride = (uint32_t) stride;

    /* Guard against integer overflow in image buffer allocation.
     * Both 'stride' and 'height' are guest-controlled 'uint32_t' values whose
     * product can silently wrap around in 32-bit arithmetic, resulting in
     * an undersized 'malloc()' while later transfers write to the full extent.
     */
    size_t image_size = (size_t) res_2d->stride * res_2d->height;
    if (res_2d->height && image_size / res_2d->height != res_2d->stride) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): image size overflow (%u x %u)\n",
                __func__, res_2d->width, res_2d->height);
        vgpu_sw_destroy_resource_2d(res_2d);
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        return;
    }

    if (image_size > VGPU_SW_MAX_HOSTMEM ||
        g_vgpu_sw_hostmem > VGPU_SW_MAX_HOSTMEM - image_size) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): image memory limit exceeded (%zu bytes)\n",
                __func__, image_size);
        vgpu_sw_destroy_resource_2d(res_2d);
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        return;
    }

    res_2d->image = calloc(1, image_size);

    /* Failed to create image buffer */
    if (!res_2d->image) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): Failed to allocate image buffer\n",
                __func__);
        vgpu_sw_destroy_resource_2d(res_2d);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }
    res_2d->image_size = image_size;
    g_vgpu_sw_hostmem += image_size;

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static void vgpu_sw_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_res_unref *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_unref));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): failed to destroy resource %d\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Clear any visible plane using this resource before it is freed. */
    for (uint32_t i = 0; i < PRIV(vgpu)->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[i];

        if (!scanout->enabled)
            continue;

        if (scanout->primary_resource_id == request->resource_id) {
            scanout->primary_resource_id = 0;
            scanout->src_x = scanout->src_y = 0;
            scanout->src_w = scanout->src_h = 0;
            vgpu_display_publish_primary_clear(i);
        }

        if (scanout->cursor_resource_id == request->resource_id) {
            scanout->cursor_resource_id = 0;
            vgpu_display_publish_cursor_clear(i);
        }
    }

    vgpu_sw_destroy_resource_2d(res_2d);

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static void vgpu_sw_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                            struct virtq_desc *vq_desc,
                                            uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_set_scanout *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_set_scanout));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_scanout_info *scanout =
        vgpu_sw_get_scanout(vgpu, request->scanout_id);
    if (!scanout) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid scanout id %u\n",
                __func__, request->scanout_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        return;
    }

    /* Keep 'resource_id' 0 unavailable for real resources. The virtio spec
     * explicitly documents 'resource_id = 0' as the 'SET_SCANOUT' disable
     * sentinel.
     * The Linux virtio-gpu driver also allocates guest-generated resource IDs
     * as 'handle + 1', so they are always greater than 0. See
     * 'virtgpu_object.c' for details.
     */
    if (request->resource_id == 0) {
        scanout->primary_resource_id = 0;
        scanout->src_x = scanout->src_y = 0;
        scanout->src_w = scanout->src_h = 0;
        vgpu_display_publish_primary_clear(request->scanout_id);
        goto leave;
    }

    /* Retrieve 2D resource */
    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource id %d\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Validate that the source rectangle fits within the resource without
     * relying on wrapping 32-bit additions.
     */
    if (!vgpu_sw_rect_fits(res_2d->width, res_2d->height, &request->r)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): source rect %u,%u %ux%u exceeds resource %ux%u\n",
                __func__, request->r.x, request->r.y, request->r.width,
                request->r.height, res_2d->width, res_2d->height);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* The source rectangle is displayed into this scanout, view size is bounded
     * by the advertised scanout size.
     */
    if (request->r.width > scanout->width ||
        request->r.height > scanout->height) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): source rect %ux%u exceeds scanout %ux%u\n",
                __func__, request->r.width, request->r.height, scanout->width,
                scanout->height);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* Bind scanout with resource and record the source rectangle */
    scanout->primary_resource_id = res_2d->resource_id;
    scanout->src_x = request->r.x;
    scanout->src_y = request->r.y;
    scanout->src_w = request->r.width;
    scanout->src_h = request->r.height;

leave:
    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static void vgpu_sw_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_res_flush *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_flush));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource id %d\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    if (!vgpu_sw_rect_fits(res_2d->width, res_2d->height, &request->r)) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid flush rect\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* Flush the resource to every scanout currently bound to it, using the
     * source rectangle recorded by 'SET_SCANOUT' to display only the requested
     * sub-region of the resource.
     */
    for (uint32_t i = 0; i < PRIV(vgpu)->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[i];

        if (!scanout->enabled ||
            scanout->primary_resource_id != request->resource_id)
            continue;

        /* Keep the producer non-blocking: if the display queue is full or
         * snapshot allocation fails below, this flush frame for scanout 'i' is
         * dropped and the frontend keeps showing its previous published frame.
         */
        if (!vgpu_display_can_publish())
            continue;

        struct vgpu_display_payload *payload =
            vgpu_sw_create_window_payload(res_2d, scanout, "primary");
        if (!payload)
            continue;

        /* The publish path snapshots the whole 'SET_SCANOUT' view for this
         * scanout. 'request->r' is not used here to further trim the payload
         * for now.
         */
        vgpu_display_publish_primary_set(i, payload);
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static void vgpu_sw_cmd_transfer_to_host_2d_handler(virtio_gpu_state_t *vgpu,
                                                    struct virtq_desc *vq_desc,
                                                    uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_trans_to_host_2d *req = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_trans_to_host_2d));
    if (!req) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(req->resource_id);
    if (!res_2d) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource id %d\n",
                __func__, req->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &req->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Check if backing has been attached */
    if (!res_2d->iovec) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing not attached for resource %d\n",
                __func__, req->resource_id);
        *plen = virtio_gpu_write_ctrl_response(vgpu, &req->hdr, response_desc,
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Validate that the destination rectangle fits within the resource
     * without relying on wrapping 32-bit additions. Mirrors the check in
     * 'vgpu_sw_cmd_set_scanout_handler()'.
     */
    if (!vgpu_sw_rect_fits(res_2d->width, res_2d->height, &req->r)) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid image size\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &req->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    if (!vgpu_sw_transfer_source_fits(req, res_2d)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): transfer source exceeds backing\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &req->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* Transfer frame data from guest to host */
    if (!vgpu_sw_copy_image_from_pages(req, res_2d)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): incomplete transfer from backing\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(vgpu, &req->hdr, response_desc,
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &req->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static void vgpu_sw_cmd_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_res_attach_backing *backing_info = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_attach_backing));
    if (!backing_info) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (vq_desc[1].flags & VIRTIO_DESC_F_WRITE) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing entries descriptor is writable\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &backing_info->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    if (backing_info->nr_entries == 0 ||
        backing_info->nr_entries > VGPU_SW_MAX_BACKING_ENTRIES) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid backing entry count %u\n",
                __func__, backing_info->nr_entries);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &backing_info->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* The entry cap above keeps 'entries_size' small. semu currently targets
     * 64-bit hosts, so this path does not guard for 32-bit host overflow yet.
     */
    size_t entries_size =
        sizeof(struct virtio_gpu_mem_entry) * backing_info->nr_entries;

    if (vq_desc[1].len < entries_size) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing entries descriptor too small\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &backing_info->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    struct virtio_gpu_mem_entry *pages = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[1].addr, (uint32_t) entries_size);
    if (!pages) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(backing_info->resource_id);
    if (!res_2d) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource id %d\n",
                __func__, backing_info->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &backing_info->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Check if backing is already attached */
    if (res_2d->iovec) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing already attached for resource %d\n",
                __func__, backing_info->resource_id);
        *plen = virtio_gpu_write_ctrl_response(vgpu, &backing_info->hdr,
                                               response_desc,
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Dispatch page memories to the 2D resource */
    res_2d->page_cnt = backing_info->nr_entries;
    res_2d->iovec = malloc(sizeof(struct iovec) * backing_info->nr_entries);
    if (!res_2d->iovec) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): failed to allocate io vector\n",
                __func__);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Convert each guest-provided backing entry into one host-side 'iovec'. */
    for (size_t i = 0; i < backing_info->nr_entries; i++) {
        if (pages[i].addr > UINT32_MAX) {
            fprintf(stderr,
                    VIRTIO_GPU_LOG_PREFIX "%s(): page %zu addr_high non-zero\n",
                    __func__, i);
            free(res_2d->iovec);
            res_2d->iovec = NULL;
            res_2d->page_cnt = 0;
            *plen = virtio_gpu_write_ctrl_response(
                vgpu, &backing_info->hdr, response_desc,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
            return;
        }

        /* Attach address and length of i-th page to the 2D resource. */
        res_2d->iovec[i].iov_base = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) pages[i].addr, pages[i].length);
        res_2d->iovec[i].iov_len = pages[i].length;

        /* Corrupted page address */
        if (!res_2d->iovec[i].iov_base) {
            fprintf(stderr,
                    VIRTIO_GPU_LOG_PREFIX "%s(): invalid page address\n",
                    __func__);
            free(res_2d->iovec);
            res_2d->iovec = NULL;
            res_2d->page_cnt = 0;
            *plen = virtio_gpu_write_ctrl_response(
                vgpu, &backing_info->hdr, response_desc,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
            return;
        }
    }

    *plen = virtio_gpu_write_ctrl_response(
        vgpu, &backing_info->hdr, response_desc, VIRTIO_GPU_RESP_OK_NODATA);
}

static void vgpu_sw_cmd_resource_detach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_sw_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_res_detach_backing *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_detach_backing));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(request->resource_id);

    if (!res_2d) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource id %d\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Check if backing exists */
    if (!res_2d->iovec) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): no backing for resource %d\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Detach backing and free the 'iovec' array. */
    free(res_2d->iovec);
    res_2d->iovec = NULL;
    res_2d->page_cnt = 0;

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static int32_t vgpu_sw_decode_cursor_coord(uint32_t coord)
{
    /* Linux passes signed cursor plane 'crtc_x'/'crtc_y' through virtio-gpu's
     * unsigned 32-bit wire fields. Decode that two's-complement value
     * explicitly instead of relying on implementation-defined signed casts:
     * values above 'INT32_MAX' represent negative coordinates, so subtract
     * 2^32 to recover them, e.g. '0xffffffff' -> -1 and '0xfffffffe' -> -2.
     */
    if (coord <= (uint32_t) INT32_MAX)
        return (int32_t) coord;
    return (int32_t) ((int64_t) coord - ((int64_t) UINT32_MAX + 1));
}

static void vgpu_sw_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    struct virtio_gpu_update_cursor *cursor = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_update_cursor));
    if (!cursor) {
        *plen = 0;
        return;
    }

    /* Normal cursor commands have no response descriptor, while fenced cursor
     * commands need a response to echo the fence. Current Linux does not fence
     * this path; see 'virtgpu_vq.c' ('virtio_gpu_queue_cursor()'). Warn only.
     */
    if (cursor->hdr.flags & VIRTIO_GPU_FLAG_FENCE)
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): fenced cursor command is unsupported\n",
                __func__);

    struct virtio_gpu_scanout_info *scanout =
        vgpu_sw_get_scanout(vgpu, cursor->pos.scanout_id);
    if (!scanout) {
        *plen = 0;
        return;
    }

    /* Keep 'resource_id' 0 unavailable for real resources. The virtio spec
     * explicitly documents 'resource_id = 0' as the 'SET_SCANOUT' disable
     * sentinel.
     * The Linux virtio-gpu driver also allocates guest-generated resource IDs
     * as 'handle + 1', so they are always greater than 0. See
     * 'virtgpu_object.c' for details.
     */
    if (cursor->resource_id == 0) {
        scanout->cursor_resource_id = 0;
        vgpu_display_publish_cursor_clear(cursor->pos.scanout_id);
        *plen = 0;
        return;
    }

    /* Update cursor image */
    struct vgpu_sw_resource_2d *res_2d =
        vgpu_sw_get_resource_2d(cursor->resource_id);
    if (!res_2d) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid resource id %d\n",
                __func__, cursor->resource_id);
        *plen = 0;
        return;
    }

    if (res_2d->width == 0 || res_2d->height == 0 ||
        res_2d->width > scanout->width || res_2d->height > scanout->height) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid cursor size %ux%u\n",
                __func__, res_2d->width, res_2d->height);
        *plen = 0;
        return;
    }

    if (!vgpu_display_can_publish()) {
        *plen = 0;
        return;
    }

    struct vgpu_display_payload *payload =
        vgpu_sw_create_window_payload(res_2d, NULL, "cursor");
    if (!payload) {
        *plen = 0;
        return;
    }
    vgpu_display_publish_cursor_set(cursor->pos.scanout_id, payload,
                                    vgpu_sw_decode_cursor_coord(cursor->pos.x),
                                    vgpu_sw_decode_cursor_coord(cursor->pos.y),
                                    cursor->hot_x, cursor->hot_y);
    scanout->cursor_resource_id = cursor->resource_id;

    *plen = 0;
}

static void vgpu_sw_cmd_move_cursor_handler(virtio_gpu_state_t *vgpu,
                                            struct virtq_desc *vq_desc,
                                            uint32_t *plen)
{
    struct virtio_gpu_update_cursor *cursor = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_update_cursor));
    if (!cursor) {
        *plen = 0;
        return;
    }

    /* Normal cursor commands have no response descriptor, while fenced cursor
     * commands need a response to echo the fence. Current Linux does not fence
     * this path; see 'virtgpu_vq.c' ('virtio_gpu_queue_cursor()'). Warn only.
     */
    if (cursor->hdr.flags & VIRTIO_GPU_FLAG_FENCE)
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): fenced cursor command is unsupported\n",
                __func__);

    if (!vgpu_sw_get_scanout(vgpu, cursor->pos.scanout_id)) {
        *plen = 0;
        return;
    }

    /* Move cursor to new position */
    vgpu_display_publish_cursor_move(
        cursor->pos.scanout_id, vgpu_sw_decode_cursor_coord(cursor->pos.x),
        vgpu_sw_decode_cursor_coord(cursor->pos.y));

    *plen = 0;
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_backend = {
    .reset = vgpu_sw_reset,
    .get_display_info = virtio_gpu_get_display_info_handler,
    .resource_create_2d = vgpu_sw_resource_create_2d_handler,
    .resource_unref = vgpu_sw_cmd_resource_unref_handler,
    .set_scanout = vgpu_sw_cmd_set_scanout_handler,
    .resource_flush = vgpu_sw_cmd_resource_flush_handler,
    .transfer_to_host_2d = vgpu_sw_cmd_transfer_to_host_2d_handler,
    .resource_attach_backing = vgpu_sw_cmd_resource_attach_backing_handler,
    .resource_detach_backing = vgpu_sw_cmd_resource_detach_backing_handler,
    .get_capset_info = VIRTIO_GPU_CMD_UNDEF,
    .get_capset = VIRTIO_GPU_CMD_UNDEF,
    .get_edid = virtio_gpu_get_edid_handler,
    .resource_assign_uuid = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_blob = VIRTIO_GPU_CMD_UNDEF,
    .set_scanout_blob = VIRTIO_GPU_CMD_UNDEF,
    .ctx_create = VIRTIO_GPU_CMD_UNDEF,
    .ctx_destroy = VIRTIO_GPU_CMD_UNDEF,
    .ctx_attach_resource = VIRTIO_GPU_CMD_UNDEF,
    .ctx_detach_resource = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_3d = VIRTIO_GPU_CMD_UNDEF,
    .transfer_to_host_3d = VIRTIO_GPU_CMD_UNDEF,
    .transfer_from_host_3d = VIRTIO_GPU_CMD_UNDEF,
    .submit_3d = VIRTIO_GPU_CMD_UNDEF,
    .resource_map_blob = VIRTIO_GPU_CMD_UNDEF,
    .resource_unmap_blob = VIRTIO_GPU_CMD_UNDEF,
    .update_cursor = vgpu_sw_cmd_update_cursor_handler,
    .move_cursor = vgpu_sw_cmd_move_cursor_handler,
};
