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

/* The FUSE OP codes are from
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/fuse.h
 */
#define FUSE_INIT 26
#define FUSE_GETATTR 3
#define FUSE_OPENDIR 27
#define FUSE_READDIRPLUS 44
#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_RELEASEDIR 29
#define FUSE_OPEN 14
#define FUSE_READ 15
#define FUSE_RELEASE 18
#define FUSE_FLUSH 25
#define FUSE_DESTROY 38
/* TODO OP codes are below: */
#define FUSE_MKDIR 9
#define FUSE_WRITE 16
#define FUSE_RMDIR 11
#define FUSE_CREATE 35

#define FUSE_ASYNC_READ (1 << 0)
#define FUSE_POSIX_LOCKS (1 << 1)
#define FUSE_FILE_OPS (1 << 2)
#define FUSE_ATOMIC_O_TRUNC (1 << 3)
#define FUSE_EXPORT_SUPPORT (1 << 4)
#define FUSE_BIG_WRITES (1 << 5)
#define FUSE_DONT_MASK (1 << 6)
#define FUSE_SPLICE_WRITE (1 << 7)
#define FUSE_SPLICE_MOVE (1 << 8)
#define FUSE_SPLICE_READ (1 << 9)
#define FUSE_FLOCK_LOCKS (1 << 10)
#define FUSE_HAS_IOCTL_DIR (1 << 11)
#define FUSE_AUTO_INVAL_DATA (1 << 12)
#define FUSE_DO_READDIRPLUS (1 << 13)
#define FUSE_READDIRPLUS_AUTO (1 << 14)
#define FUSE_ASYNC_DIO (1 << 15)
#define FUSE_WRITEBACK_CACHE (1 << 16)
#define FUSE_NO_OPEN_SUPPORT (1 << 17)
#define FUSE_PARALLEL_DIROPS (1 << 18)
#define FUSE_HANDLE_KILLPRIV (1 << 19)
#define FUSE_POSIX_ACL (1 << 20)
#define FUSE_ABORT_ERROR (1 << 21)
#define FUSE_MAX_PAGES (1 << 22)
#define FUSE_CACHE_SYMLINKS (1 << 23)
#define FUSE_NO_OPENDIR_SUPPORT (1 << 24)

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

PACKED(struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
});
