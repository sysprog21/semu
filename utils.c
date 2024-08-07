#include <time.h>

#include "utils.h"

#if defined(__APPLE__)
#define HAVE_MACH_TIMER
#include <mach/mach_time.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#define HAVE_POSIX_TIMER

/*
 * Use a faster but less precise clock source because we need quick
 * timestamps rather than fine-grained precision.
 */
#ifdef CLOCK_MONOTONIC_COARSE
#define CLOCKID CLOCK_MONOTONIC_COARSE
#else
#define CLOCKID CLOCK_REALTIME_COARSE
#endif
#endif

void semu_timer_init(semu_timer_t *timer, uint64_t freq)
{
    timer->freq = freq;
    semu_timer_rebase(timer, 0);
}

static uint64_t semu_timer_clocksource(uint64_t freq)
{
#if defined(HAVE_POSIX_TIMER)
    struct timespec t;
    clock_gettime(CLOCKID, &t);
    return (t.tv_sec * freq) + (t.tv_nsec * freq / 1e9);
#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t t;
    if (mach_clk.denom == 0)
        (void) mach_timebase_info(&t);
    return mach_absolute_time() * freq / t.denom * t.numer;
#else
    return time(0) * freq;
#endif
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer->freq) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer->freq) - time;
}