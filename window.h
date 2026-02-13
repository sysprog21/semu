#pragma once

#if SEMU_HAS(VIRTIOGPU)
/* Cursor size is always 64*64 in VirtIO GPU */
#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64

#define CURSOR_BPP 4 /* Bytes per pixel, using ARGB */
#define CURSOR_STRIDE (CURSOR_WIDTH * CURSOR_BPP)

struct window_backend {
    void (*window_init)(void);
    void (*window_add)(uint32_t width, uint32_t height);
    void (*window_set_scanout)(int scanout_id, uint32_t texture_id);
    void (*window_clear)(int scanout_id);
    void (*window_flush)(int scanout_id, int res_id);
    void (*cursor_clear)(int scanout_id);
    void (*cursor_update)(int scanout_id, int res_id, int x, int y);
    void (*cursor_move)(int scanout_id, int x, int y);
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
};

#if SEMU_HAS(VIRTIOINPUT)
bool handle_window_events(void);
#endif

#endif
