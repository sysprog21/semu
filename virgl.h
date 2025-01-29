#pragma once

#include "list.h"

typedef struct {
    struct list_head fence_queue;
} virgl_data_t;

void virgl_cmd_resource_create_2d_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen);

void virgl_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen);

void virgl_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen);

void virgl_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen);

void virgl_cmd_transfer_to_host_2d_handler(virtio_gpu_state_t *vgpu,
                                           struct virtq_desc *vq_desc,
                                           uint32_t *plen);

void virgl_cmd_resource_attach_backing_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen);

void virgl_cmd_resource_detach_backing_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen);

void virgl_cmd_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen);

void virgl_get_capset_handler(virtio_gpu_state_t *vgpu,
                              struct virtq_desc *vq_desc,
                              uint32_t *plen);

void virgl_cmd_ctx_create_handler(virtio_gpu_state_t *vgpu,
                                  struct virtq_desc *vq_desc,
                                  uint32_t *plen);

void virgl_cmd_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen);

void virgl_cmd_attach_resource_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen);

void virgl_cmd_detach_resource_handler(virtio_gpu_state_t *vgpu,
                                       struct virtq_desc *vq_desc,
                                       uint32_t *plen);

void virgl_cmd_resource_create_3d_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen);

void virgl_cmd_transfer_to_host_3d_handler(virtio_gpu_state_t *vgpu,
                                           struct virtq_desc *vq_desc,
                                           uint32_t *plen);

void virgl_cmd_transfer_from_host_3d_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen);

void virgl_cmd_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen);

void virgl_cmd_update_cursor_handler(virtio_gpu_state_t *vgpu,
                                     struct virtq_desc *vq_desc,
                                     uint32_t *plen);

void semu_virgl_init(virtio_gpu_state_t *vgpu);
