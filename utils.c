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
static double scale_factor;

/* for testing */
uint64_t count = 0;
struct timespec boot_begin, boot_end;
double TEST_ns_per_call, TEST_predict_sec;
static int G_n_harts = 0;

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

/* BogoMips is a rough measurement of CPU speed, typically calculated by
 * executing a counting loop to estimate the CPU's performance.
 *
 * This function apply BogoMips method to measure the overhead of a
 * high-resolution timer call, typically `clock_gettime()` on POSIX or
 * `mach_absolute_time()` on macOS.
 *
 * 1) Times how long it takes to call `host_time_ns()` repeatedly for a given
 *    number of iterations. This is used to derive an average overhead per call
 *    (`ns_per_call`).
 * 2) Eliminates loop overhead by performing two measurements:
 *    - In the first measurement, `host_time_ns()` is called once per iteration.
 *    - In the second measurement, `host_time_ns()` is called twice per
 *      iteration.
 *    By subtracting the two results, the loop overhead is effectively canceled.
 * 3) Predicts the total time spent in `semu_timer_clocksource` during the boot
 *    process based on the measured overhead per call and the number of calls
 *    made (~2e8 times * SMP). This allows scaling the emulator clock to meet
 *    the target boot time (`SEMU_BOOT_TARGET_TIME`).
 */
static void measure_bogomips_ns(uint64_t iterations, int n_harts)
{
    G_n_harts = n_harts;

    /* Perform 'iterations' times calling the host HRT.
     *
     * Assuming the cost of loop overhead is 'e' and the cost of 'host_time_ns'
     * is 't', we perform a two-stage measurement to eliminate the loop
     * overhead. In the first loop, 'host_time_ns' is called only once per
     * iteration, while in the second loop, it is called twice per iteration.
     *
     * In this way, the cost of the first loop is 'e + t', and the cost of the
     * second loop is 'e + 2t'. By subtracting the two, we can effectively
     * eliminate the loop overhead.
     *
     * Reference:
     * https://ates.dev/posts/2025-01-12-accurate-benchmarking/
     */
    const uint64_t start1_ns = host_time_ns();
    for (uint64_t loops = 0; loops < iterations; loops++)
        (void) host_time_ns();

    const uint64_t end1_ns = host_time_ns();
    const uint64_t elapsed1_ns = end1_ns - start1_ns;

    /* Second measurement */
    const uint64_t start2_ns = host_time_ns();
    for (uint64_t loops = 0; loops < iterations; loops++) {
        (void) host_time_ns();
        (void) host_time_ns();
    }

    const uint64_t end2_ns = host_time_ns();
    const uint64_t elapsed2_ns = end2_ns - start2_ns;

    /* Calculate average overhead per call */
    const double ns_per_call =
        (double) (elapsed2_ns - elapsed1_ns) / (double) iterations;

    /* 'semu_timer_clocksource' is called ~2e8 times per SMP. Each call's
     * overhead ~ ns_per_call. The total overhead is ~ ns_per_call * SMP *
     * 2e8. That overhead is about 10~40% of the entire boot, we take the
     * minimum here to get more fault tolerance. Thus, effectively:
     *   predict_sec = ns_per_call * SMP * 2e8 * (100%/10%) / 1e9
     *               = ns_per_call * SMP * 2.0
     * Then scale_factor = (desired_time) / (predict_sec).
     */
    const double predict_sec = ns_per_call * n_harts * 2.0;
    scale_factor = SEMU_BOOT_TARGET_TIME / predict_sec;

    /* for testing */
    TEST_ns_per_call = ns_per_call;
    TEST_predict_sec = predict_sec;
}

/* The function that returns the "emulated time" in ticks.
 *
 * Before the boot completes, we scale time by 'scale_factor' for a "fake
 * increments" approach. After boot completes, we switch to real time
 * with an offset bridging so that there's no big jump.
 */
static uint64_t semu_timer_clocksource(semu_timer_t *timer)
{
    static uint64_t cnt = 0;
    static uint64_t time_1 = 0, time_2 = 0;
    static bool start_count = false;
    static FILE *time_log_file = NULL;

    count++;
    cnt++;

    /* After boot process complete, the timer will switch to real time. Thus,
     * there is an offset between the real time and the emulator time.
     *
     * After switching to real time, the correct way to update time is to
     * calculate the increment of time. Then add it to the emulator time.
     */
    static int64_t offset = 0;
    static bool first_switch = true;

    /* for testing */
    static volatile uint64_t local_total_ns = 0;
    static volatile uint64_t local_start;
    static volatile uint64_t local_end;
    static volatile uint64_t total_clocksource_ns = 0;

#if defined(HAVE_POSIX_TIMER) || defined(HAVE_MACH_TIMER)
    uint64_t now_ns = host_time_ns();
    local_start = now_ns;
    if (!start_count) {
        start_count = true;
        time_1 = now_ns;
    }

    /* real_ticks = (now_ns * freq) / 1e9 */
    uint64_t real_ticks = mult_frac(now_ns, timer->freq, 1e9);

    /* scaled_ticks = (now_ns * (freq*scale_factor)) / 1e9
     *              = ((now_ns * freq) / 1e9) * scale_factor
     */
    uint64_t scaled_ticks = real_ticks * 0.001;

    if (!boot_complete) {
        local_end = host_time_ns();
        total_clocksource_ns += local_end - local_start;
        local_total_ns += local_end - local_start;
        char filename[50];
        snprintf(filename, sizeof(filename), "./time_log/time_log_%d.txt",
                 G_n_harts);

        if (cnt % 1000000 == 0) {
            time_2 = now_ns;
            time_log_file = fopen(filename, "a");
            fprintf(time_log_file, "diff: %lu, total: %lu\n", (time_2 - time_1),
                    local_total_ns);
            fclose(time_log_file);

            cnt = 0;
            start_count = false;
            local_total_ns = 0;
        }
        return scaled_ticks; /* Return scaled ticks in the boot phase. */
    }

    /* The boot is done => switch to real freq with an offset bridging. */
    if (first_switch) {
        first_switch = false;
        offset = (int64_t) (real_ticks - scaled_ticks);

        /* for testing */
        local_end = host_time_ns();
        total_clocksource_ns += local_end - local_start;
        clock_gettime(CLOCK_REALTIME, &boot_end);

        double boot_time = (boot_end.tv_sec - boot_begin.tv_sec) +
                           (boot_end.tv_nsec - boot_begin.tv_nsec) / 1e9;
        printf(
            "\033[1;31m[SEMU LOG]: Real boot time: %.5f seconds, called %lu "
            "times semu_timer_clocksource\033[0m\n",
            boot_time, count);

        printf(
            "\033[1;31m[SEMU LOG]: ns_per_call = %.5f, predict_sec = %.5f, "
            "scale_factor = %.5f\033[0m\n",
            TEST_ns_per_call, TEST_predict_sec, scale_factor);

        printf(
            "\033[1;31m[SEMU LOG]: total_clocksource_ns = %lu, "
            "percentage = %.5f\033[0m\n",
            total_clocksource_ns,
            ((double) total_clocksource_ns / 2) /
                (((boot_end.tv_sec - boot_begin.tv_sec) * 1e9 +
                  boot_end.tv_nsec - boot_begin.tv_nsec) -
                 (total_clocksource_ns / 2)));

        printf(
            "\033[1;31m[SEMU LOG]: real_ns_per_call = %.5f, diff_ns_per_call = "
            "%.5f\033[0m\n",
            ((double) total_clocksource_ns / 2) / count,
            (((double) total_clocksource_ns / 2) / count) - TEST_ns_per_call);

        exit(0);
    }
    return (uint64_t) ((int64_t) real_ticks - offset);

#elif defined(HAVE_MACH_TIMER)
    /* Because we don't rely on sub-second calls to 'host_time_ns()' here,
     * we directly use time(0). This means the time resolution is coarse (1
     * second), but the logic is the same: we do a scaled approach pre-boot,
     * then real freq with an offset post-boot.
     */
    time_t now_sec = time(0);

    /* Before boot done, scale time. */
    if (!boot_complete)
        return (uint64_t) now_sec * timer->freq * 0.001;

    if (first_switch) {
        first_switch = false;
        uint64_t real_val = (uint64_t) now_sec * timer->freq;
        uint64_t scaled_val =
            (uint64_t) now_sec * (uint64_t) (timer->freq * 0.001);
        offset = (int64_t) (real_val - scaled_val);
    }

    /* Return real freq minus offset. */
    uint64_t real_freq_val = (uint64_t) now_sec * (uint64_t) timer->freq;
    return real_freq_val - offset;
#endif
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
    measure_bogomips_ns(freq, n_harts);
    timer->freq = freq;
    semu_timer_rebase(timer, 0);
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    return semu_timer_clocksource(timer) - timer->begin;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    timer->begin = semu_timer_clocksource(timer) - time;
}
