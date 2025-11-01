#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef MMU_CACHE_STATS
#include <sys/time.h>
#endif

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#else
#include <sys/timerfd.h>
#endif

#include "coro.h"
#include "device.h"
#include "mini-gdbstub/include/gdbstub.h"
#include "riscv.h"
#include "riscv_private.h"
#define PRIV(x) ((emu_state_t *) x->priv)

/* Forward declarations for coroutine support */
static void wfi_handler(hart_t *hart);
static void hart_exec_loop(void *arg);

/* Define fetch separately since it is simpler (fixed width, already checked
 * alignment, only main RAM is executable).
 */
static void mem_fetch(hart_t *hart, uint32_t n_pages, uint32_t **page_addr)
{
    emu_state_t *data = PRIV(hart);
    if (unlikely(n_pages >= RAM_SIZE / RV_PAGE_SIZE)) {
        /* TODO: check for other regions */
        vm_set_exception(hart, RV_EXC_FETCH_FAULT, hart->exc_val);
        return;
    }
    *page_addr = &data->ram[n_pages << (RV_PAGE_SHIFT - 2)];
}

/* Similarly, only main memory pages can be used as page tables. */
static uint32_t *mem_page_table(const hart_t *hart, uint32_t ppn)
{
    emu_state_t *data = PRIV(hart);
    if (ppn < (RAM_SIZE / RV_PAGE_SIZE))
        return &data->ram[ppn << (RV_PAGE_SHIFT - 2)];
    return NULL;
}

static void emu_update_uart_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    u8250_update_interrupts(&data->uart);
    if (data->uart.pending_ints)
        data->plic.active |= IRQ_UART_BIT;
    else
        data->plic.active &= ~IRQ_UART_BIT;
    plic_update_interrupts(vm, &data->plic);
}

#if SEMU_HAS(VIRTIONET)
static void emu_update_vnet_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    if (data->vnet.InterruptStatus)
        data->plic.active |= IRQ_VNET_BIT;
    else
        data->plic.active &= ~IRQ_VNET_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

#if SEMU_HAS(VIRTIOBLK)
static void emu_update_vblk_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    if (data->vblk.InterruptStatus)
        data->plic.active |= IRQ_VBLK_BIT;
    else
        data->plic.active &= ~IRQ_VBLK_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

#if SEMU_HAS(VIRTIORNG)
static void emu_update_vrng_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    if (data->vrng.InterruptStatus)
        data->plic.active |= IRQ_VRNG_BIT;
    else
        data->plic.active &= ~IRQ_VRNG_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

static void emu_update_timer_interrupt(hart_t *hart)
{
    emu_state_t *data = PRIV(hart);

    /* Sync global timer with local timer */
    hart->time = data->mtimer.mtime;
    aclint_mtimer_update_interrupts(hart, &data->mtimer);
}

static void emu_update_swi_interrupt(hart_t *hart)
{
    emu_state_t *data = PRIV(hart);
    aclint_mswi_update_interrupts(hart, &data->mswi);
    aclint_sswi_update_interrupts(hart, &data->sswi);
}

#if SEMU_HAS(VIRTIOSND)
static void emu_update_vsnd_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    if (data->vsnd.InterruptStatus)
        data->plic.active |= IRQ_VSND_BIT;
    else
        data->plic.active &= ~IRQ_VSND_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

#if SEMU_HAS(VIRTIOFS)
static void emu_update_vfs_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    if (data->vfs.InterruptStatus)
        data->plic.active |= IRQ_VFS_BIT;
    else
        data->plic.active &= ~IRQ_VFS_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

/* Peripheral I/O polling strategy
 *
 * We use inline polling instead of dedicated I/O coroutines for peripherals.
 *
 * Rationale:
 * 1. Non-blocking poll() is extremely cheap (~200ns syscall overhead)
 * 2. Inline polling provides lowest latency (checked every 64 instructions)
 * 3. All harts share peripheral_update_ctr, ensuring frequent polling
 *    regardless of hart count (e.g., 4 harts = 4 polls per 256 instructions)
 * 4. Coroutine-based I/O would INCREASE latency by n_hart factor due to
 *    scheduler round-robin, without reducing poll() overhead meaningfully
 *
 * Coroutines are reserved for hart scheduling where they provide real value:
 * - Enable event-driven WFI (avoid busy-wait when guest is idle)
 * - Support SBI HSM (Hart State Management) for dynamic hart start/stop
 * - Provide clean abstraction for multi-hart execution
 *
 * For simple non-blocking I/O, inline polling is superior.
 */
static inline void emu_tick_peripherals(emu_state_t *emu)
{
    vm_t *vm = &emu->vm;

    if (emu->peripheral_update_ctr-- == 0) {
        emu->peripheral_update_ctr = 64;

        u8250_check_ready(&emu->uart);
        if (emu->uart.in_ready)
            emu_update_uart_interrupts(vm);

#if SEMU_HAS(VIRTIONET)
        virtio_net_refresh_queue(&emu->vnet);
        if (emu->vnet.InterruptStatus)
            emu_update_vnet_interrupts(vm);
#endif

#if SEMU_HAS(VIRTIOBLK)
        if (emu->vblk.InterruptStatus)
            emu_update_vblk_interrupts(vm);
#endif

#if SEMU_HAS(VIRTIORNG)
        if (emu->vrng.InterruptStatus)
            emu_update_vrng_interrupts(vm);
#endif

#if SEMU_HAS(VIRTIOSND)
        if (emu->vsnd.InterruptStatus)
            emu_update_vsnd_interrupts(vm);
#endif

#if SEMU_HAS(VIRTIOFS)
        if (emu->vfs.InterruptStatus)
            emu_update_vfs_interrupts(vm);
#endif
    }
}

static void mem_load(hart_t *hart,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    emu_state_t *data = PRIV(hart);
    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_read(hart, data->ram, addr, width, value);
        return;
    }

    if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */
        /* 256 regions of 1MiB */
        switch ((addr >> 20) & MASK(8)) {
        case 0x0:
        case 0x2: /* PLIC (0 - 0x3F) */
            plic_read(hart, &data->plic, addr & 0x3FFFFFF, width, value);
            return;
        case 0x40: /* UART */
            u8250_read(hart, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(hart->vm);
            return;
#if SEMU_HAS(VIRTIONET)
        case 0x41: /* virtio-net */
            virtio_net_read(hart, &data->vnet, addr & 0xFFFFF, width, value);
            return;
#endif
#if SEMU_HAS(VIRTIOBLK)
        case 0x42: /* virtio-blk */
            virtio_blk_read(hart, &data->vblk, addr & 0xFFFFF, width, value);
            return;
#endif
        case 0x43: /* mtimer */
            aclint_mtimer_read(hart, &data->mtimer, addr & 0xFFFFF, width,
                               value);
            return;
        case 0x44: /* mswi */
            aclint_mswi_read(hart, &data->mswi, addr & 0xFFFFF, width, value);
            return;
        case 0x45: /* sswi */
            aclint_sswi_read(hart, &data->sswi, addr & 0xFFFFF, width, value);
            return;
#if SEMU_HAS(VIRTIORNG)
        case 0x46: /* virtio-rng */
            virtio_rng_read(hart, &data->vrng, addr & 0xFFFFF, width, value);
            return;
#endif

#if SEMU_HAS(VIRTIOSND)
        case 0x47: /* virtio-snd */
            virtio_snd_read(hart, &data->vsnd, addr & 0xFFFFF, width, value);
            return;
#endif

#if SEMU_HAS(VIRTIOFS)
        case 0x48: /* virtio-fs */
            virtio_fs_read(hart, &data->vfs, addr & 0xFFFFF, width, value);
            return;
#endif
        }
    }
    vm_set_exception(hart, RV_EXC_LOAD_FAULT, hart->exc_val);
}

static void mem_store(hart_t *hart,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    emu_state_t *data = PRIV(hart);
    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_write(hart, data->ram, addr, width, value);
        return;
    }

    if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */
        /* 256 regions of 1MiB */
        switch ((addr >> 20) & MASK(8)) {
        case 0x0:
        case 0x2: /* PLIC (0 - 0x3F) */
            plic_write(hart, &data->plic, addr & 0x3FFFFFF, width, value);
            plic_update_interrupts(hart->vm, &data->plic);
            return;
        case 0x40: /* UART */
            u8250_write(hart, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(hart->vm);
            return;
#if SEMU_HAS(VIRTIONET)
        case 0x41: /* virtio-net */
            virtio_net_write(hart, &data->vnet, addr & 0xFFFFF, width, value);
            emu_update_vnet_interrupts(hart->vm);
            return;
#endif
#if SEMU_HAS(VIRTIOBLK)
        case 0x42: /* virtio-blk */
            virtio_blk_write(hart, &data->vblk, addr & 0xFFFFF, width, value);
            emu_update_vblk_interrupts(hart->vm);
            return;
#endif
        case 0x43: /* mtimer */
            aclint_mtimer_write(hart, &data->mtimer, addr & 0xFFFFF, width,
                                value);
            aclint_mtimer_update_interrupts(hart, &data->mtimer);
            return;
        case 0x44: /* mswi */
            aclint_mswi_write(hart, &data->mswi, addr & 0xFFFFF, width, value);
            aclint_mswi_update_interrupts(hart, &data->mswi);
            return;
        case 0x45: /* sswi */
            aclint_sswi_write(hart, &data->sswi, addr & 0xFFFFF, width, value);
            aclint_sswi_update_interrupts(hart, &data->sswi);
            return;

#if SEMU_HAS(VIRTIORNG)
        case 0x46: /* virtio-rng */
            virtio_rng_write(hart, &data->vrng, addr & 0xFFFFF, width, value);
            emu_update_vrng_interrupts(hart->vm);
            return;
#endif

#if SEMU_HAS(VIRTIOSND)
        case 0x47: /* virtio-snd */
            virtio_snd_write(hart, &data->vsnd, addr & 0xFFFFF, width, value);
            emu_update_vsnd_interrupts(hart->vm);
            return;
#endif

#if SEMU_HAS(VIRTIOFS)
        case 0x48: /* virtio-fs */
            virtio_fs_write(hart, &data->vfs, addr & 0xFFFFF, width, value);
            emu_update_vfs_interrupts(hart->vm);
            return;
#endif
        }
    }
    vm_set_exception(hart, RV_EXC_STORE_FAULT, hart->exc_val);
}

/* SBI */
#define SBI_IMPL_ID 0x999
#define SBI_IMPL_VERSION 1

typedef struct {
    int32_t error;
    int32_t value;
} sbi_ret_t;

static inline sbi_ret_t handle_sbi_ecall_TIMER(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);
    switch (fid) {
    case SBI_TIMER__SET_TIMER:
        data->mtimer.mtimecmp[hart->mhartid] =
            (((uint64_t) hart->x_regs[RV_R_A1]) << 32) |
            (uint64_t) (hart->x_regs[RV_R_A0]);
        hart->sip &= ~RV_INT_STI_BIT;
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_RST(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);
    switch (fid) {
    case SBI_RST__SYSTEM_RESET:
        fprintf(stderr, "system reset: type=%u, reason=%u\n",
                hart->x_regs[RV_R_A0], hart->x_regs[RV_R_A1]);
        data->stopped = true;
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_HSM(hart_t *hart, int32_t fid)
{
    uint32_t hartid, start_addr, opaque, suspend_type, resume_addr;
    vm_t *vm = hart->vm;
    switch (fid) {
    case SBI_HSM__HART_START:
        hartid = hart->x_regs[RV_R_A0];
        start_addr = hart->x_regs[RV_R_A1];
        opaque = hart->x_regs[RV_R_A2];
        vm->hart[hartid]->hsm_status = SBI_HSM_STATE_STARTED;
        vm->hart[hartid]->satp = 0;
        vm->hart[hartid]->sstatus_sie = 0;
        vm->hart[hartid]->x_regs[RV_R_A0] = hartid;
        vm->hart[hartid]->x_regs[RV_R_A1] = opaque;
        vm->hart[hartid]->pc = start_addr;
        vm->hart[hartid]->s_mode = true;
        return (sbi_ret_t){SBI_SUCCESS, 0};
    case SBI_HSM__HART_STOP:
        hart->hsm_status = SBI_HSM_STATE_STOPPED;
        return (sbi_ret_t){SBI_SUCCESS, 0};
    case SBI_HSM__HART_GET_STATUS:
        hartid = hart->x_regs[RV_R_A0];
        return (sbi_ret_t){SBI_SUCCESS, vm->hart[hartid]->hsm_status};
    case SBI_HSM__HART_SUSPEND:
        suspend_type = hart->x_regs[RV_R_A0];
        resume_addr = hart->x_regs[RV_R_A1];
        opaque = hart->x_regs[RV_R_A2];
        hart->hsm_status = SBI_HSM_STATE_SUSPENDED;
        if (suspend_type == 0x00000000) {
            hart->hsm_resume_is_ret = true;
            hart->hsm_resume_pc = hart->pc;
        } else if (suspend_type == 0x80000000) {
            hart->hsm_resume_is_ret = false;
            hart->hsm_resume_pc = resume_addr;
            hart->hsm_resume_opaque = opaque;
        }
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
    return (sbi_ret_t){SBI_ERR_FAILED, 0};
}

static inline sbi_ret_t handle_sbi_ecall_IPI(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);
    uint64_t hart_mask, hart_mask_base;
    switch (fid) {
    case SBI_IPI__SEND_IPI:
        hart_mask = (uint64_t) hart->x_regs[RV_R_A0];
        hart_mask_base = (uint64_t) hart->x_regs[RV_R_A1];
        if (hart_mask_base == 0xFFFFFFFFFFFFFFFF) {
            for (uint32_t i = 0; i < hart->vm->n_hart; i++)
                data->sswi.ssip[i] = 1;
        } else {
            for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++)
                data->sswi.ssip[i] = hart_mask & 1;
        }

        return (sbi_ret_t){SBI_SUCCESS, 0};
        break;
    default:
        return (sbi_ret_t){SBI_ERR_FAILED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_RFENCE(hart_t *hart, int32_t fid)
{
    /* TODO: Since the current implementation sequentially emulates
     * multi-core execution, the implementation of RFENCE extension is not
     * complete, for example, FENCE.I is currently ignored. To support
     * multi-threaded system emulation, RFENCE extension has to be implemented
     * completely.
     */
    uint64_t hart_mask, hart_mask_base;
    switch (fid) {
    case 0:
        return (sbi_ret_t){SBI_SUCCESS, 0};
    case 1:
        hart_mask = (uint64_t) hart->x_regs[RV_R_A0];
        hart_mask_base = (uint64_t) hart->x_regs[RV_R_A1];
        if (hart_mask_base == 0xFFFFFFFFFFFFFFFF) {
            for (uint32_t i = 0; i < hart->vm->n_hart; i++) {
                mmu_invalidate(hart->vm->hart[i]);
            }
        } else {
            for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++) {
                mmu_invalidate(hart->vm->hart[i]);
            }
        }
        return (sbi_ret_t){SBI_SUCCESS, 0};
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_FAILED, 0};
    }
}

#define RV_MVENDORID 0x12345678
#define RV_MARCHID ((1ULL << 31) | 1)
#define RV_MIMPID 1

static inline sbi_ret_t handle_sbi_ecall_BASE(hart_t *hart, int32_t fid)
{
    switch (fid) {
    case SBI_BASE__GET_SBI_IMPL_ID:
        return (sbi_ret_t){SBI_SUCCESS, SBI_IMPL_ID};
    case SBI_BASE__GET_SBI_IMPL_VERSION:
        return (sbi_ret_t){SBI_SUCCESS, SBI_IMPL_VERSION};
    case SBI_BASE__GET_MVENDORID:
        return (sbi_ret_t){SBI_SUCCESS, RV_MVENDORID};
    case SBI_BASE__GET_MARCHID:
        return (sbi_ret_t){SBI_SUCCESS, RV_MARCHID};
    case SBI_BASE__GET_MIMPID:
        return (sbi_ret_t){SBI_SUCCESS, RV_MIMPID};
    case SBI_BASE__GET_SBI_SPEC_VERSION:
        return (sbi_ret_t){SBI_SUCCESS, (2 << 24) | 0}; /* version 2.0 */
    case SBI_BASE__PROBE_EXTENSION: {
        int32_t eid = (int32_t) hart->x_regs[RV_R_A0];
        bool available = eid == SBI_EID_BASE || eid == SBI_EID_TIMER ||
                         eid == SBI_EID_RST || eid == SBI_EID_HSM ||
                         eid == SBI_EID_IPI || eid == SBI_EID_RFENCE;
        return (sbi_ret_t){SBI_SUCCESS, available};
    }
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

#define SBI_HANDLE(TYPE) \
    ret = handle_sbi_ecall_##TYPE(hart, hart->x_regs[RV_R_A6])

static void handle_sbi_ecall(hart_t *hart)
{
    sbi_ret_t ret;
    switch (hart->x_regs[RV_R_A7]) {
    case SBI_EID_BASE:
        SBI_HANDLE(BASE);
        break;
    case SBI_EID_TIMER:
        SBI_HANDLE(TIMER);
        break;
    case SBI_EID_RST:
        SBI_HANDLE(RST);
        break;
    case SBI_EID_HSM:
        SBI_HANDLE(HSM);
        break;
    case SBI_EID_IPI:
        SBI_HANDLE(IPI);
        break;
    case SBI_EID_RFENCE:
        SBI_HANDLE(RFENCE);
        break;
    default:
        ret = (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
    hart->x_regs[RV_R_A0] = (uint32_t) ret.error;
    hart->x_regs[RV_R_A1] = (uint32_t) ret.value;

    /* Clear error to allow execution to continue */
    hart->error = ERR_NONE;
}

#define N_MAPPERS 4

struct mapper {
    char *addr;
    uint32_t size;
};

static struct mapper mapper[N_MAPPERS] = {0};
static int map_index = 0;
static void unmap_files(void)
{
    while (map_index--) {
        if (!mapper[map_index].addr)
            continue;
        munmap(mapper[map_index].addr, mapper[map_index].size);
    }
}

static void map_file(char **ram_loc, const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "could not open %s\n", name);
        exit(2);
    }

    /* get file size */
    struct stat st;
    fstat(fd, &st);

    /* remap to a memory region */
    *ram_loc = mmap(*ram_loc, st.st_size, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_PRIVATE, fd, 0);
    if (*ram_loc == MAP_FAILED) {
        perror("mmap");
        close(fd);
        exit(2);
    }

    mapper[map_index].addr = *ram_loc;
    mapper[map_index].size = st.st_size;
    map_index++;

    /* The kernel selects a nearby page boundary and attempts to create
     * the mapping.
     */
    *ram_loc += st.st_size;

    close(fd);
}

static void usage(const char *execpath)
{
    fprintf(stderr,
            "Usage: %s -k linux-image [-b dtb] [-i initrd-image] [-d "
            "disk-image] [-s shared-directory]\n",
            execpath);
}

static void handle_options(int argc,
                           char **argv,
                           char **kernel_file,
                           char **dtb_file,
                           char **initrd_file,
                           char **disk_file,
                           char **net_dev,
                           int *hart_count,
                           bool *debug,
                           char **shared_dir)
{
    *kernel_file = *dtb_file = *initrd_file = *disk_file = *net_dev =
        *shared_dir = NULL;

    int optidx = 0;
    struct option opts[] = {{"kernel", 1, NULL, 'k'},    {"dtb", 1, NULL, 'b'},
                            {"initrd", 1, NULL, 'i'},    {"disk", 1, NULL, 'd'},
                            {"netdev", 1, NULL, 'n'},    {"smp", 1, NULL, 'c'},
                            {"gdbstub", 0, NULL, 'g'},   {"help", 0, NULL, 'h'},
                            {"shared_dir", 1, NULL, 's'}};

    int c;
    while ((c = getopt_long(argc, argv, "k:b:i:d:n:c:s:gh", opts, &optidx)) !=
           -1) {
        switch (c) {
        case 'k':
            *kernel_file = optarg;
            break;
        case 'b':
            *dtb_file = optarg;
            break;
        case 'i':
            *initrd_file = optarg;
            break;
        case 'd':
            *disk_file = optarg;
            break;
        case 'n':
            *net_dev = optarg;
            break;
        case 'c':
            *hart_count = atoi(optarg);
            break;
        case 's':
            *shared_dir = optarg;
            break;
        case 'g':
            *debug = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            break;
        }
    }

    if (!*kernel_file) {
        fprintf(stderr,
                "Linux kernel image file must "
                "be provided via -k option.\n");
        usage(argv[0]);
        exit(2);
    }

    if (!*dtb_file)
        *dtb_file = "minimal.dtb";
}

#define INIT_HART(hart, emu, id)                  \
    do {                                          \
        hart->priv = emu;                         \
        hart->mhartid = id;                       \
        hart->mem_fetch = mem_fetch;              \
        hart->mem_load = mem_load;                \
        hart->mem_store = mem_store;              \
        hart->mem_page_table = mem_page_table;    \
        hart->s_mode = true;                      \
        hart->hsm_status = SBI_HSM_STATE_STOPPED; \
        vm_init(hart);                            \
    } while (0)

static int semu_init(emu_state_t *emu, int argc, char **argv)
{
    char *kernel_file;
    char *dtb_file;
    char *initrd_file;
    char *disk_file;
    char *netdev;
    char *shared_dir;
    int hart_count = 1;
    bool debug = false;
#if SEMU_HAS(VIRTIONET)
    bool netdev_ready = false;
#endif
    vm_t *vm = &emu->vm;
    handle_options(argc, argv, &kernel_file, &dtb_file, &initrd_file,
                   &disk_file, &netdev, &hart_count, &debug, &shared_dir);

    /* Initialize the emulator */
    memset(emu, 0, sizeof(*emu));

    /* Set up RAM */
    emu->ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (emu->ram == MAP_FAILED) {
        fprintf(stderr, "Could not map RAM\n");
        return 2;
    }
    assert(!(((uintptr_t) emu->ram) & 0b11));

    /* *-----------------------------------------*
     * |              Memory layout              |
     * *----------------*----------------*-------*
     * |  kernel image  |  initrd image  |  dtb  |
     * *----------------*----------------*-------*
     */
    char *ram_loc = (char *) emu->ram;
    /* Load Linux kernel image */
    map_file(&ram_loc, kernel_file);
    /* Load at last 1 MiB to prevent kernel from overwriting it */
    uint32_t dtb_addr = RAM_SIZE - DTB_SIZE; /* Device tree */
    ram_loc = ((char *) emu->ram) + dtb_addr;
    map_file(&ram_loc, dtb_file);
    /* Load optional initrd image at last 8 MiB before the dtb region to
     * prevent kernel from overwritting it
     */
    if (initrd_file) {
        uint32_t initrd_addr = dtb_addr - INITRD_SIZE; /* Init RAM disk */
        ram_loc = ((char *) emu->ram) + initrd_addr;
        map_file(&ram_loc, initrd_file);
    }

    /* Hook for unmapping files */
    atexit(unmap_files);

    /* Set up RISC-V harts */
    vm->n_hart = hart_count;
    vm->hart = malloc(sizeof(hart_t *) * vm->n_hart);
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *newhart = calloc(1, sizeof(hart_t));
        if (!newhart) {
            fprintf(stderr, "Failed to allocate hart #%u.\n", i);
            return 1;
        }
        INIT_HART(newhart, emu, i);
        newhart->x_regs[RV_R_A0] = i;
        newhart->x_regs[RV_R_A1] = dtb_addr;
        if (i == 0) {
            newhart->hsm_status = SBI_HSM_STATE_STARTED;
            /* Set initial PC for hart 0 to kernel entry point (semu RAM base at
             * 0x0) */
            newhart->pc = 0x00000000;
        }

        newhart->vm = vm;
        newhart->wfi = wfi_handler; /* Set WFI callback for coroutine support */
        vm->hart[i] = newhart;
    }

    /* Set up peripherals */
    emu->uart.in_fd = 0, emu->uart.out_fd = 1;
    emu->uart.waiting_hart_id = UINT32_MAX;
    emu->uart.has_waiting_hart = false;
    capture_keyboard_input(); /* set up uart */
#if SEMU_HAS(VIRTIONET)
    /* Always set ram pointer, even if netdev is not configured.
     * Device tree may still expose the device to guest.
     */
    emu->vnet.ram = emu->ram;
    if (netdev) {
        if (!virtio_net_init(&emu->vnet, netdev)) {
            fprintf(stderr, "Failed to initialize virtio-net device.\n");
            return 1;
        }
        netdev_ready = true;
    }
#endif
#if SEMU_HAS(VIRTIOBLK)
    emu->vblk.ram = emu->ram;
    emu->disk = virtio_blk_init(&(emu->vblk), disk_file);
#endif
#if SEMU_HAS(VIRTIORNG)
    emu->vrng.ram = emu->ram;
    virtio_rng_init();
#endif
    /* Set up ACLINT */
    semu_timer_init(&emu->mtimer.mtime, CLOCK_FREQ, hart_count);
    emu->mtimer.mtimecmp = calloc(vm->n_hart, sizeof(uint64_t));
    emu->mswi.msip = calloc(vm->n_hart, sizeof(uint32_t));
    emu->sswi.ssip = calloc(vm->n_hart, sizeof(uint32_t));
#if SEMU_HAS(VIRTIOSND)
    if (!virtio_snd_init(&(emu->vsnd)))
        fprintf(stderr, "No virtio-snd functioned\n");
    emu->vsnd.ram = emu->ram;
#endif
#if SEMU_HAS(VIRTIOFS)
    emu->vfs.ram = emu->ram;
    if (!virtio_fs_init(&(emu->vfs), "myfs", shared_dir))
        fprintf(stderr, "No virtio-fs functioned\n");
#endif

    emu->peripheral_update_ctr = 0;
    emu->debug = debug;

    /* Initialize coroutine system for multi-hart mode (SMP > 1) */
    if (vm->n_hart > 1) {
        uint32_t total_slots = vm->n_hart;
#if SEMU_HAS(VIRTIONET)
        if (netdev_ready)
            total_slots++;
#endif
        if (!coro_init(total_slots, vm->n_hart)) {
            fprintf(stderr, "Failed to initialize coroutine subsystem\n");
            fflush(stderr);
            return 1;
        }

        /* Create coroutine for each hart */
        for (uint32_t i = 0; i < vm->n_hart; i++) {
            if (!coro_create_hart(i, hart_exec_loop, vm->hart[i])) {
                fprintf(stderr, "Failed to create coroutine for hart %u\n", i);
                coro_cleanup();
                return 1;
            }
        }
    }

    return 0;
}

/* WFI callback for coroutine-based scheduling in SMP mode
 *
 * This handler implements the RISC-V WFI (Wait For Interrupt) instruction
 * semantics in the context of cooperative multitasking with coroutines.
 *
 * Per RISC-V privileged spec:
 * - WFI is a hint to suspend execution until an interrupt becomes pending
 * - WFI returns immediately if an interrupt is already pending
 * - WFI may complete for any reason (implementation-defined)
 *
 * Our implementation:
 * - In SMP mode (n_hart > 1): yield to scheduler if no interrupt pending
 * - In single-hart mode: WFI is a no-op (inline polling handles I/O)
 * - The in_wfi flag tracks whether hart is waiting, allowing scheduler to
 *   block until all harts reach WFI (power-efficient idle state)
 */
static void wfi_handler(hart_t *hart)
{
    /* Per RISC-V spec: WFI returns immediately if interrupt is pending.
     * We check if any interrupt is actually pending (sip & sie != 0).
     */
    bool interrupt_pending = (hart->sip & hart->sie) != 0;

    if (!interrupt_pending) {
        emu_state_t *emu = PRIV(hart);
        vm_t *vm = &emu->vm;

        /* Only use coroutine yielding in multi-hart mode where the coroutine
         * scheduler loop is active. In single-hart mode, WFI is a no-op since
         * there's no scheduler to resume execution after yield.
         */
        if (vm->n_hart > 1) {
            hart->in_wfi = true; /* Mark as waiting for interrupt */
            coro_yield();        /* Suspend until scheduler resumes us */
            /* NOTE: Do NOT clear in_wfi here to avoid race condition.
             * The scheduler needs to see this flag to detect idle state.
             * The flag will be cleared when an interrupt is actually injected.
             */
        }
    } else {
        hart->in_wfi = false; /* Clear if interrupt already pending */
    }
}

/* Hart execution loop - each hart runs in its own coroutine
 *
 * This is the main entry point for each RISC-V hart when running in SMP mode.
 * Each hart executes independently as a coroutine, cooperatively yielding to
 * the scheduler to allow other harts and I/O coroutines to make progress.
 *
 * Execution model:
 * - Harts execute in batches of 64 instructions before yielding
 * - Peripheral polling and interrupt checks happen before each batch
 * - WFI instruction triggers immediate yield (via wfi_handler callback)
 * - Harts in HSM_STATE_STOPPED remain suspended until IPI wakes them
 *
 * This design balances responsiveness and throughput:
 * - Small batch size (64 insns) keeps latency low for I/O and IPI
 * - Cooperative scheduling avoids overhead of preemptive context switches
 * - WFI-based blocking allows efficient idle when all harts are waiting
 */
static void hart_exec_loop(void *arg)
{
    hart_t *hart = (hart_t *) arg;
    emu_state_t *emu = PRIV(hart);

    /* Run hart until emulator stops */
    while (!emu->stopped) {
        /* Check HSM (Hart State Management) state via SBI extension */
        if (hart->hsm_status != SBI_HSM_STATE_STARTED) {
            /* Hart is STOPPED or SUSPENDED - update peripherals and yield.
             * An IPI (via SBI_HSM__HART_START) will change state to STARTED.
             */
            emu_tick_peripherals(emu);
            emu_update_timer_interrupt(hart);
            emu_update_swi_interrupt(hart);
            coro_yield();
            continue;
        }

        /* Execute a batch of instructions before yielding.
         * Batch size of 64 balances throughput and responsiveness.
         */
        for (int i = 0; i < 64; i++) {
            emu_tick_peripherals(emu);
            emu_update_timer_interrupt(hart);
            emu_update_swi_interrupt(hart);

            /* Execute one RISC-V instruction */
            vm_step(hart);

            /* Handle execution errors */
            if (unlikely(hart->error)) {
                if (hart->error == ERR_EXCEPTION &&
                    hart->exc_cause == RV_EXC_ECALL_S) {
                    /* S-mode ecall: handle SBI call and continue */
                    handle_sbi_ecall(hart);
                    continue;
                }

                if (hart->error == ERR_EXCEPTION) {
                    /* Other exception: delegate to supervisor via trap */
                    hart_trap(hart);
                    continue;
                }

                /* Fatal error: report and stop emulation */
                vm_error_report(hart);
                emu->stopped = true;
                goto cleanup;
            }
        }

        /* Yield to scheduler after executing batch */
        coro_yield();
    }
cleanup:
    return;
}

static int semu_step(emu_state_t *emu)
{
    vm_t *vm = &emu->vm;

    /* TODO: Add support for multi-threaded system emulation after the
     * RFENCE extension is completely implemented.
     */
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        emu_tick_peripherals(emu);

        emu_update_timer_interrupt(vm->hart[i]);
        emu_update_swi_interrupt(vm->hart[i]);

        vm_step(vm->hart[i]);
        if (likely(!vm->hart[i]->error))
            continue;

        if (vm->hart[i]->error == ERR_EXCEPTION &&
            vm->hart[i]->exc_cause == RV_EXC_ECALL_S) {
            handle_sbi_ecall(vm->hart[i]);
            continue;
        }

        if (vm->hart[i]->error == ERR_EXCEPTION) {
            hart_trap(vm->hart[i]);
            continue;
        }

        vm_error_report(vm->hart[i]);
        return 2;
    }

    return 0;
}

#ifdef MMU_CACHE_STATS
static vm_t *global_vm_for_signal = NULL;
static volatile sig_atomic_t signal_received = 0;

/* Forward declaration */
static void print_mmu_cache_stats(vm_t *vm);

/* Async-signal-safe handler: only set flag, defer printing */
static void signal_handler_stats(int sig UNUSED)
{
    signal_received = 1;
}

static void print_mmu_cache_stats(vm_t *vm)
{
    fprintf(stderr, "\n=== MMU Cache Statistics ===\n");
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *hart = vm->hart[i];
        uint64_t fetch_total =
            hart->cache_fetch.hits + hart->cache_fetch.misses;

        /* Combine 8-set × 2-way load cache statistics */
        uint64_t load_hits = 0, load_misses = 0;
        for (int set = 0; set < 8; set++) {
            for (int way = 0; way < 2; way++) {
                load_hits += hart->cache_load[set].ways[way].hits;
                load_misses += hart->cache_load[set].ways[way].misses;
            }
        }
        uint64_t load_total = load_hits + load_misses;

        /* Combine 8-set × 2-way store cache statistics */
        uint64_t store_hits = 0, store_misses = 0;
        for (int set = 0; set < 8; set++) {
            for (int way = 0; way < 2; way++) {
                store_hits += hart->cache_store[set].ways[way].hits;
                store_misses += hart->cache_store[set].ways[way].misses;
            }
        }
        uint64_t store_total = store_hits + store_misses;

        fprintf(stderr, "\nHart %u:\n", i);
        fprintf(stderr, "  Fetch: %12llu hits, %12llu misses",
                hart->cache_fetch.hits, hart->cache_fetch.misses);
        if (fetch_total > 0)
            fprintf(stderr, " (%.2f%% hit rate)",
                    100.0 * hart->cache_fetch.hits / fetch_total);
        fprintf(stderr, "\n");

        fprintf(stderr, "  Load:  %12llu hits, %12llu misses (8x2)", load_hits,
                load_misses);
        if (load_total > 0)
            fprintf(stderr, " (%.2f%% hit rate)",
                    100.0 * load_hits / load_total);
        fprintf(stderr, "\n");

        fprintf(stderr, "  Store: %12llu hits, %12llu misses (8x2)", store_hits,
                store_misses);
        if (store_total > 0)
            fprintf(stderr, " (%.2f%% hit rate)",
                    100.0 * store_hits / store_total);
        fprintf(stderr, "\n");
    }
}
#endif

static int semu_run(emu_state_t *emu)
{
    int ret;
    vm_t *vm = &emu->vm;

    if (vm->n_hart > 1) {
        /* SMP mode: Use coroutine-based hart scheduling
         *
         * Architecture:
         * - Each hart runs as an independent coroutine
         * - Peripherals (VirtIO-Net, UART, etc.) use inline polling
         * - Main loop acts as scheduler, resuming hart coroutines round-robin
         * - poll() monitors timer and UART for power management
         *
         * Power management optimization:
         * - When all harts execute WFI (Wait For Interrupt), scheduler blocks
         *   in poll() with timeout=-1 (indefinite) until:
         *   * UART input arrives (keyboard)
         *   * Timer expires (1ms periodic timer for guest timer emulation)
         * - This avoids busy-waiting when guest OS is idle
         *
         * Peripheral I/O handling:
         * - Peripherals are polled inline during hart execution (see
         *   emu_tick_peripherals), not via separate coroutines
         * - Non-blocking poll() for network/disk I/O (~200ns overhead)
         * - Inline polling provides lowest latency (checked every 64
         * instructions)
         */
#ifdef __APPLE__
        int kq = kqueue();
        if (kq < 0) {
            perror("kqueue");
            return -1;
        }

        struct kevent kev_timer;
        EV_SET(&kev_timer, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, 1, NULL);
        if (kevent(kq, &kev_timer, 1, NULL, 0, NULL) < 0) {
            perror("kevent timer setup");
            close(kq);
            return -1;
        }

        if (isatty(emu->uart.in_fd)) {
            struct kevent kev_uart;
            EV_SET(&kev_uart, emu->uart.in_fd, EVFILT_READ, EV_ADD | EV_ENABLE,
                   0, 0, NULL);
            if (kevent(kq, &kev_uart, 1, NULL, 0, NULL) < 0) {
                perror("kevent uart setup");
                close(kq);
                return -1;
            }
        }
#else
        int wfi_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (wfi_timer_fd < 0) {
            perror("timerfd_create");
            return -1;
        }

        struct itimerspec its = {
            .it_interval = {.tv_sec = 0, .tv_nsec = 1000000},
            .it_value = {.tv_sec = 0, .tv_nsec = 1000000},
        };
        if (timerfd_settime(wfi_timer_fd, 0, &its, NULL) < 0) {
            perror("timerfd_settime");
            close(wfi_timer_fd);
            return -1;
        }
#endif

        /* Poll-based event loop for I/O monitoring:
         * - Timer fd: 1 descriptor for periodic timer (kqueue/timerfd)
         * - UART fd: 1 descriptor for keyboard input
         */
        struct pollfd *pfds = NULL;
        size_t poll_capacity = 0;

        while (!emu->stopped) {
#ifdef MMU_CACHE_STATS
            /* Check if signal received (SIGINT/SIGTERM) */
            if (signal_received) {
                print_mmu_cache_stats(&emu->vm);
                return 0;
            }
#endif
            /* Only need fds for timer and UART (no coroutine I/O) */
            size_t needed = 2;

            /* Grow buffer if needed (amortized realloc) */
            if (needed > poll_capacity) {
                struct pollfd *new_pfds =
                    realloc(pfds, needed * sizeof(*new_pfds));
                if (!new_pfds) {
                    free(pfds);
#ifdef __APPLE__
                    close(kq);
#else
                    close(wfi_timer_fd);
#endif
                    return -1;
                }
                pfds = new_pfds;
                poll_capacity = needed;
            }

            /* Determine poll timeout based on hart states BEFORE setting up
             * poll fds. This check must happen before coro_resume_hart()
             * modifies flags.
             *
             * - If no harts are STARTED, block indefinitely (wait for IPI)
             * - If all STARTED harts are idle (WFI or UART waiting), block
             * - Otherwise, use non-blocking poll (timeout=0)
             */
            int poll_timeout = 0;
            uint32_t started_harts = 0;
            uint32_t idle_harts = 0;
            for (uint32_t i = 0; i < vm->n_hart; i++) {
                if (vm->hart[i]->hsm_status == SBI_HSM_STATE_STARTED) {
                    started_harts++;
                    /* Count hart as idle if it's in WFI or waiting for UART */
                    if (vm->hart[i]->in_wfi ||
                        (emu->uart.has_waiting_hart &&
                         emu->uart.waiting_hart_id == i)) {
                        idle_harts++;
                    }
                }
            }

            /* Collect file descriptors for poll() */
            size_t pfd_count = 0;
            int timer_index = -1;

            /* Add periodic timer fd (1ms interval for guest timer emulation).
             * Only add timer when ALL harts are active (none idle) to allow
             * poll() to sleep when any harts are in WFI. When harts are idle,
             * timer updates can be deferred until they wake up.
             *
             * During SMP boot (started_harts < vm->n_hart), always include the
             * timer to ensure secondary harts can complete initialization. Only
             * apply conditional exclusion after all harts have started.
             *
             * For single-hart configurations (n_hart == 1), disable
             * optimization entirely to avoid boot issues, as the first hart
             * starts immediately.
             */
            bool all_harts_started = (started_harts >= vm->n_hart);
            bool harts_active =
                (vm->n_hart == 1) || !all_harts_started || (idle_harts == 0);
#ifdef __APPLE__
            /* macOS: use kqueue with EVFILT_TIMER */
            if (kq >= 0 && pfd_count < poll_capacity && harts_active) {
                pfds[pfd_count] = (struct pollfd){kq, POLLIN, 0};
                timer_index = (int) pfd_count;
                pfd_count++;
            }
#else
            /* Linux: use timerfd */
            if (wfi_timer_fd >= 0 && pfd_count < poll_capacity &&
                harts_active) {
                pfds[pfd_count] = (struct pollfd){wfi_timer_fd, POLLIN, 0};
                timer_index = (int) pfd_count;
                pfd_count++;
            }
#endif

            /* Add UART input fd (stdin for keyboard input).
             * Only add UART when:
             * 1. Single-hart configuration (n_hart == 1), OR
             * 2. Not all harts started (!all_harts_started), OR
             * 3. All harts are active (idle_harts == 0), OR
             * 4. A hart is actively waiting for UART input
             *
             * This prevents UART (which is always "readable" on TTY) from
             * preventing poll() sleep when harts are idle. Trade-off: user
             * input (Ctrl+A x) may be delayed by up to poll_timeout (10ms)
             * when harts are idle, which is acceptable for an emulator.
             */
            bool need_uart = (vm->n_hart == 1) || !all_harts_started ||
                             (idle_harts == 0) || emu->uart.has_waiting_hart;
            if (emu->uart.in_fd >= 0 && pfd_count < poll_capacity &&
                need_uart) {
                pfds[pfd_count] = (struct pollfd){emu->uart.in_fd, POLLIN, 0};
                pfd_count++;
            }

            /* Set poll timeout based on current idle state (adaptive timeout).
             * Three-tier strategy:
             * 1. Blocking (-1): All harts idle + have fds → wait for events
             * 2. Short sleep (10ms): Some harts idle → reduce CPU usage
             * 3. Non-blocking (0): All harts active → maximum responsiveness
             *
             * SAFETY: Never use blocking timeout when pfd_count==0, as
             * poll(0,-1) would hang indefinitely. Always use 10ms timeout as
             * fallback.
             */
            if (pfd_count > 0 &&
                (started_harts == 0 || idle_harts == started_harts)) {
                /* All harts idle + have fds: block until event */
                poll_timeout = -1;
            } else if (idle_harts > 0) {
                /* Some/all harts idle (or all idle but no fds): 10ms sleep */
                poll_timeout = 10;
            } else {
                /* All harts active: non-blocking */
                poll_timeout = 0;
            }

            /* Execute poll() to wait for I/O events.
             * - timeout=0: non-blocking poll when harts are active
             * - timeout=10: short sleep when some harts idle
             * - timeout=-1: blocking poll when all harts idle (WFI or UART
             *   wait)
             *
             * When pfd_count==0, poll() acts as a pure sleep mechanism.
             */
            int nevents = poll(pfds, pfd_count, poll_timeout);

            if (pfd_count > 0 && nevents > 0) {
                /* Consume timer expiration events to prevent fd staying
                 * readable
                 */
                if (timer_index >= 0 && (pfds[timer_index].revents & POLLIN)) {
#ifdef __APPLE__
                    /* drain kqueue events with non-blocking kevent */
                    struct kevent events[32];
                    struct timespec timeout_zero = {0, 0};
                    kevent(kq, NULL, 0, events, 32, &timeout_zero);
#else
                    /* Linux: read timerfd to consume expiration count */
                    uint64_t expirations;
                    ssize_t ret_read =
                        read(wfi_timer_fd, &expirations, sizeof(expirations));
                    (void) ret_read;
#endif
                }
            } else if (nevents < 0 && errno != EINTR) {
                perror("poll");
            }

            /* Resume all hart coroutines (round-robin scheduling).
             * Each hart executes a batch of instructions, then yields back.
             * Harts in WFI will have their in_wfi flag cleared by interrupt
             * handlers (ACLINT, PLIC, UART) when interrupts are injected.
             *
             * Note: We must always resume harts after poll() returns, even if
             * all harts appear idle. The in_wfi flag is only cleared when
             * interrupt sources inject interrupts, so skipping resume would
             * cause a deadlock where harts remain stuck waiting even after
             * events arrive.
             */
            for (uint32_t i = 0; i < vm->n_hart; i++) {
                coro_resume_hart(i);
            }

#if SEMU_HAS(VIRTIONET)
            /* VirtIO-Net coroutine disabled for now */
#endif
        }

        free(pfds);
#ifdef __APPLE__
        close(kq);
#else
        close(wfi_timer_fd);
#endif

        if (emu->stopped)
            return 1;

        return 0;
    }

    /* Single-hart mode: use original scheduling */
    while (!emu->stopped) {
#if SEMU_HAS(VIRTIONET)
        int i = 0;
        if (emu->vnet.peer.type == NETDEV_IMPL_user && boot_complete) {
            net_user_options_t *usr = (net_user_options_t *) emu->vnet.peer.op;

            uint32_t timeout = -1;
            usr->pfd_len = 1;
            slirp_pollfds_fill_socket(usr->slirp, &timeout,
                                      semu_slirp_add_poll_socket, usr);

            /* Poll the internal pipe for incoming data. If data is
             * available (POLL_IN), process it and forward it to the
             * virtio-net device.
             */
            int pollout = poll(usr->pfd, usr->pfd_len, 1);
            if (usr->pfd[0].revents & POLLIN) {
                virtio_net_recv_from_peer(usr->peer);
            }
            slirp_pollfds_poll(usr->slirp, (pollout <= 0),
                               semu_slirp_get_revents, usr);
            for (i = 0; i < SLIRP_POLL_INTERVAL; i++) {
                ret = semu_step(emu);
                if (ret)
                    return ret;
            }
        } else
#endif
        {
            ret = semu_step(emu);
            if (ret)
                return ret;
        }
    }

    /* unreachable */
    return 0;
}

static inline bool semu_is_interrupt(emu_state_t *emu)
{
    return __atomic_load_n(&emu->is_interrupted, __ATOMIC_RELAXED);
}

static size_t semu_get_reg_bytes(UNUSED int regno)
{
    return 4;
}

static int semu_read_reg(void *args, int regno, void *data)
{
    emu_state_t *emu = (emu_state_t *) args;

    if (regno > 32)
        return EFAULT;

    assert((uint32_t) emu->curr_cpuid < emu->vm.n_hart);

    if (regno == 32)
        *(uint32_t *) data = emu->vm.hart[emu->curr_cpuid]->pc;
    else
        *(uint32_t *) data = emu->vm.hart[emu->curr_cpuid]->x_regs[regno];

    return 0;
}

static int semu_read_mem(void *args, size_t addr, size_t len, void *val)
{
    emu_state_t *emu = (emu_state_t *) args;
    hart_t *hart = emu->vm.hart[emu->curr_cpuid];
    mem_load(hart, addr, len, val);
    return 0;
}

static gdb_action_t semu_cont(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    while (!semu_is_interrupt(emu)) {
        semu_step(emu);
    }

    /* Clear the interrupt if it's pending */
    __atomic_store_n(&emu->is_interrupted, false, __ATOMIC_RELAXED);

    return ACT_RESUME;
}

static gdb_action_t semu_stepi(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    semu_step(emu);
    return ACT_RESUME;
}

static void semu_on_interrupt(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    /* Notify the emulator to break out the for loop in rv_cont */
    __atomic_store_n(&emu->is_interrupted, true, __ATOMIC_RELAXED);
}

static int semu_get_cpu(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    return emu->curr_cpuid;
}

static void semu_set_cpu(void *args, int cpuid)
{
    emu_state_t *emu = (emu_state_t *) args;
    emu->curr_cpuid = cpuid;
}

static int semu_run_debug(emu_state_t *emu)
{
    vm_t *vm = &emu->vm;

    gdbstub_t gdbstub;
    struct target_ops gdbstub_ops = {
        .get_reg_bytes = semu_get_reg_bytes,
        .read_reg = semu_read_reg,
        .write_reg = NULL,
        .read_mem = semu_read_mem,
        .write_mem = NULL,
        .cont = semu_cont,
        .stepi = semu_stepi,
        .set_bp = NULL,
        .del_bp = NULL,
        .on_interrupt = semu_on_interrupt,

        .get_cpu = semu_get_cpu,
        .set_cpu = semu_set_cpu,
    };

    emu->curr_cpuid = 0;
    if (!gdbstub_init(&gdbstub, &gdbstub_ops,
                      (arch_info_t){
                          .smp = vm->n_hart,
                          .reg_num = 33,
                          .target_desc = TARGET_RV32,
                      },
                      "127.0.0.1:1234")) {
        return 1;
    }

    emu->is_interrupted = false;
    if (!gdbstub_run(&gdbstub, (void *) emu))
        return 1;

    gdbstub_close(&gdbstub);

    return 0;
}

int main(int argc, char **argv)
{
    int ret;
    emu_state_t emu;
    ret = semu_init(&emu, argc, argv);
    if (ret)
        return ret;

#ifdef MMU_CACHE_STATS
    global_vm_for_signal = &emu.vm;
    signal(SIGINT, signal_handler_stats);
    signal(SIGTERM, signal_handler_stats);
#endif

    if (emu.debug)
        ret = semu_run_debug(&emu);
    else
        ret = semu_run(&emu);

#ifdef MMU_CACHE_STATS
    print_mmu_cache_stats(&emu.vm);
#endif
    return ret;
}
