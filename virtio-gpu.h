#pragma once

#if !SEMU_HAS(VIRTIOGPU)
#error Only valid when Virtio-GPU is enabled.
#endif

#include <stddef.h>
#include <stdint.h>

#include "virtio.h"

#define VIRTIO_GPU_MAX_SCANOUTS 16
#define VIRTIO_GPU_LOG_PREFIX "[SEMU VGPU] "
#define VIRTIO_GPU_CMD_UNDEF virtio_gpu_cmd_undefined_handler
#define VIRTIO_GPU_FLAG_FENCE (1 << 0)

/* Maximum descriptor chain length accepted by 'virtio_gpu_desc_handler()'.
 *
 * semu follows the common Linux virtio-gpu control queue shape in
 * 'virtio_gpu_queue_fenced_ctrl_buffer()': 'sgs[3]' holds 'vcmd' (request),
 * optional 'vout' (command data, e.g. 'RESOURCE_ATTACH_BACKING' entries), and
 * optional 'vresp' (response). The supported commands therefore fit in "request
 * + one data segment + response".
 *
 * This is not a general virtio-gpu descriptor-chain limit. Linux allocates the
 * backing-entry array in 'virtio_gpu_object_shmem_init()' with
 * 'kvmalloc_objs()'. If that buffer falls back to 'vmalloc',
 * 'virtio_gpu_queue_fenced_ctrl_buffer()' detects it with 'is_vmalloc_addr()'
 * and 'vmalloc_to_sgt()' expands 'vout' into multiple scatter-gather entries.
 *
 * Supporting that path would require accepting a longer descriptor chain and
 * auditing every handler that indexes 'vq_desc[]'. Longer chains are rejected.
 * The current response-descriptor lookup is also part of this fixed-shape
 * parser: it scans the zero-initialized 3-entry array, not an arbitrary
 * guest-provided scatter-gather chain.
 *
 * TODO: Support generic descriptor-chain parsing.
 */
#define VIRTIO_GPU_MAX_DESC 3

/* Core per-scanout metadata keyed by the guest-visible 'scanout_id'. This
 * combines guest-visible display info ('width'/'height'/'enabled') with the
 * current primary/cursor resource bindings.
 */
struct virtio_gpu_scanout_info {
    uint32_t width, height;
    uint32_t enabled;
    uint32_t primary_resource_id;
    uint32_t cursor_resource_id;
    uint32_t src_x, src_y, src_w, src_h;
};

typedef struct {
    struct virtio_gpu_scanout_info scanouts[VIRTIO_GPU_MAX_SCANOUTS];
    uint32_t num_scanouts;
} virtio_gpu_data_t;

PACKED(struct virtio_gpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
});

PACKED(struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
});

PACKED(struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
});

PACKED(struct virtio_gpu_resp_disp_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
});

PACKED(struct virtio_gpu_res_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
});

PACKED(struct virtio_gpu_res_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
});

PACKED(struct virtio_gpu_res_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct virtio_gpu_trans_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct virtio_gpu_res_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
});

PACKED(struct virtio_gpu_res_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
});

PACKED(struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
});

PACKED(struct virtio_gpu_cmd_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t padding;
});

PACKED(struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
    char edid[1024];
});

PACKED(struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
});

PACKED(struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
});

PACKED(struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
});

PACKED(struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint8_t capset_data[];
});

PACKED(struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
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
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
});

/* clang-format off */
PACKED(struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
});
/* clang-format on */

PACKED(struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
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
    struct virtio_gpu_ctrl_hdr hdr;
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
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
});

PACKED(struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t num_in_fences;
});

PACKED(struct virtio_gpu_resp_map_info {
    struct virtio_gpu_ctrl_hdr hdr;
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

typedef void (*virtio_gpu_cmd_func)(virtio_gpu_state_t *vgpu,
                                    struct virtq_desc *vq_desc,
                                    uint32_t *plen);
typedef void (*virtio_gpu_backend_lifecycle_func)(virtio_gpu_state_t *vgpu);

struct virtio_gpu_cmd_backend {
    virtio_gpu_backend_lifecycle_func reset;
    /* 2D commands */
    virtio_gpu_cmd_func get_display_info;
    virtio_gpu_cmd_func resource_create_2d;
    virtio_gpu_cmd_func resource_unref;
    virtio_gpu_cmd_func set_scanout;
    virtio_gpu_cmd_func resource_flush;
    virtio_gpu_cmd_func transfer_to_host_2d;
    virtio_gpu_cmd_func resource_attach_backing;
    virtio_gpu_cmd_func resource_detach_backing;
    virtio_gpu_cmd_func get_capset_info;
    virtio_gpu_cmd_func get_capset;
    virtio_gpu_cmd_func get_edid;
    virtio_gpu_cmd_func resource_assign_uuid;
    virtio_gpu_cmd_func resource_create_blob;
    virtio_gpu_cmd_func set_scanout_blob;
    /* 3D commands */
    virtio_gpu_cmd_func ctx_create;
    virtio_gpu_cmd_func ctx_destroy;
    virtio_gpu_cmd_func ctx_attach_resource;
    virtio_gpu_cmd_func ctx_detach_resource;
    virtio_gpu_cmd_func resource_create_3d;
    virtio_gpu_cmd_func transfer_to_host_3d;
    virtio_gpu_cmd_func transfer_from_host_3d;
    virtio_gpu_cmd_func submit_3d;
    virtio_gpu_cmd_func resource_map_blob;
    virtio_gpu_cmd_func resource_unmap_blob;
    /* Cursor commands */
    virtio_gpu_cmd_func update_cursor;
    virtio_gpu_cmd_func move_cursor;
};

void *virtio_gpu_mem_guest_to_host(virtio_gpu_state_t *vgpu,
                                   uint32_t addr,
                                   uint32_t size);
void *virtio_gpu_get_request(virtio_gpu_state_t *vgpu,
                             struct virtq_desc *vq_desc,
                             size_t request_size);
int virtio_gpu_get_response_desc(struct virtq_desc *vq_desc,
                                 int max_desc,
                                 size_t response_size);
uint32_t virtio_gpu_write_ctrl_response(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t type);

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu);

void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen);
void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen);
void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen);
