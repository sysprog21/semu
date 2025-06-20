#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <virglrenderer.h>

#include "device.h"
#include "utils.h"
#include "virgl.h"
#include "virtio-gpu.h"
#include "virtio.h"
#include "window.h"

/* TODO: envvar */
#define RENDER_NODE "/dev/dri/renderD128"

/* Specify top-left corner as origin */
#define VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP (1 << 0)

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

struct virgl_fence_cmd {
    struct vgpu_ctrl_hdr hdr;
    uint64_t response_addr;
    struct list_head fence_queue;
};

extern const struct window_backend g_window;

static void virgl_detect_fence(virtio_gpu_state_t *vgpu,
                               struct virtq_desc *request_desc,
                               struct virtq_desc *response_desc,
                               struct vgpu_ctrl_hdr *hdr)
{
    if (request_desc->flags & VIRTIO_GPU_FLAG_FENCE) {
        /* Allocate memory for storing fencing command */
        struct virgl_fence_cmd *cmd = malloc(sizeof(struct virgl_fence_cmd));
        if (!cmd) {
            fprintf(stderr, "%s(): failed to allocate fence item\n", __func__);
            virtio_gpu_set_fail(vgpu);
            return;
        }

        memcpy(&cmd->hdr, hdr, sizeof(struct vgpu_ctrl_hdr));
        cmd->response_addr = response_desc->addr;

        /* Insert command into the fence queue */
        struct list_head *g_fence_queue = &(PRIV(vgpu)->virgl_data.fence_queue);
        list_push(&cmd->fence_queue, g_fence_queue);
    }
}

uint32_t semu_virgl_get_num_capsets(void)
{
    uint32_t capset_max_ver, capset_max_size;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2, &capset_max_ver,
                               &capset_max_size);
    return capset_max_ver ? 2 : 1;
}

static void virgl_cmd_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                                 struct virtq_desc *vq_desc,
                                                 uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_create_2d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Resource ID should not be zero */
    if (request->resource_id == 0) {
        fprintf(stderr, "%s(): resource id should not be 0\n", __func__);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[1].addr, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Create 2D resource with virgl renderer */
    struct virgl_renderer_resource_create_args virgl_args = {
        .handle = request->resource_id,
        .target = 2,
        .format = request->format,
        .bind = (1 << 1),
        .width = request->width,
        .height = request->height,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
    };
    virgl_renderer_resource_create(&virgl_args, NULL, 0);

    /* Create 2D resource list entry */
    struct vgpu_resource_2d *res_2d =
        vgpu_create_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): failed to allocate 2D resource list entry\n",
                __func__);
        return;
    }

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);

    /* Handle fencing flag */
    virtio_gpu_set_response_fencing(vgpu, &request->hdr, vq_desc[1].addr);
}

static void virgl_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_unref *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Detach the iovec array from the resource */
    struct iovec *res_iovec = NULL;
    int iovec_num = 0;
    virgl_renderer_resource_detach_iov(request->resource_id, &res_iovec,
                                       &iovec_num);

    /* Unreference the resource */
    virgl_renderer_resource_unref(request->resource_id);

    /* Destroy 2D resource list entry*/
    int result = vgpu_destory_resource_2d(request->resource_id);
    if (result != 0) {
        fprintf(stderr, "%s(): failed to destroy resource list entry\n",
                __func__);
        *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                          VIRTIO_GPU_RESP_ERR_UNSPEC);
        return;
    }

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    /* Read request */
    struct vgpu_set_scanout *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Resource ID 0 for stop displaying */
    if (request->resource_id == 0) {
        g_window.window_clear(request->scanout_id);
        goto leave;
    }

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Get resource information */
    struct virgl_renderer_resource_info res_info;
    memset(&res_info, 0, sizeof(res_info));
    int ret = virgl_renderer_resource_get_info(request->resource_id, &res_info);
    if (ret) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[1].addr, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Set scanout */
    int scanout_id = request->scanout_id;
    g_window.window_set_scanout(scanout_id, res_info.tex_id);

    /* Bind scanout with resource */
    struct vgpu_resource_2d *res_2d =
        vgpu_get_resource_2d(request->resource_id);
    res_2d->scanout_id = request->scanout_id;

leave:
    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_flush *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Retrieve resource data */
    struct vgpu_resource_2d *res_2d =
        vgpu_get_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__,
                request->resource_id);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[1].addr, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    /* Retreive scanout ID from the resource list */
    int scanout_id = res_2d->scanout_id;

    /* Trigger display window flushing */
    g_window.window_flush(scanout_id, request->resource_id);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_transfer_to_host_2d_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Read request */
    struct vgpu_trans_to_host_2d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Transfer to host with virglrenderer */
    struct virtio_gpu_box box = {
        .x = request->r.x,
        .y = request->r.y,
        .z = 0,
        .w = request->r.width,
        .h = request->r.height,
        .d = 1,
    };
    virgl_renderer_transfer_write_iov(request->resource_id, 0, 0, 0, 0,
                                      (struct virgl_box *) &box,
                                      request->offset, NULL, 0);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);

    /* Handle fencing flag */
    virtio_gpu_set_response_fencing(vgpu, &request->hdr, vq_desc[1].addr);
}

static void virgl_cmd_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_attach_backing *backing_info =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);
    struct vgpu_mem_entry *pages =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[2], &backing_info->hdr);

    /* Dispatch page memories to the 2D resource */
    struct iovec *iovec =
        malloc(sizeof(struct iovec) * backing_info->nr_entries);
    if (!iovec) {
        fprintf(stderr, "%s(): failed to allocate io vector\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    struct vgpu_mem_entry *mem_entries = (struct vgpu_mem_entry *) pages;
    for (size_t i = 0; i < backing_info->nr_entries; i++) {
        /* Attach address and length of i-th page to the 2D resource */
        iovec[i].iov_base = vgpu_mem_host_to_guest(vgpu, mem_entries[i].addr);
        iovec[i].iov_len = mem_entries[i].length;

        /* Corrupted page address */
        if (!iovec[i].iov_base) {
            fprintf(stderr, "%s(): invalid page address\n", __func__);
            *plen = virtio_gpu_write_response(vgpu, vq_desc[2].addr,
                                              VIRTIO_GPU_RESP_ERR_UNSPEC);
            return;
        }
    }

    /* Attach resource with virl renderer */
    int ret = virgl_renderer_resource_attach_iov(
        backing_info->resource_id, iovec, backing_info->nr_entries);

    /* Clean up if virgl renderer failed to attach the resource */
    if (ret) {
        fprintf(stderr, "%s(): failed to attach resource\n", __func__);
        free(iovec);
    }

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[2].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);

    /* Handle fencing flag */
    virtio_gpu_set_response_fencing(vgpu, &backing_info->hdr, vq_desc[2].addr);
}

static void virgl_cmd_resource_detach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_attach_backing *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Detach the iovec array from the resource */
    struct iovec *res_iovec = NULL;
    int iovec_num = 0;
    virgl_renderer_resource_detach_iov(request->resource_id, &res_iovec,
                                       &iovec_num);

    /* Free the iovec array */
    free(res_iovec);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    /* Read request */
    struct vgpu_get_capset_info *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Response capability info */
    struct vgpu_resp_capset_info *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    uint32_t capset_max_version, capset_max_size;

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;

    if (request->capset_index == 0) {
        response->capset_id = VIRTIO_GPU_CAPSET_VIRGL;
        virgl_renderer_get_cap_set(response->capset_id, &capset_max_version,
                                   &capset_max_size);
        response->capset_max_version = capset_max_version;
        response->capset_max_size = capset_max_size;
    } else if (request->capset_index == 1) {
        response->capset_id = VIRTIO_GPU_CAPSET_VIRGL2;
        virgl_renderer_get_cap_set(response->capset_id, &capset_max_version,
                                   &capset_max_size);
        response->capset_max_version = capset_max_version;
        response->capset_max_size = capset_max_size;
    } else {
        response->capset_max_version = 0;
        response->capset_max_size = 0;
    }

    /* Update write length */
    *plen = sizeof(*response);
}

static void virgl_get_capset_handler(virtio_gpu_state_t *vgpu,
                                     struct virtq_desc *vq_desc,
                                     uint32_t *plen)
{
    /* Read request */
    struct vgpu_get_capset *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Get capset information */
    uint32_t max_ver, max_size;
    virgl_renderer_get_cap_set(request->capset_id, &max_ver, &max_size);

    if (!max_size) {
        fprintf(stderr, "%s(): invalid capset id\n", __func__);
        virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Response capability set */
    struct virtio_gpu_resp_capset *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(request->capset_id, request->capset_version,
                             (void *) response->capset_data);

    /* Update write length */
    *plen = sizeof(*response) + max_size;
}

static void virgl_cmd_ctx_create_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_create *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Create 3D context with VirGL */
    virgl_renderer_context_create(request->hdr.ctx_id, request->nlen,
                                  request->debug_name);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_destroy *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Destroy 3D context with virglrenderer */
    virgl_renderer_context_destroy(request->hdr.ctx_id);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_attach_resource_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_resource *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Attach 3D context to resource with virglrenderer */
    virgl_renderer_ctx_attach_resource(request->hdr.ctx_id,
                                       request->resource_id);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_detach_resource_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_resource *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Detach 3D context and resource with virglrenderer */
    virgl_renderer_ctx_detach_resource(request->hdr.ctx_id,
                                       request->resource_id);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_resource_create_3d_handler(virtio_gpu_state_t *vgpu,
                                                 struct virtq_desc *vq_desc,
                                                 uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_resource_create_3d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Create 3D resource with VirGL */
    struct virgl_renderer_resource_create_args args = {
        .handle = request->resource_id,
        .target = request->target,
        .format = request->format,
        .bind = request->bind,
        .width = request->width,
        .height = request->height,
        .depth = request->depth,
        .array_size = request->array_size,
        .last_level = request->last_level,
        .nr_samples = request->nr_samples,
        .flags = request->flags,
    };
    virgl_renderer_resource_create(&args, NULL, 0);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_transfer_to_host_3d_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_transfer_host_3d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Transfer data from target to host with VirGL */
    virgl_renderer_transfer_write_iov(
        request->resource_id, request->hdr.ctx_id, request->level,
        request->stride, request->layer_stride,
        (struct virgl_box *) &request->box, request->offset, NULL, 0);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_transfer_from_host_3d_handler(virtio_gpu_state_t *vgpu,
                                                    struct virtq_desc *vq_desc,
                                                    uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_transfer_host_3d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Transfer data from host to target with virglrenderer */
    virgl_renderer_transfer_read_iov(
        request->resource_id, request->hdr.ctx_id, request->level,
        request->stride, request->layer_stride,
        (struct virgl_box *) &request->box, request->offset, NULL, 0);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                        struct virtq_desc *vq_desc,
                                        uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_cmd_submit *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    void *buffer = malloc(request->size);
    if (!buffer) {
        fprintf(stderr, "%s(): failed to allocate command buffer\n", __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Copy 3D commands from the iovec list to a contiguous buffer */
    // iov_to_buf(, , sizeof(*request), buffer, request->size); /* TODO */

    /* Submit 3D command with VirGL */
    virgl_renderer_submit_cmd(buffer, request->hdr.ctx_id, request->size / 4);

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                            struct virtq_desc *vq_desc,
                                            uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_update_cursor *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Update cursor */
    g_window.cursor_update(request->pos.scanout_id, request->resource_id,
                           request->pos.x, request->pos.y);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static void virgl_cmd_move_cursor_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_update_cursor *cursor =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Move cursor to the new position */
    g_window.cursor_move(cursor->pos.scanout_id, cursor->pos.x, cursor->pos.y);

    /* Response OK */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static int virgl_get_drm_fd(void *opaque)
{
    (void) opaque;

    printf("virglrenderer: open %s\n", RENDER_NODE);

    int drm_fd = open(RENDER_NODE, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "%s(): failed to open %s: %s\n", __func__, RENDER_NODE,
                strerror(errno));
        return -1;
    }

    return drm_fd;
}

static void virgl_write_fence(void *opaque, uint32_t fence)
{
    printf("%s() called\n", __func__);

    /* Get fence queue */
    virtio_gpu_state_t *vgpu = (virtio_gpu_state_t *) opaque;
    struct list_head *fence_queue = &(PRIV(vgpu)->virgl_data.fence_queue);

    /* Interate through the fence queue */
    struct list_head *curr, *next;
    list_for_each_safe (curr, next, fence_queue) {
        /* Get fenced command */
        struct virgl_fence_cmd *cmd =
            list_entry(curr, struct virgl_fence_cmd, fence_queue);

        /* Ignore commands that have higher ID */
        if (cmd->hdr.fence_id > fence)
            continue;

        /* Write response */
        virtio_gpu_write_response(vgpu, cmd->response_addr,
                                  VIRTIO_GPU_RESP_OK_NODATA);

        /* Remove fenced command */
        list_del(&cmd->fence_queue);
        free(cmd);
    }
}

static virgl_renderer_gl_context virgl_create_context(
    void *opaque,
    int scanout_id,
    struct virgl_renderer_gl_ctx_param *params)
{
    (void) opaque;
    printf("virgl_create_context called\n");
    return sdl_create_context(scanout_id, params);
}

static void virgl_destroy_context(void *opaque, virgl_renderer_gl_context ctx)
{
    (void) opaque;
    printf("virgl_destroy_context called\n");
    sdl_destroy_context(ctx);
}

static int virgl_make_context_current(void *opaque,
                                      int scanout_id,
                                      virgl_renderer_gl_context ctx)
{
    (void) opaque;
    printf("virgl_make_context_current called\n");
    return sdl_make_context_current(scanout_id, ctx);
}

static struct virgl_renderer_callbacks virgl_callbacks = {
    .get_drm_fd = virgl_get_drm_fd,
    .version = 1,
    .write_fence = virgl_write_fence,
    .create_gl_context = virgl_create_context,
    .destroy_gl_context = virgl_destroy_context,
    .make_current = virgl_make_context_current,
};

void semu_virgl_init(virtio_gpu_state_t *vgpu)
{
    /* Initialize fence queue */
    struct list_head *fence_queue = &(PRIV(vgpu)->virgl_data.fence_queue);
    INIT_LIST_HEAD(fence_queue);

    /* Initialize virglrenderer */
    int flags = VIRGL_RENDERER_THREAD_SYNC /*| VIRGL_RENDERER_RENDER_SERVER*/;
    int ret = virgl_renderer_init(vgpu, flags, &virgl_callbacks);

    if (ret) {
        fprintf(stderr, "%s(): failed to initialize virgl renderer: %s\n",
                __func__, strerror(ret));
        exit(2);
    }
}

#if 1
/* FIXME: Refactor the code here later */
#include <sys/time.h>

#define VIRGL_FENCE_POLL_HZ 100

/* FIXME: Foating point is inefficient */
static double get_time_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (double) tv.tv_sec + (double) tv.tv_usec * 1e-6;
}

void semu_virgl_fence_poll(void)
{
    static bool ready = false;

    static double timer = 0.0;
    static double period = 1.0 / (double) VIRGL_FENCE_POLL_HZ;

    if (ready == false) {
        ready = true;
        timer = get_time_sec();
        return;
    }

    double now = get_time_sec();
    double elapsed = now - timer;
    if (elapsed >= period) {
        timer = now;
        virgl_renderer_poll();
    }
}
#endif

const struct vgpu_cmd_backend g_vgpu_backend = {
    .get_display_info = virtio_gpu_get_display_info_handler,
    .resource_create_2d = virgl_cmd_resource_create_2d_handler,
    .resource_unref = virgl_cmd_resource_unref_handler,
    .set_scanout = virgl_cmd_set_scanout_handler,
    .resource_flush = virgl_cmd_resource_flush_handler,
    .trasfer_to_host_2d = virgl_cmd_transfer_to_host_2d_handler,
    .resource_attach_backing = virgl_cmd_resource_attach_backing_handler,
    .resource_detach_backing = virgl_cmd_resource_detach_backing_handler,
    .get_capset_info = virgl_cmd_get_capset_info_handler,
    .get_capset = virgl_get_capset_handler,
    .get_edid = virtio_gpu_get_edid_handler,
    .resource_assign_uuid = VGPU_CMD_UNDEF,
    .resource_create_blob = VGPU_CMD_UNDEF,
    .set_scanout_blob = VGPU_CMD_UNDEF,
    .ctx_create = virgl_cmd_ctx_create_handler,
    .ctx_destroy = virgl_cmd_ctx_destroy_handler,
    .ctx_attach_resource = virgl_cmd_attach_resource_handler,
    .ctx_detach_resource = virgl_cmd_detach_resource_handler,
    .resource_create_3d = virgl_cmd_resource_create_3d_handler,
    .transfer_to_host_3d = virgl_cmd_transfer_to_host_3d_handler,
    .transfer_from_host_3d = virgl_cmd_transfer_from_host_3d_handler,
    .submit_3d = virgl_cmd_submit_3d_handler,
    .resource_map_blob = VGPU_CMD_UNDEF,
    .resource_unmap_blob = VGPU_CMD_UNDEF,
    .update_cursor = virgl_cmd_update_cursor_handler,
    .move_cursor = virgl_cmd_move_cursor_handler,
};
