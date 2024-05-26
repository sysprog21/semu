#pragma once

#include "riscv.h"
#include "virtio.h"

/* RAM */

#define RAM_SIZE (512 * 1024 * 1024)
#define DTB_SIZE (1 * 1024 * 1024)
#define INITRD_SIZE (8 * 1024 * 1024)

void ram_read(vm_t *core,
              uint32_t *mem,
              const uint32_t addr,
              const uint8_t width,
              uint32_t *value);

void ram_write(vm_t *core,
               uint32_t *mem,
               const uint32_t addr,
               const uint8_t width,
               const uint32_t value);

/* ACLINT */
#define ACLINT_REG_LIST    \
    _(SSWI, 0x0000)        \
    _(MTIMECMP_LO, 0x4000) \
    _(MTIMECMP_HI, 0x4004) \
    _(MTIME_LO, 0x7FF8)    \
    _(MTIME_HI, 0x7FFC)

enum {
#define _(reg, addr) ACLINT_##reg = addr >> 2,
    ACLINT_REG_LIST
#undef _
};

typedef struct {
    vm_t *vm;
    vm_timer_t mtimer;
    uint64_t mtimecmp;
    uint32_t setssip;
} aclint_state_t;

void aclint_update_interrupts(vm_t *core, aclint_state_t *aclint);
void aclint_timer_interrupts(vm_t *core, aclint_state_t *aclint);
void aclint_read(vm_t *core,
                 aclint_state_t *aclint,
                 uint32_t addr,
                 uint8_t width,
                 uint32_t *value);
void aclint_write(vm_t *core,
                  aclint_state_t *aclint,
                  uint32_t addr,
                  uint8_t width,
                  uint32_t value);
void aclint_send_ipi(vm_t *vm, aclint_state_t *aclint, uint32_t target_hart);

/* PLIC */
#define PLIC_REG_LIST               \
    _(InterruptPending, 0x1000)     \
    _(InterruptEnable, 0x2000)      \
    _(PriorityThresholds, 0x200000) \
    _(InterruptClaim, 0x200004)     \
    _(InterruptCompletion, 0x200004)

enum {
#define _(reg, addr) PLIC_##reg = addr >> 2,
    PLIC_REG_LIST
#undef _
};

typedef struct {
    uint32_t masked;
    uint32_t ip;
    uint32_t ie;
    /* state of input interrupt lines (level-triggered), set by environment */
    uint32_t active;
} plic_state_t;

void plic_update_interrupts(vm_t *core, plic_state_t *plic);
void plic_read(vm_t *core,
               plic_state_t *plic,
               uint32_t addr,
               uint8_t width,
               uint32_t *value);
void plic_write(vm_t *core,
                plic_state_t *plic,
                uint32_t addr,
                uint8_t width,
                uint32_t value);
/* UART */

#define IRQ_UART 1
#define IRQ_UART_BIT (1 << IRQ_UART)

typedef struct {
    uint8_t dll, dlh;                  /**< divisor (ignored) */
    uint8_t lcr;                       /**< UART config */
    uint8_t ier;                       /**< interrupt config */
    uint8_t current_int, pending_ints; /**< interrupt status */
    /* other output signals, loopback mode (ignored) */
    uint8_t mcr;
    /* I/O handling */
    int in_fd, out_fd;
    bool in_ready;
} u8250_state_t;

void u8250_update_interrupts(u8250_state_t *uart);
void u8250_read(vm_t *core,
                u8250_state_t *uart,
                uint32_t addr,
                uint8_t width,
                uint32_t *value);
void u8250_write(vm_t *core,
                 u8250_state_t *uart,
                 uint32_t addr,
                 uint8_t width,
                 uint32_t value);
void u8250_check_ready(u8250_state_t *uart);
void capture_keyboard_input();

/* virtio-net */

#if SEMU_HAS(VIRTIONET)
#define IRQ_VNET 2
#define IRQ_VNET_BIT (1 << IRQ_VNET)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
    bool fd_ready;
} virtio_net_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_net_queue_t queues[2];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    int tap_fd;
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_net_state_t;

void virtio_net_read(vm_t *core,
                     virtio_net_state_t *vnet,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);
void virtio_net_write(vm_t *core,
                      virtio_net_state_t *vnet,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);
void virtio_net_refresh_queue(virtio_net_state_t *vnet);

bool virtio_net_init(virtio_net_state_t *vnet);
#endif /* SEMU_HAS(VIRTIONET) */

/* VirtIO-Block */

#if SEMU_HAS(VIRTIOBLK)

#define IRQ_VBLK 3
#define IRQ_VBLK_BIT (1 << IRQ_VBLK)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_blk_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_blk_queue_t queues[2];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    uint32_t *ram;
    uint32_t *disk;
    /* implementation-specific */
    void *priv;
} virtio_blk_state_t;

void virtio_blk_read(vm_t *vm,
                     virtio_blk_state_t *vblk,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_blk_write(vm_t *vm,
                      virtio_blk_state_t *vblk,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk, char *disk_file);
#endif /* SEMU_HAS(VIRTIOBLK) */

/* memory mapping */

typedef struct {
    bool stopped;
    uint32_t *ram;
    uint32_t *disk;
    aclint_state_t aclint;
    plic_state_t plic;
    u8250_state_t uart;
#if SEMU_HAS(VIRTIONET)
    virtio_net_state_t vnet;
#endif
#if SEMU_HAS(VIRTIOBLK)
    virtio_blk_state_t vblk;
#endif
} emu_state_t;
