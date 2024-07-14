#pragma once

#include <stdint.h>

/* TIMER */
typedef struct {
    uint64_t begin;
    uint64_t freq;
} semu_timer_t;

void semu_timer_init(semu_timer_t *timer, uint64_t freq);
uint64_t semu_timer_get(semu_timer_t *timer);
void semu_timer_rebase(semu_timer_t *timer, uint64_t time);