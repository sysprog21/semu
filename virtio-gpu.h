#pragma once

#if SEMU_HAS(VIRTIOGPU)

#include "virgl.h"
#include "virtio.h"

#define VGPU_CMD_UNDEF virtio_gpu_cmd_undefined_handler

struct vgpu_scanout_info {
    uint32_t width;
    uint32_t height;
    uint32_t enabled;
};

typedef struct {
    struct vgpu_scanout_info scanouts[VIRTIO_GPU_MAX_SCANOUTS];
    virgl_data_t virgl_data;
} virtio_gpu_data_t;

struct vgpu_resource_2d {
    uint32_t scanout_id;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint32_t *image;
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

PACKED(struct vgpu_get_capset {
    struct vgpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
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

enum virtio_gpu_ctrl_type {
    /* 2D commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
    VIRTIO_GPU_CMD_GET_CAPSET,
    VIRTIO_GPU_CMD_GET_EDID,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB,

    /* 3D commands */
    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D,
    VIRTIO_GPU_CMD_SUBMIT_3D,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB,

    /* Cursor commands */
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,

    /* Success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID,

    /* Error responses */
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

enum virtio_gpu_formats {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,
    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134
};

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

size_t iov_to_buf(const struct iovec *iov,
                  const unsigned int iov_cnt,
                  size_t offset,
                  void *buf,
                  size_t bytes);

struct vgpu_resource_2d *vgpu_create_resource_2d(int resource_id);
struct vgpu_resource_2d *vgpu_get_resource_2d(uint32_t resource_id);
int vgpu_destory_resource_2d(uint32_t resource_id);

void *vgpu_mem_host_to_guest(virtio_gpu_state_t *vgpu, uint32_t addr);
uint32_t virtio_gpu_write_response(virtio_gpu_state_t *vgpu,
                                   uint64_t addr,
                                   uint32_t type);
void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu);
void virtio_gpu_set_response_fencing(virtio_gpu_state_t *vgpu,
                                     struct vgpu_ctrl_hdr *request,
                                     uint64_t addr);
void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen);
void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen);
void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen);
#endif
