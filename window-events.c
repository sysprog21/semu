#include <SDL.h>
#include <SDL_thread.h>

#include "device.h"
#include "input-event-codes.h"

#define DEF_KEY_MAP(_sdl_key, _linux_key)            \
    {                                                \
        .sdl_key = _sdl_key, .linux_key = _linux_key \
    }

struct key_map_entry {
    int sdl_key;
    int linux_key;
};

struct key_map_entry key_map[] = {
    /* Mouse */
    DEF_KEY_MAP(SDL_BUTTON_LEFT, BTN_LEFT),
    DEF_KEY_MAP(SDL_BUTTON_RIGHT, BTN_RIGHT),
    DEF_KEY_MAP(SDL_BUTTON_MIDDLE, BTN_MIDDLE),
    /* Keyboard */
    DEF_KEY_MAP(SDLK_ESCAPE, KEY_ESC),
    DEF_KEY_MAP(SDLK_1, KEY_1),
    DEF_KEY_MAP(SDLK_2, KEY_2),
    DEF_KEY_MAP(SDLK_3, KEY_3),
    DEF_KEY_MAP(SDLK_4, KEY_4),
    DEF_KEY_MAP(SDLK_5, KEY_5),
    DEF_KEY_MAP(SDLK_6, KEY_6),
    DEF_KEY_MAP(SDLK_7, KEY_7),
    DEF_KEY_MAP(SDLK_8, KEY_8),
    DEF_KEY_MAP(SDLK_9, KEY_9),
    DEF_KEY_MAP(SDLK_0, KEY_0),
    DEF_KEY_MAP(SDLK_MINUS, KEY_MINUS),
    DEF_KEY_MAP(SDLK_EQUALS, KEY_EQUAL),
    DEF_KEY_MAP(SDLK_BACKSPACE, KEY_BACKSPACE),
    DEF_KEY_MAP(SDLK_TAB, KEY_TAB),
    DEF_KEY_MAP(SDLK_q, KEY_Q),
    DEF_KEY_MAP(SDLK_w, KEY_W),
    DEF_KEY_MAP(SDLK_e, KEY_E),
    DEF_KEY_MAP(SDLK_r, KEY_R),
    DEF_KEY_MAP(SDLK_t, KEY_T),
    DEF_KEY_MAP(SDLK_y, KEY_Y),
    DEF_KEY_MAP(SDLK_u, KEY_U),
    DEF_KEY_MAP(SDLK_i, KEY_I),
    DEF_KEY_MAP(SDLK_o, KEY_O),
    DEF_KEY_MAP(SDLK_p, KEY_P),
    DEF_KEY_MAP(SDLK_LEFTBRACKET, KEY_LEFTBRACE),
    DEF_KEY_MAP(SDLK_RIGHTBRACKET, KEY_RIGHTBRACE),
    DEF_KEY_MAP(SDLK_RETURN, KEY_ENTER),
    DEF_KEY_MAP(SDLK_LCTRL, KEY_LEFTCTRL),
    DEF_KEY_MAP(SDLK_a, KEY_A),
    DEF_KEY_MAP(SDLK_s, KEY_S),
    DEF_KEY_MAP(SDLK_d, KEY_D),
    DEF_KEY_MAP(SDLK_f, KEY_F),
    DEF_KEY_MAP(SDLK_g, KEY_G),
    DEF_KEY_MAP(SDLK_h, KEY_H),
    DEF_KEY_MAP(SDLK_j, KEY_J),
    DEF_KEY_MAP(SDLK_k, KEY_K),
    DEF_KEY_MAP(SDLK_l, KEY_L),
    DEF_KEY_MAP(SDLK_SEMICOLON, KEY_SEMICOLON),
    DEF_KEY_MAP(SDLK_QUOTE, KEY_APOSTROPHE),
    DEF_KEY_MAP(SDLK_BACKQUOTE, KEY_GRAVE),
    DEF_KEY_MAP(SDLK_LSHIFT, KEY_LEFTSHIFT),
    DEF_KEY_MAP(SDLK_BACKSLASH, KEY_BACKSLASH),
    DEF_KEY_MAP(SDLK_z, KEY_Z),
    DEF_KEY_MAP(SDLK_x, KEY_X),
    DEF_KEY_MAP(SDLK_c, KEY_C),
    DEF_KEY_MAP(SDLK_v, KEY_V),
    DEF_KEY_MAP(SDLK_b, KEY_B),
    DEF_KEY_MAP(SDLK_n, KEY_N),
    DEF_KEY_MAP(SDLK_m, KEY_M),
    DEF_KEY_MAP(SDLK_COMMA, KEY_COMMA),
    DEF_KEY_MAP(SDLK_PERIOD, KEY_DOT),
    DEF_KEY_MAP(SDLK_SLASH, KEY_SLASH),
    DEF_KEY_MAP(SDLK_RSHIFT, KEY_RIGHTSHIFT),
    DEF_KEY_MAP(SDLK_LALT, KEY_LEFTALT),
    DEF_KEY_MAP(SDLK_SPACE, KEY_SPACE),
    DEF_KEY_MAP(SDLK_CAPSLOCK, KEY_CAPSLOCK),
    DEF_KEY_MAP(SDLK_F1, KEY_F1),
    DEF_KEY_MAP(SDLK_F2, KEY_F2),
    DEF_KEY_MAP(SDLK_F3, KEY_F3),
    DEF_KEY_MAP(SDLK_F4, KEY_F4),
    DEF_KEY_MAP(SDLK_F5, KEY_F5),
    DEF_KEY_MAP(SDLK_F6, KEY_F6),
    DEF_KEY_MAP(SDLK_F7, KEY_F7),
    DEF_KEY_MAP(SDLK_F8, KEY_F8),
    DEF_KEY_MAP(SDLK_F9, KEY_F9),
    DEF_KEY_MAP(SDLK_F10, KEY_F10),
    DEF_KEY_MAP(SDLK_NUMLOCKCLEAR, KEY_NUMLOCK),
    DEF_KEY_MAP(SDLK_SCROLLLOCK, KEY_SCROLLLOCK),
    DEF_KEY_MAP(SDLK_KP_7, KEY_KP7),
    DEF_KEY_MAP(SDLK_KP_8, KEY_KP8),
    DEF_KEY_MAP(SDLK_KP_9, KEY_KP9),
    DEF_KEY_MAP(SDLK_KP_MULTIPLY, KEY_KPASTERISK),
    DEF_KEY_MAP(SDLK_KP_MINUS, KEY_KPMINUS),
    DEF_KEY_MAP(SDLK_KP_4, KEY_KP4),
    DEF_KEY_MAP(SDLK_KP_5, KEY_KP5),
    DEF_KEY_MAP(SDLK_KP_6, KEY_KP6),
    DEF_KEY_MAP(SDLK_KP_PLUS, KEY_KPPLUS),
    DEF_KEY_MAP(SDLK_KP_1, KEY_KP1),
    DEF_KEY_MAP(SDLK_KP_2, KEY_KP2),
    DEF_KEY_MAP(SDLK_KP_3, KEY_KP3),
    DEF_KEY_MAP(SDLK_KP_0, KEY_KP0),
    DEF_KEY_MAP(SDLK_KP_PERIOD, KEY_KPDOT),
    DEF_KEY_MAP(SDLK_F11, KEY_F11),
    DEF_KEY_MAP(SDLK_F12, KEY_F12),
    DEF_KEY_MAP(SDLK_KP_ENTER, KEY_KPENTER),
    DEF_KEY_MAP(SDLK_KP_DIVIDE, KEY_KPSLASH),
    DEF_KEY_MAP(SDLK_RCTRL, KEY_RIGHTCTRL),
    DEF_KEY_MAP(SDLK_RALT, KEY_RIGHTALT),
    DEF_KEY_MAP(SDLK_HOME, KEY_HOME),
    DEF_KEY_MAP(SDLK_UP, KEY_UP),
    DEF_KEY_MAP(SDLK_PAGEUP, KEY_PAGEUP),
    DEF_KEY_MAP(SDLK_LEFT, KEY_LEFT),
    DEF_KEY_MAP(SDLK_RIGHT, KEY_RIGHT),
    DEF_KEY_MAP(SDLK_END, KEY_END),
    DEF_KEY_MAP(SDLK_DOWN, KEY_DOWN),
    DEF_KEY_MAP(SDLK_PAGEDOWN, KEY_PAGEDOWN),
    DEF_KEY_MAP(SDLK_INSERT, KEY_INSERT),
    DEF_KEY_MAP(SDLK_DELETE, KEY_DELETE),
};

static int sdl_key_to_linux_key(int sdl_key)
{
    unsigned long key_cnt = sizeof(key_map) / sizeof(struct key_map_entry);
    for (unsigned long i = 0; i < key_cnt; i++)
        if (sdl_key == key_map[i].sdl_key)
            return key_map[i].linux_key;

    return -1;
}

int window_events_thread(void *data)
{
    (void) data;

    int linux_key;

    while (1) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            continue;

        switch (e.type) {
        case SDL_QUIT:
            exit(0);
        case SDL_KEYDOWN:
            linux_key = sdl_key_to_linux_key(e.key.keysym.sym);
            if (linux_key >= 0)
                virtio_input_update_key(linux_key, 1);
            break;
        case SDL_KEYUP:
            linux_key = sdl_key_to_linux_key(e.key.keysym.sym);
            if (linux_key >= 0)
                virtio_input_update_key(linux_key, 0);
            break;
        case SDL_MOUSEBUTTONDOWN:
            linux_key = sdl_key_to_linux_key(e.button.button);
            if (linux_key >= 0)
                virtio_input_update_mouse_button_state(linux_key, true);
            break;
        case SDL_MOUSEBUTTONUP:
            linux_key = sdl_key_to_linux_key(e.button.button);
            if (linux_key >= 0)
                virtio_input_update_mouse_button_state(linux_key, false);
            break;
        case SDL_MOUSEMOTION:
            virtio_input_update_cursor(e.motion.x, e.motion.y);
            break;
        }
    }
}
