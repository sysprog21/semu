#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feature.h"

#if SEMU_HAS(VIRTIOINPUT)
/* Per virtio-input spec, config string/bitmap payloads are 128 bytes. */
#define VIRTIO_INPUT_CFG_PAYLOAD_SIZE 128

/* Per-device identifier shared between the window backend and the
 * virtio-input device model.
 */
enum {
    VINPUT_KEYBOARD_ID = 0,
    VINPUT_MOUSE_ID = 1,
    VINPUT_DEV_CNT,
};

/* Host-side input commands produced by the window backend. The SDL/main
 * thread translates platform input into this neutral form. The emulator
 * thread consumes it and updates the virtio-input device state.
 */
enum vinput_cmd_type {
    VINPUT_CMD_KEYBOARD_KEY = 0,
    VINPUT_CMD_MOUSE_BUTTON,
    VINPUT_CMD_MOUSE_MOTION,
    VINPUT_CMD_MOUSE_WHEEL,
};

/* Input command of the queued backend. Used to make producer does not need to
 * touch virtio queues, guest RAM, or heap allocation.
 */
struct vinput_cmd {
    enum vinput_cmd_type type;
    union {
        struct {
            uint32_t key;
            uint32_t value;
        } keyboard_key;
        struct {
            uint32_t button;
            bool pressed;
        } mouse_button;
        struct {
            int32_t dx;
            int32_t dy;
        } mouse_motion;
        struct {
            int32_t dx;
            int32_t dy;
        } mouse_wheel;
    } u;
};

/* Poll and translate pending SDL events on the main thread. Returns true if a
 * quit/close request was observed, which tells the caller to shut down the
 * frontend loop.
 */
bool vinput_handle_events(void);

/* Pop one translated backend input event from the per-device queue. Called by
 * the emulator thread while draining work that arrived from the SDL/main
 * thread. dev_id selects which device's queue to read.
 */
bool vinput_pop_cmd(int dev_id, struct vinput_cmd *cmd);

/* Reopen the producer wake gate after the emulator thread drains the current
 * batch of queued input events across all device queues. Returns true if every
 * queue is empty across the rearm, or false if the producer raced and more
 * events are already pending.
 */
bool vinput_rearm_cmd_wake(void);

/* Returns true once the backend has published input work for the emulator
 * thread. This is a cheap fast-path check used to skip queue-drain bookkeeping
 * when no translated input events are pending.
 */
bool vinput_may_have_pending_cmds(void);

/* Drop all pending events for one virtio-input device. Called when the guest
 * resets that device; the other device's queue is left untouched.
 */
void vinput_reset_host_events(int dev_id);

/* Fill bitmap[] with exactly the key codes this backend can generate.
 * Per the virtio-input spec, only advertise key codes this device will
 * actually generate. Returns the minimum byte count needed (index of the
 * highest set byte + 1), matching the virtio-input config "size" field.
 * bitmap_size must be >= VIRTIO_INPUT_CFG_PAYLOAD_SIZE.
 */
int virtio_input_fill_ev_key_bitmap(uint8_t *bitmap, size_t bitmap_size);
#endif /* SEMU_HAS(VIRTIOINPUT) */
