#pragma once

#include "riscv.h"

/* RAM */

#define RAM_SIZE (512 * 1024 * 1024)

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

/* PLIC */

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

/* VirtIO-Net */

#if defined(ENABLE_VIRTIONET)
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
#endif /* ENABLE_VIRTIONET */

/* memory mapping */

typedef struct {
    bool stopped;
    uint32_t *ram;
    plic_state_t plic;
    u8250_state_t uart;
#if defined(ENABLE_VIRTIONET)
    virtio_net_state_t vnet;
#endif
    uint32_t timer_lo, timer_hi;
} emu_state_t;
