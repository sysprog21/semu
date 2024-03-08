#pragma once

#if SEMU_HAS(VIRTIOGPU)
#include <SDL.h>

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
void window_lock(uint32_t id);
void window_unlock(uint32_t id);
#endif
