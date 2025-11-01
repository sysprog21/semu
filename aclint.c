#include <stdint.h>
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* ACLINT MTIMER */

/* Recalculate the next interrupt time by finding the minimum mtimecmp
 * across all harts. This is called whenever mtimecmp is written.
 */
void aclint_mtimer_recalc_next_interrupt(mtimer_state_t *mtimer)
{
    uint64_t min_cmp = UINT64_MAX;
    for (uint32_t i = 0; i < mtimer->n_harts; i++) {
        if (mtimer->mtimecmp[i] < min_cmp)
            min_cmp = mtimer->mtimecmp[i];
    }
    mtimer->next_interrupt_at = min_cmp;
}

void aclint_mtimer_update_interrupts(hart_t *hart, mtimer_state_t *mtimer)
{
    if (semu_timer_get(&mtimer->mtime) >= mtimer->mtimecmp[hart->mhartid])
        hart->sip |= RV_INT_STI_BIT; /* Set Supervisor Timer Interrupt */
    else
        hart->sip &= ~RV_INT_STI_BIT; /* Clear Supervisor Timer Interrupt */
}

static bool aclint_mtimer_reg_read(mtimer_state_t *mtimer,
                                   uint32_t addr,
                                   uint32_t *value)
{
    /* 'addr & 0x4' is used to determine the upper or lower 32 bits
     * of the mtimecmp register. If 'addr & 0x4' is 0, then the lower 32
     * bits are accessed.
     *
     * 'addr >> 3' is used to get the index of the mtimecmp array. In
     * "ACLINT MTIMER Compare Register Map", each mtimecmp register is 8
     * bytes long. So, we need to divide the address by 8 to get the index.
     */

    /* mtimecmp (0x4300000 ~ 0x4307FF8) */
    if (addr < 0x7FF8) {
        *value =
            (uint32_t) (mtimer->mtimecmp[addr >> 3] >> (addr & 0x4 ? 32 : 0));
        return true;
    }

    /* mtime (0x4307FF8 ~ 0x4308000) */
    if (addr < 0x8000) {
        *value = (uint32_t) (semu_timer_get(&mtimer->mtime) >>
                             (addr & 0x4 ? 32 : 0));
        return true;
    }
    return false;
}

static bool aclint_mtimer_reg_write(mtimer_state_t *mtimer,
                                    uint32_t addr,
                                    uint32_t value)
{
    /* The 'cmp_val & 0xFFFFFFFF' is used to select the upper 32 bits
     * of mtimer->mtimecmp[addr >> 3], then shift the value to the left by
     * 32 bits to set the upper 32 bits.
     *
     * Similarly, 'cmp_val & 0xFFFFFFFF00000000ULL' is used to select the lower
     * 32 bits of mtimer->mtimecmp[addr >> 3].
     */

    /* mtimecmp (0x4300000 ~ 0x4307FF8) */
    if (addr < 0x7FF8) {
        uint64_t cmp_val = mtimer->mtimecmp[addr >> 3];

        if (addr & 0x4)
            cmp_val = (cmp_val & 0xFFFFFFFF) | ((uint64_t) value << 32);
        else
            cmp_val = (cmp_val & 0xFFFFFFFF00000000ULL) | value;

        mtimer->mtimecmp[addr >> 3] = cmp_val;

        /* Recalculate next interrupt time when mtimecmp is updated.
         * This is critical for lazy timer checking optimization.
         */
        aclint_mtimer_recalc_next_interrupt(mtimer);
        return true;
    }

    /* mtime (0x4307FF8 ~ 0x4308000) */
    if (addr < 0x8000) {
        uint64_t mtime_val = mtimer->mtime.begin;
        if (addr & 0x4)
            mtime_val = (mtime_val & 0xFFFFFFFF) | ((uint64_t) value << 32);
        else
            mtime_val = (mtime_val & 0xFFFFFFFF00000000ULL) | value;

        semu_timer_rebase(&mtimer->mtime, mtime_val);
        return true;
    }

    return false;
}

void aclint_mtimer_read(hart_t *hart,
                        mtimer_state_t *mtimer,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t *value)
{
    if (!aclint_mtimer_reg_read(mtimer, addr, value))
        vm_set_exception(hart, RV_EXC_LOAD_FAULT, hart->exc_val);

    *value >>= RV_MEM_SW - width;
}

void aclint_mtimer_write(hart_t *hart,
                         mtimer_state_t *mtimer,
                         uint32_t addr,
                         uint8_t width,
                         uint32_t value)
{
    if (!aclint_mtimer_reg_write(mtimer, addr, value << (RV_MEM_SW - width)))
        vm_set_exception(hart, RV_EXC_STORE_FAULT, hart->exc_val);
}

/* ACLINT MSWI */
void aclint_mswi_update_interrupts(hart_t *hart, mswi_state_t *mswi)
{
    if (mswi->msip[hart->mhartid])
        hart->sip |= RV_INT_SSI_BIT; /* Set Machine Software Interrupt */
    else
        hart->sip &= ~RV_INT_SSI_BIT; /* Clear Machine Software Interrupt */
}

static bool aclint_mswi_reg_read(mswi_state_t *mswi,
                                 uint32_t addr,
                                 uint32_t *value)
{
    /* 'msip' is an array where each entry corresponds to a Hart,
     * each entry is 4 bytes (32 bits). So, we need to divide the address
     * by 4 to get the index.
     */

    /* Address range for msip: 0x4400000 ~ 0x4404000 */
    if (addr < 0x4000) {
        *value = mswi->msip[addr >> 2];
        return true;
    }
    return false;
}

static bool aclint_mswi_reg_write(mswi_state_t *mswi,
                                  uint32_t addr,
                                  uint32_t value)
{
    if (addr < 0x4000) {
        mswi->msip[addr >> 2] = value & 0x1; /* Only the LSB is valid */
        return true;
    }
    return false;
}

void aclint_mswi_read(hart_t *hart,
                      mswi_state_t *mswi,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t *value)
{
    if (!aclint_mswi_reg_read(mswi, addr, value))
        vm_set_exception(hart, RV_EXC_LOAD_FAULT, hart->exc_val);

    *value >>= RV_MEM_SW - width;
}

void aclint_mswi_write(hart_t *hart,
                       mswi_state_t *mswi,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t value)
{
    if (!aclint_mswi_reg_write(mswi, addr, value << (RV_MEM_SW - width)))
        vm_set_exception(hart, RV_EXC_STORE_FAULT, hart->exc_val);
}

/* ACLINT SSWI */
void aclint_sswi_update_interrupts(hart_t *hart, sswi_state_t *sswi)
{
    if (sswi->ssip[hart->mhartid])
        hart->sip |= RV_INT_SSI_BIT; /* Set Supervisor Software Interrupt */
    else
        hart->sip &= ~RV_INT_SSI_BIT; /* Clear Supervisor Software Interrupt */
}

static bool aclint_sswi_reg_read(__attribute__((unused)) sswi_state_t *sswi,
                                 uint32_t addr,
                                 uint32_t *value)
{
    /* Address range for ssip: 0x4500000 ~ 0x4504000 */
    if (addr < 0x4000) {
        *value = 0; /* Upper 31 bits are zero, and LSB reads as 0 */
        return true;
    }
    return false;
}

static bool aclint_sswi_reg_write(sswi_state_t *sswi,
                                  uint32_t addr,
                                  uint32_t value)
{
    if (addr < 0x4000) {
        sswi->ssip[addr >> 2] = value & 0x1; /* Only the LSB is valid */

        return true;
    }
    return false;
}

void aclint_sswi_read(hart_t *hart,
                      sswi_state_t *sswi,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t *value)
{
    if (!aclint_sswi_reg_read(sswi, addr, value))
        vm_set_exception(hart, RV_EXC_LOAD_FAULT, hart->exc_val);

    *value >>= RV_MEM_SW - width;
}

void aclint_sswi_write(hart_t *hart,
                       sswi_state_t *sswi,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t value)
{
    if (!aclint_sswi_reg_write(sswi, addr, value << (RV_MEM_SW - width)))
        vm_set_exception(hart, RV_EXC_STORE_FAULT, hart->exc_val);
}
