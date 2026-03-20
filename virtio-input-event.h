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

/* Poll and translate pending SDL events on the main thread. Returns true if a
 * quit/close request was observed, which tells the caller to shut down the
 * frontend loop.
 */
bool vinput_handle_events(void);

/* Fill bitmap[] with exactly the key codes this backend can generate.
 * Per the virtio-input spec, only advertise key codes this device will
 * actually generate. Returns the minimum byte count needed (index of the
 * highest set byte + 1), matching the virtio-input config "size" field.
 * bitmap_size must be >= VIRTIO_INPUT_CFG_PAYLOAD_SIZE.
 */
int virtio_input_fill_ev_key_bitmap(uint8_t *bitmap, size_t bitmap_size);
#endif /* SEMU_HAS(VIRTIOINPUT) */
