#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <virglrenderer.h>

#include "device.h"
#include "list.h"
#include "virgl.h"
#include "virtio-gpu.h"
#include "virtio.h"
#include "window.h"

/* TODO: envvar */
#define RENDER_NODE "/dev/dri/renderD128"

/* TODO: Copied from the QEMU, check for the source */
#define VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP (1 << 0)

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

struct virgl_fence_cmd {
    struct vgpu_ctrl_hdr hdr;
    uint64_t response_addr;
    struct list_head fence_queue;
};

static void virgl_detect_fence(virtio_gpu_state_t *vgpu,
                               struct virtq_desc *request_desc,
                               struct virtq_desc *response_desc,
                               struct vgpu_ctrl_hdr *hdr)
{
    if (request_desc->flags &= VIRTIO_GPU_FLAG_FENCE) {
        /* Allocate memory for storing fencing command */
        struct virgl_fence_cmd *cmd = malloc(sizeof(struct virgl_fence_cmd));
        memcpy(&cmd->hdr, hdr, sizeof(struct vgpu_ctrl_hdr));
        cmd->response_addr = response_desc->addr;

        /* Insert command into the fence queue */
        struct list_head *g_fence_queue = &(PRIV(vgpu)->virgl_data.fence_queue);
        list_add(&cmd->fence_queue, g_fence_queue);
    }
}

void virgl_cmd_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_create_2d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    if (request->resource_id == 0) {
        fprintf(stderr, "%s: resource id should not be 0\n", __func__);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[1].addr, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        virtio_gpu_set_fail(vgpu);
        return;
    }

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
        create_vgpu_resource_2d(request->resource_id);
    if (!res_2d) {
        fprintf(stderr, "%s: failed to allocate 2D resource list entry\n",
                __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
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
    int result = destroy_vgpu_resource_2d(request->resource_id);
    if (result != 0) {
        fprintf(stderr, "%s: failed to destroy 2D resource list entry\n",
                __func__);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen)
{
    /* Read request */
    struct vgpu_set_scanout *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Resource ID 0 for stop displaying */
    if (request->resource_id == 0)
        goto leave;

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Get extended resource information */
    struct virgl_renderer_resource_info_ext res_info_ext;
    memset(&res_info_ext, 0, sizeof(res_info_ext));
    int ret = virgl_renderer_resource_get_info_ext(request->resource_id,
                                                   &res_info_ext);
    if (ret) {
        fprintf(stderr, "%s: invalid resource id\n", __func__);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[1].addr, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Get GL texture */
    struct virgl_renderer_resource_info res_info;
    memset(&res_info, 0, sizeof(res_info));
    res_info = res_info_ext.base;

    /* Setup GL scanout */
    int scanout_id = request->scanout_id;
    window_lock(scanout_id);
    setup_scanout_gl(scanout_id, res_info.tex_id);
    printf("###res_info.tex_id = %d\n", res_info.tex_id);
    window_unlock(scanout_id);

    /* Bind resource ID and scanout ID information */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(request->resource_id);
    res_2d->scanout_id = request->scanout_id;
    res_2d->scanout_attached = true;

leave:
    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_flush *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Obtain scanout ID from resource list */
    struct vgpu_resource_2d *res_2d =
        acquire_vgpu_resource_2d(request->resource_id);
    int scanout_id = res_2d->scanout_id;

    /* Trigger display window rendering */
    window_lock(scanout_id);
    window_render_gl(scanout_id);
    window_unlock(scanout_id);

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_transfer_to_host_2d_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_resource_attach_backing_handler(virtio_gpu_state_t *vgpu,
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

    /* Dispatch page memories to the 2D resource (TODO: new function) */
    struct iovec *res_iovec =
        malloc(sizeof(struct iovec) * backing_info->nr_entries);
    struct vgpu_mem_entry *mem_entries = (struct vgpu_mem_entry *) pages;

    for (size_t i = 0; i < backing_info->nr_entries; i++) {
        /* Attach address and length of i-th page to the 2D resource */
        res_iovec[i].iov_base =
            vgpu_mem_host_to_guest(vgpu, mem_entries[i].addr);
        res_iovec[i].iov_len = mem_entries[i].length;

        /* Corrupted page address */
        if (!res_iovec[i].iov_base) {
            fprintf(stderr, "%s(): Invalid page address\n", __func__);
            virtio_gpu_set_fail(vgpu);
            return;
        }
    }

    /* Attach resource with virl renderer */
    int ret = virgl_renderer_resource_attach_iov(
        backing_info->resource_id, res_iovec, backing_info->nr_entries);

    /* Clean up if virgl renderer failed to attach the resource */
    if (ret) {
        fprintf(stderr, "%s: failed to attach resource\n", __func__);
        free(res_iovec);
    }

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[2].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_resource_detach_backing_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen)
{
    /* Read request */
    struct vgpu_get_capset_info *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Write capability info */
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

void virgl_get_capset_handler(virtio_gpu_state_t *vgpu,
                              struct virtq_desc *vq_desc,
                              uint32_t *plen)
{
    /* Read request */
    struct vgpu_get_capset *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Get capset information */
    uint32_t max_ver, max_size;
    virgl_renderer_get_cap_set(request->capset_id, &max_ver, &max_size);

    if (!max_size) {
        fprintf(stderr, "%s: invalid capset id\n", __func__);
        virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        virtio_gpu_set_fail(vgpu);
    }

    /* Write capability set */
    struct virtio_gpu_resp_capset *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(request->capset_id, request->capset_version,
                             (void *) response->capset_data);

    /* Update write length */
    *plen = sizeof(*response) + max_size;
}

void virgl_cmd_ctx_create_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_destroy *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Destroy 3D context with VirGL */
    virgl_renderer_context_destroy(request->hdr.ctx_id);

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_attach_resource_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_detach_resource_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_resource_create_3d_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_transfer_to_host_3d_handler(virtio_gpu_state_t *vgpu,
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

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_transfer_from_host_3d_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_transfer_host_3d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Transfer data from host to target with VirGL */
    virgl_renderer_transfer_read_iov(
        request->resource_id, request->hdr.ctx_id, request->level,
        request->stride, request->layer_stride,
        (struct virgl_box *) &request->box, request->offset, NULL, 0);

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_cmd_submit *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* TODO: Copy commands from iovec list to the buffer */
    void *buffer = malloc(request->size);
    if (!buffer) {
        fprintf(stderr, "malloc failed\n"); /* TODO */
    }

    /* Copy 3D commands from the iovec list to a contiguous buffer */
    // copy_iov_to_buf(, , sizeof(*request), buffer, request->size); /* TODO */

    /* Submit 3D command with VirGL */
    virgl_renderer_submit_cmd(buffer, request->hdr.ctx_id, request->size / 4);

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

void virgl_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                     struct virtq_desc *vq_desc,
                                     uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_update_cursor *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detect command fencing */
    virgl_detect_fence(vgpu, &vq_desc[0], &vq_desc[1], &request->hdr);

    /* Get cursor data */
    uint32_t width = CURSOR_WIDTH, height = CURSOR_HEIGHT;
    uint32_t *cursor_data =
        virgl_renderer_get_cursor_data(request->resource_id, &width, &height);

    if (width != 64 || height != 64 || !cursor_data) {
        fprintf(stderr, "%s: invalid cursor data\n", __func__);
        *plen = virtio_gpu_write_response(
            vgpu, vq_desc[1].addr, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    /* TODO: SDL */
    // Check sdl_mouse_warp from qemu
    // SDL_SetCursor
    // SDL_WarpMouseInWindow

    /* Write response */
    *plen = virtio_gpu_write_response(vgpu, vq_desc[1].addr,
                                      VIRTIO_GPU_RESP_OK_NODATA);
}

static int virgl_get_drm_fd(void *opaque)
{
    (void) opaque;

    printf("virglrenderer: open %s\n", RENDER_NODE);

    int drm_fd = open(RENDER_NODE, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", RENDER_NODE,
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

static struct virgl_renderer_callbacks virgl_callbacks = {
    .get_drm_fd = virgl_get_drm_fd,
    .version = 2,
    .write_fence = virgl_write_fence,
};

void semu_virgl_init(virtio_gpu_state_t *vgpu)
{
    /* Initialize fence queue */
    struct list_head *fence_queue = &(PRIV(vgpu)->virgl_data.fence_queue);
    INIT_LIST_HEAD(fence_queue);

    /* Initialize virglrenderer */
    int flags = VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_THREAD_SYNC;
    int ret = virgl_renderer_init(vgpu, flags, &virgl_callbacks);
    if (ret) {
        fprintf(stderr, "failed to initialize virgl renderer: %s\n",
                strerror(ret));
        exit(2);
    }
}
