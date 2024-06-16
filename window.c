#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_thread.h>

#include "virtio.h"
#include "window.h"

#define SDL_COND_TIMEOUT 1 /* ms */

struct display_info {
    struct gpu_resource resource;
    uint32_t sdl_format;
    SDL_mutex *img_mtx;
    SDL_cond *img_cond;
    SDL_Thread *thread_id;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Surface *surface;
    SDL_Texture *texture;
};

static struct display_info displays[VIRTIO_GPU_MAX_SCANOUTS];
static int display_cnt;

void window_add(uint32_t width, uint32_t height)
{
    displays[display_cnt].resource.width = width;
    displays[display_cnt].resource.height = height;
    display_cnt++;
}

static int window_thread(void *data)
{
    struct display_info *display = (struct display_info *) data;
    struct gpu_resource *resource = &display->resource;

    /* Create SDL window */
    display->window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, resource->width,
                                       resource->height, SDL_WINDOW_SHOWN);

    if (!display->window) {
        fprintf(stderr, "%s(): failed to create window\n", __func__);
        exit(2);
    }

    /* Create SDL render */
    display->renderer =
        SDL_CreateRenderer(display->window, -1, SDL_RENDERER_ACCELERATED);

    if (!display->renderer) {
        fprintf(stderr, "%s(): failed to create renderer\n", __func__);
        exit(2);
    }

    while (1) {
        SDL_LockMutex(display->img_mtx);

        /* Wait until the image is arrived */
        while (SDL_CondWaitTimeout(display->img_cond, display->img_mtx,
                                   SDL_COND_TIMEOUT)) {
            /* Read event */
            SDL_Event e;
            SDL_PollEvent(&e);  // TODO: Handle events
        }

        /* Render image */
        display->surface = SDL_CreateRGBSurfaceWithFormatFrom(
            resource->image, resource->width, resource->height,
            resource->bits_per_pixel, resource->stride, display->sdl_format);
        display->texture =
            SDL_CreateTextureFromSurface(display->renderer, display->surface);
        SDL_RenderCopy(display->renderer, display->texture, NULL, NULL);
        SDL_RenderPresent(display->renderer);
        SDL_DestroyTexture(display->texture);

        SDL_UnlockMutex(display->img_mtx);
    }
}

void window_init(void)
{
    char thread_name[20] = {0};

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "%s(): failed to initialize SDL\n", __func__);
        exit(2);
    }

    for (int i = 0; i < display_cnt; i++) {
        displays[i].img_mtx = SDL_CreateMutex();
        displays[i].img_cond = SDL_CreateCond();

        sprintf(thread_name, "sdl thread %d", i);
        displays[i].thread_id =
            SDL_CreateThread(window_thread, thread_name, (void *) &displays[i]);
        SDL_DetachThread(displays[i].thread_id);
    }
}

void window_lock(uint32_t id)
{
    SDL_LockMutex(displays[id].img_mtx);
}

void window_unlock(uint32_t id)
{
    SDL_UnlockMutex(displays[id].img_mtx);
}

static bool virtio_gpu_to_sdl_format(uint32_t virtio_gpu_format,
                                     uint32_t *sdl_format)
{
    switch (virtio_gpu_format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ARGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XRGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRA8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRX8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ABGR8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBX8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBA8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XBGR8888;
        return true;
    default:
        return false;
    }
}

void window_render(struct gpu_resource *resource)
{
    int id = resource->scanout_id;

    /* Resource update */
    memcpy(&displays[id].resource, resource, sizeof(struct gpu_resource));
    bool legal_format =
        virtio_gpu_to_sdl_format(resource->format, &displays[id].sdl_format);

    if (legal_format)
        SDL_CondSignal(displays[id].img_cond);
}
