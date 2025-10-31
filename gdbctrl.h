/* GDB control layer for coroutine-based multi-hart execution */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "riscv.h"

/* Initialize GDB debug subsystem for a VM
 * @vm: VM instance to initialize debugging for
 * Returns: true on success, false on failure
 */
bool gdb_debug_init(vm_t *vm);

/* Cleanup GDB debug subsystem
 * @vm: VM instance to cleanup
 */
void gdb_debug_cleanup(vm_t *vm);

/* Check if a hart has hit a breakpoint at its current PC
 * This should be called before executing each instruction.
 * @hart: Hart to check
 * Returns: true if breakpoint hit, false otherwise
 */
bool gdb_check_breakpoint(hart_t *hart);

/* Set a breakpoint at the specified address
 * @vm: VM instance
 * @addr: Virtual address for breakpoint
 * Returns: true on success, false if breakpoint table is full
 */
bool gdb_set_breakpoint(vm_t *vm, uint32_t addr);

/* Delete a breakpoint at the specified address
 * @vm: VM instance
 * @addr: Virtual address of breakpoint to remove
 * Returns: true if breakpoint was found and removed, false otherwise
 */
bool gdb_del_breakpoint(vm_t *vm, uint32_t addr);

/* Clear all breakpoints
 * @vm: VM instance
 */
void gdb_clear_all_breakpoints(vm_t *vm);

/* Get breakpoint count
 * @vm: VM instance
 * Returns: Number of active breakpoints
 */
uint32_t gdb_get_breakpoint_count(vm_t *vm);

/* Suspend a hart for debugging (breakpoint or user interrupt)
 * This marks the hart as DEBUG_BREAK and prevents it from being scheduled.
 * @hart: Hart to suspend
 */
void gdb_suspend_hart(hart_t *hart);

/* Resume a hart from debug suspension
 * This marks the hart as RUNNING and allows it to be scheduled.
 * @hart: Hart to resume
 */
void gdb_resume_hart(hart_t *hart);

/* Enable single-step mode for a hart
 * The hart will execute one instruction then suspend again.
 * @hart: Hart to single-step
 */
void gdb_enable_single_step(hart_t *hart);

/* Disable single-step mode for a hart
 * @hart: Hart to disable single-stepping
 */
void gdb_disable_single_step(hart_t *hart);

/* Check if a hart is in single-step mode
 * @hart: Hart to check
 * Returns: true if single-stepping, false otherwise
 */
bool gdb_is_single_stepping(hart_t *hart);
