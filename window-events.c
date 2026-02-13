#include <SDL.h>
#include <stdbool.h>

#define SDL_COND_TIMEOUT 1 /* ms */

#include "device.h"
#include "virtio-input-codes.h"
#include "window.h"

#define DEF_KEY_MAP(_sdl_scancode, _linux_key)                 \
    {                                                          \
        .sdl_scancode = _sdl_scancode, .linux_key = _linux_key \
    }

struct key_map_entry {
    int sdl_scancode;
    int linux_key;
};

static struct key_map_entry key_map[] = {
    /* Keyboard */
    DEF_KEY_MAP(SDL_SCANCODE_ESCAPE, SEMU_KEY_ESC),
    DEF_KEY_MAP(SDL_SCANCODE_1, SEMU_KEY_1),
    DEF_KEY_MAP(SDL_SCANCODE_2, SEMU_KEY_2),
    DEF_KEY_MAP(SDL_SCANCODE_3, SEMU_KEY_3),
    DEF_KEY_MAP(SDL_SCANCODE_4, SEMU_KEY_4),
    DEF_KEY_MAP(SDL_SCANCODE_5, SEMU_KEY_5),
    DEF_KEY_MAP(SDL_SCANCODE_6, SEMU_KEY_6),
    DEF_KEY_MAP(SDL_SCANCODE_7, SEMU_KEY_7),
    DEF_KEY_MAP(SDL_SCANCODE_8, SEMU_KEY_8),
    DEF_KEY_MAP(SDL_SCANCODE_9, SEMU_KEY_9),
    DEF_KEY_MAP(SDL_SCANCODE_0, SEMU_KEY_0),
    DEF_KEY_MAP(SDL_SCANCODE_MINUS, SEMU_KEY_MINUS),
    DEF_KEY_MAP(SDL_SCANCODE_EQUALS, SEMU_KEY_EQUAL),
    DEF_KEY_MAP(SDL_SCANCODE_BACKSPACE, SEMU_KEY_BACKSPACE),
    DEF_KEY_MAP(SDL_SCANCODE_TAB, SEMU_KEY_TAB),
    DEF_KEY_MAP(SDL_SCANCODE_Q, SEMU_KEY_Q),
    DEF_KEY_MAP(SDL_SCANCODE_W, SEMU_KEY_W),
    DEF_KEY_MAP(SDL_SCANCODE_E, SEMU_KEY_E),
    DEF_KEY_MAP(SDL_SCANCODE_R, SEMU_KEY_R),
    DEF_KEY_MAP(SDL_SCANCODE_T, SEMU_KEY_T),
    DEF_KEY_MAP(SDL_SCANCODE_Y, SEMU_KEY_Y),
    DEF_KEY_MAP(SDL_SCANCODE_U, SEMU_KEY_U),
    DEF_KEY_MAP(SDL_SCANCODE_I, SEMU_KEY_I),
    DEF_KEY_MAP(SDL_SCANCODE_O, SEMU_KEY_O),
    DEF_KEY_MAP(SDL_SCANCODE_P, SEMU_KEY_P),
    DEF_KEY_MAP(SDL_SCANCODE_LEFTBRACKET, SEMU_KEY_LEFTBRACE),
    DEF_KEY_MAP(SDL_SCANCODE_RIGHTBRACKET, SEMU_KEY_RIGHTBRACE),
    DEF_KEY_MAP(SDL_SCANCODE_RETURN, SEMU_KEY_ENTER),
    DEF_KEY_MAP(SDL_SCANCODE_LCTRL, SEMU_KEY_LEFTCTRL),
    DEF_KEY_MAP(SDL_SCANCODE_A, SEMU_KEY_A),
    DEF_KEY_MAP(SDL_SCANCODE_S, SEMU_KEY_S),
    DEF_KEY_MAP(SDL_SCANCODE_D, SEMU_KEY_D),
    DEF_KEY_MAP(SDL_SCANCODE_F, SEMU_KEY_F),
    DEF_KEY_MAP(SDL_SCANCODE_G, SEMU_KEY_G),
    DEF_KEY_MAP(SDL_SCANCODE_H, SEMU_KEY_H),
    DEF_KEY_MAP(SDL_SCANCODE_J, SEMU_KEY_J),
    DEF_KEY_MAP(SDL_SCANCODE_K, SEMU_KEY_K),
    DEF_KEY_MAP(SDL_SCANCODE_L, SEMU_KEY_L),
    DEF_KEY_MAP(SDL_SCANCODE_SEMICOLON, SEMU_KEY_SEMICOLON),
    DEF_KEY_MAP(SDL_SCANCODE_APOSTROPHE, SEMU_KEY_APOSTROPHE),
    DEF_KEY_MAP(SDL_SCANCODE_GRAVE, SEMU_KEY_GRAVE),
    DEF_KEY_MAP(SDL_SCANCODE_LSHIFT, SEMU_KEY_LEFTSHIFT),
    DEF_KEY_MAP(SDL_SCANCODE_BACKSLASH, SEMU_KEY_BACKSLASH),
    DEF_KEY_MAP(SDL_SCANCODE_Z, SEMU_KEY_Z),
    DEF_KEY_MAP(SDL_SCANCODE_X, SEMU_KEY_X),
    DEF_KEY_MAP(SDL_SCANCODE_C, SEMU_KEY_C),
    DEF_KEY_MAP(SDL_SCANCODE_V, SEMU_KEY_V),
    DEF_KEY_MAP(SDL_SCANCODE_B, SEMU_KEY_B),
    DEF_KEY_MAP(SDL_SCANCODE_N, SEMU_KEY_N),
    DEF_KEY_MAP(SDL_SCANCODE_M, SEMU_KEY_M),
    DEF_KEY_MAP(SDL_SCANCODE_COMMA, SEMU_KEY_COMMA),
    DEF_KEY_MAP(SDL_SCANCODE_PERIOD, SEMU_KEY_DOT),
    DEF_KEY_MAP(SDL_SCANCODE_SLASH, SEMU_KEY_SLASH),
    DEF_KEY_MAP(SDL_SCANCODE_RSHIFT, SEMU_KEY_RIGHTSHIFT),
    DEF_KEY_MAP(SDL_SCANCODE_LALT, SEMU_KEY_LEFTALT),
    DEF_KEY_MAP(SDL_SCANCODE_SPACE, SEMU_KEY_SPACE),
    DEF_KEY_MAP(SDL_SCANCODE_CAPSLOCK, SEMU_KEY_CAPSLOCK),
    DEF_KEY_MAP(SDL_SCANCODE_F1, SEMU_KEY_F1),
    DEF_KEY_MAP(SDL_SCANCODE_F2, SEMU_KEY_F2),
    DEF_KEY_MAP(SDL_SCANCODE_F3, SEMU_KEY_F3),
    DEF_KEY_MAP(SDL_SCANCODE_F4, SEMU_KEY_F4),
    DEF_KEY_MAP(SDL_SCANCODE_F5, SEMU_KEY_F5),
    DEF_KEY_MAP(SDL_SCANCODE_F6, SEMU_KEY_F6),
    DEF_KEY_MAP(SDL_SCANCODE_F7, SEMU_KEY_F7),
    DEF_KEY_MAP(SDL_SCANCODE_F8, SEMU_KEY_F8),
    DEF_KEY_MAP(SDL_SCANCODE_F9, SEMU_KEY_F9),
    DEF_KEY_MAP(SDL_SCANCODE_F10, SEMU_KEY_F10),
    DEF_KEY_MAP(SDL_SCANCODE_NUMLOCKCLEAR, SEMU_KEY_NUMLOCK),
    DEF_KEY_MAP(SDL_SCANCODE_SCROLLLOCK, SEMU_KEY_SCROLLLOCK),
    DEF_KEY_MAP(SDL_SCANCODE_KP_7, SEMU_KEY_KP7),
    DEF_KEY_MAP(SDL_SCANCODE_KP_8, SEMU_KEY_KP8),
    DEF_KEY_MAP(SDL_SCANCODE_KP_9, SEMU_KEY_KP9),
    DEF_KEY_MAP(SDL_SCANCODE_KP_MULTIPLY, SEMU_KEY_KPASTERISK),
    DEF_KEY_MAP(SDL_SCANCODE_KP_MINUS, SEMU_KEY_KPMINUS),
    DEF_KEY_MAP(SDL_SCANCODE_KP_4, SEMU_KEY_KP4),
    DEF_KEY_MAP(SDL_SCANCODE_KP_5, SEMU_KEY_KP5),
    DEF_KEY_MAP(SDL_SCANCODE_KP_6, SEMU_KEY_KP6),
    DEF_KEY_MAP(SDL_SCANCODE_KP_PLUS, SEMU_KEY_KPPLUS),
    DEF_KEY_MAP(SDL_SCANCODE_KP_1, SEMU_KEY_KP1),
    DEF_KEY_MAP(SDL_SCANCODE_KP_2, SEMU_KEY_KP2),
    DEF_KEY_MAP(SDL_SCANCODE_KP_3, SEMU_KEY_KP3),
    DEF_KEY_MAP(SDL_SCANCODE_KP_0, SEMU_KEY_KP0),
    DEF_KEY_MAP(SDL_SCANCODE_KP_PERIOD, SEMU_KEY_KPDOT),
    DEF_KEY_MAP(SDL_SCANCODE_F11, SEMU_KEY_F11),
    DEF_KEY_MAP(SDL_SCANCODE_F12, SEMU_KEY_F12),
    DEF_KEY_MAP(SDL_SCANCODE_KP_ENTER, SEMU_KEY_KPENTER),
    DEF_KEY_MAP(SDL_SCANCODE_KP_DIVIDE, SEMU_KEY_KPSLASH),
    DEF_KEY_MAP(SDL_SCANCODE_RCTRL, SEMU_KEY_RIGHTCTRL),
    DEF_KEY_MAP(SDL_SCANCODE_RALT, SEMU_KEY_RIGHTALT),
    DEF_KEY_MAP(SDL_SCANCODE_HOME, SEMU_KEY_HOME),
    DEF_KEY_MAP(SDL_SCANCODE_UP, SEMU_KEY_UP),
    DEF_KEY_MAP(SDL_SCANCODE_PAGEUP, SEMU_KEY_PAGEUP),
    DEF_KEY_MAP(SDL_SCANCODE_LEFT, SEMU_KEY_LEFT),
    DEF_KEY_MAP(SDL_SCANCODE_RIGHT, SEMU_KEY_RIGHT),
    DEF_KEY_MAP(SDL_SCANCODE_END, SEMU_KEY_END),
    DEF_KEY_MAP(SDL_SCANCODE_DOWN, SEMU_KEY_DOWN),
    DEF_KEY_MAP(SDL_SCANCODE_PAGEDOWN, SEMU_KEY_PAGEDOWN),
    DEF_KEY_MAP(SDL_SCANCODE_INSERT, SEMU_KEY_INSERT),
    DEF_KEY_MAP(SDL_SCANCODE_DELETE, SEMU_KEY_DELETE),
};

/* Mouse button mapping uses SDL button IDs, not scancodes */
static int sdl_button_to_linux_key(int sdl_button)
{
    switch (sdl_button) {
    case SDL_BUTTON_LEFT:
        return SEMU_BTN_LEFT;
    case SDL_BUTTON_RIGHT:
        return SEMU_BTN_RIGHT;
    case SDL_BUTTON_MIDDLE:
        return SEMU_BTN_MIDDLE;
    default:
        return -1;
    }
}

/* TODO: The current implementation has an O(n) time complexity, which should be
 * optimizable using a hash table or some lookup table.
 */
static int sdl_scancode_to_linux_key(int sdl_scancode)
{
    unsigned long key_cnt = sizeof(key_map) / sizeof(struct key_map_entry);
    for (unsigned long i = 0; i < key_cnt; i++)
        if (sdl_scancode == key_map[i].sdl_scancode)
            return key_map[i].linux_key;

    return -1;
}

bool handle_window_events(void)
{
    SDL_Event e;
    int linux_key;
    bool quit = false;

    while (SDL_WaitEventTimeout(&e, SDL_COND_TIMEOUT)) {
        switch (e.type) {
        case SDL_QUIT:
            quit = true;
            break;
        case SDL_KEYDOWN:
            linux_key = sdl_scancode_to_linux_key(e.key.keysym.scancode);
            if (linux_key >= 0)
                virtio_input_update_key(linux_key, 1);
            break;
        case SDL_KEYUP:
            linux_key = sdl_scancode_to_linux_key(e.key.keysym.scancode);
            if (linux_key >= 0)
                virtio_input_update_key(linux_key, 0);
            break;
        case SDL_MOUSEBUTTONDOWN:
            linux_key = sdl_button_to_linux_key(e.button.button);
            if (linux_key >= 0)
                virtio_input_update_mouse_button_state(linux_key, true);
            break;
        case SDL_MOUSEBUTTONUP:
            linux_key = sdl_button_to_linux_key(e.button.button);
            if (linux_key >= 0)
                virtio_input_update_mouse_button_state(linux_key, false);
            break;
        case SDL_MOUSEMOTION:
            virtio_input_update_cursor(e.motion.x, e.motion.y);
            break;
        }
    }

    return quit;
}
