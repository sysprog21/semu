#pragma once

#if SEMU_HAS(VIRTIONET)
#include "netdev.h"
#endif
#include "riscv.h"
#include "virtio.h"

/* RAM */

#define RAM_SIZE (512 * 1024 * 1024)
#define DTB_SIZE (1 * 1024 * 1024)
#define INITRD_SIZE (8 * 1024 * 1024)

void ram_read(hart_t *core,
              uint32_t *mem,
              const uint32_t addr,
              const uint8_t width,
              uint32_t *value);

void ram_write(hart_t *core,
               uint32_t *mem,
               const uint32_t addr,
               const uint8_t width,
               const uint32_t value);

/* PLIC */

typedef struct {
    uint32_t masked;
    uint32_t ip;     /* support 32 interrupt sources only */
    uint32_t ie[32]; /* support 32 sources to 32 contexts only */
    /* state of input interrupt lines (level-triggered), set by environment */
    uint32_t active;
} plic_state_t;

void plic_update_interrupts(vm_t *vm, plic_state_t *plic);
void plic_read(hart_t *core,
               plic_state_t *plic,
               uint32_t addr,
               uint8_t width,
               uint32_t *value);
void plic_write(hart_t *core,
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
void u8250_read(hart_t *core,
                u8250_state_t *uart,
                uint32_t addr,
                uint8_t width,
                uint32_t *value);
void u8250_write(hart_t *core,
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
    netdev_t peer;
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_net_state_t;

void virtio_net_read(hart_t *core,
                     virtio_net_state_t *vnet,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);
void virtio_net_write(hart_t *core,
                      virtio_net_state_t *vnet,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);
void virtio_net_refresh_queue(virtio_net_state_t *vnet);

void virtio_net_recv_from_peer(void *peer);

bool virtio_net_init(virtio_net_state_t *vnet, const char *name);
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

void virtio_blk_read(hart_t *vm,
                     virtio_blk_state_t *vblk,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_blk_write(hart_t *vm,
                      virtio_blk_state_t *vblk,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk, char *disk_file);
#endif /* SEMU_HAS(VIRTIOBLK) */

/* VirtIO-RNG */

#if SEMU_HAS(VIRTIORNG)

#define IRQ_VRNG 4
#define IRQ_VRNG_BIT (1 << IRQ_VRNG)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_rng_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_rng_queue_t queues[1];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    uint32_t *ram;
} virtio_rng_state_t;

void virtio_rng_read(hart_t *vm,
                     virtio_rng_state_t *rng,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_rng_write(hart_t *vm,
                      virtio_rng_state_t *vrng,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

void virtio_rng_init(void);
#endif /* SEMU_HAS(VIRTIORNG) */

/* ACLINT MTIMER */
typedef struct {
    /* A MTIMER device has two separate base addresses: one for the MTIME
     * register and another for the MTIMECMP registers.
     *
     * The MTIME register is a 64-bit read-write register that contains the
     * number of cycles counted based on a fixed reference frequency.
     *
     * The MTIMECMP registers are 'per-HART' 64-bit read-write registers. It
     * contains the MTIME register value at which machine-level timer interrupt
     * is to be triggered for the corresponding HART.
     *
     * Up to 4095 MTIMECMP registers can exist, corresponding to 4095 HARTs in
     * the system.
     *
     * For more details, please refer to the register map at:
     * https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc#21-register-map
     */
    uint64_t *mtimecmp;
    semu_timer_t mtime;
} mtimer_state_t;

void aclint_mtimer_update_interrupts(hart_t *hart, mtimer_state_t *mtimer);
void aclint_mtimer_read(hart_t *hart,
                        mtimer_state_t *mtimer,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t *value);
void aclint_mtimer_write(hart_t *hart,
                         mtimer_state_t *mtimer,
                         uint32_t addr,
                         uint8_t width,
                         uint32_t value);

/* ACLINT MSWI */
typedef struct {
    /* The MSWI device provides machine-level IPI functionality for a set of
     * HARTs on a RISC-V platform. It has an IPI register (MSIP) for each HART
     * connected to the MSWI device.
     *
     * Up to 4095 MSIP registers can be used, corresponding to 4095 HARTs in the
     * system. The 4096th MSIP register is reserved for future use.
     *
     * For more details, please refer to the register map at:
     * https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc#31-register-map
     */
    uint32_t *msip;
} mswi_state_t;

void aclint_mswi_update_interrupts(hart_t *hart, mswi_state_t *mswi);
void aclint_mswi_read(hart_t *hart,
                      mswi_state_t *mswi,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t *value);
void aclint_mswi_write(hart_t *hart,
                       mswi_state_t *mswi,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t value);

/* ACLINT SSWI */
typedef struct {
    /* The SSWI device provides supervisor-level IPI functionality for a set of
     * HARTs on a RISC-V platform. It provides a register to set an IPI
     * (SETSSIP) for each HART connected to the SSWI device.
     *
     * Up to 4095 SETSSIP registers can be used, corresponding to 4095 HARTs in
     * the system. The 4096th SETSSIP register is reserved for future use.
     *
     * For more details, please refer to the register map at:
     * https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc#41-register-map
     */
    uint32_t *ssip;
} sswi_state_t;

void aclint_sswi_update_interrupts(hart_t *hart, sswi_state_t *sswi);
void aclint_sswi_read(hart_t *hart,
                      sswi_state_t *sswi,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t *value);
void aclint_sswi_write(hart_t *hart,
                       sswi_state_t *sswi,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t value);

/* VirtIO-Sound */

#if SEMU_HAS(VIRTIOSND)
#define IRQ_VSND 5
#define IRQ_VSND_BIT (1 << IRQ_VSND)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_snd_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_snd_queue_t queues[4];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_snd_state_t;

void virtio_snd_read(hart_t *core,
                     virtio_snd_state_t *vsnd,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_snd_write(hart_t *core,
                      virtio_snd_state_t *vsnd,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

bool virtio_snd_init(virtio_snd_state_t *vsnd);
#endif /* SEMU_HAS(VIRTIOSND) */

/* VirtIO-File-System */

#if SEMU_HAS(VIRTIOFS)
#define IRQ_VFS 6
#define IRQ_VFS_BIT (1 << IRQ_VFS)

typedef struct inode_map_entry {
    uint64_t ino;
    char *path;
    struct inode_map_entry *next;
} inode_map_entry;

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_fs_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;

    /* queue config */
    uint32_t QueueSel;
    virtio_fs_queue_t queues[3];

    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;

    /* guest memory base */
    uint32_t *ram;

    char *mount_tag; /* guest sees this tag */
    char *shared_dir;

    inode_map_entry *inode_map;

    /* optional implementation-specific */
    void *priv;
} virtio_fs_state_t;

/* MMIO read/write */
void virtio_fs_read(hart_t *core,
                    virtio_fs_state_t *vfs,
                    uint32_t addr,
                    uint8_t width,
                    uint32_t *value);

void virtio_fs_write(hart_t *core,
                     virtio_fs_state_t *vfs,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t value);

bool virtio_fs_init(virtio_fs_state_t *vfs, char *mtag, char *dir);

#endif /* SEMU_HAS(VIRTIOFS) */

/* memory mapping */
typedef struct {
    bool debug;
    bool stopped;
    uint32_t *ram;
    uint32_t *disk;
    vm_t vm;
    plic_state_t plic;
    u8250_state_t uart;
#if SEMU_HAS(VIRTIONET)
    virtio_net_state_t vnet;
#endif
#if SEMU_HAS(VIRTIOBLK)
    virtio_blk_state_t vblk;
#endif
#if SEMU_HAS(VIRTIORNG)
    virtio_rng_state_t vrng;
#endif
    /* ACLINT */
    mtimer_state_t mtimer;
    mswi_state_t mswi;
    sswi_state_t sswi;
#if SEMU_HAS(VIRTIOSND)
    virtio_snd_state_t vsnd;
#endif
#if SEMU_HAS(VIRTIOFS)
    virtio_fs_state_t vfs;
#endif

    uint32_t peripheral_update_ctr;

    /* The fields used for debug mode */
    bool is_interrupted;
    int curr_cpuid;
} emu_state_t;
