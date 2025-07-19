#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common.h"
#include "device.h"
#include "input-event-codes.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio.h"

#define BUS_VIRTUAL 0x06 /* Definition from the Linux kernel */

#define VINPUT_KEYBOARD_NAME "VirtIO Keyboard"
#define VINPUT_MOUSE_NAME "VirtIO Mouse"

#define VIRTIO_INPUT_SERIAL "None"

#define VIRTIO_F_VERSION_1 1

#define VINPUT_FEATURES_0 0
#define VINPUT_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */

#define VINPUT_QUEUE_NUM_MAX 1024
#define VINPUT_QUEUE (vinput->queues[vinput->QueueSel])

enum {
    VINPUT_KEYBOARD_ID = 0,
    VINPUT_MOUSE_ID = 1,
    VINPUT_DEV_CNT,
};

enum {
    EVENTQ = 0,
    STATUSQ = 1,
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
        char string[128];
        uint8_t bitmap[128];
        struct virtio_input_absinfo abs;
        struct virtio_input_devids ids;
    } u;
});

PACKED(struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
});

struct virio_input_data {
    uint32_t ev_notify;
    virtio_input_state_t *vinput;
    struct virtio_input_config cfg;
};

static pthread_mutex_t virtio_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t virtio_input_cond = PTHREAD_COND_INITIALIZER;

static struct virio_input_data vinput_dev[VINPUT_DEV_CNT];
static int vinput_dev_cnt;

static char *vinput_dev_name[VINPUT_DEV_CNT] = {
    VINPUT_KEYBOARD_NAME,
    VINPUT_MOUSE_NAME,
};

static void virtio_input_set_fail(virtio_input_state_t *vinput)
{
    vinput->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vinput->Status & VIRTIO_STATUS__DRIVER_OK)
        vinput->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vinput_preprocess(virtio_input_state_t *vinput,
                                         uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_input_set_fail(vinput), 0;

    return addr >> 2;
}

static void virtio_input_update_status(virtio_input_state_t *vinput,
                                       uint32_t status)
{
    vinput->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vinput->ram;
    // void *priv = vinput->priv; /* TODO */
    int id = vinput->id; /* TODO: Store in vinput->priv */
    memset(vinput, 0, sizeof(*vinput));
    vinput->ram = ram;
    vinput->id = id;
    // vinput->priv = priv;
}

static void virtio_input_desc_handler(virtio_input_state_t *vinput,
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
     * beginning */
    if (new_avail < queue->last_avail)
        flattened_avail_idx += UINT16_MAX;

    /* Check if need to wait until the driver supplies new buffers */
    if (flattened_avail_idx < end)
        return;

    for (uint32_t i = 0; i < ev_cnt; i++) {
        /* Obtain the available ring index */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        desc = &vinput->ram[queue->QueueDesc + buffer_idx * 4];
        vq_desc.addr = desc[0];
        vq_desc.len = desc[2];
        vq_desc.flags = desc[3];
        ev = (struct virtio_input_event *) ((uintptr_t) vinput->ram +
                                            vq_desc.addr);

        desc[3] = 0;

        /* Write event */
        ev->type = input_ev[i].type;
        ev->code = input_ev[i].code;
        ev->value = input_ev[i].value;

        /* Used ring */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;
        ram[vq_used_addr + 1] = sizeof(struct virtio_input_event);

        new_used++;
        queue->last_avail++;
    }

    /* Reset used ring flag to zero (virtq_used.flags) */
    vinput->ram[queue->QueueUsed] &= MASK(16);
    /* Update the used ring pointer (virtq_used.idx) */
    /* TODO: Check if the or-ing is valid or not */
    // vinput->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16;
    uint16_t *used = (uint16_t *) &vinput->ram[queue->QueueUsed];
    used[1] = new_used;

    return;
}

static void virtio_queue_event_update(int dev_id,
                                      struct virtio_input_event *input_ev,
                                      uint32_t ev_cnt)
{
    virtio_input_state_t *vinput = vinput_dev[dev_id].vinput;
    int index = EVENTQ;

    /* Start of the critical section */
    pthread_mutex_lock(&virtio_input_mutex);

    /* Wait until event buffer to be ready */
    while (vinput_dev[vinput->id].ev_notify <= 0)
        pthread_cond_wait(&virtio_input_cond, &virtio_input_mutex);

    /* Consume notification count */
    vinput_dev[dev_id].ev_notify--;

    uint32_t *ram = vinput->ram;
    virtio_input_queue_t *queue = &vinput->queues[index];
    if (vinput->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        goto success;

    if (!((vinput->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        goto fail;

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum) {
        fprintf(stderr, "%s(): size check failed\n", __func__);
        goto fail;
    }

    if (queue->last_avail == new_avail)
        goto success;

    virtio_input_desc_handler(vinput, input_ev, ev_cnt, queue);

    /* Send interrupt, unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    if (!(ram[queue->QueueAvail] & 1))
        vinput->InterruptStatus |= VIRTIO_INT__USED_RING;

    goto success;

fail:
    virtio_input_set_fail(vinput);

success:
    /* End of the critical section */
    pthread_mutex_unlock(&virtio_input_mutex);
}

void virtio_input_update_key(uint32_t key, uint32_t state)
{
    struct virtio_input_event input_ev[] = {
        {.type = EV_KEY, .code = key, .value = state},
        {.type = EV_SYN, .code = SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_queue_event_update(VINPUT_KEYBOARD_ID, input_ev, ev_cnt);
}

void virtio_input_update_mouse_button_state(uint32_t button, bool pressed)
{
    struct virtio_input_event input_ev[] = {
        {.type = EV_KEY, .code = button, .value = pressed},
        {.type = EV_SYN, .code = SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_queue_event_update(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

void virtio_input_update_cursor(uint32_t x, uint32_t y)
{
    struct virtio_input_event input_ev[] = {
        {.type = EV_ABS, .code = ABS_X, .value = x},
        {.type = EV_ABS, .code = ABS_Y, .value = y},
        {.type = EV_SYN, .code = SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_queue_event_update(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

static void virtio_input_properties(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    memset(cfg->u.bitmap, 0, 128);
    set_bit(INPUT_PROP_POINTER, (unsigned long *) cfg->u.bitmap);
    set_bit(INPUT_PROP_DIRECT, (unsigned long *) cfg->u.bitmap);
    cfg->size = 128;
}

static void virtio_keyboard_support_events(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    memset(cfg->u.bitmap, 0, 128);

    switch (event) {
    case EV_KEY:
        memset(cfg->u.bitmap, 0xff, 128);
        cfg->size = 128;
        break;
    case EV_MSC:
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, REP_DELAY);
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, REP_PERIOD);
        cfg->size = 128;
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_mouse_support_events(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    memset(cfg->u.bitmap, 0, 128);

    switch (event) {
    case EV_KEY:
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, BTN_LEFT);
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, BTN_RIGHT);
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, BTN_MIDDLE);
        cfg->size = 128;
        break;
    case EV_ABS:
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, ABS_X);
        bitmap_set_bit((unsigned long *) cfg->u.bitmap, ABS_Y);
        cfg->size = 128;
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_input_support_events(int dev_id, uint8_t event)
{
    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        virtio_keyboard_support_events(dev_id, event);
        break;
    case VINPUT_MOUSE_ID:
        virtio_mouse_support_events(dev_id, event);
        break;
    }
}

static void virtio_input_abs_range(int dev_id, uint8_t code)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    switch (code) {
    case ABS_X:
        cfg->u.abs.min = 0;
        cfg->u.abs.max = SCREEN_WIDTH;
        cfg->u.abs.res = 1;
        cfg->size = sizeof(struct virtio_input_absinfo);
        break;
    case ABS_Y:
        cfg->u.abs.min = 0;
        cfg->u.abs.max = SCREEN_HEIGHT;
        cfg->u.abs.res = 1;
        cfg->size = sizeof(struct virtio_input_absinfo);
        break;
    default:
        cfg->size = 0;
    }
}

static bool virtio_input_cfg_read(int dev_id)
{
    uint8_t select = vinput_dev[dev_id].cfg.select;
    uint8_t subsel = vinput_dev[dev_id].cfg.subsel;
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    switch (select) {
    case VIRTIO_INPUT_CFG_ID_NAME:
        strcpy(cfg->u.string, vinput_dev_name[dev_id]);
        cfg->size = strlen(vinput_dev_name[dev_id]);
        return true;
    case VIRTIO_INPUT_CFG_ID_SERIAL:
        strcpy(cfg->u.string, VIRTIO_INPUT_SERIAL);
        cfg->size = strlen(VIRTIO_INPUT_SERIAL);
        return true;
    case VIRTIO_INPUT_CFG_ID_DEVIDS:
        cfg->u.ids.bustype = BUS_VIRTUAL;
        cfg->u.ids.vendor = 0;
        cfg->u.ids.product = 0;
        cfg->u.ids.version = 1;
        cfg->size = sizeof(struct virtio_input_devids);
        return true;
    case VIRTIO_INPUT_CFG_PROP_BITS:
        virtio_input_properties(dev_id);
        return true;
    case VIRTIO_INPUT_CFG_EV_BITS:
        virtio_input_support_events(dev_id, subsel);
        return true;
    case VIRTIO_INPUT_CFG_ABS_INFO:
        virtio_input_abs_range(dev_id, subsel);
        return true;
    default:
        fprintf(stderr,
                "virtio-input: Unknown value written to select register.\n");
        return false;
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
                     ? VINPUT_FEATURES_0
                     : (vinput->DeviceFeaturesSel == 1 ? VINPUT_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VINPUT_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VINPUT_QUEUE.ready ? 1 : 0;
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
        if (!virtio_input_cfg_read(vinput->id))
            return false;
        *value = vinput_dev[vinput->id].cfg.size;
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_input_config)))
            return false;

        /* Read virtio-input specific registers */
        off_t offset = addr - VIRTIO_INPUT_REG_SELECT;
        uint8_t *reg =
            (uint8_t *) ((uintptr_t) &vinput_dev[vinput->id].cfg + offset);
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
        vinput->DriverFeaturesSel == 0 ? (vinput->DriverFeatures = value) : 0;
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
        if (value > 0 && value <= VINPUT_QUEUE_NUM_MAX)
            VINPUT_QUEUE.QueueNum = value;
        else
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueReady):
        VINPUT_QUEUE.ready = value & 1;
        if (value & 1)
            VINPUT_QUEUE.last_avail =
                vinput->ram[VINPUT_QUEUE.QueueAvail] >> 16;
        return true;
    case _(QueueDescLow):
        VINPUT_QUEUE.QueueDesc = vinput_preprocess(vinput, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueDriverLow):
        VINPUT_QUEUE.QueueAvail = vinput_preprocess(vinput, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueDeviceLow):
        VINPUT_QUEUE.QueueUsed = vinput_preprocess(vinput, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vinput->queues)) {
            /* Handle event queue only for minimal implementation */
            if (value == EVENTQ) {
                vinput_dev[vinput->id].ev_notify++;
                pthread_cond_signal(&virtio_input_cond);
            }
        } else {
            virtio_input_set_fail(vinput);
        }
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
        vinput_dev[vinput->id].cfg.select = value;
        return true;
    case VIRTIO_INPUT_REG_SUBSEL:
        vinput_dev[vinput->id].cfg.subsel = value;
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
    pthread_mutex_lock(&virtio_input_mutex);

    /* XXX: 4-byte alignment (i.e., addr >> 2) is removed due to the per
    byte accessing */
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_input_reg_read(vinput, addr, value, 4))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        /*FIXME: virtio-input driver need to access device config register per
         * byte. the following code that derived from other virtio devices'
         * implementation will cause kernel panic */
        // vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
#if 1
        // printf("read addr: 0x%x, width: %d\n", addr, width);
        if (!virtio_input_reg_read(vinput, addr, value, 1))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
#endif
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }

    pthread_mutex_unlock(&virtio_input_mutex);
}

void virtio_input_write(hart_t *vm,
                        virtio_input_state_t *vinput,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t value)
{
    pthread_mutex_lock(&virtio_input_mutex);

    /* XXX: 4-byte alignment (i.e., addr >> 2) is removed due to the per
    byte accessing */
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_input_reg_write(vinput, addr, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        /* FIXME: virtio-input driver need to access device config register per
         * byte. the following code that derived from other virtio devices'
         * implementation will cause kernel panic */
        // vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
#if 1
        // printf("read addr: 0x%x, width: %d\n", addr, width);
        if (!virtio_input_reg_write(vinput, addr, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
#endif
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }

    pthread_mutex_unlock(&virtio_input_mutex);
}

void virtio_input_init(virtio_input_state_t *vinput)
{
    vinput->id = vinput_dev_cnt;
    vinput_dev_cnt++;

    vinput_dev[vinput->id].ev_notify = 0;
    vinput_dev[vinput->id].vinput = vinput;
}
