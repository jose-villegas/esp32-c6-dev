/*=============================================================================
 * Specification for display - shell-owned orientation, decided from a
 * smoothed gravity vector with hysteresis around the quarter boundaries.
 *
 * The test that matters is the boundary sweep: a slow tilt crossing the old
 * 45-degree snap point must change orientation exactly once, not chatter
 * back and forth as the reading wobbles either side of it. Everything else
 * here pins down the four unambiguous orientations, the no-oscillation
 * behaviour parked exactly on a boundary, the asymmetric "60 out, 30 back"
 * thresholds, and that display_update() only reports true on a genuine
 * change.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"
#include "display/display.h"

/* A magnitude comfortably past every threshold this module uses, so a vector
 * built from it reads as "unambiguously" pointing one way. */
#define STRONG 1000

/* Feeds the same steady (gx, gy) repeatedly, as a steadily-held tilt would
 * read at ~10 Hz, until display_update() stops reporting a change (or a
 * generous cap is hit) - see display.h's own note that a single call can
 * only ever move one quarter, so settling into a fully opposite orientation
 * from a strong start can take a second call. Four calls is more than any
 * reachable case needs. */
static void settle(display_t *d, int gx, int gy)
{
    for (int i = 0; i < 4; i++) {
        if (!display_update(d, gx, gy)) {
            return;
        }
    }
}

/* --- the four unambiguous orientations ----------------------------------- */

void test_gravity_straight_down_reads_upright(void)
{
    display_t d;
    display_init(&d);
    settle(&d, 0, STRONG);
    TEST_ASSERT_EQUAL_INT(0, display_quarter(&d));
}

void test_gravity_to_the_left_reads_quarter_one(void)
{
    display_t d;
    display_init(&d);
    settle(&d, -STRONG, 0);
    TEST_ASSERT_EQUAL_INT(1, display_quarter(&d));
}

void test_gravity_straight_up_reads_upside_down(void)
{
    display_t d;
    display_init(&d);
    settle(&d, 0, -STRONG);
    TEST_ASSERT_EQUAL_INT(2, display_quarter(&d));
}

void test_gravity_to_the_right_reads_quarter_three(void)
{
    display_t d;
    display_init(&d);
    settle(&d, STRONG, 0);
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));
}

/* --- the test that matters: a slow sweep changes orientation ONCE --------- */

void test_a_slow_sweep_through_a_boundary_flips_exactly_once(void)
{
    display_t d;
    display_init(&d);   /* quarter 0: down is down */

    /* gy held fixed while gx climbs from 0 well past the old 45-degree snap
     * point (gx == gy) and on past the 60-degree hysteresis trigger, in
     * small steps - a tilt swept smoothly rather than jumped. */
    const int gy = STRONG;
    int changes = 0;
    for (int gx = 0; gx <= 2 * STRONG; gx += 5) {
        if (display_update(&d, gx, gy)) {
            changes++;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, changes,
        "a smooth sweep across one boundary must report exactly one change");
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));
}

/* --- parked exactly on a boundary does not oscillate ---------------------- */

void test_parked_on_the_old_boundary_does_not_oscillate(void)
{
    display_t d;
    display_init(&d);   /* quarter 0 */

    /* |gx| == |gy| is the OLD snap-to-nearest boundary (45 degrees) - well
     * inside this module's 30..60 degree hysteresis band either way, so it
     * must never be enough to switch. */
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_FALSE_MESSAGE(display_update(&d, STRONG, STRONG),
            "a vector held exactly on the old boundary must not flip");
    }
    TEST_ASSERT_EQUAL_INT(0, display_quarter(&d));
}

void test_parked_on_the_boundary_from_the_other_side_does_not_oscillate(void)
{
    display_t d;
    display_init(&d);
    settle(&d, STRONG, 0);   /* quarter 3 */
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));

    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_FALSE_MESSAGE(display_update(&d, STRONG, STRONG),
            "the same parked vector must not flip quarter 3 back either");
    }
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));
}

/* --- returning partway does not flip until past the inner threshold ------- */

void test_returning_partway_does_not_flip_until_the_inner_threshold(void)
{
    display_t d;
    display_init(&d);
    settle(&d, STRONG, 0);   /* quarter 3: down is to the right */
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));

    /* Tilting back toward "down is down", but only a little - angle from the
     * x axis is arctan(700/1000) =~ 35 degrees, short of the 60-degree
     * threshold this module needs to leave quarter 3. Must hold. */
    TEST_ASSERT_FALSE_MESSAGE(display_update(&d, 1000, 700),
        "a partial return must not flip the orientation yet");
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));

    /* Further still - angle from the x axis is now arctan(1000/500) =~ 63
     * degrees, past the threshold (equivalently, ~27 degrees from "down is
     * down", inside the 30-degree inner band) - now it must flip. */
    TEST_ASSERT_TRUE_MESSAGE(display_update(&d, 500, 1000),
        "a return well past the inner threshold must flip back");
    TEST_ASSERT_EQUAL_INT(0, display_quarter(&d));
}

/* --- display_update() reports true only on an actual change --------------- */

void test_update_reports_true_only_on_an_actual_change(void)
{
    display_t d;
    display_init(&d);   /* already quarter 0 */

    TEST_ASSERT_FALSE_MESSAGE(display_update(&d, 0, STRONG),
        "feeding the orientation the module already reports must not claim a change");

    TEST_ASSERT_TRUE_MESSAGE(display_update(&d, STRONG, 0),
        "a genuine switch must report true");
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));

    TEST_ASSERT_FALSE_MESSAGE(display_update(&d, STRONG, 0),
        "holding the same tilt steady after the switch must not report true again");
    TEST_ASSERT_EQUAL_INT(3, display_quarter(&d));
}

/* --- suite ---------------------------------------------------------------- */

void run_display_suite(void)
{
    RUN_TEST(test_gravity_straight_down_reads_upright);
    RUN_TEST(test_gravity_to_the_left_reads_quarter_one);
    RUN_TEST(test_gravity_straight_up_reads_upside_down);
    RUN_TEST(test_gravity_to_the_right_reads_quarter_three);

    RUN_TEST(test_a_slow_sweep_through_a_boundary_flips_exactly_once);

    RUN_TEST(test_parked_on_the_old_boundary_does_not_oscillate);
    RUN_TEST(test_parked_on_the_boundary_from_the_other_side_does_not_oscillate);

    RUN_TEST(test_returning_partway_does_not_flip_until_the_inner_threshold);

    RUN_TEST(test_update_reports_true_only_on_an_actual_change);
}

SUITE_REGISTER(run_display_suite);
