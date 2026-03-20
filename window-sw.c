#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "device.h"
#include "feature.h"
#include "virtio-input-event.h"
#include "window.h"

static SDL_Window *sdl_window;
static int wake_write_fd = -1;
static bool headless_mode = false;
static bool mouse_grabbed = false;
static bool should_exit = false;

/* The backend only needs the pipe's write end. The emulator owns the read end
 * and drains it after poll() returns.
 */
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

static inline void window_shutdown_sw(void)
{
    /* Both user-driven close and emulator-driven shutdown funnel through the
     * same flag so the main thread and emulator thread observe one exit state.
     */
    __atomic_store_n(&should_exit, true, __ATOMIC_RELAXED);
    /* Unblock any poll(-1) in the SMP emulator loop immediately. */
    window_wake_backend_sw();
}

static bool window_is_closed_sw(void)
{
    return __atomic_load_n(&should_exit, __ATOMIC_RELAXED);
}

/* Main-thread-only helper for relative-pointer devices. SDL's grab and
 * relative mouse APIs are part of the windowing backend, so callers use this
 * to switch between normal host-pointer mode and guest-directed mouse mode.
 */
static void window_set_mouse_grab_sw(bool grabbed)
{
    if (headless_mode || !sdl_window) {
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
        SDL_SetWindowGrab(sdl_window, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowGrab(sdl_window, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
    }

    mouse_grabbed = grabbed;
}

static bool window_is_mouse_grabbed_sw(void)
{
    return mouse_grabbed;
}

/* Main loop runs on the main thread */
static void window_main_loop_sw(void)
{
    if (headless_mode) {
        /* Block until the emulator calls window_shutdown_sw(), so main() can
         * proceed to pthread_join() rather than stopping the emulator
         * immediately. There is no SDL event loop in this mode, so the main
         * thread just polls the shared close flag.
         */
        while (!window_is_closed_sw())
            usleep(10000);
        return;
    }

    /* relaxed ordering is sufficient: the only consequence of reading a stale
     * false is a few extra loop iterations (each blocked up to 1 ms inside
     * SDL_WaitEventTimeout). Ordering with the emulator thread is provided by
     * pthread_join(), not by this flag.
     */
    while (!window_is_closed_sw()) {
        if (vinput_handle_events()) {
            /* User closed the window. Set the flag so window_shutdown_sw()
             * (called from the emulator thread) does not race with us, then
             * return normally so main() can pthread_join the emulator thread
             * and collect its exit code.
             */
            window_shutdown_sw();
            return;
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

    sdl_window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH,
                                  SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!sdl_window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        return;
    }

    fprintf(stderr,
            "semu: click window to capture mouse, Ctrl+Alt+G to "
            "release\n");
}

const struct window_backend g_window = {
    .window_init = window_init_sw,
    .window_main_loop = window_main_loop_sw,
    .window_shutdown = window_shutdown_sw,
    .window_is_closed = window_is_closed_sw,
    .window_set_wake_fd = window_set_wake_fd_sw,
    .window_wake_backend = window_wake_backend_sw,
    .window_set_mouse_grab = window_set_mouse_grab_sw,
    .window_is_mouse_grabbed = window_is_mouse_grabbed_sw,
};
