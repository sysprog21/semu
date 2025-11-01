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
