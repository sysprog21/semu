#pragma once

#include <stdbool.h>

#include "feature.h"

#if SEMU_HAS(VIRTIOINPUT)
struct window_backend {
    /* When headless is true, the backend skips SDL_Init / window creation and
     * behaves as if SDL had failed -- useful for batch runs (CI, 'make check')
     * that have no display attached.
     */
    void (*window_init)(bool headless);
    /* Main loop function that runs on the main thread (for macOS SDL2).
     * If non-NULL, the emulator runs in a background thread while this
     * function handles window events on the main thread.
     * Returns when the emulator should exit.
     */
    void (*window_main_loop)(void);
    /* Called from the emulator thread when semu_run() returns, to unblock
     * window_main_loop() so the main thread can proceed to pthread_join.
     */
    void (*window_shutdown)(void);
    /* Returns true once the window has been closed (or SDL failed to
     * initialize). Safe to call from any thread.
     */
    bool (*window_is_closed)(void);
    /* Register the write end of a pipe to be written when the window shuts
     * down. Must be called before window_main_loop().
     */
    void (*window_set_wake_fd)(int fd);
    /* Best-effort wakeup hook for the backend self-pipe. */
    void (*window_wake_backend)(void);
    /* Enable or disable SDL's relative mouse mode for the frontend window.
     * When this returns with grab enabled, pointer motion is reported as
     * relative deltas, the host cursor is hidden, and SDL confines the
     * pointer to the semu window until the grab is released again.
     */
    void (*window_set_mouse_grab)(bool grabbed);
    /* Returns true once the frontend window currently owns the host mouse
     * grab. Safe to call from the main thread while translating SDL events.
     */
    bool (*window_is_mouse_grabbed)(void);
};

extern const struct window_backend g_window;
#endif
