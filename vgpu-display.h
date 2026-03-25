#pragma once

#if !SEMU_HAS(VIRTIOGPU)
#error Only valid when Virtio-GPU is enabled.
#endif

#include <stdbool.h>
#include <stdint.h>

#include "virtio-gpu.h"

/* Immutable CPU-frame payload published by the VirtIO GPU backend and later
 * consumed by the window backend when it uploads pixels into its own textures.
 */
struct vgpu_display_cpu_payload {
    enum virtio_gpu_formats format;
    uint32_t width, height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint8_t *pixels;
};

/* Owning payload object passed through the display queue. The bridge queues
 * and disposes this object, while GPU and window backends only fill or
 * consume the payload it carries.
 */
struct vgpu_display_payload {
    struct vgpu_display_cpu_payload cpu;
    /* TODO: Add a GL/virgl payload when 3D scanout is implemented. The display
     * bridge currently transports CPU-owned 2D frames only.
     */
};

/* Runtime display commands published by the GPU backend and consumed by the
 * window backend. 'PRIMARY_*' updates the main scanout image, while 'CURSOR_*'
 * updates or moves the separate cursor plane.
 *
 * Clear commands are reliable generation changes, frame/move commands are lossy
 * SPSC queue entries.
 */
enum vgpu_display_cmd_type {
    VGPU_DISPLAY_CMD_PRIMARY_SET = 0,
    VGPU_DISPLAY_CMD_PRIMARY_CLEAR,
    VGPU_DISPLAY_CMD_CURSOR_SET,
    VGPU_DISPLAY_CMD_CURSOR_CLEAR,
    VGPU_DISPLAY_CMD_CURSOR_MOVE,
};

/* One synthesized display bridge command. 'scanout_id' selects which scanout
 * to update, and the union carries the payload or coordinates required by the
 * specific command type above.
 */
struct vgpu_display_cmd {
    enum vgpu_display_cmd_type type;
    uint32_t scanout_id;
    uint32_t generation;
    union {
        struct {
            struct vgpu_display_payload *payload;
        } primary_set;
        struct {
            struct vgpu_display_payload *payload;
            int32_t x;
            int32_t y;
            uint32_t hot_x;
            uint32_t hot_y;
        } cursor_set;
        struct {
            int32_t x;
            int32_t y;
        } cursor_move;
    } u;
};

void vgpu_display_set_scanout_count(uint32_t scanout_count);
void vgpu_display_publish_primary_clear(uint32_t scanout_id);
void vgpu_display_publish_cursor_clear(uint32_t scanout_id);

void vgpu_display_release_cmd(struct vgpu_display_cmd *cmd);
bool vgpu_display_pop_cmd(struct vgpu_display_cmd *cmd);
void vgpu_display_set_unavailable(void);
bool vgpu_display_can_publish(void);
void vgpu_display_publish_primary_set(uint32_t scanout_id,
                                      struct vgpu_display_payload *payload);
void vgpu_display_publish_cursor_set(uint32_t scanout_id,
                                     struct vgpu_display_payload *payload,
                                     int32_t x,
                                     int32_t y,
                                     uint32_t hot_x,
                                     uint32_t hot_y);
void vgpu_display_publish_cursor_move(uint32_t scanout_id,
                                      int32_t x,
                                      int32_t y);
