/* GDB control layer for coroutine-based multi-hart execution */

#include <string.h>

#include "coro.h"
#include "gdbctrl.h"

bool gdb_debug_init(vm_t *vm)
{
    if (!vm)
        return false;

    /* Initialize debug context */
    memset(&vm->debug_ctx, 0, sizeof(vm_debug_ctx_t));

    /* Initialize debug info for all harts */
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *hart = vm->hart[i];
        if (!hart)
            continue;

        memset(&hart->debug_info, 0, sizeof(hart_debug_info_t));
        hart->debug_info.state = HART_STATE_RUNNING;
        hart->debug_info.single_step_mode = false;
        hart->debug_info.breakpoint_pending = false;
    }

    return true;
}

void gdb_debug_cleanup(vm_t *vm)
{
    if (!vm)
        return;

    /* Clear all breakpoints */
    gdb_clear_all_breakpoints(vm);

    /* Reset all hart debug states */
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *hart = vm->hart[i];
        if (!hart)
            continue;

        hart->debug_info.state = HART_STATE_RUNNING;
        hart->debug_info.single_step_mode = false;
        hart->debug_info.breakpoint_pending = false;
    }
}

bool gdb_check_breakpoint(hart_t *hart)
{
    if (!hart || !hart->vm)
        return false;

    vm_t *vm = hart->vm;
    uint32_t pc = hart->pc;

    /* Check if current PC matches any enabled breakpoint */
    for (uint32_t i = 0; i < vm->debug_ctx.bp_count; i++) {
        breakpoint_t *bp = &vm->debug_ctx.breakpoints[i];
        if (bp->enabled && bp->addr == pc) {
            /* Breakpoint hit */
            hart->debug_info.breakpoint_pending = true;
            return true;
        }
    }

    return false;
}

bool gdb_set_breakpoint(vm_t *vm, uint32_t addr)
{
    if (!vm)
        return false;

    /* Check if breakpoint already exists at this address */
    for (uint32_t i = 0; i < vm->debug_ctx.bp_count; i++) {
        if (vm->debug_ctx.breakpoints[i].addr == addr) {
            /* Already exists, just ensure it's enabled */
            vm->debug_ctx.breakpoints[i].enabled = true;
            return true;
        }
    }

    /* Check if we have room for a new breakpoint */
    if (vm->debug_ctx.bp_count >= MAX_BREAKPOINTS)
        return false;

    /* Add new breakpoint */
    breakpoint_t *bp = &vm->debug_ctx.breakpoints[vm->debug_ctx.bp_count];
    bp->addr = addr;
    bp->enabled = true;
    vm->debug_ctx.bp_count++;

    return true;
}

bool gdb_del_breakpoint(vm_t *vm, uint32_t addr)
{
    if (!vm)
        return false;

    /* Find the breakpoint */
    for (uint32_t i = 0; i < vm->debug_ctx.bp_count; i++) {
        if (vm->debug_ctx.breakpoints[i].addr == addr) {
            /* Found it - remove by shifting remaining breakpoints down */
            uint32_t remaining = vm->debug_ctx.bp_count - i - 1;
            if (remaining > 0) {
                memmove(&vm->debug_ctx.breakpoints[i],
                        &vm->debug_ctx.breakpoints[i + 1],
                        remaining * sizeof(breakpoint_t));
            }
            vm->debug_ctx.bp_count--;
            return true;
        }
    }

    return false;
}

void gdb_clear_all_breakpoints(vm_t *vm)
{
    if (!vm)
        return;

    memset(&vm->debug_ctx.breakpoints, 0, sizeof(vm->debug_ctx.breakpoints));
    vm->debug_ctx.bp_count = 0;
}

uint32_t gdb_get_breakpoint_count(vm_t *vm)
{
    if (!vm)
        return 0;

    return vm->debug_ctx.bp_count;
}

void gdb_suspend_hart(hart_t *hart)
{
    if (!hart)
        return;

    /* Mark hart as suspended for debugging */
    hart->debug_info.state = HART_STATE_DEBUG_BREAK;

    /* Suspend the coroutine (only in SMP mode) */
    if (hart->vm && hart->vm->n_hart > 1)
        coro_suspend_hart_debug(hart->mhartid);
}

void gdb_resume_hart(hart_t *hart)
{
    if (!hart)
        return;

    /* Clear breakpoint pending flag */
    hart->debug_info.breakpoint_pending = false;

    /* If single-step mode is enabled, mark as DEBUG_STEP instead of RUNNING */
    if (hart->debug_info.single_step_mode) {
        hart->debug_info.state = HART_STATE_DEBUG_STEP;
    } else {
        hart->debug_info.state = HART_STATE_RUNNING;
    }

    /* Resume the coroutine (only in SMP mode) */
    if (hart->vm && hart->vm->n_hart > 1) {
        coro_resume_hart_debug(hart->mhartid);
    }
}

void gdb_enable_single_step(hart_t *hart)
{
    if (!hart)
        return;

    hart->debug_info.single_step_mode = true;
    hart->debug_info.state = HART_STATE_DEBUG_STEP;
}

void gdb_disable_single_step(hart_t *hart)
{
    if (!hart)
        return;

    hart->debug_info.single_step_mode = false;
    if (hart->debug_info.state == HART_STATE_DEBUG_STEP)
        hart->debug_info.state = HART_STATE_RUNNING;
}

bool gdb_is_single_stepping(hart_t *hart)
{
    if (!hart)
        return false;

    return hart->debug_info.single_step_mode;
}
