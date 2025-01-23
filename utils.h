#pragma once

#include <stdbool.h>
#include <stdint.h>

/* To suppress RCU CPU stall warnings, the emulator provides a scaled time to
 * the Guest OS during the boot process. After the boot process is complete, the
 * scaling is disabled to achieve a real-time timer.
 *
 * Since the Guest OS transitions to U mode for the first time when it loads the
 * initial user-mode process, we use this transition to determine whether the
 * boot process has completed.
 */
extern bool boot_complete;

/* TIMER */
typedef struct {
    uint64_t begin;
    uint64_t freq;
} semu_timer_t;

void semu_timer_init(semu_timer_t *timer, uint64_t freq, int n_harts);
uint64_t semu_timer_get(semu_timer_t *timer);
void semu_timer_rebase(semu_timer_t *timer, uint64_t time);