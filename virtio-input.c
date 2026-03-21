#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-input-codes.h"
#include "virtio-input-event.h"
#include "virtio.h"

#define BUS_VIRTUAL 0x06

#define VINPUT_DEBUG_PREFIX "[SEMU vinput-log]: "

#define VINPUT_KEYBOARD_NAME "VirtIO Keyboard"
#define VINPUT_MOUSE_NAME "VirtIO Mouse"

#define VINPUT_SERIAL "None"

#define VIRTIO_INPUT_FEATURES_0 0
#define VIRTIO_INPUT_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */

#define VIRTIO_INPUT_QUEUE_NUM_MAX 1024
#define VIRTIO_INPUT_QUEUE (vinput->queues[vinput->QueueSel])

#define PRIV(x) ((struct vinput_data *) (x)->priv)

enum {
    VIRTIO_INPUT_EVENTQ = 0,
    VIRTIO_INPUT_STATUSQ = 1,
};

enum {
    VIRTIO_INPUT_REG_SELECT = 0x100,
    VIRTIO_INPUT_REG_SUBSEL = 0x101,
    VIRTIO_INPUT_REG_SIZE = 0x102,
};

enum virtio_input_config_select {
    VIRTIO_INPUT_CFG_UNSET = 0x00,
    VIRTIO_INPUT_CFG_ID_NAME = 0x01,
    VIRTIO_INPUT_CFG_ID_SERIAL = 0x02,
    VIRTIO_INPUT_CFG_ID_DEVIDS = 0x03,
    VIRTIO_INPUT_CFG_PROP_BITS = 0x10,
    VIRTIO_INPUT_CFG_EV_BITS = 0x11,
    VIRTIO_INPUT_CFG_ABS_INFO = 0x12,
};

PACKED(struct virtio_input_absinfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
});

PACKED(struct virtio_input_devids {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
});

PACKED(struct virtio_input_config {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[VIRTIO_INPUT_CFG_PAYLOAD_SIZE];
        uint8_t bitmap[VIRTIO_INPUT_CFG_PAYLOAD_SIZE];
        struct virtio_input_absinfo abs;
        struct virtio_input_devids ids;
    } u;
});

PACKED(struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
});

struct vinput_data {
    virtio_input_state_t *vinput;
    struct virtio_input_config cfg;
    int type; /* VINPUT_KEYBOARD_ID or VINPUT_MOUSE_ID */
};

static pthread_mutex_t vinput_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct vinput_data vinput_dev[VINPUT_DEV_CNT];
static const char *vinput_dev_name[VINPUT_DEV_CNT] = {
    VINPUT_KEYBOARD_NAME,
    VINPUT_MOUSE_NAME,
};

static inline void vinput_bitmap_set_bit(uint8_t *map, unsigned long bit)
{
    map[bit / 8] |= (uint8_t) (1U << (bit % 8));
}

/* Return the number of bytes the driver needs to read from the config bitmap,
 * defined by the virtio input spec as "highest set byte index + 1".
 */
static inline unsigned long vinput_bitmap_get_size(const uint8_t *bitmap,
                                                   unsigned long max_bytes)
{
    while (max_bytes > 0 && bitmap[max_bytes - 1] == 0)
        max_bytes--;
    return max_bytes;
}

static inline void virtio_input_set_fail(virtio_input_state_t *vinput)
{
    vinput->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vinput->Status & VIRTIO_STATUS__DRIVER_OK)
        vinput->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline bool virtio_input_is_config_access(uint32_t addr,
                                                 size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_input_config);

    /* [base, end) */
    if (access_size == 0)
        return false;
    if (addr < base)
        return false;
    if (addr + access_size > end)
        return false;
    return true;
}

static inline uint32_t virtio_input_preprocess(virtio_input_state_t *vinput,
                                               uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_input_set_fail(vinput), 0;

    return addr >> 2;
}

/* Consume all pending buffers from the status queue and return them to the
 * used ring. The guest driver uses statusq to send EV_LED events (Caps Lock,
 * Num Lock, etc.) and acknowledges each buffer so the queue never stalls.
 * SDL has no portable LED-control API, so LED state is not applied to the host
 * keyboard here. Must be called with vinput_mutex held.
 */
static void virtio_input_drain_statusq(virtio_input_state_t *vinput)
{
    virtio_input_queue_t *queue = &vinput->queues[VIRTIO_INPUT_STATUSQ];
    uint32_t *ram = vinput->ram;

    if (!(vinput->Status & VIRTIO_STATUS__DRIVER_OK) || !queue->ready)
        return;

    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    uint16_t avail_delta = (uint16_t) (new_avail - queue->last_avail);
    uint16_t new_used = ram[queue->QueueUsed] >> 16;
    bool consumed = false;

    if (avail_delta > (uint16_t) queue->QueueNum) {
        virtio_input_set_fail(vinput);
        return;
    }

    const uint32_t event_size = (uint32_t) sizeof(struct virtio_input_event);

    while (queue->last_avail != new_avail) {
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        if (buffer_idx >= queue->QueueNum) {
            virtio_input_set_fail(vinput);
            return;
        }

        uint32_t *desc = &ram[queue->QueueDesc + buffer_idx * 4];
        uint32_t desc_addr = desc[0];
        uint32_t desc_addr_high = desc[1];
        uint32_t desc_len = desc[2];
        uint16_t desc_flags = desc[3] & 0xFFFF;

        if (desc_addr_high != 0 || (desc_flags & VIRTIO_DESC_F_WRITE) ||
            desc_len < event_size || desc_addr > RAM_SIZE - event_size) {
            virtio_input_set_fail(vinput);
            return;
        }

        /* Device is read-only on this queue, so no bytes are written into
         * the device-writable portion of the buffer. used.len must be 0.
         */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;
        ram[vq_used_addr + 1] = 0;
        new_used++;
        queue->last_avail++;
        consumed = true;
    }

    if (consumed) {
        uint16_t *used_hdr = (uint16_t *) &ram[queue->QueueUsed];
        used_hdr[0] = 0;
        used_hdr[1] = new_used;
        if (!(ram[queue->QueueAvail] & 1))
            vinput->InterruptStatus |= VIRTIO_INT__USED_RING;
    }
}

static void virtio_input_update_status(virtio_input_state_t *vinput,
                                       uint32_t status)
{
    vinput->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vinput->ram;
    void *priv = vinput->priv;
    memset(vinput, 0, sizeof(*vinput));
    vinput->ram = ram;
    vinput->priv = priv;
}

/* Returns true if any events were written to used ring, false otherwise */
static bool virtio_input_desc_handler(virtio_input_state_t *vinput,
                                      struct virtio_input_event *input_ev,
                                      uint32_t ev_cnt,
                                      virtio_input_queue_t *queue)
{
    uint32_t *desc;
    struct virtq_desc vq_desc;
    struct virtio_input_event *ev;

    uint32_t *ram = vinput->ram;
    uint16_t new_avail =
        ram[queue->QueueAvail] >> 16; /* virtq_avail.idx (le16) */
    uint16_t new_used = ram[queue->QueueUsed] >> 16; /* virtq_used.idx (le16) */

    /* For checking if the event buffer has enough space to write */
    uint32_t end = queue->last_avail + ev_cnt;
    uint32_t flattened_avail_idx = new_avail;

    /* Handle if the available index has overflowed and returned to the
     * beginning
     */
    if (new_avail < queue->last_avail)
        flattened_avail_idx += (1U << 16);

    /* Check if need to wait until the driver supplies new buffers */
    if (flattened_avail_idx < end)
        return false;

    for (uint32_t i = 0; i < ev_cnt; i++) {
        /* Obtain the available ring index */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        if (buffer_idx >= queue->QueueNum) {
            virtio_input_set_fail(vinput);
            return false;
        }

        desc = &ram[queue->QueueDesc + buffer_idx * 4];
        vq_desc.addr = desc[0];
        uint32_t addr_high = desc[1];
        vq_desc.len = desc[2];
        vq_desc.flags = desc[3] & 0xFFFF;

        /* Validate descriptor: 32-bit addressing only, WRITE flag set,
         * buffer large enough, and address within RAM bounds. Compare the
         * start address against the last valid event-sized window in RAM so
         * guest-controlled addr cannot wrap past UINT32_MAX during validation.
         */
        const uint32_t event_size =
            (uint32_t) sizeof(struct virtio_input_event);
        if (addr_high != 0 || !(vq_desc.flags & VIRTIO_DESC_F_WRITE) ||
            vq_desc.len < event_size || vq_desc.addr > RAM_SIZE - event_size) {
            virtio_input_set_fail(vinput);
            return false;
        }

        /* Write event into guest buffer directly */
        ev = (struct virtio_input_event *) ((uintptr_t) ram + vq_desc.addr);
        ev->type = input_ev[i].type;
        ev->code = input_ev[i].code;
        ev->value = input_ev[i].value;

        /* Update used ring */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;
        ram[vq_used_addr + 1] = sizeof(struct virtio_input_event);

        new_used++;
        queue->last_avail++;
    }

    /* Update used ring header */
    uint16_t *used_hdr = (uint16_t *) &ram[queue->QueueUsed];
    used_hdr[0] = 0;        /* virtq_used.flags */
    used_hdr[1] = new_used; /* virtq_used.idx */

    return true;
}

static void virtio_input_update_eventq(int dev_id,
                                       struct virtio_input_event *input_ev,
                                       uint32_t ev_cnt)
{
    virtio_input_state_t *vinput = vinput_dev[dev_id].vinput;
    if (!vinput)
        return;

    int index = VIRTIO_INPUT_EVENTQ;

    /* Start of the critical section */
    pthread_mutex_lock(&vinput_mutex);

    uint32_t *ram = vinput->ram;
    virtio_input_queue_t *queue = &vinput->queues[index];

    /* Check device status */
    if (vinput->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        goto out;

    if (!((vinput->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        goto out;

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    uint16_t avail_delta = (uint16_t) (new_avail - queue->last_avail);
    if (avail_delta > (uint16_t) queue->QueueNum) {
        fprintf(stderr, "%s(): size check failed\n", __func__);
        goto fail;
    }

    /* No buffers available - drop event or handle later */
    if (queue->last_avail == new_avail) {
#if SEMU_INPUT_DEBUG
        fprintf(stderr, VINPUT_DEBUG_PREFIX "drop dev=%d (no guest buffers)\n",
                dev_id);
#endif
        /* TODO: Consider buffering events instead of dropping them */
        goto out;
    }

    /* Try to write events to used ring */
    bool wrote_events =
        virtio_input_desc_handler(vinput, input_ev, ev_cnt, queue);

    /* Send interrupt only if we actually wrote events, unless
     * VIRTQ_AVAIL_F_NO_INTERRUPT is set
     */
    if (wrote_events && !(ram[queue->QueueAvail] & 1))
        vinput->InterruptStatus |= VIRTIO_INT__USED_RING;

    goto out;

fail:
    virtio_input_set_fail(vinput);

out:
    /* End of the critical section */
    pthread_mutex_unlock(&vinput_mutex);
}

void virtio_input_update_key(uint32_t key, uint32_t ev_value)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "key code=%u value=%u\n", key,
            ev_value);
#endif
    /* ev_value follows Linux evdev: 0=release, 1=press, 2=repeat */
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_KEY, .code = key, .value = ev_value},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_input_update_eventq(VINPUT_KEYBOARD_ID, input_ev, ev_cnt);
}

void virtio_input_update_mouse_button_state(uint32_t button, bool pressed)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "button code=%u pressed=%u\n", button,
            pressed);
#endif
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_KEY, .code = button, .value = pressed},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_input_update_eventq(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

void virtio_input_update_mouse_motion(int32_t dx, int32_t dy)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "motion dx=%d dy=%d\n", dx, dy);
#endif
    struct virtio_input_event input_ev[3];
    uint32_t ev_cnt = 0;

    if (dx)
        input_ev[ev_cnt++] = (struct virtio_input_event) {
            .type = SEMU_EV_REL, .code = SEMU_REL_X, .value = (uint32_t) dx};
    if (dy)
        input_ev[ev_cnt++] = (struct virtio_input_event) {
            .type = SEMU_EV_REL, .code = SEMU_REL_Y, .value = (uint32_t) dy};
    if (!ev_cnt)
        return;

    input_ev[ev_cnt++] = (struct virtio_input_event) {
        .type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0};

    virtio_input_update_eventq(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

void virtio_input_update_scroll(int32_t dx, int32_t dy)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "scroll dx=%d dy=%d\n", dx, dy);
#endif
    /* Build only the non-zero axis events and always terminate with SYN_REPORT.
     * dx > 0: scroll right, dy > 0: scroll up (matches Linux evdev convention).
     */
    struct virtio_input_event input_ev[3];
    uint32_t ev_cnt = 0;

    if (dx)
        input_ev[ev_cnt++] =
            (struct virtio_input_event) {.type = SEMU_EV_REL,
                                         .code = SEMU_REL_HWHEEL,
                                         .value = (uint32_t) dx};
    if (dy)
        input_ev[ev_cnt++] =
            (struct virtio_input_event) {.type = SEMU_EV_REL,
                                         .code = SEMU_REL_WHEEL,
                                         .value = (uint32_t) dy};
    if (!ev_cnt)
        return;

    input_ev[ev_cnt++] = (struct virtio_input_event) {
        .type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0};

    virtio_input_update_eventq(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

static void virtio_input_properties(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);

    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        cfg->size = 0;
        break;
    case VINPUT_MOUSE_ID:
        /* INPUT_PROP_POINTER marks this as a pointer device. */
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_INPUT_PROP_POINTER);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    }
}

static void virtio_input_fill_keyboard_ev_bits(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);

    switch (event) {
    case SEMU_EV_KEY:
        /* Only advertise key codes that key_map[] actually generates. */
        cfg->size = (uint8_t) virtio_input_fill_ev_key_bitmap(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    case SEMU_EV_LED:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_LED_NUML);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_LED_CAPSL);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_LED_SCROLLL);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    case SEMU_EV_REP:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REP_DELAY);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REP_PERIOD);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_input_fill_mouse_ev_bits(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);

    switch (event) {
    case SEMU_EV_KEY:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_LEFT);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_RIGHT);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_MIDDLE);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    case SEMU_EV_REL:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_X);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_Y);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_HWHEEL);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_WHEEL);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_input_fill_ev_bits(int dev_id, uint8_t event)
{
    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        virtio_input_fill_keyboard_ev_bits(dev_id, event);
        break;
    case VINPUT_MOUSE_ID:
        virtio_input_fill_mouse_ev_bits(dev_id, event);
        break;
    }
}

static void virtio_input_fill_abs_info(int dev_id, uint8_t code)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    (void) code;

    /* The current pointing device is a relative mouse, so no ABS axes or
     * ABS_INFO ranges are exposed.
     */
    cfg->size = 0;
}

static void virtio_input_cfg_read(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(&cfg->u, 0, sizeof(cfg->u));
    cfg->size = 0;

    switch (cfg->select) {
    case VIRTIO_INPUT_CFG_UNSET:
        return;
    case VIRTIO_INPUT_CFG_ID_NAME:
        strcpy(cfg->u.string, vinput_dev_name[dev_id]);
        cfg->size = strlen(vinput_dev_name[dev_id]);
        return;
    case VIRTIO_INPUT_CFG_ID_SERIAL:
        strcpy(cfg->u.string, VINPUT_SERIAL);
        cfg->size = strlen(VINPUT_SERIAL);
        return;
    case VIRTIO_INPUT_CFG_ID_DEVIDS:
        cfg->u.ids.bustype = BUS_VIRTUAL;
        cfg->u.ids.vendor = 0;
        cfg->u.ids.product = 0;
        cfg->u.ids.version = 1;
        cfg->size = sizeof(struct virtio_input_devids);
        return;
    case VIRTIO_INPUT_CFG_PROP_BITS:
        virtio_input_properties(dev_id);
        return;
    case VIRTIO_INPUT_CFG_EV_BITS:
        virtio_input_fill_ev_bits(dev_id, cfg->subsel);
        return;
    case VIRTIO_INPUT_CFG_ABS_INFO:
        virtio_input_fill_abs_info(dev_id, cfg->subsel);
        return;
    default:
        return;
    }
}

static bool virtio_input_reg_read(virtio_input_state_t *vinput,
                                  uint32_t addr,
                                  uint32_t *value,
                                  size_t size)
{
#define _(reg) (VIRTIO_##reg << 2)
    switch (addr) {
    case _(MagicValue):
        *value = 0x74726976;
        return true;
    case _(Version):
        *value = 2;
        return true;
    case _(DeviceID):
        *value = 18;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vinput->DeviceFeaturesSel == 0
                     ? VIRTIO_INPUT_FEATURES_0
                     : (vinput->DeviceFeaturesSel == 1 ? VIRTIO_INPUT_FEATURES_1
                                                       : 0);
        return true;
    case _(QueueNumMax):
        *value = VIRTIO_INPUT_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VIRTIO_INPUT_QUEUE.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vinput->InterruptStatus;
        return true;
    case _(Status):
        *value = vinput->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    case VIRTIO_INPUT_REG_SIZE:
        virtio_input_cfg_read(PRIV(vinput)->type);
        *value = PRIV(vinput)->cfg.size;
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_input_config)))
            return false;

        /* Read virtio-input specific registers */
        uint32_t offset = addr - VIRTIO_INPUT_REG_SELECT;
        uint8_t *reg = (uint8_t *) ((uintptr_t) &PRIV(vinput)->cfg + offset);

        /* Clear value first to avoid returning dirty high bits on partial reads
         */
        *value = 0;
        memcpy(value, reg, size);

        return true;
    }
#undef _
}

static bool virtio_input_reg_write(virtio_input_state_t *vinput,
                                   uint32_t addr,
                                   uint32_t value)
{
#define _(reg) (VIRTIO_##reg << 2)
    switch (addr) {
    case _(DeviceFeaturesSel):
        vinput->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        if (vinput->DriverFeaturesSel == 0)
            vinput->DriverFeatures = value;
        return true;
    case _(DriverFeaturesSel):
        vinput->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vinput->queues))
            vinput->QueueSel = value;
        else
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VIRTIO_INPUT_QUEUE_NUM_MAX)
            VIRTIO_INPUT_QUEUE.QueueNum = value;
        else
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueReady):
        VIRTIO_INPUT_QUEUE.ready = value & 1;
        if (VIRTIO_INPUT_QUEUE.ready) {
            uint32_t qnum = VIRTIO_INPUT_QUEUE.QueueNum;
            uint32_t ram_words = RAM_SIZE / 4;

            /* Validate that the entire avail ring, desc table, and used ring
             * fit within guest RAM. virtio_input_preprocess() only checks the
             * base address of each ring — without this check a guest could
             * place a ring near the end of RAM and cause out-of-bounds host
             * accesses when the ring entries are subsequently dereferenced.
             *
             * Max words accessed per ring:
             *   avail: QueueAvail + 1 + (qnum-1)/2
             *   desc:  QueueDesc  + qnum*4 - 1
             *   used:  QueueUsed  + qnum*2      (vq_used_addr+1)
             */
            if (qnum == 0 ||
                VIRTIO_INPUT_QUEUE.QueueAvail + 1 + (qnum - 1) / 2 >=
                    ram_words ||
                VIRTIO_INPUT_QUEUE.QueueDesc + qnum * 4 > ram_words ||
                VIRTIO_INPUT_QUEUE.QueueUsed + qnum * 2 >= ram_words) {
                virtio_input_set_fail(vinput);
                return true;
            }

            VIRTIO_INPUT_QUEUE.last_avail =
                vinput->ram[VIRTIO_INPUT_QUEUE.QueueAvail] >> 16;
        }
        return true;
    case _(QueueDescLow):
        VIRTIO_INPUT_QUEUE.QueueDesc = virtio_input_preprocess(vinput, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueDriverLow):
        VIRTIO_INPUT_QUEUE.QueueAvail = virtio_input_preprocess(vinput, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueDeviceLow):
        VIRTIO_INPUT_QUEUE.QueueUsed = virtio_input_preprocess(vinput, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueNotify):
        if (value >= ARRAY_SIZE(vinput->queues)) {
            virtio_input_set_fail(vinput);
            return true;
        }
        /* EVENTQ: actual buffer availability is checked lazily in
         * virtio_input_update_eventq() when the next event arrives.
         * STATUSQ: drain LED-state buffers from the guest immediately so
         * the driver's status queue never runs out of available entries.
         */
        if (value == VIRTIO_INPUT_STATUSQ)
            virtio_input_drain_statusq(vinput);
        return true;
    case _(InterruptACK):
        vinput->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_input_update_status(vinput, value);
        return true;
    case _(SHMSel):
        return true;
    case VIRTIO_INPUT_REG_SELECT:
        PRIV(vinput)->cfg.select = value;
        return true;
    case VIRTIO_INPUT_REG_SUBSEL:
        PRIV(vinput)->cfg.subsel = value;
        return true;
    default:
        /* No other writable registers */
        return false;
    }
#undef _
}

void virtio_input_read(hart_t *vm,
                       virtio_input_state_t *vinput,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg = false;

    pthread_mutex_lock(&vinput_mutex);

    switch (width) {
    case RV_MEM_LW:
        access_size = 4;
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
        access_size = 1;
        break;
    case RV_MEM_LHU:
    case RV_MEM_LH:
        access_size = 2;
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        goto out;
    }

    is_cfg = virtio_input_is_config_access(addr, access_size);

    /*
     * Common registers (before Config): only allow aligned 32-bit LW.
     * Device-specific config (Config and after): allow 8/16/32-bit with
     * natural alignment.
     */
    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            goto out;
        }
    } else {
        if (addr & (access_size - 1)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            goto out;
        }
    }

    if (!virtio_input_reg_read(vinput, addr, value, access_size))
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);

out:
    pthread_mutex_unlock(&vinput_mutex);
}

void virtio_input_write(hart_t *vm,
                        virtio_input_state_t *vinput,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg = false;

    pthread_mutex_lock(&vinput_mutex);

    switch (width) {
    case RV_MEM_SW:
        access_size = 4;
        break;
    case RV_MEM_SB:
        access_size = 1;
        break;
    case RV_MEM_SH:
        access_size = 2;
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        goto out;
    }

    is_cfg = virtio_input_is_config_access(addr, access_size);

    /*
     * Common registers (before Config): only allow aligned 32-bit SW.
     * Device-specific config (Config and after): allow 8/16/32-bit with
     * natural alignment. Note: only select/subsel are writable — others
     * will return false and be reported as STORE_FAULT below.
     */
    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            goto out;
        }
    } else {
        if (addr & (access_size - 1)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            goto out;
        }
    }

    if (!virtio_input_reg_write(vinput, addr, value))
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);

out:
    pthread_mutex_unlock(&vinput_mutex);
}

bool virtio_input_irq_pending(virtio_input_state_t *vinput)
{
    pthread_mutex_lock(&vinput_mutex);
    bool pending = vinput->InterruptStatus != 0;
    pthread_mutex_unlock(&vinput_mutex);
    return pending;
}

void virtio_input_init(virtio_input_state_t *vinput)
{
    static int vinput_dev_cnt = 0;
    if (vinput_dev_cnt >= VINPUT_DEV_CNT) {
        fprintf(stderr,
                "Exceeded the number of virtio-input devices that can be "
                "allocated.\n");
        exit(2);
    }

    vinput->priv = &vinput_dev[vinput_dev_cnt];
    PRIV(vinput)->type = vinput_dev_cnt;
    PRIV(vinput)->vinput = vinput;
    vinput_dev_cnt++;
}
