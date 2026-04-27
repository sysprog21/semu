#include <SDL.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "feature.h"
#if SEMU_HAS(VIRTIOGPU)
#include "vgpu-display.h"
#include "virtio-gpu.h"
#endif
#if SEMU_HAS(VIRTIOINPUT)
#include "virtio-input-event.h"
#endif
#include "window.h"

#define WINDOW_LOG_PREFIX "[SEMU WINDOW] "

static int wake_write_fd = -1;
static bool sdl_initialized = false;
static bool headless_mode = false;
static bool should_exit = false;

#if SEMU_HAS(VIRTIOINPUT)
static bool mouse_grabbed = false;
static SDL_Window *sdl_input_window;
#else
#define SDL_EVENT_WAIT_TIMEOUT_MS 1 /* ms */
#define SDL_EVENT_BURST_LIMIT 64U
#endif

#if SEMU_HAS(VIRTIOGPU)
/* SDL-owned retained state for a single plane. Textures live only on the SDL
 * thread and are updated from immutable CPU-frame display resources.
 */
struct sdl_plane_info {
    uint32_t width;
    uint32_t height;
    uint32_t sdl_format;
    bool alpha_blend;
    SDL_Texture *texture;
};

/* SDL-owned retained state for one scanout. 'window_init_sw()' creates the
 * window/renderer, then 'window_drain_display_queue()' updates the primary and
 * cursor planes from queued display payloads before rendering them.
 */
struct sdl_scanout_info {
    struct sdl_plane_info primary_plane;
    struct sdl_plane_info cursor_plane;
    SDL_Rect cursor_rect;
    uint32_t cursor_hot_x;
    uint32_t cursor_hot_y;
    uint32_t window_width;
    uint32_t window_height;

    SDL_Window *window;
    SDL_Renderer *renderer;
};

static struct sdl_scanout_info sdl_scanouts[VIRTIO_GPU_MAX_SCANOUTS];
#endif

static void window_set_wake_fd_sw(int fd)
{
    wake_write_fd = fd;
}

static void window_wake_backend_sw(void)
{
    if (wake_write_fd >= 0) {
        char byte = 1;
        /* Best-effort wakeup: the pipe is non-blocking, and the byte value has
         * no meaning beyond making the read end readable.
         */
        ssize_t bytes_written = write(wake_write_fd, &byte, 1);
        (void) bytes_written;
    }
}

static bool window_is_closed_sw(void)
{
    return __atomic_load_n(&should_exit, __ATOMIC_RELAXED);
}

static void window_shutdown_sw(void)
{
    /* Both user-driven close and emulator-driven shutdown funnel through the
     * same flag so the main thread and emulator thread observe one exit state.
     */
    __atomic_store_n(&should_exit, true, __ATOMIC_RELAXED);
    /* Unblock any 'poll(-1)' in the SMP emulator loop immediately. */
    window_wake_backend_sw();
}

#if SEMU_HAS(VIRTIOINPUT)
/* Main-thread-only helper for relative-pointer devices. SDL's grab and
 * relative mouse APIs are part of the windowing backend, so callers use this
 * to switch between normal host-pointer mode and guest-directed mouse mode.
 */
static void window_set_mouse_grab_sw(bool grabbed)
{
    if (headless_mode || !sdl_input_window) {
        mouse_grabbed = false;
        return;
    }

    if (mouse_grabbed == grabbed)
        return;

    if (grabbed) {
        if (SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
            fprintf(stderr,
                    "window_set_mouse_grab_sw(): failed to enable relative "
                    "mouse mode: %s\n",
                    SDL_GetError());
            return;
        }
        SDL_SetWindowGrab(sdl_input_window, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowGrab(sdl_input_window, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
    }

    mouse_grabbed = grabbed;
}

static bool window_is_mouse_grabbed_sw(void)
{
    return mouse_grabbed;
}
#endif

#if SEMU_HAS(VIRTIOGPU)
static bool vgpu_format_to_sdl_format(enum virtio_gpu_formats virtio_gpu_format,
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

static void sdl_plane_info_reset(struct sdl_plane_info *plane)
{
    bool alpha_blend = plane->alpha_blend;
    if (plane->texture)
        SDL_DestroyTexture(plane->texture);
    memset(plane, 0, sizeof(*plane));
    plane->alpha_blend = alpha_blend;
}

static void sdl_plane_info_cleanup(struct sdl_plane_info *plane)
{
    if (plane->texture)
        SDL_DestroyTexture(plane->texture);
    memset(plane, 0, sizeof(*plane));
}

static void sdl_scanout_info_cleanup(struct sdl_scanout_info *scanout)
{
    sdl_plane_info_cleanup(&scanout->primary_plane);
    sdl_plane_info_cleanup(&scanout->cursor_plane);

    if (scanout->renderer)
        SDL_DestroyRenderer(scanout->renderer);
    if (scanout->window)
        SDL_DestroyWindow(scanout->window);

    memset(scanout, 0, sizeof(*scanout));
}

static bool sdl_plane_info_ensure_texture(
    SDL_Renderer *renderer,
    struct sdl_plane_info *plane,
    const struct vgpu_display_payload *payload)
{
    /* The plane keeps its SDL objects across frames, but the payload format is
     * still per-update data. Resolve the incoming VirtIO-GPU format first,
     * then adjust it below if this plane requires alpha.
     */
    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    uint32_t sdl_format;
    if (!vgpu_format_to_sdl_format(frame->format, &sdl_format)) {
        fprintf(stderr, "%s(): invalid resource format %u\n", __func__,
                (uint32_t) frame->format);
        return false;
    }

    /* Cursor textures need an alpha-capable SDL format. If the incoming format
     * is an XRGB/XBGR/BGRX/RGBX variant, switch to the matching alpha version
     * so the high byte is preserved as transparency instead of being ignored.
     */
    if (plane->alpha_blend) {
        switch (sdl_format) {
        case SDL_PIXELFORMAT_XRGB8888:
            sdl_format = SDL_PIXELFORMAT_ARGB8888;
            break;
        case SDL_PIXELFORMAT_BGRX8888:
            sdl_format = SDL_PIXELFORMAT_BGRA8888;
            break;
        case SDL_PIXELFORMAT_RGBX8888:
            sdl_format = SDL_PIXELFORMAT_RGBA8888;
            break;
        case SDL_PIXELFORMAT_XBGR8888:
            sdl_format = SDL_PIXELFORMAT_ABGR8888;
            break;
        default:
            break;
        }
    }

    /* Recreate the retained SDL texture if this frame's geometry or format
     * no longer matches the one currently cached in the plane.
     */
    if (plane->texture &&
        (plane->width != frame->width || plane->height != frame->height ||
         plane->sdl_format != sdl_format)) {
        SDL_DestroyTexture(plane->texture);
        plane->texture = NULL;
    }

    /* Create the texture on first use, or after the mismatched one above was
     * destroyed.
     */
    if (!plane->texture) {
        plane->texture =
            SDL_CreateTexture(renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING,
                              frame->width, frame->height);
        if (!plane->texture) {
            fprintf(stderr, "%s(): failed to create texture: %s\n", __func__,
                    SDL_GetError());
            return false;
        }
        if (plane->alpha_blend) {
            if (SDL_SetTextureBlendMode(plane->texture, SDL_BLENDMODE_BLEND) <
                0) {
                fprintf(stderr, "%s(): failed to enable texture blending: %s\n",
                        __func__, SDL_GetError());
            }
        }
    }

    plane->width = frame->width;
    plane->height = frame->height;
    plane->sdl_format = sdl_format;
    return true;
}

static bool sdl_scanout_apply_primary_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload)
{
    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    struct sdl_plane_info *plane = &scanout->primary_plane;

    if (!sdl_plane_info_ensure_texture(scanout->renderer, plane, payload))
        return false;

    if (SDL_UpdateTexture(plane->texture, NULL, frame->pixels, frame->stride) !=
        0) {
        fprintf(stderr, "%s(): failed to update primary texture: %s\n",
                __func__, SDL_GetError());
        return false;
    }

    return true;
}

static bool sdl_cursor_rect_update_position(SDL_Rect *rect,
                                            int32_t x,
                                            int32_t y,
                                            uint32_t hot_x,
                                            uint32_t hot_y)
{
    int64_t rect_x = (int64_t) x - (int64_t) hot_x;
    int64_t rect_y = (int64_t) y - (int64_t) hot_y;

    if (rect_x < INT_MIN || rect_x > INT_MAX || rect_y < INT_MIN ||
        rect_y > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor position out of SDL range "
                "(x=%" PRId32 " y=%" PRId32 " hot_x=%u hot_y=%u)\n",
                __func__, x, y, (unsigned) hot_x, (unsigned) hot_y);
        return false;
    }

    rect->x = (int) rect_x;
    rect->y = (int) rect_y;
    return true;
}

static bool sdl_scanout_apply_cursor_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y)
{
    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    struct sdl_plane_info *plane = &scanout->cursor_plane;
    SDL_Rect new_cursor_rect = scanout->cursor_rect;

    if (frame->width > INT_MAX || frame->height > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor size out of SDL range (%ux%u)\n",
                __func__, frame->width, frame->height);
        return false;
    }

    if (!sdl_cursor_rect_update_position(&new_cursor_rect, x, y, hot_x, hot_y))
        return false;

    if (!sdl_plane_info_ensure_texture(scanout->renderer, plane, payload))
        return false;

    if (SDL_UpdateTexture(plane->texture, NULL, frame->pixels, frame->stride) !=
        0) {
        fprintf(stderr, "%s(): failed to update cursor texture: %s\n", __func__,
                SDL_GetError());
        return false;
    }

    scanout->cursor_hot_x = hot_x;
    scanout->cursor_hot_y = hot_y;
    new_cursor_rect.w = (int) frame->width;
    new_cursor_rect.h = (int) frame->height;
    scanout->cursor_rect = new_cursor_rect;
    return true;
}

static void sdl_scanout_render(const struct sdl_scanout_info *scanout)
{
    SDL_RenderClear(scanout->renderer);

    if (scanout->primary_plane.texture)
        SDL_RenderCopy(scanout->renderer, scanout->primary_plane.texture, NULL,
                       NULL);

    if (scanout->cursor_plane.texture)
        SDL_RenderCopy(scanout->renderer, scanout->cursor_plane.texture, NULL,
                       &scanout->cursor_rect);

    SDL_RenderPresent(scanout->renderer);
}

static void window_drain_display_queue(void)
{
    bool dirty_scanouts[VIRTIO_GPU_MAX_SCANOUTS] = {0};
    struct vgpu_display_cmd cmd;

    /* Drain display bridge commands, update only SDL-owned state, then render
     * each affected scanout once. The bridge publishes reliable clear
     * generations and filters stale lossy frame/move queue entries.
     */
    while (vgpu_display_pop_cmd(&cmd)) {
        /* 'scanout_id' was validated by the guest-facing backend before the
         * command entered the display bridge.
         */
        struct sdl_scanout_info *scanout = &sdl_scanouts[cmd.scanout_id];
        if (!scanout->window || !scanout->renderer) {
            vgpu_display_release_cmd(&cmd);
            continue;
        }

        switch (cmd.type) {
        case VGPU_DISPLAY_CMD_PRIMARY_CLEAR:
            sdl_plane_info_reset(&scanout->primary_plane);
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        case VGPU_DISPLAY_CMD_CURSOR_CLEAR:
            memset(&scanout->cursor_rect, 0, sizeof(scanout->cursor_rect));
            scanout->cursor_hot_x = 0;
            scanout->cursor_hot_y = 0;
            sdl_plane_info_reset(&scanout->cursor_plane);
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        case VGPU_DISPLAY_CMD_PRIMARY_SET:
            /* Use '|=' to keep earlier dirty state for this scanout. */
            dirty_scanouts[cmd.scanout_id] |= sdl_scanout_apply_primary_frame(
                scanout, cmd.u.primary_set.payload);
            break;
        case VGPU_DISPLAY_CMD_CURSOR_SET:
            /* Use '|=' to keep earlier dirty state for this scanout. */
            dirty_scanouts[cmd.scanout_id] |= sdl_scanout_apply_cursor_frame(
                scanout, cmd.u.cursor_set.payload, cmd.u.cursor_set.x,
                cmd.u.cursor_set.y, cmd.u.cursor_set.hot_x,
                cmd.u.cursor_set.hot_y);
            break;
        case VGPU_DISPLAY_CMD_CURSOR_MOVE:
            if (!sdl_cursor_rect_update_position(
                    &scanout->cursor_rect, cmd.u.cursor_move.x,
                    cmd.u.cursor_move.y, scanout->cursor_hot_x,
                    scanout->cursor_hot_y))
                break;
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        }

        vgpu_display_release_cmd(&cmd);
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (!dirty_scanouts[i] || !sdl_scanouts[i].window ||
            !sdl_scanouts[i].renderer)
            continue;
        sdl_scanout_render(&sdl_scanouts[i]);
    }
}
#endif

/* Main loop runs on the main thread */
static void window_main_loop_sw(void)
{
    if (headless_mode) {
        /* Block until the emulator calls 'window_shutdown_sw()', so 'main()'
         * can proceed to 'pthread_join()' rather than stopping the emulator
         * immediately. There is no SDL event loop in this mode, so the main
         * thread just polls the shared close flag.
         */
        while (!window_is_closed_sw())
            usleep(10000);
        return;
    }

    /* relaxed ordering is sufficient: the only consequence of reading a stale
     * false is a few extra loop iterations. Ordering with the emulator thread
     * is provided by 'pthread_join()', not by this flag.
     */
    while (!window_is_closed_sw()) {
#if SEMU_HAS(VIRTIOINPUT)
        if (vinput_handle_events()) {
            /* User closed the window. Set the flag so 'window_shutdown_sw()'
             * (called from the emulator thread) does not race with us, then
             * return normally so 'main()' can 'pthread_join()' the emulator
             * thread and collect its exit code.
             */
            window_shutdown_sw();
            return;
        }
#else
        SDL_Event e;
        /* Without 'virtio-input', there is no SDL event pump to wake on display
         * commands. Use a short timeout so 'VIRTIOGPU'-only builds periodically
         * drain the display bridge; a future SDL user-event bridge could make
         * this fully event-driven.
         */
        if (SDL_WaitEventTimeout(&e, SDL_EVENT_WAIT_TIMEOUT_MS)) {
            uint32_t processed = 0;
            do {
                if (e.type == SDL_QUIT) {
                    window_shutdown_sw();
                    return;
                }
                processed++;
            } while (processed < SDL_EVENT_BURST_LIMIT && SDL_PollEvent(&e));
        }
#endif

#if SEMU_HAS(VIRTIOGPU)
        window_drain_display_queue();
#endif
    }
}

static void window_init_sw(uint32_t width, uint32_t height)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr,
                "window_init_sw(): failed to initialize SDL: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
#if SEMU_HAS(VIRTIOGPU)
        vgpu_display_set_unavailable();
#endif
        return;
    }
    sdl_initialized = true;

#if !SEMU_HAS(VIRTIOGPU)
    /* 'VIRTIOINPUT'-only builds do not use display dimensions; suppress
     * unused-parameter warnings.
     */
    (void) width;
    (void) height;
#endif

#if SEMU_HAS(VIRTIOGPU)
    /* The current machine setup registers exactly one scanout before calling
     * 'window_init_sw()', so materialize scanout 0 directly here. If semu grows
     * multiple scanouts later, this can be extended to iterate all registered
     * scanouts or restored to an explicit per-scanout setup path.
     */
    struct sdl_scanout_info *scanout = &sdl_scanouts[0];
    scanout->window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, width, height,
                                       SDL_WINDOW_SHOWN);
    if (!scanout->window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return;
    }

    scanout->renderer =
        SDL_CreateRenderer(scanout->window, -1, SDL_RENDERER_ACCELERATED);
    if (!scanout->renderer) {
        fprintf(stderr,
                "window_init_sw(): accelerated renderer not available, "
                "trying software renderer: %s\n",
                SDL_GetError());
        scanout->renderer =
            SDL_CreateRenderer(scanout->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!scanout->renderer) {
        fprintf(stderr,
                "window_init_sw(): failed to create renderer for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        SDL_DestroyWindow(scanout->window);
        scanout->window = NULL;
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return;
    }

    scanout->window_width = width;
    scanout->window_height = height;
    scanout->cursor_plane.alpha_blend = true;

#if SEMU_HAS(VIRTIOINPUT)
    if (!sdl_input_window) {
        sdl_input_window = scanout->window;
        fprintf(stderr,
                "semu: click window to capture mouse, Ctrl+Alt+G to "
                "release\n");
    }
#endif

    SDL_SetRenderDrawColor(scanout->renderer, 0, 0, 0, 255);
    SDL_RenderClear(scanout->renderer);
    SDL_RenderPresent(scanout->renderer);
#else
    sdl_input_window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                        SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH,
                                        SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!sdl_input_window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        return;
    }
#endif
}

static void window_cleanup_sw(void)
{
#if SEMU_HAS(VIRTIOINPUT)
    if (sdl_initialized)
        window_set_mouse_grab_sw(false);
    /* Keep cleanup idempotent when SDL was never initialized or grab release
     * returned early.
     */
    mouse_grabbed = false;
#endif

    wake_write_fd = -1;

#if SEMU_HAS(VIRTIOGPU)
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
        sdl_scanout_info_cleanup(&sdl_scanouts[i]);

    struct vgpu_display_cmd cmd;
    while (vgpu_display_pop_cmd(&cmd))
        vgpu_display_release_cmd(&cmd);
#endif

#if SEMU_HAS(VIRTIOINPUT) && !SEMU_HAS(VIRTIOGPU)
    if (sdl_input_window)
        SDL_DestroyWindow(sdl_input_window);
#endif
#if SEMU_HAS(VIRTIOINPUT)
    sdl_input_window = NULL;
#endif

    if (sdl_initialized) {
        SDL_Quit();
        sdl_initialized = false;
    }

    /* Cleanup normally runs before process exit. Reset frontend flags anyway
     * so a future re-init path cannot inherit stale headless/shutdown state.
     */
    headless_mode = false;
    should_exit = false;
}

const struct window_backend g_window = {
    .window_init = window_init_sw,
    .window_main_loop = window_main_loop_sw,
    .window_shutdown = window_shutdown_sw,
    .window_cleanup = window_cleanup_sw,
    .window_is_closed = window_is_closed_sw,
    .window_set_wake_fd = window_set_wake_fd_sw,
    .window_wake_backend = window_wake_backend_sw,
#if SEMU_HAS(VIRTIOINPUT)
    .window_set_mouse_grab = window_set_mouse_grab_sw,
    .window_is_mouse_grabbed = window_is_mouse_grabbed_sw,
#endif
};
