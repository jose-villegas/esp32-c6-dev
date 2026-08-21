/*=============================================================================
 * Specification for the home gesture - a swipe up from the bottom edge, which
 * is how an app is closed.
 *
 * This has to be forgiving enough to trigger reliably with a fingertip, and
 * strict enough that it never fires while an app is being used normally. Those
 * pull in opposite directions, so the boundaries are worth pinning down.
 *===========================================================================*/

#include "unity.h"
#include "gesture.h"

#define SCREEN_H 448

/* Builds the input state for a finger currently at (x, y) that first touched
 * down at (press_x, press_y). */
static input_t dragging(int press_x, int press_y, int x, int y)
{
    input_t in = { 0 };
    in.down    = true;
    in.press_x = press_x;
    in.press_y = press_y;
    in.x       = x;
    in.y       = y;
    return in;
}

/* --- what should trigger it --------------------------------------------- */

void test_swipe_up_from_the_bottom_edge_triggers(void)
{
    /* Starts 20 px from the bottom, travels 150 px up. */
    input_t in = dragging(180, SCREEN_H - 20, 180, SCREEN_H - 170);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, SCREEN_H));
}

void test_it_triggers_anywhere_along_the_bottom_edge(void)
{
    /* No target to miss is the entire point of using a gesture. */
    const int xs[] = { 5, 100, 184, 300, 363 };
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        input_t in = dragging(xs[i], SCREEN_H - 10, xs[i], SCREEN_H - 160);
        TEST_ASSERT_TRUE_MESSAGE(gesture_is_home_swipe(&in, SCREEN_H),
            "a swipe from the bottom edge must work at any x");
    }
}

void test_a_diagonal_swipe_still_counts(void)
{
    /* Fingers do not travel in straight lines; only vertical travel matters. */
    input_t in = dragging(100, SCREEN_H - 20, 260, SCREEN_H - 170);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, SCREEN_H));
}

/* --- what should not ---------------------------------------------------- */

void test_a_swipe_starting_mid_screen_does_not_trigger(void)
{
    /* Otherwise any upward drag inside an app would close it. */
    input_t in = dragging(180, SCREEN_H / 2, 180, SCREEN_H / 2 - 200);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, SCREEN_H));
}

void test_a_short_swipe_does_not_trigger(void)
{
    /* Guards against a tap near the bottom edge wobbling a few pixels. */
    input_t in = dragging(180, SCREEN_H - 10, 180, SCREEN_H - 40);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, SCREEN_H));
}

void test_a_downward_swipe_does_not_trigger(void)
{
    input_t in = dragging(180, SCREEN_H - 200, 180, SCREEN_H - 20);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, SCREEN_H));
}

void test_a_lifted_finger_does_not_trigger(void)
{
    /* The gesture fires mid-swipe, so it must require contact - otherwise the
     * stale coordinates left behind after a lift would re-trigger it. */
    input_t in = dragging(180, SCREEN_H - 20, 180, SCREEN_H - 170);
    in.down = false;
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, SCREEN_H));
}

void test_a_stationary_press_at_the_bottom_does_not_trigger(void)
{
    input_t in = dragging(180, SCREEN_H - 10, 180, SCREEN_H - 10);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, SCREEN_H));
}

/* --- boundaries --------------------------------------------------------- */

void test_the_start_zone_boundary_is_inclusive(void)
{
    const int edge = SCREEN_H - GESTURE_HOME_ZONE_HEIGHT;

    input_t just_inside = dragging(180, edge, 180, edge - GESTURE_HOME_SWIPE_DIST);
    TEST_ASSERT_TRUE_MESSAGE(gesture_is_home_swipe(&just_inside, SCREEN_H),
        "starting exactly at the zone boundary should count");

    input_t just_outside = dragging(180, edge - 1, 180, edge - 1 - GESTURE_HOME_SWIPE_DIST);
    TEST_ASSERT_FALSE_MESSAGE(gesture_is_home_swipe(&just_outside, SCREEN_H),
        "starting one pixel above the zone should not");
}

void test_the_distance_threshold_is_inclusive(void)
{
    const int start = SCREEN_H - 10;

    input_t exactly = dragging(180, start, 180, start - GESTURE_HOME_SWIPE_DIST);
    TEST_ASSERT_TRUE_MESSAGE(gesture_is_home_swipe(&exactly, SCREEN_H),
        "travelling exactly the threshold should count");

    input_t one_short = dragging(180, start, 180, start - GESTURE_HOME_SWIPE_DIST + 1);
    TEST_ASSERT_FALSE_MESSAGE(gesture_is_home_swipe(&one_short, SCREEN_H),
        "one pixel short should not");
}

/* --- suite ------------------------------------------------------------- */

void run_gesture_suite(void)
{

    RUN_TEST(test_swipe_up_from_the_bottom_edge_triggers);
    RUN_TEST(test_it_triggers_anywhere_along_the_bottom_edge);
    RUN_TEST(test_a_diagonal_swipe_still_counts);

    RUN_TEST(test_a_swipe_starting_mid_screen_does_not_trigger);
    RUN_TEST(test_a_short_swipe_does_not_trigger);
    RUN_TEST(test_a_downward_swipe_does_not_trigger);
    RUN_TEST(test_a_lifted_finger_does_not_trigger);
    RUN_TEST(test_a_stationary_press_at_the_bottom_does_not_trigger);

    RUN_TEST(test_the_start_zone_boundary_is_inclusive);
    RUN_TEST(test_the_distance_threshold_is_inclusive);

}
