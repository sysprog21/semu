#include <SDL.h>
#include <stdbool.h>

#include "device.h"
#include "virtio-input-codes.h"
#include "virtio-input-event.h"
#include "window.h"

#define VINPUT_CMD_QUEUE_SIZE 1024U
#define VINPUT_CMD_QUEUE_MASK (VINPUT_CMD_QUEUE_SIZE - 1U)

#define VINPUT_SDL_EVENT_WAIT_TIMEOUT_MS 1 /* ms */
#define VINPUT_SDL_EVENT_BURST_LIMIT 64U

#define DEF_KEY_MAP(_sdl_scancode, _linux_key) \
    {.sdl_scancode = _sdl_scancode, .linux_key = _linux_key}

struct vinput_key_map_entry {
    int sdl_scancode;
    int linux_key;
};

/* Per-device SPSC queue. The queue stays entirely on the host side so SDL
 * never touches guest-facing virtio-input state directly. Each virtio-input
 * device gets its own queue so that resetting one device (on guest Status=0)
 * does not drop pending events destined for the other device.
 */
struct vinput_cmd_queue {
    struct vinput_cmd entries[VINPUT_CMD_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
};

static struct vinput_cmd_queue vinput_cmd_queues[VINPUT_DEV_CNT];

/* Single wake gate across all device queues. The emulator drains every queue
 * after one pipe wake-up, so coalescing through a single gate is enough.
 */
static bool vinput_cmd_wake_pending;

static struct vinput_key_map_entry vinput_key_map[] = {
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

static bool vinput_push_cmd(int dev_id, const struct vinput_cmd *event)
{
    struct vinput_cmd_queue *queue = &vinput_cmd_queues[dev_id];
    uint32_t head = __atomic_load_n(&queue->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&queue->tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VINPUT_CMD_QUEUE_MASK;

    /* Keep the producer non-blocking. If the queue is full, the newest event
     * is dropped. This remains intentionally lossy even for key/button events,
     * which means a sustained overflow can lose a release edge. We keep that
     * tradeoff explicit here rather than synthesizing corrective events.
     */
    if (next == tail)
        return false;

    queue->entries[head] = *event;
    __atomic_store_n(&queue->head, next, __ATOMIC_RELEASE);

    /* Coalesce wakeups across a whole drain batch. The producer only writes to
     * the wake pipe when transitioning wake_pending false -> true and the
     * consumer clears it after draining queued events and rechecks for races.
     *
     * SEQ_CST on this exchange pairs with the SEQ_CST store in
     * vinput_rearm_cmd_wake(). The total order guarantees that if this
     * exchange reads the stale "true", the consumer's later reads of the
     * queue head/tail will observe the store above. Without it, weakly-
     * ordered architectures can lose a wake-up.
     */
    if (!__atomic_exchange_n(&vinput_cmd_wake_pending, true, __ATOMIC_SEQ_CST))
        g_window.window_wake_backend();

    return true;
}

static bool vinput_all_queues_empty(void)
{
    for (int i = 0; i < VINPUT_DEV_CNT; i++) {
        struct vinput_cmd_queue *queue = &vinput_cmd_queues[i];
        uint32_t tail = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);
        uint32_t head = __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE);
        if (tail != head)
            return false;
    }
    return true;
}

/* Mouse button mapping uses SDL button IDs, not scancodes */
static int vinput_sdl_button_to_linux_key(int sdl_button)
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
static int vinput_sdl_scancode_to_linux_key(int sdl_scancode)
{
    unsigned long key_cnt =
        sizeof(vinput_key_map) / sizeof(struct vinput_key_map_entry);
    for (unsigned long i = 0; i < key_cnt; i++)
        if (sdl_scancode == vinput_key_map[i].sdl_scancode)
            return vinput_key_map[i].linux_key;

    return -1;
}

bool vinput_pop_cmd(int dev_id, struct vinput_cmd *event)
{
    /* Consumer-side dequeue. Called from the emulator thread after poll()
     * wakes, and also from the periodic peripheral tick while work remains.
     */
    struct vinput_cmd_queue *queue = &vinput_cmd_queues[dev_id];
    uint32_t tail = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE);

    if (tail == head)
        return false;

    *event = queue->entries[tail];
    tail = (tail + 1U) & VINPUT_CMD_QUEUE_MASK;
    __atomic_store_n(&queue->tail, tail, __ATOMIC_RELEASE);

    return true;
}

bool vinput_rearm_cmd_wake(void)
{
    /* Clear wake_pending only after the current batch has been drained. If the
     * producer published while wake_pending was still true, one of the queues
     * will be non-empty here and the consumer must keep draining instead of
     * returning to poll().
     *
     * SEQ_CST pairs with the SEQ_CST exchange in vinput_push_cmd(). See the
     * note there: without a total order, the producer can read a stale "true"
     * while this thread reads stale empty queues, losing the wake-up.
     */
    __atomic_store_n(&vinput_cmd_wake_pending, false, __ATOMIC_SEQ_CST);
    return vinput_all_queues_empty();
}

bool vinput_may_have_pending_cmds(void)
{
    return __atomic_load_n(&vinput_cmd_wake_pending, __ATOMIC_RELAXED);
}

void vinput_reset_host_events(int dev_id)
{
    /* Drop every pending event for this device only. The other device's queue
     * is left intact.
     */
    struct vinput_cmd event;
    while (vinput_pop_cmd(dev_id, &event))
        ;

    /* Restore the wake-gate invariant: wake_pending true means a pipe byte is
     * in flight, or the consumer has not rearmed yet.
     *
     * Reset can run on the emulator thread between main.c consuming the pipe
     * byte and the next emu_tick_peripherals() drain. If we left
     * wake_pending=true with no backing pipe byte and no events for this
     * device to process, a later producer push would see wake_pending=true and
     * skip its pipe write, and the emulator could block in poll(-1)
     * indefinitely.
     *
     * Mirror the producer's rearm idiom: clear the gate, then if the other
     * device still has work, re-arm the gate with a fresh pipe byte so the
     * consumer is guaranteed to be woken and drain it next tick.
     */
    __atomic_store_n(&vinput_cmd_wake_pending, false, __ATOMIC_SEQ_CST);
    if (!vinput_all_queues_empty() &&
        !__atomic_exchange_n(&vinput_cmd_wake_pending, true,
                             __ATOMIC_SEQ_CST)) {
        g_window.window_wake_backend();
    }
}

bool vinput_handle_events(void)
{
    SDL_Event e;
    uint32_t processed = 0;
    int linux_key;

    /* SDL stays on the main thread. Wait for one event, then drain a bounded
     * burst so the window loop can still return to GPU display work under
     * continuous input traffic.
     */
    if (!SDL_WaitEventTimeout(&e, VINPUT_SDL_EVENT_WAIT_TIMEOUT_MS))
        return false;

    do {
        switch (e.type) {
        case SDL_QUIT:
            return true;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                g_window.window_set_mouse_grab(false);
            break;
        case SDL_KEYDOWN:
            if (g_window.window_is_mouse_grabbed() &&
                e.key.keysym.scancode == SDL_SCANCODE_G &&
                (e.key.keysym.mod & KMOD_CTRL) &&
                (e.key.keysym.mod & KMOD_ALT)) {
                g_window.window_set_mouse_grab(false);
                break;
            }
            /* EV_REP is advertised, so the guest kernel drives key repeat.
             * Drop host autorepeat events to avoid double repeat.
             */
            if (e.key.repeat)
                break;
            linux_key = vinput_sdl_scancode_to_linux_key(e.key.keysym.scancode);
            if (linux_key >= 0) {
                struct vinput_cmd event = {
                    .type = VINPUT_CMD_KEYBOARD_KEY,
                    .u.keyboard_key = {.key = (uint32_t) linux_key, .value = 1},
                };
                vinput_push_cmd(VINPUT_KEYBOARD_ID, &event);
            }
            break;
        case SDL_KEYUP:
            linux_key = vinput_sdl_scancode_to_linux_key(e.key.keysym.scancode);
            if (linux_key >= 0) {
                struct vinput_cmd event = {
                    .type = VINPUT_CMD_KEYBOARD_KEY,
                    .u.keyboard_key = {.key = (uint32_t) linux_key, .value = 0},
                };
                vinput_push_cmd(VINPUT_KEYBOARD_ID, &event);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            g_window.window_set_mouse_grab(true);
            linux_key = vinput_sdl_button_to_linux_key(e.button.button);
            if (linux_key >= 0) {
                struct vinput_cmd event = {
                    .type = VINPUT_CMD_MOUSE_BUTTON,
                    .u.mouse_button = {.button = (uint32_t) linux_key,
                                       .pressed = true},
                };
                vinput_push_cmd(VINPUT_MOUSE_ID, &event);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            linux_key = vinput_sdl_button_to_linux_key(e.button.button);
            if (linux_key >= 0) {
                struct vinput_cmd event = {
                    .type = VINPUT_CMD_MOUSE_BUTTON,
                    .u.mouse_button = {.button = (uint32_t) linux_key,
                                       .pressed = false},
                };
                vinput_push_cmd(VINPUT_MOUSE_ID, &event);
            }
            break;
        case SDL_MOUSEMOTION: {
            if (!g_window.window_is_mouse_grabbed() ||
                (e.motion.xrel == 0 && e.motion.yrel == 0))
                break;
            struct vinput_cmd event = {
                .type = VINPUT_CMD_MOUSE_MOTION,
                .u.mouse_motion = {.dx = e.motion.xrel, .dy = e.motion.yrel},
            };
            vinput_push_cmd(VINPUT_MOUSE_ID, &event);
        } break;
        case SDL_MOUSEWHEEL: {
            int dx = e.wheel.x;
            int dy = e.wheel.y;
            /* SDL_MOUSEWHEEL_FLIPPED means natural/reversed scrolling —
             * negate to get standard evdev REL_WHEEL convention.
             */
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                dx = -dx;
                dy = -dy;
            }
            struct vinput_cmd event = {
                .type = VINPUT_CMD_MOUSE_WHEEL,
                .u.mouse_wheel = {.dx = dx, .dy = dy},
            };
            vinput_push_cmd(VINPUT_MOUSE_ID, &event);
            break;
        }
        }
        processed++;
    } while (processed < VINPUT_SDL_EVENT_BURST_LIMIT && SDL_PollEvent(&e));

    return false;
}

int virtio_input_fill_ev_key_bitmap(uint8_t *bitmap, size_t bitmap_size)
{
    unsigned long key_cnt =
        sizeof(vinput_key_map) / sizeof(struct vinput_key_map_entry);
    int max_byte = 0;
    for (unsigned long i = 0; i < key_cnt; i++) {
        int code = vinput_key_map[i].linux_key;
        int byte_idx = code / 8;
        if ((size_t) byte_idx >= bitmap_size)
            continue;
        bitmap[byte_idx] |= (uint8_t) (1U << (code % 8));
        if (byte_idx > max_byte)
            max_byte = byte_idx;
    }
    return max_byte + 1;
}
