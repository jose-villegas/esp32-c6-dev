/*=============================================================================
 * Host runner - the fast loop.
 *
 * Builds and runs in well under a second, which is what makes
 * red-green-refactor practical. It runs only the portable suites; the
 * hardware-dependent ones live in the firmware and run on the device.
 *
 * The same suite sources are compiled into the firmware's self-test, so a
 * green run here is the same set of assertions the board will make.
 *===========================================================================*/

#include <stdio.h>

#include "unity.h"
#include "suites.h"

/* Unity requires these once per binary. Suites manage their own fixtures,
 * because several of them share this program. */
void setUp(void) { }
void tearDown(void) { }

int main(void)
{
    UNITY_BEGIN();

    suites_run_all();

    int failures = UNITY_END();

    /* A suite that did not fit is a test that did not run, so this run must
     * not come back green having quietly checked less than the whole set. */
    if (suites_dropped() > 0) {
        printf("FAIL: %d suite(s) dropped; raise SUITE_MAX in suites.h\n",
               suites_dropped());
        failures += suites_dropped();
    }
    return failures;
}
