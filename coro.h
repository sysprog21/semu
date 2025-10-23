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
