/*=============================================================================
 * Specification for the touch state machine.
 *
 * This is the logic that turns "the controller reported a point / reported
 * nothing" into press and release events. It is deliberately free of I2C and
 * FreeRTOS so it can be exercised here in microseconds, with time as a plain
 * argument rather than a clock we would have to wait for.
 *
 * The debounce cases below are not hypothetical: the FT5x06's INT line signals
 * "data ready" rather than "finger down" and drops briefly mid-touch, which
 * made a held finger flicker between pressed and released.
 *===========================================================================*/

#include "unity.h"
#include "touch_fsm.h"

/* Time is supplied, never read from a clock, so tests are instant and
 * deterministic. All helpers work in milliseconds for readability. */
#define MS(x) ((int64_t)(x) * 1000)

static touch_fsm_t fsm;

/* Each test starts from a clean state. Unity's setUp() is global and this
 * suite shares a binary with others, so the fixture is explicit instead. */
static void fixture(void)
{
    touch_fsm_init(&fsm);
}

/* --- pressing ----------------------------------------------------------- */

void test_starts_with_nothing_pressed(void)
{
    fixture();
    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.down);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_FALSE(in.released);
}

void test_first_contact_reports_a_press(void)
{
    fixture();
    touch_fsm_update(&fsm, true, 100, 200, MS(0));

    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.pressed);
    TEST_ASSERT_TRUE(in.down);
    TEST_ASSERT_EQUAL_INT(100, in.x);
    TEST_ASSERT_EQUAL_INT(200, in.y);
}

void test_press_is_reported_only_once(void)
{
    fixture();
    touch_fsm_update(&fsm, true, 100, 200, MS(0));

    input_t first, second;
    touch_fsm_take(&fsm, &first);
    touch_fsm_take(&fsm, &second);

    TEST_ASSERT_TRUE(first.pressed);
    TEST_ASSERT_FALSE_MESSAGE(second.pressed,
        "an edge must be consumed by the frame that reads it");
}

void test_holding_does_not_repeat_the_press(void)
{
    fixture();
    touch_fsm_update(&fsm, true, 100, 200, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, true, 100, 200, MS(10));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_TRUE_MESSAGE(in.down, "the finger is still down");
}

void test_press_position_is_where_the_touch_began(void)
{
    fixture();
    touch_fsm_update(&fsm, true, 50, 400, MS(0));
    touch_fsm_update(&fsm, true, 55, 300, MS(20));
    touch_fsm_update(&fsm, true, 60, 200, MS(40));

    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_EQUAL_INT_MESSAGE(50, in.press_x, "press_x must not follow the finger");
    TEST_ASSERT_EQUAL_INT_MESSAGE(400, in.press_y, "press_y must not follow the finger");
    TEST_ASSERT_EQUAL_INT_MESSAGE(60, in.x, "x tracks the current position");
    TEST_ASSERT_EQUAL_INT_MESSAGE(200, in.y, "y tracks the current position");
}

/* --- releasing, and the debounce that makes it reliable ----------------- */

void test_release_is_reported_after_the_quiet_period(void)
{
    fixture();
    touch_fsm_update(&fsm, true, 10, 10, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, 0, 0, MS(0) + TOUCH_RELEASE_QUIET_US + 1);
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_FALSE(in.down);
}

void test_brief_dropout_is_not_a_release(void)
{
    fixture();
    /* The exact failure that made a held finger flicker: the controller goes
     * quiet for a few milliseconds mid-touch. That must not look like a lift. */
    touch_fsm_update(&fsm, true, 10, 10, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, 0, 0, MS(10));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE_MESSAGE(in.released, "a short gap is not a release");
    TEST_ASSERT_TRUE_MESSAGE(in.down, "the finger is still considered down");
}

void test_contact_resuming_after_a_dropout_does_not_re_press(void)
{
    fixture();
    touch_fsm_update(&fsm, true, 10, 10, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, 0, 0, MS(10));   /* dropout */
    touch_fsm_update(&fsm, true, 12, 12, MS(20));  /* back again */
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE_MESSAGE(in.pressed,
        "a dropout mid-touch must not produce a second press");
    TEST_ASSERT_FALSE(in.released);
    TEST_ASSERT_TRUE(in.down);
}

void test_quiet_period_is_measured_from_the_last_contact(void)
{
    fixture();
    /* Contact keeps arriving, each time restarting the clock, so the total
     * elapsed time far exceeds the quiet period without ever releasing. */
    touch_fsm_update(&fsm, true, 10, 10, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    for (int t = 10; t <= 500; t += 10) {
        touch_fsm_update(&fsm, true, 10, 10, MS(t));
    }
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.released);
    TEST_ASSERT_TRUE(in.down);
}

void test_a_full_tap_produces_exactly_one_press_and_one_release(void)
{
    fixture();
    int presses = 0, releases = 0;
    input_t in;

    touch_fsm_update(&fsm, true, 80, 90, MS(0));
    touch_fsm_update(&fsm, true, 80, 90, MS(20));
    for (int t = 40; t <= 400; t += 10) {
        touch_fsm_update(&fsm, false, 0, 0, MS(t));
    }

    /* Drain everything the sequence produced. */
    for (int i = 0; i < 8; i++) {
        touch_fsm_take(&fsm, &in);
        if (in.pressed)  presses++;
        if (in.released) releases++;
    }

    TEST_ASSERT_EQUAL_INT(1, presses);
    TEST_ASSERT_EQUAL_INT(1, releases);
}

/* --- suite ------------------------------------------------------------- */

void run_touch_fsm_suite(void)
{

    RUN_TEST(test_starts_with_nothing_pressed);
    RUN_TEST(test_first_contact_reports_a_press);
    RUN_TEST(test_press_is_reported_only_once);
    RUN_TEST(test_holding_does_not_repeat_the_press);
    RUN_TEST(test_press_position_is_where_the_touch_began);

    RUN_TEST(test_release_is_reported_after_the_quiet_period);
    RUN_TEST(test_brief_dropout_is_not_a_release);
    RUN_TEST(test_contact_resuming_after_a_dropout_does_not_re_press);
    RUN_TEST(test_quiet_period_is_measured_from_the_last_contact);
    RUN_TEST(test_a_full_tap_produces_exactly_one_press_and_one_release);

}
