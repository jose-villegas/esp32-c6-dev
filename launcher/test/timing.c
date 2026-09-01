/*=============================================================================
 * Implements the RUN_TEST override declared in timing.h.
 *
 * Wraps Unity's own dispatcher (UnityDefaultTestRun) from outside: the timer
 * starts before it and stops after it returns, so nothing here runs inside
 * setUp(), the test body, or tearDown(). That matters because a handful of
 * tests time their own subject with esp_timer_get_time() around a narrower
 * window (a single sand_step(), say) - this must never be what widens that
 * window.
 *
 * The elapsed-time line is printed AFTER UnityDefaultTestRun returns, so it
 * never touches the existing "file:line:name:PASS" line - the one two
 * tools (launcher/tools/sweeps/validate_capture.py and
 * launcher/main/apps/sand/tools/report_performance.py) already parse.
 *===========================================================================*/
#include "timing.h"

#include <stdint.h>
#include <stdio.h>

#ifdef DEVICE_BUILD
#include "esp_timer.h"
#else
#include <time.h>
#endif

/* Not pulled from unity.h: that header only declares this when RUN_TEST is
 * NOT already defined (see timing.h's top comment) - the opposite of this
 * file's own situation, since it is what RUN_TEST now expands to. The real
 * definition lives in Unity's own unity.c/UnityDefaultTestRun and is
 * untouched; this is only the prototype, hand-matched to it. */
extern void UnityDefaultTestRun(void (*Func)(void), const char *FuncName, const int FuncLineNum);

void suite_run_test_timed(void (*func)(void), const char *name, int line)
{
#ifdef DEVICE_BUILD
    const int64_t started = esp_timer_get_time();
#else
    const clock_t started = clock();
#endif

    UnityDefaultTestRun(func, name, line);

#ifdef DEVICE_BUILD
    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
#else
    const long elapsed_ms = (clock() - started) * 1000L / CLOCKS_PER_SEC;
#endif

    /* Own sentinel line, same key=value shape as selftest.c's
     * SELFTEST_COMPLETE - a new line rather than an appended suffix, so the
     * existing result line's format never changes. %lld/int64_t rather than
     * a narrower width: these range from under a millisecond to the better
     * part of eight minutes. */
    printf("TEST_TIME name=%s elapsed_ms=%lld\n", name, (long long)elapsed_ms);
}
