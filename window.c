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
    /* Request type: primary or cursor */
    int render_type;

    /* Primary plane */
    struct gpu_resource resource;
    uint32_t primary_sdl_format;
    SDL_Texture *primary_texture;

    /* Cursor plane */
    struct gpu_resource cursor;
    uint32_t cursor_sdl_format;
    uint32_t *cursor_img;
    SDL_Rect cursor_rect; /* Cursor size and position */
    SDL_Texture *cursor_texture;

    SDL_mutex *img_mtx;
    SDL_cond *img_cond;
    SDL_Thread *thread_id;
    SDL_Window *window;
    SDL_Renderer *renderer;
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
    struct gpu_resource *cursor = &display->cursor;

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

    /* Render the whole screen with black color */
    SDL_SetRenderDrawColor(display->renderer, 0, 0, 0, 255);
    SDL_RenderClear(display->renderer);
    SDL_RenderPresent(display->renderer);

    SDL_Surface *surface;

    while (1) {
        /* Mutex lock */
        SDL_LockMutex(display->img_mtx);

        /* Wait until the image is arrived */
        while (SDL_CondWaitTimeout(display->img_cond, display->img_mtx,
                                   SDL_COND_TIMEOUT)) {
            /* Read event */
            SDL_Event e;
            SDL_PollEvent(&e);  // TODO: Handle events
        }

        if (display->render_type == RENDER_PRIMARY_PLANE) {
            /* Generate primary plane texture */
            surface = SDL_CreateRGBSurfaceWithFormatFrom(
                resource->image, resource->width, resource->height,
                resource->bits_per_pixel, resource->stride,
                display->primary_sdl_format);

            SDL_DestroyTexture(display->primary_texture);
            display->primary_texture =
                SDL_CreateTextureFromSurface(display->renderer, surface);
            SDL_FreeSurface(surface);
        } else if (display->render_type == UPDATE_CURSOR_RESOURCE) {
            /* Generate cursor plane texture */
            surface = SDL_CreateRGBSurfaceWithFormatFrom(
                cursor->image, cursor->width, cursor->height, CURSOR_BPP,
                CURSOR_STRIDE, SDL_PIXELFORMAT_ARGB8888);

            SDL_DestroyTexture(display->cursor_texture);
            display->cursor_texture =
                SDL_CreateTextureFromSurface(display->renderer, surface);
            SDL_FreeSurface(surface);
        } else if (display->render_type == CLEAR_CURSOR_RESOURCE) {
            SDL_DestroyTexture(display->cursor_texture);
            display->cursor_texture = NULL;
        }

        /* Render primary and cursor planes */
        SDL_RenderClear(display->renderer);

        if (display->primary_texture)
            SDL_RenderCopy(display->renderer, display->primary_texture, NULL,
                           NULL);

        if (display->cursor_texture)
            SDL_RenderCopy(display->renderer, display->cursor_texture, NULL,
                           &display->cursor_rect);

        SDL_RenderPresent(display->renderer);

        /* Mutex unlock */
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

void cursor_clear(int scanout_id)
{
    /* Reset cursor information */
    struct display_info *display = &displays[scanout_id];
    memset(&display->cursor_rect, 0, sizeof(SDL_Rect));
    display->cursor_sdl_format = 0;

    /* Reset cursor resource */
    memset(&display->cursor, 0, sizeof(struct gpu_resource));
    free(display->cursor_img);
    display->cursor_img = NULL;
    display->cursor.image = NULL;

    /* Trigger plane rendering */
    display->render_type = CLEAR_CURSOR_RESOURCE;
    SDL_CondSignal(display->img_cond);
}

void cursor_update(struct gpu_resource *resource, int scanout_id, int x, int y)
{
    /* Convert virtio-gpu resource format to SDL format */
    uint32_t sdl_format;
    bool legal_format = virtio_gpu_to_sdl_format(resource->format, &sdl_format);

    if (!legal_format) {
        fprintf(stderr, "Invalid resource format.\n");
        return;
    }

    /* Update cursor information */
    struct display_info *display = &displays[scanout_id];
    display->cursor_rect.x = x;
    display->cursor_rect.y = y;
    display->cursor_rect.w = resource->width;
    display->cursor_rect.h = resource->height;
    display->cursor_sdl_format = sdl_format;

    /* Cursor resource update */
    memcpy(&display->cursor, resource, sizeof(struct gpu_resource));
    size_t pixels_size = sizeof(uint32_t) * resource->width * resource->height;
    free(display->cursor_img);
    display->cursor_img = malloc(pixels_size);
    display->cursor.image = display->cursor_img;
    memcpy(display->cursor_img, resource->image, pixels_size);

    /* Trigger cursor rendering */
    display->render_type = UPDATE_CURSOR_RESOURCE;
    SDL_CondSignal(display->img_cond);
}

void cursor_move(int scanout_id, int x, int y)
{
    /* Update cursor position */
    struct display_info *display = &displays[scanout_id];
    display->cursor_rect.x = x;
    display->cursor_rect.y = y;

    /* Trigger cursor rendering */
    display->render_type = MOVE_CURSOR_POSITION;
    SDL_CondSignal(display->img_cond);
}

void window_render(struct gpu_resource *resource)
{
    int id = resource->scanout_id;
    struct display_info *display = &displays[id];

    /* Convert virtio-gpu resource format to SDL format */
    uint32_t sdl_format;
    bool legal_format = virtio_gpu_to_sdl_format(resource->format, &sdl_format);

    if (!legal_format) {
        fprintf(stderr, "Invalid resource format.\n");
        return;
    }

    /* Update primary plane resource */
    display->primary_sdl_format = sdl_format;
    memcpy(&display->resource, resource, sizeof(struct gpu_resource));

    /* Trigger primary plane rendering */
    displays[id].render_type = RENDER_PRIMARY_PLANE;
    SDL_CondSignal(display->img_cond);
}
