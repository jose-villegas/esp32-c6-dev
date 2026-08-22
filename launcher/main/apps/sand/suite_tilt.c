/*=============================================================================
 * Portable suite: the tilt filter.
 *
 * Everything here would be miserable to test on hardware - "does a step change
 * arrive in about a quarter of a second" needs a controllable clock, and
 * "is the smoothing framerate-independent" needs two framerates at once. Both
 * are trivial when time is a parameter.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "tilt.h"

/* Roughly 1 g in the units the QMI8658 reports, which is what the filter sees
 * in the app. The exact value does not matter - the filter is unit-agnostic -
 * but using the real magnitude keeps the tolerances meaningful. */
#define ONE_G 4096

static tilt_t t;

static void fixture(void)
{
    tilt_reset(&t);
}

/* Feed a constant reading for `ms` milliseconds in `dt_ms` slices. */
static void hold(int gx, int gy, int shake, uint32_t dt_ms, uint32_t ms)
{
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += dt_ms) {
        tilt_update(&t, gx, gy, shake, dt_ms);
    }
}

static void test_the_first_sample_is_adopted_exactly(void)
{
    fixture();

    tilt_update(&t, 1000, ONE_G, 0, 14);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1000, tilt_x(&t),
        "the filter must start at the first reading, not ramp up from zero - "
        "otherwise the sand visibly swings into place when the app opens");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ONE_G, tilt_y(&t), "likewise for y");
}

static void test_a_change_is_approached_gradually(void)
{
    fixture();
    tilt_update(&t, 0, ONE_G, 0, 14);

    /* Gravity swings a quarter turn in one frame, as if the board were
     * snapped sideways. */
    tilt_update(&t, ONE_G, 0, 0, 14);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, tilt_x(&t),
        "the filter must move toward the new reading");
    TEST_ASSERT_LESS_THAN_MESSAGE(ONE_G / 4, tilt_x(&t),
        "but nowhere near reach it in a single frame - jumping is exactly the "
        "rigidity this filter exists to remove");
}

static void test_it_converges_when_the_reading_is_held(void)
{
    fixture();
    tilt_update(&t, 0, ONE_G, 0, 14);

    hold(ONE_G, 0, 0, 14, 2000);

    TEST_ASSERT_INT_WITHIN_MESSAGE(ONE_G / 50, ONE_G, tilt_x(&t),
        "a held reading must eventually be reached, or the sand would never "
        "quite point where the board does");
    TEST_ASSERT_INT_WITHIN_MESSAGE(ONE_G / 50, 0, tilt_y(&t), "likewise for y");
}

static void test_smoothing_is_independent_of_framerate(void)
{
    /* The same elapsed time at 70 fps and at 25 fps must land in the same
     * place. A naive "move 10% per frame" filter fails this badly, and this
     * project's framerate has already moved from 25 to 70. */
    tilt_t fast, slow;

    tilt_reset(&fast);
    tilt_update(&fast, 0, ONE_G, 0, 14);
    for (int i = 0; i < 21; i++) {          /* 21 * 14 ms = 294 ms */
        tilt_update(&fast, ONE_G, 0, 0, 14);
    }

    tilt_reset(&slow);
    tilt_update(&slow, 0, ONE_G, 0, 40);
    for (int i = 0; i < 7; i++) {           /* 7 * 42 ms = 294 ms */
        tilt_update(&slow, ONE_G, 0, 0, 42);
    }

    TEST_ASSERT_INT_WITHIN_MESSAGE(ONE_G / 12, tilt_x(&fast), tilt_x(&slow),
        "equal elapsed time must give an equal result at any framerate");
}

static void test_shaking_makes_it_track_faster(void)
{
    tilt_t still, moving;

    tilt_reset(&still);
    tilt_update(&still, 0, ONE_G, 0, 14);
    hold(ONE_G, 0, 0, 14, 140);

    tilt_reset(&moving);
    tilt_update(&moving, 0, ONE_G, 255, 14);
    for (int i = 0; i < 10; i++) {
        tilt_update(&moving, ONE_G, 0, 255, 14);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(tilt_x(&still), tilt_x(&moving),
        "the gyro says the board is genuinely moving, so the filter must stop "
        "smoothing and start tracking - that is the whole point of using it");
}

static void test_noise_is_attenuated_when_the_board_is_still(void)
{
    fixture();
    tilt_update(&t, 0, ONE_G, 0, 14);

    /* A still board still reports a few hundred counts of noise. Alternating
     * it is the worst case for a smoother. */
    int worst = 0;
    for (int i = 0; i < 60; i++) {
        const int noise = (i & 1) ? 300 : -300;
        tilt_update(&t, noise, ONE_G, 0, 14);
        const int magnitude = tilt_x(&t) < 0 ? -tilt_x(&t) : tilt_x(&t);
        if (magnitude > worst) {
            worst = magnitude;
        }
    }

    TEST_ASSERT_LESS_THAN_MESSAGE(60, worst,
        "noise of +/-300 must come out several times smaller, or a board "
        "sitting on a desk will have visibly fidgeting sand");
}

static void test_a_stalled_frame_does_not_teleport_the_filter(void)
{
    fixture();
    tilt_update(&t, 0, ONE_G, 0, 14);

    /* A two-second stall - a long flash, or a breakpoint. Without a clamp the
     * filter would jump straight to the new reading. */
    tilt_update(&t, ONE_G, 0, 0, 2000);

    TEST_ASSERT_LESS_THAN_MESSAGE(ONE_G * 2 / 3, tilt_x(&t),
        "a long stall must not be allowed to snap the filter to the newest "
        "reading");
}

static void test_zero_elapsed_time_changes_nothing(void)
{
    fixture();
    tilt_update(&t, 0, ONE_G, 0, 14);
    const int before = tilt_y(&t);

    tilt_update(&t, ONE_G, 0, 0, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, tilt_y(&t),
        "no time passed, so nothing may be integrated - and nothing may be "
        "divided by zero either");
}

void run_tilt_suite(void)
{
    RUN_TEST(test_the_first_sample_is_adopted_exactly);
    RUN_TEST(test_a_change_is_approached_gradually);
    RUN_TEST(test_it_converges_when_the_reading_is_held);
    RUN_TEST(test_smoothing_is_independent_of_framerate);
    RUN_TEST(test_shaking_makes_it_track_faster);
    RUN_TEST(test_noise_is_attenuated_when_the_board_is_still);
    RUN_TEST(test_a_stalled_frame_does_not_teleport_the_filter);
    RUN_TEST(test_zero_elapsed_time_changes_nothing);
}

SUITE_REGISTER(run_tilt_suite);
