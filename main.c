#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

#define PRIV(x) ((emu_state_t *) x->priv)

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

static void emu_update_timer_interrupt(hart_t *hart)
{
    emu_state_t *data = PRIV(hart);

    /* Sync global timer with local timer */
    hart->time = data->clint.mtime;
    clint_update_interrupts(hart, &data->clint);
}

static void emu_update_global_timer(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    data->clint.mtime++;
    return;
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
            plic_update_interrupts(hart->vm, &data->plic);
            return;
        case 0x40: /* UART */
            u8250_read(hart, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(hart->vm);
            return;
#if SEMU_HAS(VIRTIONET)
        case 0x41: /* virtio-net */
            virtio_net_read(hart, &data->vnet, addr & 0xFFFFF, width, value);
            emu_update_vnet_interrupts(hart->vm);
            return;
#endif
#if SEMU_HAS(VIRTIOBLK)
        case 0x42: /* virtio-blk */
            virtio_blk_read(hart, &data->vblk, addr & 0xFFFFF, width, value);
            emu_update_vblk_interrupts(hart->vm);
            return;
#endif
        case 0x43: /* clint */
            clint_read(hart, &data->clint, addr & 0xFFFFF, width, value);
            clint_update_interrupts(hart, &data->clint);
            return;
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
        case 0x43: /* clint */
            clint_write(hart, &data->clint, addr & 0xFFFFF, width, value);
            clint_update_interrupts(hart, &data->clint);
            return;
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
        data->clint.mtimecmp[hart->mhartid] =
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
            for (uint32_t i = 0; i < hart->vm->hart_number; i++) {
                data->clint.msip[i] = 1;
            }
        } else {
            for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++) {
                data->clint.msip[i] = hart_mask & 1;
            }
        }

        return (sbi_ret_t){SBI_SUCCESS, 0};
        break;
    default:
        return (sbi_ret_t){SBI_ERR_FAILED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_RFENCE(hart_t *hart, int32_t fid)
{
    /* TODO: RFENCE SBI extension */
    uint64_t hart_mask, hart_mask_base;
    switch (fid) {
    case 0:
        return (sbi_ret_t){SBI_SUCCESS, 0};
    case 1:
        hart_mask = (uint64_t) hart->x_regs[RV_R_A0];
        hart_mask_base = (uint64_t) hart->x_regs[RV_R_A1];
        if (hart_mask_base == 0xFFFFFFFFFFFFFFFF) {
            for (uint32_t i = 0; i < hart->vm->hart_number; i++) {
                hart->vm->hart[i]->cache_fetch.n_pages = 0xFFFFFFFF;
            }
        } else {
            for (int i = hart_mask_base; hart_mask; hart_mask >>= 1, i++) {
                hart->vm->hart[i]->cache_fetch.n_pages = 0xFFFFFFFF;
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

#define MAPPER_SIZE 4

struct mapper {
    char *addr;
    uint32_t size;
};

static struct mapper mapper[MAPPER_SIZE] = {0};
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
    fprintf(
        stderr,
        "Usage: %s -k linux-image [-b dtb] [-i initrd-image] [-d disk-image]\n",
        execpath);
}

static void handle_options(int argc,
                           char **argv,
                           char **kernel_file,
                           char **dtb_file,
                           char **initrd_file,
                           char **disk_file,
                           int *hart_number)
{
    *kernel_file = *dtb_file = *initrd_file = *disk_file = NULL;

    int optidx = 0;
    struct option opts[] = {
        {"kernel", 1, NULL, 'k'}, {"dtb", 1, NULL, 'b'},
        {"initrd", 1, NULL, 'i'}, {"disk", 1, NULL, 'd'},
        {"smp", 1, NULL, 'c'},    {"help", 0, NULL, 'h'},
    };

    int c;
    while ((c = getopt_long(argc, argv, "k:b:i:d:c:h", opts, &optidx)) != -1) {
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
        case 'c':
            *hart_number = atoi(optarg);
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
    ({                                            \
        hart->priv = emu;                         \
        hart->mhartid = id;                       \
        hart->mem_fetch = mem_fetch;              \
        hart->mem_load = mem_load;                \
        hart->mem_store = mem_store;              \
        hart->mem_page_table = mem_page_table;    \
        hart->s_mode = true;                      \
        hart->hsm_status = SBI_HSM_STATE_STOPPED; \
        vm_init(hart);                            \
    })

static int semu_start(int argc, char **argv)
{
    char *kernel_file;
    char *dtb_file;
    char *initrd_file;
    char *disk_file;
    int hart_count = 1;
    handle_options(argc, argv, &kernel_file, &dtb_file, &initrd_file,
                   &disk_file, &hart_count);

    /* Initialize the emulator */
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));

    /* Set up RAM */
    emu.ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (emu.ram == MAP_FAILED) {
        fprintf(stderr, "Could not map RAM\n");
        return 2;
    }
    assert(!(((uintptr_t) emu.ram) & 0b11));

    /* *-----------------------------------------*
     * |              Memory layout              |
     * *----------------*----------------*-------*
     * |  kernel image  |  initrd image  |  dtb  |
     * *----------------*----------------*-------*
     */
    char *ram_loc = (char *) emu.ram;
    /* Load Linux kernel image */
    map_file(&ram_loc, kernel_file);
    /* Load at last 1 MiB to prevent kernel from overwriting it */
    uint32_t dtb_addr = RAM_SIZE - DTB_SIZE; /* Device tree */
    ram_loc = ((char *) emu.ram) + dtb_addr;
    map_file(&ram_loc, dtb_file);
    /* Load optional initrd image at last 8 MiB before the dtb region to
     * prevent kernel from overwritting it
     */
    if (initrd_file) {
        uint32_t initrd_addr = dtb_addr - INITRD_SIZE; /* Init RAM disk */
        ram_loc = ((char *) emu.ram) + initrd_addr;
        map_file(&ram_loc, initrd_file);
    }

    /* Hook for unmapping files */
    atexit(unmap_files);

    /* Set up RISC-V hart */
    vm_t vm;
    vm.hart_number = hart_count;
    vm.hart = malloc(sizeof(hart_t *) * vm.hart_number);
    for (uint32_t i = 0; i < vm.hart_number; i++) {
        hart_t *newhart = malloc(sizeof(hart_t));
        INIT_HART(newhart, &emu, i);
        newhart->x_regs[RV_R_A0] = i;
        newhart->x_regs[RV_R_A1] = dtb_addr;
        if (i == 0)
            newhart->hsm_status = SBI_HSM_STATE_STARTED;

        newhart->vm = &vm;
        vm.hart[i] = newhart;
    }

    /* Set up peripherals */
    emu.uart.in_fd = 0, emu.uart.out_fd = 1;
    capture_keyboard_input(); /* set up uart */
#if SEMU_HAS(VIRTIONET)
    if (!virtio_net_init(&(emu.vnet)))
        fprintf(stderr, "No virtio-net functioned\n");
    emu.vnet.ram = emu.ram;
#endif
#if SEMU_HAS(VIRTIOBLK)
    emu.vblk.ram = emu.ram;
    emu.disk = virtio_blk_init(&(emu.vblk), disk_file);
#endif

    /* Emulate */
    uint32_t peripheral_update_ctr = 0;
    while (!emu.stopped) {
        emu_update_global_timer(&vm);

        for (uint32_t i = 0; i < vm.hart_number; i++) {
            if (peripheral_update_ctr-- == 0) {
                peripheral_update_ctr = 64;

                u8250_check_ready(&emu.uart);
                if (emu.uart.in_ready)
                    emu_update_uart_interrupts(&vm);

#if SEMU_HAS(VIRTIONET)
                virtio_net_refresh_queue(&emu.vnet);
                if (emu.vnet.InterruptStatus)
                    emu_update_vnet_interrupts(&vm);
#endif

#if SEMU_HAS(VIRTIOBLK)
                if (emu.vblk.InterruptStatus)
                    emu_update_vblk_interrupts(&vm);
#endif
            }

            emu_update_timer_interrupt(vm.hart[i]);

            vm_step(vm.hart[i]);
            if (likely(!vm.hart[i]->error))
                continue;

            if (vm.hart[i]->error == ERR_EXCEPTION &&
                vm.hart[i]->exc_cause == RV_EXC_ECALL_S) {
                handle_sbi_ecall(vm.hart[i]);
                continue;
            }

            if (vm.hart[i]->error == ERR_EXCEPTION) {
                hart_trap(vm.hart[i]);
                continue;
            }

            vm_error_report(vm.hart[i]);
            return 2;
        }
    }

    /* unreachable */
    return 0;
}

int main(int argc, char **argv)
{
    return semu_start(argc, argv);
}
