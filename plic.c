#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* Make PLIC as simple as possible: 32 interrupts, no priority */

void plic_update_interrupts(vm_t *vm, plic_state_t *plic)
{
    /* Update pending interrupts */
    plic->ip |= plic->active & ~plic->masked;
    plic->masked |= plic->active;
    /* Send interrupt to target */
    for (uint32_t i = 0; i < vm->hart_number; i++) {
        if (plic->ip & plic->ie[i])
            vm->hart[i]->sip |= RV_INT_SEI_BIT;
        else
            vm->hart[i]->sip &= ~RV_INT_SEI_BIT;
    }
}

static bool plic_reg_read(plic_state_t *plic, uint32_t addr, uint32_t *value)
{
    /* no priority support: source priority hardwired to 1 */
    if (1 <= addr && addr <= 31)
        return true;

    if (addr == 0x400) {
        *value = plic->ip;
        return true;
    }

    int addr_mask = MASK(ilog2(addr)) ^ (1 & addr);
    int context = (addr_mask & addr);
    context >>= __builtin_ffs(context) - (__builtin_ffs(context) & 1);
    switch (addr & ~addr_mask) {
    case 0x800:
        *value = plic->ie[context];
        return true;
    case 0x80000:
        *value = 0;
        /* no priority support: target priority threshold hardwired to 0 */
        return true;
    case 0x80001:
        /* claim */
        *value = 0;
        uint32_t candidates = plic->ip & plic->ie[context];
        if (candidates) {
            *value = ilog2(candidates);
            plic->ip &= ~(1 << (*value));
        }
        return true;
    default:
        return false;
    }
}

static bool plic_reg_write(plic_state_t *plic, uint32_t addr, uint32_t value)
{
    /* no priority support: source priority hardwired to 1 */
    if (1 <= addr && addr <= 31)
        return true;

    int addr_mask = MASK(ilog2(addr)) ^ (1 & addr);
    int context = (addr_mask & addr);
    context >>= __builtin_ffs(context) - (__builtin_ffs(context) & 1);
    switch (addr & ~addr_mask) {
    case 0x800:
        value &= ~1;
        plic->ie[context] = value;
        return true;
    case 0x80000:
        /* no priority support: target priority threshold hardwired to 0 */
        return true;
    case 0x80001:
        /* completion */
        if (plic->ie[context] & (1 << value))
            plic->masked &= ~(1 << value);
        return true;
    default:
        return false;
    }
}

void plic_read(hart_t *vm,
               plic_state_t *plic,
               uint32_t addr,
               uint8_t width,
               uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!plic_reg_read(plic, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void plic_write(hart_t *vm,
                plic_state_t *plic,
                uint32_t addr,
                uint8_t width,
                uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!plic_reg_write(plic, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}