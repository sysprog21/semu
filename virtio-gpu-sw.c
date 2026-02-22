#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "virtio-gpu.h"
#include "virtio.h"
#include "window.h"

extern const struct window_backend g_window;

static void virtio_gpu_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_res_create_2d)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_res_create_2d *request = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_res_create_2d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Resource ID should not be zero */
    if (request->resource_id == 0) {
        fprintf(stderr, "%s(): resource id should not be 0\n", __func__);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Create 2D resource */
    struct vgpu_resource_2d *res_2d =
        vgpu_create_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): failed to allocate new resource\n", __func__);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Select image formats */
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
        fprintf(stderr, "%s(): unsupported format %d\n", __func__,
                request->format);
        vgpu_destroy_resource_2d(request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* Set 2D resource */
    res_2d->width = request->width;
    res_2d->height = request->height;
    res_2d->format = request->format;
    res_2d->bits_per_pixel = bits_per_pixel;
    res_2d->stride =
        ((request->width * bits_per_pixel + 0x1f) >> 5) * sizeof(uint32_t);

    /* Guard against integer overflow in image buffer allocation.
     * Both stride and height are guest-controlled uint32_t values whose
     * product can silently wrap around in 32-bit arithmetic, resulting in
     * an undersized malloc while later transfers write to the full extent. */
    size_t image_size = (size_t) res_2d->stride * request->height;
    if (request->height && image_size / request->height != res_2d->stride) {
        fprintf(stderr, "%s(): image size overflow (%u x %u)\n", __func__,
                request->width, request->height);
        vgpu_destroy_resource_2d(request->resource_id);
        *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                          vq_desc[resp_idx].len,
                                          VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        return;
    }
    res_2d->image = malloc(image_size);

    /* Failed to create image buffer */
    if (!res_2d->image) {
        fprintf(stderr, "%s(): Failed to allocate image buffer\n", __func__);
        vgpu_destroy_resource_2d(request->resource_id);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);

    /* Handle fencing flag */
    virtio_gpu_set_response_fencing(vgpu, &request->hdr, vq_desc[resp_idx].addr,
                                    vq_desc[resp_idx].len);
}

static void virtio_gpu_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_res_unref)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_res_unref *request = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_res_unref));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Destroy 2D resource */
    int result = vgpu_destroy_resource_2d(request->resource_id);
    if (result) {
        fprintf(stderr, "%s(): failed to destroy resource %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virtio_gpu_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_set_scanout)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_set_scanout *request = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_set_scanout));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Validate scanout ID */
    if (request->scanout_id >= VIRTIO_GPU_MAX_SCANOUTS) {
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        return;
    }

    /* Resource ID 0 to stop displaying */
    if (request->resource_id == 0) {
        g_window.window_clear(request->scanout_id);
        goto leave;
    }

    /* Retrieve 2D resource */
    struct vgpu_resource_2d *res_2d =
        vgpu_get_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Bind scanout with resource */
    res_2d->scanout_id = request->scanout_id;

leave:
    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virtio_gpu_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_res_flush)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_res_flush *request = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_res_flush));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_resource_2d *res_2d =
        vgpu_get_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Flush display context */
    g_window.window_flush(res_2d->scanout_id, request->resource_id);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);
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

    /* Copy image by row */
    for (uint32_t h = 0; h < height; h++) {
        /* Note that source offset is in the image coordinate. The address to
         * copy from is the page base address plus with the offset
         */
        size_t src_offset = req->offset + stride * h;
        size_t dest_offset = (req->r.y + h) * stride + (req->r.x * bpp);
        void *dest = (void *) ((uintptr_t) img_data + dest_offset);
        size_t total = width * bpp;

        iov_to_buf(res_2d->iovec, res_2d->page_cnt, src_offset, dest, total);
    }
}

static void virtio_gpu_cursor_image_copy(struct vgpu_resource_2d *res_2d)
{
    iov_to_buf(res_2d->iovec, res_2d->page_cnt, 0, res_2d->image,
               (size_t) res_2d->stride * res_2d->height);
}

static void virtio_gpu_cmd_transfer_to_host_2d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_trans_to_host_2d)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_trans_to_host_2d *req = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_trans_to_host_2d));
    if (!req) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_resource_2d *res_2d = vgpu_get_resource_2d(req->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                req->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Check if backing has been attached */
    if (!res_2d->iovec) {
        fprintf(stderr, "%s(): backing not attached for resource %d\n",
                __func__, req->resource_id);
        *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                          vq_desc[resp_idx].len,
                                          VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Check image boundary */
    if (req->r.x > res_2d->width || req->r.y > res_2d->height ||
        req->r.width > res_2d->width || req->r.height > res_2d->height ||
        req->r.x + req->r.width > res_2d->width ||
        req->r.y + req->r.height > res_2d->height) {
        fprintf(stderr, "%s(): invalid image size\n", __func__);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    uint32_t width =
        (req->r.width < res_2d->width) ? req->r.width : res_2d->width;
    uint32_t height =
        (req->r.height < res_2d->height) ? req->r.height : res_2d->height;

    /* Transfer frame data from guest to host.
     *
     * TODO: The cursor detection heuristic based on 64x64 dimensions is
     * fragile. A regular resource transferring a 64x64 sub-region would
     * incorrectly take the cursor fast-path. Consider tagging cursor resources
     * explicitly if this becomes an issue.
     */
    if (width == CURSOR_WIDTH && height == CURSOR_HEIGHT)
        virtio_gpu_cursor_image_copy(res_2d);
    else
        virtio_gpu_copy_image_from_pages(req, res_2d);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);

    /* Handle fencing flag */
    virtio_gpu_set_response_fencing(vgpu, &req->hdr, vq_desc[resp_idx].addr,
                                    vq_desc[resp_idx].len);
}

static void virtio_gpu_cmd_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_res_attach_backing)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_res_attach_backing *backing_info = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_res_attach_backing));
    if (!backing_info) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (vq_desc[1].len <
        sizeof(struct vgpu_mem_entry) * backing_info->nr_entries) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_mem_entry *pages = vgpu_mem_guest_to_host(
        vgpu, vq_desc[1].addr,
        sizeof(struct vgpu_mem_entry) * backing_info->nr_entries);
    if (!pages) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_resource_2d *res_2d =
        vgpu_get_resource_2d(backing_info->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                backing_info->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Check if backing is already attached */
    if (res_2d->iovec) {
        fprintf(stderr, "%s(): backing already attached for resource %d\n",
                __func__, backing_info->resource_id);
        *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                          vq_desc[resp_idx].len,
                                          VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Dispatch page memories to the 2D resource */
    res_2d->page_cnt = backing_info->nr_entries;
    res_2d->iovec = malloc(sizeof(struct iovec) * backing_info->nr_entries);
    if (!res_2d->iovec) {
        fprintf(stderr, "%s(): failed to allocate io vector\n", __func__);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_mem_entry *mem_entries = (struct vgpu_mem_entry *) pages;
    for (size_t i = 0; i < backing_info->nr_entries; i++) {
        /* Attach address and length of i-th page to the 2D resource */
        res_2d->iovec[i].iov_base = vgpu_mem_guest_to_host(
            vgpu, mem_entries[i].addr, mem_entries[i].length);
        res_2d->iovec[i].iov_len = mem_entries[i].length;

        /* Corrupted page address */
        if (!res_2d->iovec[i].iov_base) {
            fprintf(stderr, "%s(): invalid page address\n", __func__);
            free(res_2d->iovec);
            res_2d->iovec = NULL;
            res_2d->page_cnt = 0;
            *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                              vq_desc[resp_idx].len,
                                              VIRTIO_GPU_RESP_ERR_UNSPEC);
            return;
        }
    }

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);

    /* Handle fencing flag */
    virtio_gpu_set_response_fencing(vgpu, &backing_info->hdr,
                                    vq_desc[resp_idx].addr,
                                    vq_desc[resp_idx].len);
}

static void virtio_gpu_cmd_resource_detach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Check if there's a writable response descriptor */
    int resp_idx = virtio_gpu_find_response_desc(vq_desc, 3);
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    /* Read request */
    if (vq_desc[0].len < sizeof(struct vgpu_res_detach_backing)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_res_detach_backing *request = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct vgpu_res_detach_backing));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Retrieve 2D resource */
    struct vgpu_resource_2d *res_2d =
        vgpu_get_resource_2d(request->resource_id);

    if (!res_2d) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[resp_idx].addr, vq_desc[resp_idx].len,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Check if backing exists */
    if (!res_2d->iovec) {
        fprintf(stderr, "%s(): no backing for resource %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                          vq_desc[resp_idx].len,
                                          VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Detach backing - free iovec array */
    free(res_2d->iovec);
    res_2d->iovec = NULL;
    res_2d->page_cnt = 0;

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[resp_idx].addr,
                                      vq_desc[resp_idx].len,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virtio_gpu_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                                 struct virtq_desc *vq_desc,
                                                 uint32_t *plen)
{
    /* Read request */
    if (vq_desc[0].len < sizeof(struct virtio_gpu_update_cursor)) {
        *plen = 0;
        return;
    }

    struct virtio_gpu_update_cursor *cursor = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct virtio_gpu_update_cursor));
    if (!cursor) {
        *plen = 0;
        return;
    }

    /* Validate scanout ID */
    if (cursor->pos.scanout_id >= VIRTIO_GPU_MAX_SCANOUTS) {
        *plen = 0;
        return;
    }

    /* Update cursor image */
    struct vgpu_resource_2d *res_2d = vgpu_get_resource_2d(cursor->resource_id);

    if (res_2d) {
        g_window.cursor_update(cursor->pos.scanout_id, cursor->resource_id,
                               cursor->pos.x, cursor->pos.y);
    } else if (cursor->resource_id == 0) {
        g_window.cursor_clear(cursor->pos.scanout_id);
    } else {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                cursor->resource_id);
    }

    /* Cursor commands use only 1 descriptor and do NOT have a response
     * descriptor. The device should return with len=0.
     */
    *plen = 0;
}

static void virtio_gpu_cmd_move_cursor_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    /* Read request */
    if (vq_desc[0].len < sizeof(struct virtio_gpu_update_cursor)) {
        *plen = 0;
        return;
    }

    struct virtio_gpu_update_cursor *cursor = vgpu_mem_guest_to_host(
        vgpu, vq_desc[0].addr, sizeof(struct virtio_gpu_update_cursor));
    if (!cursor) {
        *plen = 0;
        return;
    }

    /* Validate scanout ID */
    if (cursor->pos.scanout_id >= VIRTIO_GPU_MAX_SCANOUTS) {
        *plen = 0;
        return;
    }

    /* Move cursor to new position */
    g_window.cursor_move(cursor->pos.scanout_id, cursor->pos.x, cursor->pos.y);

    /* Cursor commands use only 1 descriptor and do NOT have a response
     * descriptor. The device should return with len=0.
     */
    *plen = 0;
}

const struct vgpu_cmd_backend g_vgpu_backend = {
    .get_display_info = virtio_gpu_get_display_info_handler,
    .resource_create_2d = virtio_gpu_resource_create_2d_handler,
    .resource_unref = virtio_gpu_cmd_resource_unref_handler,
    .set_scanout = virtio_gpu_cmd_set_scanout_handler,
    .resource_flush = virtio_gpu_cmd_resource_flush_handler,
    .transfer_to_host_2d = virtio_gpu_cmd_transfer_to_host_2d_handler,
    .resource_attach_backing = virtio_gpu_cmd_resource_attach_backing_handler,
    .resource_detach_backing = virtio_gpu_cmd_resource_detach_backing_handler,
    .get_capset_info = VGPU_CMD_UNDEF,
    .get_capset = VGPU_CMD_UNDEF,
    .get_edid = virtio_gpu_get_edid_handler,
    .resource_assign_uuid = VGPU_CMD_UNDEF,
    .resource_create_blob = VGPU_CMD_UNDEF,
    .set_scanout_blob = VGPU_CMD_UNDEF,
    .ctx_create = VGPU_CMD_UNDEF,
    .ctx_destroy = VGPU_CMD_UNDEF,
    .ctx_attach_resource = VGPU_CMD_UNDEF,
    .ctx_detach_resource = VGPU_CMD_UNDEF,
    .resource_create_3d = VGPU_CMD_UNDEF,
    .transfer_to_host_3d = VGPU_CMD_UNDEF,
    .transfer_from_host_3d = VGPU_CMD_UNDEF,
    .submit_3d = VGPU_CMD_UNDEF,
    .resource_map_blob = VGPU_CMD_UNDEF,
    .resource_unmap_blob = VGPU_CMD_UNDEF,
    .update_cursor = virtio_gpu_cmd_update_cursor_handler,
    .move_cursor = virtio_gpu_cmd_move_cursor_handler,
};
