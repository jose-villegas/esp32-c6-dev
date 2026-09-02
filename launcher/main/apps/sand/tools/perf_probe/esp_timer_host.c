/*=============================================================================
 * esp_timer_host - a REAL host implementation of esp_timer_get_time(), for
 * this probe only.
 *
 * launcher/test/stubs/esp_timer.h (used by the ordinary host suite) only
 * DECLARES esp_timer_get_time() - timing.c never calls it there, because
 * the ordinary host build does not define DEVICE_BUILD and uses clock()
 * instead (coarse: 1 ms ticks on this platform's CLOCKS_PER_SEC, nowhere
 * near enough resolution to compare a handful of microsecond-scale probes
 * against each other). This probe DOES define DEVICE_BUILD (to compile
 * suite_sand.c's frame-budget scenes at all), so timing.c takes the
 * esp_timer_get_time() branch for real - this file is what makes that
 * link.
 *===========================================================================*/
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>

int64_t
esp_timer_get_time(void) {
    static LARGE_INTEGER freq;
    static int inited = 0;
    LARGE_INTEGER now;

    if (!inited) {
        QueryPerformanceFrequency(&freq);
        inited = 1;
    }
    QueryPerformanceCounter(&now);
    /* Split into whole seconds and the sub-second remainder before scaling
     * to microseconds, so this does not overflow a 64-bit product on a
     * multi-GHz counter over a multi-hour uptime. */
    const long long whole_s = now.QuadPart / freq.QuadPart;
    const long long rem = now.QuadPart % freq.QuadPart;
    return whole_s * 1000000LL + (rem * 1000000LL) / freq.QuadPart;
}

#else
#include <time.h>

int64_t
esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

#endif
