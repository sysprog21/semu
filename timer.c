#include <time.h>

#include "riscv.h"

#if defined(__APPLE__)
#define HAVE_MACH_TIMER
#include <mach/mach_time.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#define HAVE_POSIX_TIMER
#ifdef CLOCK_MONOTONIC
#define CLOCKID CLOCK_MONOTONIC
#else
#define CLOCKID CLOCK_REALTIME
#endif
#endif

uint64_t vm_timer_clocksource(uint64_t freq)
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

uint64_t vm_timer_gettime(vm_timer_t *timer)
{
    return vm_timer_clocksource(timer->freq) - timer->begin;
}

void vm_timer_rebase(vm_timer_t *timer, uint64_t time)
{
    timer->begin = vm_timer_clocksource(timer->freq) - time;
}