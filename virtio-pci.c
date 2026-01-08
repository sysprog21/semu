#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"

/* VirtIO PCI information */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_PCI_REVISION_ID 0x0

/* Header type register */
#define PCI_TYPE0_HEADER 0x0
#define PCI_TYPE1_HEADER 0x1
#define PCI_MULTI_FUNCTION_DEVICE (0x1 << 7)

/* Status register */
#define PCI_STATUS_CAPABILITIES_LIST (0x1 << 4)

/* BAR (Base Address Register) */
#define PCI_BAR_CONFIGURATION 0x00000000 /* Non-prefetchable, 32-bit memory */

/* Capabilities pointer register */
#define PCI_CAP_POINTER_OFFSET 0x40

/* Command register */
/* Enable Memory Space (MMIO access through BARs) */
#define PCI_COMMAND_MEMORY (1 << 1)
/* Enable Bus Mastering (allow device to initiate DMA) */
#define PCI_COMMAND_MASTER (1 << 2)
/* Disable legacy INTx interrupt (allow only MSI/MSI-X)*/
#define PCI_COMMAND_INTX_DISABLE (1 << 10)
/* Bitmask of all supported command register bits */
#define PCI_COMMAND_SUPPORT_MASK \
    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE)

/* ECAM (Enhanced Configuration Access Mechanism) */
#define ECAM_BASE_ADDR 0x21000000

#define ECAM_BUS_SHIFT 20
#define ECAM_DEVICE_SHIFT 15
#define ECAM_FUNCTION_SHIFT 12

#define ECAM_BUS_MASK 0xFF      /* 8 bits */
#define ECAM_DEVICE_MASK 0x1F   /* 5 bits */
#define ECAM_FUNCTION_MASK 0x07 /* 3 bits */
#define ECAM_CONFIG_MASK 0xFFF  /* 12 bits (0 to 4095) */

#define ECAM_OFFSET(addr) ((addr) -ECAM_BASE_ADDR)

#define GET_BUS(addr) ((ECAM_OFFSET(addr) >> ECAM_BUS_SHIFT) & ECAM_BUS_MASK)
#define GET_DEVICE(addr) \
    ((ECAM_OFFSET(addr) >> ECAM_DEVICE_SHIFT) & ECAM_DEVICE_MASK)
#define GET_FUNCTION(addr) \
    ((ECAM_OFFSET(addr) >> ECAM_FUNCTION_SHIFT) & ECAM_FUNCTION_MASK)
#define GET_CONFIG(addr) ((addr) &ECAM_CONFIG_MASK)

/* PCI capabilities */
#define CAP_COMMON_SIZE sizeof(struct virtio_pci_cap)
#define CAP_NOTIFY_SIZE sizeof(struct virtio_pci_notify_cap)
#define CAP_ISR_SIZE sizeof(struct virtio_pci_cap)
#define CAP_DEVICE_SIZE sizeof(struct virtio_pci_cap)

#define CAP_COMMON_START (0x0)
#define CAP_NOTIFY_START (CAP_COMMON_START + CAP_COMMON_SIZE)
#define CAP_ISR_START (CAP_NOTIFY_START + CAP_NOTIFY_SIZE)
#define CAP_DEVICE_START (CAP_ISR_START + CAP_ISR_SIZE)
#define CAP_DEVICE_END (CAP_DEVICE_START + CAP_DEVICE_SIZE)

enum {
    PCI_CONFIG_VENDOR_ID = 0x0,
    PCI_CONFIG_DEVICE_ID = 0x2,
    PCI_CONFIG_COMMAND = 0x4,
    PCI_CONFIG_STATUS = 0x6,
    PCI_CONFIG_REVISION_ID = 0x8,
    PCI_CONFIG_CLASS_CODE = 0x9,
    PCI_CONFIG_CACHE_LINE_SIZE = 0xC,
    PCI_CONFIG_MASTER_LATENCY_TIMER = 0xD,
    PCI_CONFIG_HEADER_TYPE = 0xE,
    PCI_CONFIG_BIST = 0xF,
    PCI_CONFIG_BAR0 = 0x10,
    PCI_CONFIG_BAR1 = 0x14,
    PCI_CONFIG_BAR2 = 0x18,
    PCI_CONFIG_BAR3 = 0x1C,
    PCI_CONFIG_BAR4 = 0x20,
    PCI_CONFIG_BAR5 = 0x24,
    PCI_CONFIG_CARDBUS_CIS_POINTER = 0x28,
    PCI_CONFIG_SUBSYSTEM_VENDOR_ID = 0x2C,
    PCI_CONFIG_SUBSYSTEM_ID = 0x2E,
    PCI_CONFIG_EXPANSION_ROM_BASE_ADDRESS = 0x30,
    PCI_CONFIG_CAPABILITIES_POINTER = 0x34,
    PCI_CONFIG_INTERRUPT_LINE = 0x3C,
    PCI_CONFIG_INTERRUPT_PIN = 0x3D,
    PCI_CONFIG_MIN_GNT = 0x3E,
    PCI_CONFIG_MAX_LAT = 0x3F,
};

enum {
    VIRTIO_PCI_CAP_COMMON_CFG = 1,        /* Common configuration */
    VIRTIO_PCI_CAP_NOTIFY_CFG = 2,        /* Notifications */
    VIRTIO_PCI_CAP_ISR_CFG = 3,           /* ISR Status */
    VIRTIO_PCI_CAP_DEVICE_CFG = 4,        /* Device specific configuration */
    VIRTIO_PCI_CAP_PCI_CFG = 5,           /* PCI configuration access */
    VIRTIO_PCI_CAP_SHARED_MEMORY_CFG = 8, /* Shared memory region */
    VIRTIO_PCI_CAP_VENDOR_CFG = 9,        /* Vendor-specific data */
};

PACKED(struct virtio_pci_cap {
    uint8_t cap_vndr;   /* Generic PCI field: PCI_CAP_ID_VNDR */
    uint8_t cap_next;   /* Generic PCI field: next ptr. */
    uint8_t cap_len;    /* Generic PCI field: capability length */
    uint8_t cfg_type;   /* Identifies the structure. */
    uint8_t bar;        /* Where to find it. */
    uint8_t id;         /* Multiple capabilities of the same type */
    uint8_t padding[2]; /* Pad to full dword. */
    uint32_t offset;    /* Offset within bar. */
    uint32_t length;    /* Length of the structure, in bytes. */
});

PACKED(struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier; /* Multiplier for queue_notify_off. */
});

PACKED(struct virtio_pci_common_cfg {
    /* About the whole device. */
    uint32_t device_feature_select; /* read-write */
    uint32_t device_feature;        /* read-only for driver */
    uint32_t driver_feature_select; /* read-write */
    uint32_t driver_feature;        /* read-write */
    uint16_t config_msix_vector;    /* read-write */
    uint16_t num_queues;            /* read-only for driver */
    uint8_t device_status;          /* read-write */
    uint8_t config_generation;      /* read-only for driver */

    /* About a specific virtqueue. */
    uint16_t queue_select;            /* read-write */
    uint16_t queue_size;              /* read-write */
    uint16_t queue_msix_vector;       /* read-write */
    uint16_t queue_enable;            /* read-write */
    uint16_t queue_notify_off;        /* read-only for driver */
    uint64_t queue_desc;              /* read-write */
    uint64_t queue_driver;            /* read-write */
    uint64_t queue_device;            /* read-write */
    uint16_t queue_notif_config_data; /* read-only for driver */
    uint16_t queue_reset;             /* read-write */

    /* About the administration virtqueue. */
    uint16_t admin_queue_index; /* read-only for driver */
    uint16_t admin_queue_num;   /* read-only for driver */
});

PACKED(struct virtio_pci_isr { uint8_t isr_status; });

struct virtio_pci_config {
    int device_cnt;
    struct list_head device_list;
};

struct virtio_pci_device {
    uint16_t device;
    uint16_t function_cnt;
    struct list_head function_list;
    struct list_head list;
};

struct virtio_pci_function {
    uint16_t function;

    /* PCI configuration header */
    uint16_t device_id;
    uint16_t command;
    uint8_t class_code[3];
    uint8_t cache_line_size;
    uint8_t master_latency_timer;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t probe_bitmap;
    uint32_t guest_bar_base[6];

    /* PCI Capabilities */
    struct virtio_pci_cap common_cap;
    struct virtio_pci_notify_cap notify_cap;
    struct virtio_pci_cap isr_cap;
    struct virtio_pci_cap device_cap;

    struct virtio_pci_common_cfg common_cfg;
    struct virtio_pci_isr isr;
    uint16_t *notifications;
    void *device_cfg;
    uint32_t device_config_size;

    struct list_head list;
};

static struct virtio_pci_config vpci_config;

struct virtio_pci_device *vpci_create_device(void)
{
    struct virtio_pci_device *dev = malloc(sizeof(struct virtio_pci_device));
    if (!dev)
        return NULL;

    memset(dev, 0, sizeof(*dev));
    dev->device = vpci_config.device_cnt;
    vpci_config.device_cnt++;
    INIT_LIST_HEAD(&dev->function_list);
    list_push(&dev->list, &vpci_config.device_list);

    return dev;
}

struct virtio_pci_function *vpci_create_function(
    struct virtio_pci_device *vpci_dev)
{
    struct virtio_pci_function *func =
        malloc(sizeof(struct virtio_pci_function));
    if (!func)
        return NULL;

    memset(func, 0, sizeof(*func));
    func->function = vpci_dev->function_cnt;
    func->command = PCI_COMMAND_SUPPORT_MASK;
    func->interrupt_line = 0xff; /* Disabled */
    func->interrupt_pin = 0x1;   /* Set to the minimum non-zero value */
    vpci_dev->function_cnt++;
    list_push(&func->list, &vpci_dev->function_list);

    return func;
}

static struct virtio_pci_device *vpci_get_device(uint16_t device)
{
    struct virtio_pci_device *dev;
    list_for_each_entry (dev, &vpci_config.device_list, list) {
        if (dev->device == device)
            return dev;
    }

    return NULL;
}

static struct virtio_pci_function *vpci_get_function(
    struct virtio_pci_device *vpci_dev,
    uint16_t function)
{
    struct virtio_pci_function *func;
    list_for_each_entry (func, &vpci_dev->function_list, list) {
        if (func->function == function)
            return func;
    }

    return NULL;
}

static inline void virtio_pci_bar_set_probe_mode(
    struct virtio_pci_function *vpci_func,
    int bar_id,
    uint32_t reg_value)
{
    if (reg_value == 0xFFFFFFFF)
        vpci_func->probe_bitmap |= (0x1 << bar_id);
}

static inline bool virtio_pci_bar_check_probe_mode(
    struct virtio_pci_function *vpci_func,
    int bar_id)
{
    return (vpci_func->probe_bitmap & (1 << bar_id)) != 0;
}

static inline void virtio_pci_set8(uint8_t *ptr, uint8_t value)
{
    *ptr = value;
}

static inline void virtio_pci_set16(uint16_t *ptr, uint16_t value)
{
    *ptr = value;
}

static inline void virtio_pci_set32(uint32_t *ptr, uint32_t value)
{
    *ptr = value;
}

static int virtio_pci_read_width_to_bytes(uint8_t width)
{
    switch (width) {
    case RV_MEM_LW:
        printf("RV_MEM_LW\n");
        return 4;
    case RV_MEM_LHU:
        printf("RV_MEM_LHU\n");
        return 2;
    case RV_MEM_LH:
        printf("RV_MEM_LH\n");
        return 2;
    case RV_MEM_LBU:
        printf("RV_MEM_LBU\n");
        return 1;
    case RV_MEM_LB:
        printf("RV_MEM_LB\n");
        return 1;
    default:
        return -1;
    }
}

static int virtio_pci_write_width_to_bytes(uint8_t width)
{
    switch (width) {
    case RV_MEM_SW:
        printf("RV_MEM_SW\n");
        return 4;
    case RV_MEM_SH:
        printf("RV_MEM_SH\n");
        return 2;
    case RV_MEM_SB:
        printf("RV_MEM_SB\n");
        return 1;
    default:
        return -1;
    }
}

static inline void virtio_pci_set_cap(struct virtio_pci_cap *cap,
                                      uint8_t cap_vndr,
                                      uint8_t cap_next,
                                      uint8_t cap_len,
                                      uint8_t cfg_type,
                                      uint8_t bar,
                                      uint8_t id,
                                      uint32_t offset,
                                      uint32_t length)
{
    cap->cap_vndr = cap_vndr;
    cap->cap_next = cap_next;
    cap->cap_len = cap_len;
    cap->cfg_type = cfg_type;
    cap->bar = bar;
    cap->id = id;
    cap->offset = offset;
    cap->length = length;
}

static void virtio_pci_set_capabilities(struct virtio_pci_function *vpci_func)
{
    /* First capability (common configuration) at 0x40 */
    uint32_t next_offset = PCI_CAP_POINTER_OFFSET;

    /* Initialize common configuration capability */
    next_offset += sizeof(struct virtio_pci_cap);
    virtio_pci_set_cap(&vpci_func->common_cap, 0x9, next_offset,
                       sizeof(struct virtio_pci_cap), VIRTIO_PCI_CAP_COMMON_CFG,
                       0x0, 0x0, 0x0, sizeof(struct virtio_pci_common_cfg));

    /* Initialize notification capability */
    next_offset += sizeof(struct virtio_pci_notify_cap);
    vpci_func->notify_cap.notify_off_multiplier = 0x4; /* 4 bytes */
    uint32_t notify_size = vpci_func->common_cfg.num_queues *
                           vpci_func->notify_cap.notify_off_multiplier;
    virtio_pci_set_cap(&vpci_func->notify_cap.cap, 0x9, next_offset,
                       sizeof(struct virtio_pci_notify_cap),
                       VIRTIO_PCI_CAP_NOTIFY_CFG, 0x1, 0x0, 0x0, notify_size);

    /* Initialize ISR capability */
    next_offset += sizeof(struct virtio_pci_notify_cap);
    virtio_pci_set_cap(&vpci_func->isr_cap, 0x9, next_offset,
                       sizeof(struct virtio_pci_cap), VIRTIO_PCI_CAP_ISR_CFG,
                       0x2, 0x0, 0x0, sizeof(struct virtio_pci_isr));
    next_offset += sizeof(struct virtio_pci_cap);

    /* Initialize device configuration capability */
    next_offset = 0x0; /* End of the capability list */
    virtio_pci_set_cap(&vpci_func->device_cap, 0x9, next_offset,
                       sizeof(struct virtio_pci_cap), VIRTIO_PCI_CAP_DEVICE_CFG,
                       0x3, 0x0, 0x0, vpci_func->device_config_size);
}

static void virtio_pci_bar_read_handler(struct virtio_pci_function *vpci_func,
                                        int bar_id,
                                        uint32_t *reg_value)
{
    if (virtio_pci_bar_check_probe_mode(vpci_func, bar_id)) {
        if (vpci_func->guest_bar_base[bar_id]) {
            /* Resource assignment confirmation */
            virtio_pci_set32((uint32_t *) reg_value,
                             vpci_func->guest_bar_base[bar_id]);
            printf("BAR: Resource assignment confirmation\n");
        } else {
            /* Range sizing */
            virtio_pci_set32((uint32_t *) reg_value, 0xFFFFF000); /* TODO */
            printf("BAR: Range sizing\n");
        }
    } else {
        /* BAR Configuration reading */
        virtio_pci_set32((uint32_t *) reg_value, PCI_BAR_CONFIGURATION);
        printf("BAR Read Configuration\n");
    }
    printf("BAR%d = 0x%x\n", bar_id, *reg_value);
}

static void virtio_pci_bar_write_handler(struct virtio_pci_function *vpci_func,
                                         int bar_id,
                                         uint32_t reg_value)
{
    if (virtio_pci_bar_check_probe_mode(vpci_func, bar_id))
        vpci_func->guest_bar_base[bar_id] = reg_value;
    virtio_pci_bar_set_probe_mode(vpci_func, bar_id, reg_value);
    printf("BAR%d = 0x%x\n", bar_id, reg_value);
}

static void virtio_pci_cap_list_read_handler(
    struct virtio_pci_function *vpci_func,
    uint32_t addr,
    uint32_t *value,
    int bytes)
{
    uintptr_t offset = GET_CONFIG(addr) - PCI_CAP_POINTER_OFFSET;
    uint8_t *src = (uint8_t *) value, *dst;

    if (offset < CAP_NOTIFY_START) {
        dst = (uint8_t *) &vpci_func->common_cap + (offset - CAP_COMMON_START);
        printf("CAP_COMMON_START\n");
    } else if (offset < CAP_ISR_START) {
        dst = (uint8_t *) &vpci_func->notify_cap + (offset - CAP_NOTIFY_START);
        printf("CAP_NOTIFY_START\n");
    } else if (offset < CAP_DEVICE_START) {
        dst = (uint8_t *) &vpci_func->isr_cap + (offset - CAP_ISR_START);
        printf("CAP_ISR_START\n");
    } else if (offset < CAP_DEVICE_END) {
        dst = (uint8_t *) &vpci_func->device_cap + (offset - CAP_DEVICE_START);
        printf("CAP_DEVICE_START\n");
    } else {
        fprintf(stderr, "Undefined capability list region.\n");
        return;
    }

    memcpy(src, dst, bytes);

    printf("Capability read: offset = %lu, value = 0x%x\n", offset,
           *value & MASK(bytes * 8));
}

static void virtio_pci_cap_list_write_handler(
    struct virtio_pci_function *vpci_func,
    uint32_t addr,
    uint32_t value,
    int bytes)
{
    uintptr_t offset = GET_CONFIG(addr) - PCI_CAP_POINTER_OFFSET;
    uint8_t *src, *dst = (uint8_t *) &value;

    if (offset < CAP_NOTIFY_START) {
        src = (uint8_t *) &vpci_func->common_cap + (offset - CAP_COMMON_START);
    } else if (offset < CAP_ISR_START) {
        src = (uint8_t *) &vpci_func->notify_cap + (offset - CAP_NOTIFY_START);
    } else if (offset < CAP_DEVICE_START) {
        src = (uint8_t *) &vpci_func->isr_cap + (offset - CAP_ISR_START);
    } else if (offset < CAP_DEVICE_END) {
        src = (uint8_t *) &vpci_func->device_cfg + (offset - CAP_DEVICE_START);
    } else {
        fprintf(stderr, "Undefined capability list region.\n");
        return;
    }

    memcpy(src, dst, bytes);

    printf("Capability write: offset = %lu, value = 0x%x\n", offset,
           value & MASK(bytes * 8));
}

void virtio_pci_read(hart_t *vm,
                     virtio_pci_state_t *vpci,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    int bus = GET_BUS(addr);
    int device = GET_DEVICE(addr);
    int function = GET_FUNCTION(addr);

    struct virtio_pci_device *vpci_dev = vpci_get_device(device);
    struct virtio_pci_function *vpci_func =
        vpci_dev ? vpci_get_function(vpci_dev, function) : NULL;

    printf("*** PCI read: 0x%x (bus: %d, dev: %d, func: %d) ***\n", addr, bus,
           device, function);

    int bytes = virtio_pci_read_width_to_bytes(width);
    if (bytes < 0) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

#define _(reg) PCI_CONFIG_##reg
    switch (GET_CONFIG(addr)) {
    case _(VENDOR_ID): {
        uint32_t word =
            (device < vpci_config.device_cnt)
                ? (vpci_func->device_id << 16) | VIRTIO_PCI_VENDOR_ID
                : 0xFFFF;
        virtio_pci_set32((uint32_t *) value, word);
        printf("VENDOR_ID = 0x%x\n", *value & 0xFFFF);
        printf("DEVICE_ID = 0x%x\n", (*value >> 16) & 0xFFFF);
        return;
    }
    case _(COMMAND):
        virtio_pci_set16((uint16_t *) value, vpci_func->command);
        printf("COMMAND = 0x%x\n", *value & 0xFFFF);
        return;
    case _(STATUS):
        /* Set capabilities list bit only */
        virtio_pci_set16((uint16_t *) value, PCI_STATUS_CAPABILITIES_LIST);
        printf("STATUS = 0x%x\n", *value & 0xFFFF);
        return;
    case _(REVISION_ID): {
        uint32_t word = (vpci_func->class_code[2] << 24) |
                        (vpci_func->class_code[1] << 16) |
                        (vpci_func->class_code[0] << 8) |
                        VIRTIO_PCI_REVISION_ID;
        virtio_pci_set32((uint32_t *) value, word);
        printf("REVISION_ID = %d\n", *value & 0xff);
        printf("CLASS_CODE = 0x%x\n", (*value >> 8) & 0xFFFFFF);
        return;
    }
    case _(CACHE_LINE_SIZE):
        virtio_pci_set8((uint8_t *) value, vpci_func->cache_line_size);
        return;
    case _(MASTER_LATENCY_TIMER):
        virtio_pci_set8((uint8_t *) value, vpci_func->master_latency_timer);
        return;
    case _(HEADER_TYPE): {
        uint8_t byte = (vpci_dev->function_cnt > 1)
                           ? PCI_TYPE0_HEADER | PCI_MULTI_FUNCTION_DEVICE
                           : PCI_TYPE0_HEADER;
        virtio_pci_set8((uint8_t *) value, byte);
        printf("HEADER_TYPE = %d\n", *value & 0xFF);
        return;
    }
    case _(BIST):
        virtio_pci_set8((uint8_t *) value, 0x0);
        return;
    case _(BAR0):
        virtio_pci_bar_read_handler(vpci_func, 0x0, value);
        return;
    case _(BAR1):
        virtio_pci_bar_read_handler(vpci_func, 0x1, value);
        return;
    case _(BAR2):
        virtio_pci_bar_read_handler(vpci_func, 0x2, value);
        return;
    case _(BAR3):
        virtio_pci_bar_read_handler(vpci_func, 0x3, value);
        return;
    case _(BAR4):
        virtio_pci_set32((uint32_t *) value, 0x0);
        printf("BAR4 = 0x%x\n", *value);
        return;
    case _(BAR5):
        virtio_pci_set32((uint32_t *) value, 0x0);
        printf("BAR5 = 0x%x\n", *value);
        return;
    case _(CARDBUS_CIS_POINTER):
        virtio_pci_set32((uint32_t *) value, 0x0);
        return;
    case _(SUBSYSTEM_VENDOR_ID):
        virtio_pci_set16((uint16_t *) value, 0x0);
        return;
    case _(SUBSYSTEM_ID):
        virtio_pci_set16((uint16_t *) value, 0x0);
        return;
    case _(EXPANSION_ROM_BASE_ADDRESS):
        virtio_pci_set32((uint32_t *) value, 0x0);
        return;
    case _(CAPABILITIES_POINTER):
        virtio_pci_set8((uint8_t *) value, PCI_CAP_POINTER_OFFSET);
        printf("CAPABILITIES_POINTER = 0x%x\n", *value & 0xFF);
        return;
    case _(INTERRUPT_LINE):
        virtio_pci_set8((uint8_t *) value, vpci_func->interrupt_line);
        return;
    case _(INTERRUPT_PIN):
        virtio_pci_set8((uint8_t *) value, vpci_func->interrupt_pin);
        return;
    case _(MIN_GNT):
        virtio_pci_set8((uint8_t *) value, 0x0);
        return;
    case _(MAX_LAT):
        virtio_pci_set8((uint8_t *) value, 0x0);
        return;
    default:
        virtio_pci_cap_list_read_handler(vpci_func, addr, value, bytes);
        return;
    }
}

void virtio_pci_write(hart_t *vm,
                      virtio_pci_state_t *vpci,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    int bus = GET_BUS(addr);
    int device = GET_DEVICE(addr);
    int function = GET_FUNCTION(addr);

    struct virtio_pci_device *vpci_dev = vpci_get_device(device);
    struct virtio_pci_function *vpci_func =
        vpci_dev ? vpci_get_function(vpci_dev, function) : NULL;

    printf("*** PCI write: 0x%x (bus: %d, dev: %d, func: %d) ***\n", addr, bus,
           device, function);

    int bytes = virtio_pci_write_width_to_bytes(width);
    if (bytes < 0) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

#define _(reg) PCI_CONFIG_##reg
    switch (GET_CONFIG(addr)) {
    case _(COMMAND):
        uint16_t halfword = value & PCI_COMMAND_SUPPORT_MASK;
        virtio_pci_set16((uint16_t *) &vpci_func->command, halfword);
        printf("COMMAND = 0x%x (driver), 0x%x (device)\n", value & 0xffff,
               halfword);
        return;
    case _(STATUS):
        return;
    case _(CACHE_LINE_SIZE):
        virtio_pci_set8((uint8_t *) &vpci_func->cache_line_size, value);
        return;
    case _(MASTER_LATENCY_TIMER):
        virtio_pci_set8((uint8_t *) &vpci_func->master_latency_timer, value);
        return;
    case _(BIST):
        return; /* Built-In Self Test can be ignored */
    case _(BAR0):
        virtio_pci_bar_write_handler(vpci_func, 0x0, value);
        return;
    case _(BAR1):
        virtio_pci_bar_write_handler(vpci_func, 0x1, value);
        return;
    case _(BAR2):
        virtio_pci_bar_write_handler(vpci_func, 0x2, value);
        return;
    case _(BAR3):
        virtio_pci_bar_write_handler(vpci_func, 0x3, value);
        return;
    case _(BAR4):
        printf("BAR4 = 0x%x\n", value);
        return;
    case _(BAR5):
        printf("BAR5 = 0x%x\n", value);
        return;
    case _(EXPANSION_ROM_BASE_ADDRESS):
        return;
    case _(INTERRUPT_LINE):
        virtio_pci_set8((uint8_t *) &vpci_func->interrupt_line, value);
        return;
    default:
        virtio_pci_cap_list_write_handler(vpci_func, addr, value, bytes);
        return;
    }
}

void virtio_pci_init(virtio_pci_state_t *vpci)
{
    /* Initialize PCI device list */
    INIT_LIST_HEAD(&vpci_config.device_list);

    /* XXX: Add testing device */
    struct virtio_pci_device *vpci_dev = vpci_create_device();
    struct virtio_pci_function *vpci_func = vpci_create_function(vpci_dev);
#if 0
    vpci_func->device_id = 0x1040 + 16; /* VirtIO GPU */
    vpci_func->class_code[0] = 0x00;
    vpci_func->class_code[1] = 0x00; /* VGA-compatible */
    vpci_func->class_code[2] = 0x03; /* Display Controller */
#else
    vpci_func->device_id = 0x1040 + 2; /* VirtIO Block Device */
    vpci_func->class_code[0] = 0x01;   /* Subclass: SCSI */
    vpci_func->class_code[1] = 0x00;   /* Interface: block */
    vpci_func->class_code[2] = 0x01;   /* Class: Mass Storage */
#endif

    /* Initialize VirtIO device */
    // XXX: common config
    // XXX: device config
#if 0
    vpci_func->common_cfg.device_feature_select = 0;
    vpci_func->common_cfg.device_feature = 0;
    vpci_func->common_cfg.driver_feature_select = 0;
    vpci_func->common_cfg.driver_feature = 0;
    vpci_func->common_cfg.config_msix_vector = 0;
    vpci_func->common_cfg.num_queues = 0;
    vpci_func->common_cfg.device_status = 0;
    vpci_func->common_cfg.config_generation = 0;
#endif

    /* Initialize VirtIO PCI capabilities */
    virtio_pci_set_capabilities(vpci_func);
}
