#pragma once

#include <stdbool.h>
#include <stdint.h>

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)

struct window_backend {
    /* When headless is true, the backend skips SDL_Init / window creation and
     * behaves as if SDL had failed -- useful for batch runs (CI, 'make check')
     * that have no display attached.
     * The caller also passes the default SDL window size. VirtIO-GPU builds
     * use it as the initial scanout size; input-only builds use it for the
     * grab target window because they do not have a display mode of their own.
     */
    void (*window_init)(bool headless, uint32_t width, uint32_t height);
    /* Main loop function that runs on the main thread. If non-NULL, the
     * emulator runs in a background thread while this function handles window
     * events on the main thread.
     * Returns when the emulator should exit.
     */
    void (*window_main_loop)(void);
    /* Called from the emulator thread when 'semu_run()' returns, to unblock
     * 'window_main_loop()' so the main thread can proceed to 'pthread_join()'.
     */
    void (*window_shutdown)(void);
    /* Release frontend resources after the emulator producer has stopped, or
     * after initialization fails before the producer starts.
     */
    void (*window_cleanup)(void);
    /* Returns true once the window has been closed (or initialization fell
     * back to headless mode). Safe to call from any thread.
     */
    bool (*window_is_closed)(void);
    /* Register the write end of the wake pipe used to break the emulator
     * thread out of 'poll(-1)' when the backend queues work for it.
     */
    void (*window_set_wake_fd)(int fd);
    /* Best-effort wakeup hook for the emulator thread. The backend uses this
     * after queuing work such as input events or shutdown requests.
     */
    void (*window_wake_backend)(void);
#if SEMU_HAS(VIRTIOINPUT)
    /* Switch the backend between normal host-pointer mode and grabbed
     * relative-pointer mode. Must be called from the main thread because it
     * touches window-system state directly.
     */
    void (*window_set_mouse_grab)(bool grabbed);
    /* Returns true once the backend currently owns the host mouse grab. */
    bool (*window_is_mouse_grabbed)(void);
#endif /* SEMU_HAS(VIRTIOINPUT) */
};

extern const struct window_backend g_window;
#endif /* SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU) */
