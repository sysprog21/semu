#pragma once

#include "virgl.h"
#include "virtio.h"

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

struct vgpu_resource_2d *create_vgpu_resource_2d(int resource_id);
struct vgpu_resource_2d *acquire_vgpu_resource_2d(uint32_t resource_id);
int destroy_vgpu_resource_2d(uint32_t resource_id);

uint32_t virtio_gpu_write_response(virtio_gpu_state_t *vgpu,
                                   uint64_t addr,
                                   uint32_t type);

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu);

static inline void *vgpu_mem_host_to_guest(virtio_gpu_state_t *vgpu,
                                           uint32_t addr)
{
    return (void *) ((uintptr_t) vgpu->ram + addr);
}
