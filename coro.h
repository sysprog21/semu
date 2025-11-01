/* Lightweight coroutine for multi-hart execution */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Forward declaration */
typedef struct __hart_internal hart_t;

/* Initialize coroutine subsystem for a VM with n_hart cores */
bool coro_init(uint32_t n_hart);

/* Cleanup coroutine subsystem */
void coro_cleanup(void);

/* Create coroutine for a specific hart.
 * @hart_id: Hart identifier (0 to n_hart-1)
 * @func: Entry point function for the coroutine
 * @hart: User data (hart_t pointer) passed to the entry function
 *
 * Returns: true on success, false on failure
 */
bool coro_create_hart(uint32_t hart_id, void (*func)(void *), void *hart);

/* Resume execution of a specific hart's coroutine
 * The coroutine will execute until it yields or terminates.
 */
void coro_resume_hart(uint32_t hart_id);

/* Yield from current hart (called from WFI)
 * Suspends the current coroutine and returns control to the scheduler.
 */
void coro_yield(void);

/* Check if a hart's coroutine is suspended (waiting in WFI)
 * Returns: true if suspended, false otherwise
 */
bool coro_is_suspended(uint32_t hart_id);

/* Get the currently running hart ID
 * Returns: Hart ID of the currently executing coroutine, or UINT32_MAX if idle
 */
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
