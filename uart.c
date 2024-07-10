#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* Emulate 8250 (plain, without loopback mode support) */

#define U8250_INT_THRE 1

static void reset_keyboard_input()
{
    /* Re-enable echo, etc. on keyboard. */
    struct termios term;
    tcgetattr(0, &term);
    term.c_lflag |= ICANON | ECHO;
    tcsetattr(0, TCSANOW, &term);
}

/* Asynchronous communication to capture all keyboard input for the VM. */
void capture_keyboard_input()
{
    /* Hook exit, because we want to re-enable keyboard. */
    atexit(reset_keyboard_input);

    struct termios term;
    tcgetattr(0, &term);
    term.c_lflag &= ~(ICANON | ECHO | ISIG); /* Disable echo as well */
    tcsetattr(0, TCSANOW, &term);
}

void u8250_update_interrupts(u8250_state_t *uart)
{
    /* Some interrupts are level-generated. */
    /* TODO: does it also generate an LSR change interrupt? */
    if (uart->in_ready)
        uart->pending_ints |= 1;
    else
        uart->pending_ints &= ~1;

    /* Prevent generating any disabled interrupts in the first place */
    uart->pending_ints &= uart->ier;

    /* Update current interrupt (higher bits -> more priority) */
    if (uart->pending_ints)
        uart->current_int = ilog2(uart->pending_ints);
}

void u8250_check_ready(u8250_state_t *uart)
{
    if (uart->in_ready)
        return;

    struct pollfd pfd = {uart->in_fd, POLLIN, 0};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN)
        uart->in_ready = true;
}

static void u8250_handle_out(u8250_state_t *uart, uint8_t value)
{
    if (write(uart->out_fd, &value, 1) < 1)
        fprintf(stderr, "failed to write UART output: %s\n", strerror(errno));
}

static uint8_t u8250_handle_in(u8250_state_t *uart)
{
    uint8_t value = 0;
    u8250_check_ready(uart);
    if (!uart->in_ready)
        return value;

    if (read(uart->in_fd, &value, 1) < 0)
        fprintf(stderr, "failed to read UART input: %s\n", strerror(errno));
    uart->in_ready = false;
    u8250_check_ready(uart);

    if (value == 1) {           /* start of heading (Ctrl-a) */
        if (getchar() == 120) { /* keyboard x */
            printf("\n");       /* end emulator with newline */
            exit(0);
        }
    }

    return value;
}

static void u8250_reg_read(u8250_state_t *uart, uint32_t addr, uint8_t *value)
{
    switch (addr) {
    case 0:
        if (uart->lcr & (1 << 7)) { /* DLAB */
            *value = uart->dll;
            break;
        }
        *value = u8250_handle_in(uart);
        break;
    case 1:
        if (uart->lcr & (1 << 7)) { /* DLAB */
            *value = uart->dlh;
            break;
        }
        *value = uart->ier;
        break;
    case 2:
        *value = (uart->current_int << 1) | (uart->pending_ints ? 0 : 1);
        if (uart->current_int == U8250_INT_THRE)
            uart->pending_ints &= ~(1 << uart->current_int);
        break;
    case 3:
        *value = uart->lcr;
        break;
    case 4:
        *value = uart->mcr;
        break;
    case 5:
        /* LSR = no error, TX done & ready */
        *value = 0x60 | (uint8_t) uart->in_ready;
        break;
    case 6:
        /* MSR = carrier detect, no ring, data ready, clear to send. */
        *value = 0xb0;
        break;
        /* no scratch register, so we should be detected as a plain 8250. */
    default:
        *value = 0;
    }
}

static void u8250_reg_write(u8250_state_t *uart, uint32_t addr, uint8_t value)
{
    switch (addr) {
    case 0:
        if (uart->lcr & (1 << 7)) { /* DLAB */
            uart->dll = value;
            break;
        }
        u8250_handle_out(uart, value);
        uart->pending_ints |= 1 << U8250_INT_THRE;
        break;
    case 1:
        if (uart->lcr & (1 << 7)) { /* DLAB */
            uart->dlh = value;
            break;
        }
        uart->ier = value;
        break;
    case 3:
        uart->lcr = value;
        break;
    case 4:
        uart->mcr = value;
        break;
    }
}

void u8250_read(hart_t *vm,
                u8250_state_t *uart,
                uint32_t addr,
                uint8_t width,
                uint32_t *value)
{
    uint8_t u8value;
    switch (width) {
    case RV_MEM_LBU:
        u8250_reg_read(uart, addr, &u8value);
        *value = (uint32_t) u8value;
        break;
    case RV_MEM_LB:
        u8250_reg_read(uart, addr, &u8value);
        *value = (uint32_t) (int8_t) u8value;
        break;
    case RV_MEM_LW:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void u8250_write(hart_t *vm,
                 u8250_state_t *uart,
                 uint32_t addr,
                 uint8_t width,
                 uint32_t value)
{
    switch (width) {
    case RV_MEM_SB:
        u8250_reg_write(uart, addr, value);
        break;
    case RV_MEM_SW:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}
