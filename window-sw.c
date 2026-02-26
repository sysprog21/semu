#include <SDL.h>
#include <SDL_thread.h>

#include "device.h"
#include "virtio-gpu.h"
#include "window.h"

#define SDL_COND_TIMEOUT 1 /* ms */

enum primary_op {
    PRIMARY_NONE,
    PRIMARY_CLEAR,
    PRIMARY_FLUSH,
};

enum cursor_op {
    CURSOR_NONE,
    CURSOR_CLEAR,
    CURSOR_UPDATE,
    CURSOR_MOVE,
};

struct display_info {
    enum primary_op primary_pending;
    enum cursor_op cursor_pending;

    /* Primary plane */
    struct vgpu_resource_2d resource;
    uint32_t primary_sdl_format;
    uint32_t *primary_img;
    SDL_Texture *primary_texture;

    /* Cursor plane */
    struct vgpu_resource_2d cursor;
    uint32_t *cursor_img;
    SDL_Rect cursor_rect; /* Cursor size and position */
    SDL_Texture *cursor_texture;

    SDL_mutex *img_mtx;
    SDL_cond *img_cond;
    SDL_Window *window;
    SDL_Renderer *renderer;
};

static struct display_info displays[VIRTIO_GPU_MAX_SCANOUTS];
static int display_cnt;
static bool headless_mode = false;
static bool should_exit = false;

/* Main loop runs on the main thread */
static void window_main_loop_sw(void)
{
    if (headless_mode)
        return;

    SDL_Surface *surface;

    while (!should_exit) {
#if SEMU_HAS(VIRTIOINPUT)
        /* Handle SDL events */
        if (handle_window_events()) {
            should_exit = true;
            exit(0);
        }
#endif

        /* Check each display for pending render requests */
        for (int i = 0; i < display_cnt; i++) {
            struct display_info *display = &displays[i];
            struct vgpu_resource_2d *resource = &display->resource;
            struct vgpu_resource_2d *cursor = &display->cursor;

            /* Mutex lock */
            if (SDL_LockMutex(display->img_mtx) == 0) {
                /* Wait until the image is arrived */
                SDL_CondWaitTimeout(display->img_cond, display->img_mtx,
                                    SDL_COND_TIMEOUT);

                bool any_update = display->primary_pending != PRIMARY_NONE ||
                                  display->cursor_pending != CURSOR_NONE;

                /* Handle primary plane update */
                if (display->primary_pending == PRIMARY_CLEAR) {
                    SDL_DestroyTexture(display->primary_texture);
                    display->primary_texture = NULL;
                    display->primary_pending = PRIMARY_NONE;
                } else if (display->primary_pending == PRIMARY_FLUSH) {
                    /* Generate primary plane texture */
                    surface = SDL_CreateRGBSurfaceWithFormatFrom(
                        resource->image, resource->width, resource->height,
                        resource->bits_per_pixel, resource->stride,
                        display->primary_sdl_format);

                    if (surface) {
                        SDL_DestroyTexture(display->primary_texture);
                        display->primary_texture = SDL_CreateTextureFromSurface(
                            display->renderer, surface);
                        SDL_FreeSurface(surface);
                    } else {
                        fprintf(stderr,
                                "Failed to create primary plane surface: "
                                "%s\n",
                                SDL_GetError());
                    }
                    display->primary_pending = PRIMARY_NONE;
                }

                /* Handle cursor plane update */
                if (display->cursor_pending == CURSOR_UPDATE) {
                    /* Cursor data is always treated as ARGB8888 regardless of
                     * the resource format reported by the guest, following the
                     * same approach as QEMU (see QEMUCursor: "data format is
                     * 32bit RGBA").
                     *
                     * In practice the Linux virtio-gpu driver creates dumb
                     * buffers with B8G8R8X8_UNORM and uses the X byte as alpha
                     * (0 = transparent), so following the X format would
                     * discard cursor transparency.
                     */
                    surface = SDL_CreateRGBSurfaceWithFormatFrom(
                        cursor->image, cursor->width, cursor->height,
                        cursor->bits_per_pixel, cursor->stride,
                        SDL_PIXELFORMAT_ARGB8888);

                    if (surface) {
                        SDL_DestroyTexture(display->cursor_texture);
                        display->cursor_texture = SDL_CreateTextureFromSurface(
                            display->renderer, surface);
                        SDL_FreeSurface(surface);
                    } else {
                        fprintf(stderr,
                                "Failed to create cursor plane surface: "
                                "%s\n",
                                SDL_GetError());
                    }
                    display->cursor_pending = CURSOR_NONE;
                } else if (display->cursor_pending == CURSOR_CLEAR) {
                    SDL_DestroyTexture(display->cursor_texture);
                    display->cursor_texture = NULL;
                    display->cursor_pending = CURSOR_NONE;
                } else if (display->cursor_pending == CURSOR_MOVE) {
                    /* cursor_rect already updated by caller */
                    display->cursor_pending = CURSOR_NONE;
                }

                /* Render both planes when any update is pending */
                if (any_update) {
                    SDL_RenderClear(display->renderer);

                    if (display->primary_texture)
                        SDL_RenderCopy(display->renderer,
                                       display->primary_texture, NULL, NULL);

                    if (display->cursor_texture)
                        SDL_RenderCopy(display->renderer,
                                       display->cursor_texture, NULL,
                                       &display->cursor_rect);

                    SDL_RenderPresent(display->renderer);
                }
                SDL_UnlockMutex(display->img_mtx);
            }
        }
    }
}

static void window_init_sw(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr,
                "window_init_sw(): failed to initialize SDL: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        return;
    }

    /* Create windows and renderers on main thread */
    for (int i = 0; i < display_cnt; i++) {
        displays[i].img_mtx = SDL_CreateMutex();
        if (!displays[i].img_mtx) {
            fprintf(stderr,
                    "window_init_sw(): failed to create mutex for display %d: "
                    "%s\n",
                    i, SDL_GetError());
            exit(2);
        }

        displays[i].img_cond = SDL_CreateCond();
        if (!displays[i].img_cond) {
            fprintf(stderr,
                    "window_init_sw(): failed to create condition variable for "
                    "display %d: %s\n",
                    i, SDL_GetError());
            SDL_DestroyMutex(displays[i].img_mtx);
            exit(2);
        }

        /* Create window on main thread (required for macOS) */
        displays[i].window = SDL_CreateWindow(
            "semu", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            displays[i].resource.width, displays[i].resource.height,
            SDL_WINDOW_SHOWN);

        if (!displays[i].window) {
            fprintf(stderr,
                    "window_init_sw(): failed to create SDL window for display "
                    "%d: %s\n"
                    "Possible causes:\n"
                    "  - No display available (headless environment)\n"
                    "  - SDL video driver not supported\n"
                    "  - Insufficient permissions\n"
                    "Running in headless mode.\n",
                    i, SDL_GetError());
            headless_mode = true;
            return;
        }

        /* Create renderer (try accelerated first, fall back to software) */
        displays[i].renderer = SDL_CreateRenderer(displays[i].window, -1,
                                                  SDL_RENDERER_ACCELERATED);

        if (!displays[i].renderer) {
            fprintf(stderr,
                    "window_init_sw(): accelerated renderer not available, "
                    "trying software renderer: %s\n",
                    SDL_GetError());
            displays[i].renderer = SDL_CreateRenderer(displays[i].window, -1,
                                                      SDL_RENDERER_SOFTWARE);
        }

        if (!displays[i].renderer) {
            fprintf(stderr,
                    "window_init_sw(): failed to create renderer for display "
                    "%d: %s\n",
                    i, SDL_GetError());
            exit(2);
        }

        /* Initialize with black screen */
        SDL_SetRenderDrawColor(displays[i].renderer, 0, 0, 0, 255);
        SDL_RenderClear(displays[i].renderer);
        SDL_RenderPresent(displays[i].renderer);
    }
}

static void window_shutdown_sw(void)
{
    should_exit = true;
}

static void window_add_sw(uint32_t width, uint32_t height)
{
    if (display_cnt >= VIRTIO_GPU_MAX_SCANOUTS) {
        fprintf(stderr, "%s(): display count exceeds maximum\n", __func__);
        exit(2);
    }

    displays[display_cnt].resource.width = width;
    displays[display_cnt].resource.height = height;
    display_cnt++;
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

static void cursor_clear_sw(int scanout_id)
{
    if (headless_mode)
        return;

    if (scanout_id >= display_cnt)
        return;

    struct display_info *display = &displays[scanout_id];

    /* Start of the critical section */
    if (SDL_LockMutex(displays[scanout_id].img_mtx) != 0) {
        fprintf(stderr, "%s(): failed to lock mutex: %s\n", __func__,
                SDL_GetError());
        return;
    }

    /* Reset cursor information */
    memset(&display->cursor_rect, 0, sizeof(SDL_Rect));

    /* Reset cursor resource */
    memset(&display->cursor, 0, sizeof(struct vgpu_resource_2d));
    free(display->cursor_img);
    display->cursor_img = NULL;
    display->cursor.image = NULL;

    /* Trigger cursor plane rendering */
    display->cursor_pending = CURSOR_CLEAR;
    SDL_CondSignal(display->img_cond);

    /* End of the critical section */
    SDL_UnlockMutex(displays[scanout_id].img_mtx);
}

static void cursor_update_sw(int scanout_id, int res_id, int x, int y)
{
    if (headless_mode)
        return;

    if (scanout_id >= display_cnt)
        return;

    struct vgpu_resource_2d *resource = vgpu_get_resource_2d(res_id);
    if (!resource) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__, res_id);
        return;
    }

    /* Start of the critical section */
    if (SDL_LockMutex(displays[scanout_id].img_mtx) != 0) {
        fprintf(stderr, "%s(): failed to lock mutex: %s\n", __func__,
                SDL_GetError());
        return;
    }

    /* Update cursor information */
    struct display_info *display = &displays[scanout_id];
    display->cursor_rect.x = x;
    display->cursor_rect.y = y;
    display->cursor_rect.w = resource->width;
    display->cursor_rect.h = resource->height;

    /* Cursor resource update */
    memcpy(&display->cursor, resource, sizeof(struct vgpu_resource_2d));
    size_t pixels_size = (size_t) resource->stride * resource->height;
    free(display->cursor_img);
    display->cursor_img = malloc(pixels_size);
    if (!display->cursor_img) {
        fprintf(stderr, "%s(): failed to allocate cursor image\n", __func__);
        SDL_UnlockMutex(displays[scanout_id].img_mtx);
        return;
    }

    display->cursor.image = display->cursor_img;
    memcpy(display->cursor_img, resource->image, pixels_size);

    /* Trigger cursor rendering */
    display->cursor_pending = CURSOR_UPDATE;
    SDL_CondSignal(display->img_cond);

    /* End of the critical section */
    SDL_UnlockMutex(displays[scanout_id].img_mtx);
}

static void cursor_move_sw(int scanout_id, int x, int y)
{
    if (headless_mode)
        return;

    if (scanout_id >= display_cnt)
        return;

    struct display_info *display = &displays[scanout_id];

    /* Start of the critical section */
    if (SDL_LockMutex(displays[scanout_id].img_mtx) != 0) {
        fprintf(stderr, "%s(): failed to lock mutex: %s\n", __func__,
                SDL_GetError());
        return;
    }

    /* Update cursor position */
    display->cursor_rect.x = x;
    display->cursor_rect.y = y;

    /* Trigger cursor rendering */
    display->cursor_pending = CURSOR_MOVE;
    SDL_CondSignal(display->img_cond);

    /* End of the critical section */
    SDL_UnlockMutex(displays[scanout_id].img_mtx);
}

static void window_clear_sw(int scanout_id)
{
    if (headless_mode)
        return;

    if (scanout_id >= display_cnt)
        return;

    struct display_info *display = &displays[scanout_id];

    /* Start of the critical section */
    if (SDL_LockMutex(displays[scanout_id].img_mtx) != 0) {
        fprintf(stderr, "%s(): failed to lock mutex: %s\n", __func__,
                SDL_GetError());
        return;
    }

    /* Reset primary plane resource */
    memset(&display->resource, 0, sizeof(struct vgpu_resource_2d));
    free(display->primary_img);
    display->primary_img = NULL;
    display->resource.image = NULL;

    /* Trigger primary plane rendering */
    display->primary_pending = PRIMARY_CLEAR;
    SDL_CondSignal(display->img_cond);

    /* End of the critical section */
    SDL_UnlockMutex(displays[scanout_id].img_mtx);
}

static void window_flush_sw(int scanout_id, int res_id)
{
    if (headless_mode)
        return;

    if (scanout_id >= display_cnt)
        return;

    struct display_info *display = &displays[scanout_id];
    struct vgpu_resource_2d *resource = vgpu_get_resource_2d(res_id);
    if (!resource) {
        fprintf(stderr, "%s(): invalid resource id %d\n", __func__, res_id);
        return;
    }

    /* Convert virtio-gpu resource format to SDL format.
     * Only the primary plane negotiates format at runtime; the cursor plane
     * always renders as ARGB8888 (see cursor_update_sw).
     */
    uint32_t sdl_format;
    bool legal_format = virtio_gpu_to_sdl_format(resource->format, &sdl_format);
    if (!legal_format) {
        fprintf(stderr, "%s(): invalid resource format\n", __func__);
        return;
    }

    /* Start of the critical section */
    if (SDL_LockMutex(displays[scanout_id].img_mtx) != 0) {
        fprintf(stderr, "%s(): failed to lock mutex: %s\n", __func__,
                SDL_GetError());
        return;
    }

    /* Update primary plane resource */
    display->primary_sdl_format = sdl_format;
    memcpy(&display->resource, resource, sizeof(struct vgpu_resource_2d));

    /* Deep copy pixel data to decouple from the original resource buffer */
    size_t pixels_size = (size_t) resource->stride * resource->height;
    uint32_t *new_img = realloc(display->primary_img, pixels_size);
    if (!new_img) {
        fprintf(stderr, "%s(): failed to allocate primary image\n", __func__);
        SDL_UnlockMutex(displays[scanout_id].img_mtx);
        return;
    }
    display->primary_img = new_img;
    display->resource.image = new_img;
    memcpy(new_img, resource->image, pixels_size);

    /* Trigger primary plane flushing */
    display->primary_pending = PRIMARY_FLUSH;
    SDL_CondSignal(display->img_cond);

    /* End of the critical section */
    SDL_UnlockMutex(displays[scanout_id].img_mtx);
}

const struct window_backend g_window = {
    .window_init = window_init_sw,
    .window_add = window_add_sw,
    .window_set_scanout = NULL,
    .window_clear = window_clear_sw,
    .window_flush = window_flush_sw,
    .cursor_clear = cursor_clear_sw,
    .cursor_update = cursor_update_sw,
    .cursor_move = cursor_move_sw,
    .window_main_loop = window_main_loop_sw,
    .window_shutdown = window_shutdown_sw,
};
