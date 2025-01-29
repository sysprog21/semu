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
#include "virtio-gpu.h"
#include "virtio.h"
#include "window.h"

/* TODO: envvar */
#define RENDER_NODE "/dev/dri/renderD128"

/* TODO: Copied from the QEMU, check for the source */
#define VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP (1 << 0)
void virgl_cmd_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_create_2d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

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

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* XXX: Response with fencing flag if needed */
    // virtio_gpu_set_response_fencing(&request->hdr, response);

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_unref *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detach the iovec array from the resource */
    struct iovec *res_iovec = NULL;
    int iovec_num = 0;
    virgl_renderer_resource_detach_iov(request->resource_id, &iovec_num,
                                       &res_iovec);

    /* Free the iovec array */
    if (res_iovec && iovec_num)
        free(res_iovec);

    /* Unreference the resource */
    virgl_renderer_resource_unref(request->resource_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(struct vgpu_ctrl_hdr));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen)
{
}

void virgl_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_flush *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Trigger display window rendering */
    // window_lock(res_2d->scanout_id);
    //  window_render((struct gpu_resource *) res_2d); //TODO
    // window_unlock(res_2d->scanout_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_transfer_to_host_2d_handler(virtio_gpu_state_t *vgpu,
                                           struct virtq_desc *vq_desc,
                                           uint32_t *plen)
{
    /* Read request */
    struct vgpu_trans_to_host_2d *req =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    struct virtio_gpu_box box = {
        .x = req->r.x,
        .y = req->r.y,
        .z = 0,
        .w = req->r.width,
        .h = req->r.height,
        .d = 1,
    };

    virgl_renderer_transfer_write_iov(req->resource_id, 0, 0, 0, 0,
                                      (struct virgl_box *) &box, req->offset,
                                      NULL, 0);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    struct vgpu_ctrl_hdr res_no_data = {.type = VIRTIO_GPU_RESP_OK_NODATA};
    memcpy(response, &res_no_data, sizeof(struct vgpu_ctrl_hdr));

    /* TODO: Response with fencing flag if needed */
    // virtio_gpu_set_response_fencing(&req->hdr, response);

    /* Update write length */
    *plen = sizeof(*response);
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
    if (!ret)
        free(res_iovec);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[2].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* XXX: Response with fencing flag if needed */
    // virtio_gpu_set_response_fencing(&backing_info->hdr, response);

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_resource_detach_backing_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    /* Read request */
    struct vgpu_res_attach_backing *backing_info =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detach the iovec array from the resource */
    struct iovec *res_iovec = NULL;
    int iovec_num = 0;
    virgl_renderer_resource_detach_iov(backing_info->resource_id, &res_iovec,
                                       &iovec_num);

    /* Free the iovec array */
    if (res_iovec && iovec_num)
        free(res_iovec);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen)
{
    /* Read request */
    struct vgpu_get_capset_info *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Write capability info */
    struct vgpu_resp_capset_info *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;

    uint32_t capset_max_version, capset_max_size;

    if (request->capset_index == 0) {
        response->capset_id = VIRTIO_GPU_CAPSET_VIRGL2;
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

    /* Write capability set */
    struct virtio_gpu_resp_capset *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);
    memset(response, 0, sizeof(*response));

    uint32_t max_ver, max_size;
    virgl_renderer_get_cap_set(request->capset_id, &max_ver, &max_size);

    if (!max_size) {
        response->hdr.type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    } else {
        response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
        virgl_renderer_fill_caps(request->capset_id, request->capset_version,
                                 (void *) response->capset_data);
    }

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

    /* Create 3D context with VirGL */
    virgl_renderer_context_create(request->hdr.ctx_id, request->nlen,
                                  request->debug_name);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_destroy *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Destroy 3D context with VirGL */
    virgl_renderer_context_destroy(request->hdr.ctx_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_attach_resource_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_resource *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Attach 3D context to resource with VirGL */
    virgl_renderer_ctx_attach_resource(request->hdr.ctx_id,
                                       request->resource_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_detach_resource_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_ctx_resource *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Detach 3D context and resource with VirGL */
    virgl_renderer_ctx_detach_resource(request->hdr.ctx_id,
                                       request->resource_id);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_resource_create_3d_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_resource_create_3d *request =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

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
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

void virgl_cmd_transfer_to_host_3d_handler(virtio_gpu_state_t *vgpu,
                                           struct virtq_desc *vq_desc,
                                           uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_transfer_host_3d *req =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Transfer data from target to host with VirGL */
    virgl_renderer_transfer_write_iov(
        req->resource_id, req->hdr.ctx_id, req->level, req->stride,
        req->layer_stride, (struct virgl_box *) &req->box, req->offset, NULL,
        0);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    struct vgpu_ctrl_hdr res_no_data = {.type = VIRTIO_GPU_RESP_OK_NODATA};
    memcpy(response, &res_no_data, sizeof(struct vgpu_ctrl_hdr));

    /* Update write length */
    *plen = sizeof(*response);
}

void virgl_cmd_transfer_from_host_3d_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_transfer_host_3d *req =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* Transfer data from host to target with VirGL */
    virgl_renderer_transfer_read_iov(req->resource_id, req->hdr.ctx_id,
                                     req->level, req->stride, req->layer_stride,
                                     (struct virgl_box *) &req->box,
                                     req->offset, NULL, 0);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    struct vgpu_ctrl_hdr res_no_data = {.type = VIRTIO_GPU_RESP_OK_NODATA};
    memcpy(response, &res_no_data, sizeof(struct vgpu_ctrl_hdr));

    /* Update write length */
    *plen = sizeof(*response);
}

size_t copy_iovec_to_buf(const struct iovec *iov,
                         const unsigned int iov_cnt,
                         size_t offset,
                         void *buf,
                         size_t nbytes)
{
    /* TODO */
}

void virgl_cmd_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_cmd_submit *req =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    /* TODO: Copy commands from iovec list to the buffer */
    void *buffer = malloc(req->size);
    if (!buffer) {
        fprintf(stderr, "malloc failed\n"); /* TODO */
    }

    /* Copy 3D commands from the iovec list to a contiguous buffer */
    // copy_iovec_to_buf(, , sizeof(*req), buffer, req->size); /* TODO */

    /* Submit 3D command with VirGL */
    virgl_renderer_submit_cmd(buffer, req->hdr.ctx_id, req->size / 4);

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    struct vgpu_ctrl_hdr res_no_data = {.type = VIRTIO_GPU_RESP_OK_NODATA};
    memcpy(response, &res_no_data, sizeof(struct vgpu_ctrl_hdr));

    /* Update write length */
    *plen = sizeof(*response);
}

void virgl_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                     struct virtq_desc *vq_desc,
                                     uint32_t *plen)
{
    /* Read request */
    struct virtio_gpu_update_cursor *cursor =
        vgpu_mem_host_to_guest(vgpu, vq_desc[0].addr);

    uint32_t width = CURSOR_WIDTH, height = CURSOR_HEIGHT;
    uint32_t *cursor_data =
        virgl_renderer_get_cursor_data(cursor->resource_id, &width, &height);
    /* TODO: check size */
    /* TODO: Submit to SDL2 */

    /* Write response */
    struct vgpu_ctrl_hdr *response =
        vgpu_mem_host_to_guest(vgpu, vq_desc[1].addr);

    memset(response, 0, sizeof(*response));
    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    /* Return write length */
    *plen = sizeof(*response);
}

static int virgl_get_drm_fd(void *opaque)
{
    (void) opaque;

    /* XXX */
    printf("virglrender: open %s\n", RENDER_NODE);

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
    (void) opaque;
    (void) fence;
    printf("%s() called\n", __func__);
}

static struct virgl_renderer_callbacks virgl_callbacks = {
    .get_drm_fd = virgl_get_drm_fd,
    .version = 2,
    .write_fence = virgl_write_fence,
};

void semu_virgl_init(virtio_gpu_state_t *vgpu)
{
    int flags = VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_THREAD_SYNC;

    int ret = virgl_renderer_init(vgpu, flags, &virgl_callbacks);
    if (ret) {
        fprintf(stderr, "failed to initialize virgl renderer: %s\n",
                strerror(ret));
        exit(2);
    }
}
