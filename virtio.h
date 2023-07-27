#pragma once

#define VIRTIO_VENDOR_ID 0x12345678

#define VIRTIO_STATUS__DRIVER_OK 4
#define VIRTIO_STATUS__DEVICE_NEEDS_RESET 64

#define VIRTIO_INT__USED_RING 1
#define VIRTIO_INT__CONF_CHANGE 2

#define VIRTIO_DESC_F_NEXT 1
#define VIRTIO_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_T_GET_LIFETIME 10
#define VIRTIO_BLK_T_DISCARD 11
#define VIRTIO_BLK_T_WRITE_ZEROES 13
#define VIRTIO_BLK_T_SECURE_ERASE 14

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

/* VirtIO MMIO registers */
#define VIRTIO_REG_LIST                  \
    _(MagicValue, 0x000)        /* R */  \
    _(Version, 0x004)           /* R */  \
    _(DeviceID, 0x008)          /* R */  \
    _(VendorID, 0x00c)          /* R */  \
    _(DeviceFeatures, 0x010)    /* R */  \
    _(DeviceFeaturesSel, 0x014) /* W */  \
    _(DriverFeatures, 0x020)    /* W */  \
    _(DriverFeaturesSel, 0x024) /* W */  \
    _(QueueSel, 0x030)          /* W */  \
    _(QueueNumMax, 0x034)       /* R */  \
    _(QueueNum, 0x038)          /* W */  \
    _(QueueReady, 0x044)        /* RW */ \
    _(QueueNotify, 0x050)       /* W */  \
    _(InterruptStatus, 0x60)    /* R */  \
    _(InterruptACK, 0x064)      /* W */  \
    _(Status, 0x070)            /* RW */ \
    _(QueueDescLow, 0x080)      /* W */  \
    _(QueueDescHigh, 0x084)     /* W */  \
    _(QueueDriverLow, 0x090)    /* W */  \
    _(QueueDriverHigh, 0x094)   /* W */  \
    _(QueueDeviceLow, 0x0a0)    /* W */  \
    _(QueueDeviceHigh, 0x0a4)   /* W */  \
    _(ConfigGeneration, 0x0fc)  /* R */  \
    _(Config, 0x100)            /* RW */

enum {
#define _(reg, addr) VIRTIO_##reg = addr >> 2,
    VIRTIO_REG_LIST
#undef _
};

struct virtq_desc {
    uint32_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
