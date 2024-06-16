#pragma once

/* Cursor size is always 64*64 in VirtIO GPU */
#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64

#define CURSOR_BPP 4 /* Bytes per pixel, using ARGB */
#define CURSOR_STRIDE (CURSOR_WIDTH * CURSOR_BPP)

enum {
    RENDER_PRIMARY_PLANE,
    UPDATE_CURSOR_RESOURCE,
    CLEAR_CURSOR_RESOURCE,
    MOVE_CURSOR_POSITION,
};

#if SEMU_HAS(VIRTIOGPU)
/* Public interface to the vgpu_resource_2d structure */
struct gpu_resource {
    uint32_t scanout_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint32_t *image;
};

void window_init(void);
void window_add(uint32_t width, uint32_t height);
void window_render(struct gpu_resource *resource);
void cursor_clear(int scanout_id);
void cursor_update(struct gpu_resource *resource, int scanout_id, int x, int y);
void cursor_move(int scanout_id, int x, int y);
void window_lock(uint32_t id);
void window_unlock(uint32_t id);
#endif
