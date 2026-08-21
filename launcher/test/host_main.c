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

#include "unity.h"
#include "suites.h"

/* Unity requires these once per binary. Suites manage their own fixtures,
 * because several of them share this program. */
void setUp(void) { }
void tearDown(void) { }

int main(void)
{
    UNITY_BEGIN();

    run_touch_fsm_suite();
    run_gesture_suite();

    return UNITY_END();
}
