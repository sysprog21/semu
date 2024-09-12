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

/* Calculate "x * n / d" without unnecessary overflow or loss of precision.
 *
 * Reference:
 * https://elixir.bootlin.com/linux/v6.10.7/source/include/linux/math.h#L121
 */
static inline uint64_t mult_frac(uint64_t x, uint64_t n, uint64_t d)
{
    const uint64_t q = x / d;
    const uint64_t r = x % d;

    return q * n + r * n / d;
}

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
    return t.tv_sec * freq + mult_frac(t.tv_nsec, freq, 1e9);
#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t t;
    if (t.denom == 0)
        (void) mach_timebase_info(&t);
    return mult_frac(mult_frac(mach_absolute_time(), t.numer, t.denom), freq,
                     1e9);
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
