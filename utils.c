#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

bool boot_complete = false;
static double ticks_increment;
static double boot_ticks;

/* for testing */
uint64_t count = 0;
struct timespec boot_begin, boot_end;

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

/* High-precision time measurement:
 * - POSIX systems: clock_gettime() for nanosecond precision
 * - macOS: mach_absolute_time() with timebase conversion
 * - Other platforms: time(0) with conversion to nanoseconds as fallback
 *
 * The platform-specific timing logic is now clearly separated: POSIX and macOS
 * implementations provide high-precision measurements, while the fallback path
 * uses time(0) for a coarser but portable approach.
 */
static inline uint64_t host_time_ns()
{
#if defined(HAVE_POSIX_TIMER)
    struct timespec ts;
    clock_gettime(CLOCKID, &ts);
    return (uint64_t) ts.tv_sec * 1e9 + (uint64_t) ts.tv_nsec;

#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t ts = {0};
    if (ts.denom == 0)
        (void) mach_timebase_info(&ts);

    uint64_t now = mach_absolute_time();
    /* convert to nanoseconds: (now * t.numer / t.denom) */
    return mult_frac(now, ts.numer, (uint64_t) ts.denom);

#else
    /* Fallback to non-HRT calls time(0) in seconds => convert to ns. */
    time_t now_sec = time(0);
    return (uint64_t) now_sec * 1e9;
#endif
}

/* The function that returns the "emulated time" in ticks.
 *
 * Before the boot completes, we scale time by 'scale_factor' for a "fake
 * increments" approach. After boot completes, we switch to real time
 * with an offset bridging so that there's no big jump.
 */
static uint64_t semu_timer_clocksource(semu_timer_t *timer)
{
    /* After boot process complete, the timer will switch to real time. Thus,
     * there is an offset between the real time and the emulator time.
     *
     * After switching to real time, the correct way to update time is to
     * calculate the increment of time. Then add it to the emulator time.
     */
    static int64_t offset = 0;
    static bool first_switch = true;

    if (!boot_complete) {
        ++count; /* FOR TESTING */
        boot_ticks += ticks_increment;
        return (uint64_t) boot_ticks;
    }

    uint64_t now_ns = host_time_ns();
    uint64_t real_ticks = mult_frac(now_ns, timer->freq, 1e9);

    /* The boot is done => switch to real freq with an offset bridging. */
    if (first_switch) {
        first_switch = false;
        offset = (int64_t) (real_ticks - boot_ticks);

        /* FOR TESTING */
        clock_gettime(CLOCK_REALTIME, &boot_end);

        printf(
            "\033[1;31m[SEMU LOG]: Boot complete, switch to real-time "
            "timer\033[0m\n");

        double boot_time = (boot_end.tv_sec - boot_begin.tv_sec) +
                           (boot_end.tv_nsec - boot_begin.tv_nsec) / 1.0e9;

        printf(
            "\033[1;31m[SEMU LOG]: Boot time: %.5f seconds, called %ld "
            "times semu_timer_clocksource\033[0m\n",
            boot_time, count);

        printf(
            "\033[1;31m[SEMU LOG]: timer->begin: %lu, "
            "real_ticks: %lu, boot_ticks: %lu, offset: %ld\033[0m\n",
            timer->begin, real_ticks, (uint64_t) boot_ticks, offset);

        exit(0);
    }
    return (uint64_t) ((int64_t) real_ticks - offset);
}

void semu_timer_init(semu_timer_t *timer, uint64_t freq, int n_harts)
{
#if defined(HAVE_POSIX_TIMER)
    printf("\033[1;31m[SEMU LOG]: Use clock_gettime\033[0m\n");
#elif defined(HAVE_MACH_TIMER)
    printf("\033[1;31m[SEMU LOG]: Use mach_absolute_time\033[0m\n");
#else
    printf("\033[1;31m[SEMU LOG]: Use time\033[0m\n");
#endif

    /* Measure how long each call to 'host_time_ns()' roughly takes,
     * then use that to pick 'scale_factor'. For example, pass freq
     * as the loop count or some large number to get a stable measure.
     */
    // measure_bogomips_ns(freq, n_harts);
    timer->freq = freq;

    timer->begin = mult_frac(host_time_ns(), timer->freq, 1e9);
    boot_ticks = timer->begin;
    ticks_increment = (SEMU_BOOT_TARGET_TIME * CLOCK_FREQ) / (2.0e8 * n_harts);
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer) - time;
}
