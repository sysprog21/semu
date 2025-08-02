#include <stdbool.h>
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

bool boot_complete = false;
static double ticks_increment;
static double boot_ticks;

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

/* The function that returns the "emulator time" in ticks.
 *
 * Before the boot process is completed, the emulator manually manages the
 * growth of ticks to suppress RCU CPU stall warnings. After the boot process is
 * completed, the emulator switches back to the real-time timer, using an offset
 * bridging to ensure that the ticks of both timers remain consistent.
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
        boot_ticks += ticks_increment;
        return (uint64_t) boot_ticks;
    }

    uint64_t real_ticks = mult_frac(host_time_ns(), timer->freq, 1e9);
    if (first_switch) {
        first_switch = false;

        /* Calculate the offset between the real time and the emulator time */
        offset = (int64_t) (real_ticks - boot_ticks);
    }
    return (uint64_t) ((int64_t) real_ticks - offset);
}

void semu_timer_init(semu_timer_t *timer, uint64_t freq, int n_harts)
{
    timer->freq = freq;
    timer->begin = mult_frac(host_time_ns(), timer->freq, 1e9);
    boot_ticks = timer->begin; /* Initialize the fake ticks for boot process */

    /* According to statistics, the number of times 'semu_timer_clocksource'
     * called is approximately 'SMP count * 2.15 * 1e8'. By the time the boot
     * process is completed, the emulator will have a total of 'boot seconds *
     * frequency' ticks. Therefore, each time, '(boot seconds * frequency) /
     * (2.15 * 1e8 * SMP count)' ticks need to be added.
     */
    ticks_increment =
        (SEMU_BOOT_TARGET_TIME * CLOCK_FREQ) / (2.15 * 1e8 * n_harts);
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer) - time;
}
