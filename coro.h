#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
typedef struct __hart_internal hart_t;

/* Sentinel value when no coroutine is executing */
#define CORO_INVALID_ID UINT32_MAX

/* Initialize coroutine subsystem for SMP hart scheduling.
 * total_slots: total coroutine slots (usually n_hart)
 * hart_slots: number of actual CPU harts
 */
bool coro_init(uint32_t total_slots, uint32_t hart_slots);

/* Cleanup coroutine subsystem */
void coro_cleanup(void);

/* Create coroutine for a hart.
 * slot_id: Hart identifier
 * func: Entry point (hart execution loop)
 * arg: User data (hart_t pointer)
 */
bool coro_create_hart(uint32_t slot_id, void (*func)(void *), void *arg);

/* Resume execution of a hart coroutine */
void coro_resume_hart(uint32_t slot_id);

/* Yield from current hart (called from WFI) */
void coro_yield(void);

/* Check if hart is suspended (in WFI) */
bool coro_is_suspended(uint32_t slot_id);

/* Get currently executing hart ID */
uint32_t coro_current_hart_id(void);

/* Debug-related coroutine functions for GDB integration */

/* Suspend a hart for debugging (e.g., hit breakpoint)
 * The hart will not be scheduled until explicitly resumed.
 * This is different from WFI suspension.
 */
void coro_suspend_hart_debug(uint32_t hart_id);

/* Resume a hart from debug suspension */
void coro_resume_hart_debug(uint32_t hart_id);

/* Check if a hart is suspended for debugging */
bool coro_is_debug_suspended(uint32_t hart_id);

/* Execute exactly one instruction on a hart (for single-step debugging)
 * This will resume the hart, execute one instruction, then suspend again.
 * Returns: true on success, false if hart is not in valid state
 */
bool coro_step_hart(uint32_t hart_id);
