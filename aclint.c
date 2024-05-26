#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

void aclint_timer_interrupts(vm_t *vm, aclint_state_t *aclint)
{
    uint64_t time_delta = aclint->mtimecmp - vm_timer_gettime(&vm->timer);
    if ((time_delta & 0x8000000000000000) || time_delta == 0)
        vm->sip |= RV_INT_STI_BIT;
    else
        vm->sip &= ~RV_INT_STI_BIT;
}

void aclint_update_interrupts(vm_t *vm, aclint_state_t *aclint)
{
    if (aclint->setssip)
        vm->sip |= RV_INT_SSI_BIT;
    else
        vm->sip &= ~RV_INT_SSI_BIT;
}

static bool aclint_reg_read(aclint_state_t *aclint,
                            uint32_t addr,
                            uint32_t *value)
{
#define _(reg) ACLINT_##reg
    switch (addr) {
    case _(SSWI):
        /* sswi */
        *value = aclint->setssip;
        return true;
    case _(MTIMECMP_LO):
        /* mtimecmp */
        *value = aclint->mtimecmp & 0xFFFFFFFF;
        return true;
    case _(MTIMECMP_HI):
        /* mtimecmph */
        *value = (aclint->mtimecmp >> 32) & 0xFFFFFFFF;
        return true;
    case _(MTIME_LO):
        /* mtime */
        *value = (uint32_t) (vm_timer_gettime(&aclint->mtimer) & 0xFFFFFFFF);
        return true;
    case _(MTIME_HI):
        /* mtimeh */
        *value =
            (uint32_t) (vm_timer_gettime(&aclint->mtimer) >> 32) & 0xFFFFFFFF;
        return true;
    default:
        return false;
    }
#undef _
}

static bool aclint_reg_write(aclint_state_t *aclint,
                             uint32_t addr,
                             uint32_t value)
{
#define _(reg) ACLINT_##reg
    switch (addr) {
    case _(SSWI):
        /* sswi */
        aclint->setssip = value;
        return true;
    case _(MTIMECMP_LO):
        /* mtimecmp */
        aclint->mtimecmp |= value;
        return true;
    case _(MTIMECMP_HI):
        /* mtimecmph */
        aclint->mtimecmp |= ((uint64_t) value) << 32;
        return true;
    case _(MTIME_LO):
        /* mtime */
        vm_timer_rebase(&aclint->mtimer, value);
        return true;
    case _(MTIME_HI):
        /* mtimeh */
        vm_timer_rebase(&aclint->mtimer, ((uint64_t) value) << 32);
        return true;
    default:
        return false;
    }
#undef _
}

void aclint_read(vm_t *vm,
                 aclint_state_t *aclint,
                 uint32_t addr,
                 uint8_t width,
                 uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!aclint_reg_read(aclint, addr >> 2, value))
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
void aclint_write(vm_t *vm,
                  aclint_state_t *aclint,
                  uint32_t addr,
                  uint8_t width,
                  uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!aclint_reg_write(aclint, addr >> 2, value))
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
