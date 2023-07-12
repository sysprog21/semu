#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* Define fetch separately because it is much simpler (width is fixed,
 * alignment already checked, only main RAM is executable)
 */
static void mem_fetch(vm_t *vm, uint32_t addr, uint32_t *value)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    if (unlikely(addr >= RAM_SIZE)) {
        /* TODO: check for other regions */
        vm_set_exception(vm, RV_EXC_FETCH_FAULT, vm->exc_val);
        return;
    }
    *value = data->ram[addr >> 2];
}

/* similarly only main memory pages can be used as page_tables */
static uint32_t *mem_page_table(const vm_t *vm, uint32_t ppn)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    if (ppn < (RAM_SIZE / RV_PAGE_SIZE))
        return &data->ram[ppn << (RV_PAGE_SHIFT - 2)];
    return NULL;
}

static void emu_update_uart_interrupts(vm_t *vm)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    u8250_update_interrupts(&data->uart);
    if (data->uart.pending_ints)
        data->plic.active |= IRQ_UART_BIT;
    else
        data->plic.active &= ~IRQ_UART_BIT;
    plic_update_interrupts(vm, &data->plic);
}

#if defined(ENABLE_VIRTIONET)
static void emu_update_vnet_interrupts(vm_t *vm)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    if (data->vnet.InterruptStatus)
        data->plic.active |= IRQ_VNET_BIT;
    else
        data->plic.active &= ~IRQ_VNET_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

static void mem_load(vm_t *vm, uint32_t addr, uint8_t width, uint32_t *value)
{
    emu_state_t *data = (emu_state_t *) vm->priv;

    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_read(vm, data->ram, addr, width, value);
        return;
    }

    if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */
        /* 256 regions of 1MiB */
        switch ((addr >> 20) & MASK(8)) {
        case 0x0:
        case 0x2: /* PLIC (0 - 0x3F) */
            plic_read(vm, &data->plic, addr & 0x3FFFFFF, width, value);
            plic_update_interrupts(vm, &data->plic);
            return;
        case 0x40: /* UART */
            u8250_read(vm, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(vm);
            return;
#if defined(ENABLE_VIRTIONET)
        case 0x41: /* VirtIO-Net */
            virtio_net_read(vm, &data->vnet, addr & 0xFFFFF, width, value);
            emu_update_vnet_interrupts(vm);
            return;
#endif
        }
    }
    vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

static void mem_store(vm_t *vm, uint32_t addr, uint8_t width, uint32_t value)
{
    emu_state_t *data = (emu_state_t *) vm->priv;

    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_write(vm, data->ram, addr, width, value);
        return;
    }

    if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */
        /* 256 regions of 1MiB */
        switch ((addr >> 20) & MASK(8)) {
        case 0x0:
        case 0x2: /* PLIC (0 - 0x3F) */
            plic_write(vm, &data->plic, addr & 0x3FFFFFF, width, value);
            plic_update_interrupts(vm, &data->plic);
            return;
        case 0x40: /* UART */
            u8250_write(vm, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(vm);
            return;
#if defined(ENABLE_VIRTIONET)
        case 0x41: /* VirtIO-Net */
            virtio_net_write(vm, &data->vnet, addr & 0xFFFFF, width, value);
            emu_update_vnet_interrupts(vm);
            return;
#endif
        }
    }
    vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

/* SBI */
#define SBI_IMPL_ID 0x999
#define SBI_IMPL_VERSION 1

typedef struct {
    int32_t error;
    int32_t value;
} sbi_ret_t;

static inline sbi_ret_t handle_sbi_ecall_TIMER(vm_t *vm, int32_t fid)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    switch (fid) {
    case SBI_TIMER__SET_TIMER:
        data->timer_lo = vm->x_regs[RV_R_A0];
        data->timer_hi = vm->x_regs[RV_R_A1];
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_RST(vm_t *vm, int32_t fid)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    switch (fid) {
    case SBI_RST__SYSTEM_RESET:
        fprintf(stderr, "system reset: type=%u, reason=%u\n",
                vm->x_regs[RV_R_A0], vm->x_regs[RV_R_A1]);
        data->stopped = true;
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

#define RV_MVENDORID 0x12345678
#define RV_MARCHID ((1 << 31) | 1)
#define RV_MIMPID 1

static inline sbi_ret_t handle_sbi_ecall_BASE(vm_t *vm, int32_t fid)
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
        return (sbi_ret_t){SBI_SUCCESS, (0 << 24) | 3}; /* version 0.3 */
    case SBI_BASE__PROBE_EXTENSION: {
        int32_t eid = (int32_t) vm->x_regs[RV_R_A0];
        bool available =
            eid == SBI_EID_BASE || eid == SBI_EID_TIMER || eid == SBI_EID_RST;
        return (sbi_ret_t){SBI_SUCCESS, available};
    }
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

#define SBI_HANDLE(TYPE) ret = handle_sbi_ecall_##TYPE(vm, vm->x_regs[RV_R_A6])

static void handle_sbi_ecall(vm_t *vm)
{
    sbi_ret_t ret;
    switch (vm->x_regs[RV_R_A7]) {
    case SBI_EID_BASE:
        SBI_HANDLE(BASE);
        break;
    case SBI_EID_TIMER:
        SBI_HANDLE(TIMER);
        break;
    case SBI_EID_RST:
        SBI_HANDLE(RST);
        break;
    default:
        ret = (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
    vm->x_regs[RV_R_A0] = (uint32_t) ret.error;
    vm->x_regs[RV_R_A1] = (uint32_t) ret.value;

    /* Clear error to allow execution to continue */
    vm->error = ERR_NONE;
}

static void read_file_into_ram(char **ram_loc, const char *name)
{
    FILE *input_file = fopen(name, "r");
    if (!input_file) {
        fprintf(stderr, "could not open %s\n", name);
        exit(2);
    }

    fseek(input_file, 0, SEEK_END);
    long file_size = ftell(input_file);

    /* remap to a memory region, then using memory copy to the specified location */
    *ram_loc = mmap(*ram_loc, file_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fileno(input_file), 0);
    if (*ram_loc == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }

    /* update the pointer */
    *ram_loc += file_size;

    fclose(input_file);
}

static int semu_start(int argc, char **argv)
{
    /* Initialize the emulator */
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));

    vm_t vm = {
        .priv = &emu,
        .mem_fetch = mem_fetch,
        .mem_load = mem_load,
        .mem_store = mem_store,
        .mem_page_table = mem_page_table,
    };

    /* Set up RAM */
    emu.ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (emu.ram == MAP_FAILED) {
        fprintf(stderr, "Could not map RAM\n");
        return 2;
    }
    assert(!(((uintptr_t) emu.ram) & 0b11));

    char *ram_loc = (char *) emu.ram;
    /* Load Linux kernel image */
    read_file_into_ram(&ram_loc, argv[1]);
    /* Load at last 1 MiB to prevent kernel / initrd from overwriting it */
    uint32_t dtb_addr = RAM_SIZE - 1024 * 1024; /* Device tree */
    ram_loc = ((char *) emu.ram) + dtb_addr;
    read_file_into_ram(&ram_loc, (argc == 3) ? argv[2] : "minimal.dtb");
    /* TODO: load disk image via virtio_blk */

    /* Set up RISC-V hart */
    emu.timer_hi = emu.timer_lo = 0xFFFFFFFF;
    vm.s_mode = true;
    vm.x_regs[RV_R_A0] = 0; /* hart ID. i.e., cpuid */
    vm.x_regs[RV_R_A1] = dtb_addr;

    /* Set up peripherals */
    emu.uart.in_fd = 0, emu.uart.out_fd = 1;
    capture_keyboard_input(); /* set up uart */
#if defined(ENABLE_VIRTIONET)
    if (!virtio_net_init(&(emu.vnet)))
        fprintf(stderr, "No virtio-net functioned\n");
    emu.vnet.ram = emu.ram;
#endif

    /* Emulate */
    uint32_t peripheral_update_ctr = 0;
    while (!emu.stopped) {
        if (peripheral_update_ctr-- == 0) {
            peripheral_update_ctr = 64;

            u8250_check_ready(&emu.uart);
            if (emu.uart.in_ready)
                emu_update_uart_interrupts(&vm);

#if defined(ENABLE_VIRTIONET)
            virtio_net_refresh_queue(&emu.vnet);
            if (emu.vnet.InterruptStatus)
                emu_update_vnet_interrupts(&vm);
#endif
        }

        if (vm.insn_count_hi > emu.timer_hi ||
            (vm.insn_count_hi == emu.timer_hi && vm.insn_count > emu.timer_lo))
            vm.sip |= RV_INT_STI_BIT;
        else
            vm.sip &= ~RV_INT_STI_BIT;

        /* TODO: Implement the key sequence Ctrl-a x to exit the emulator */
        vm_step(&vm);
        if (likely(!vm.error))
            continue;

        if (vm.error == ERR_EXCEPTION && vm.exc_cause == RV_EXC_ECALL_S) {
            handle_sbi_ecall(&vm);
            continue;
        }

        if (vm.error == ERR_EXCEPTION) {
            vm_trap(&vm);
            continue;
        }

        vm_error_report(&vm);
        return 2;
    }

    /* unreachable */
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <linux-image> [<dtb>]\n", argv[0]);
        return 2;
    }
    return semu_start(argc, argv);
}
