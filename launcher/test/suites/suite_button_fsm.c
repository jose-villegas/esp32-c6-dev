/*=============================================================================
 * Portable suite: button debouncing.
 *
 * Every one of these would need a stopwatch and a very steady hand on real
 * hardware. Passing time in as a parameter makes them instant.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "button_fsm.h"

#define MS 1000   /* microseconds per millisecond */

static button_fsm_t b;
static int64_t now;

static void fixture(void)
{
    button_fsm_reset(&b);
    now = 1000000;   /* arbitrary, but not zero - catches sloppy comparisons */
}

/* One sample at a moment in time - for constructing bounce sequences. */
static void advance(int64_t ms, bool level)
{
    now += ms * MS;
    button_fsm_update(&b, level, now);
}

/* Poll continuously, the way the real task does. A single sample can only ever
 * START the debounce clock; something has to be there to see it expire. */
static void hold(int64_t ms, bool level)
{
    for (int64_t t = 0; t < ms; t += 5) {
        advance(5, level);
    }
}

static void test_a_held_press_is_reported_once(void)
{
    fixture();
    advance(0, false);

    hold(40, true);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_pressed(&b),
        "a level held past the debounce window must produce a press");

    hold(40, true);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_pressed(&b),
        "holding it must not keep producing presses - an edge is an edge");
    TEST_ASSERT_TRUE(button_fsm_is_down(&b));
}

static void test_an_edge_is_consumed_by_whoever_reads_it(void)
{
    fixture();
    advance(0, false);
    hold(40, true);

    TEST_ASSERT_TRUE(button_fsm_take_pressed(&b));
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_pressed(&b),
        "reading an edge must clear it, or two readers both act on one press");
}

static void test_contact_bounce_produces_one_press_not_several(void)
{
    fixture();
    advance(0, false);

    /* A real contact chatters for a few milliseconds on make. Every one of
     * these transitions is well inside the debounce window. */
    advance(2, true);
    advance(2, false);
    advance(2, true);
    advance(2, false);
    advance(3, true);

    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_pressed(&b),
        "nothing may be reported while the contact is still chattering");

    hold(40, true);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_pressed(&b),
        "and exactly one press once it settles");
    TEST_ASSERT_FALSE(button_fsm_take_pressed(&b));
}

static void test_a_spike_shorter_than_the_debounce_is_ignored(void)
{
    fixture();
    advance(0, false);

    /* Electrical noise, not a finger. */
    advance(5, true);
    advance(5, false);
    hold(100, false);

    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_pressed(&b),
        "a spike far shorter than a human press must not register");
    TEST_ASSERT_FALSE(button_fsm_is_down(&b));
}

static void test_release_is_debounced_too(void)
{
    fixture();
    advance(0, false);
    hold(40, true);
    (void)button_fsm_take_pressed(&b);

    /* Break bounces as well as make. */
    advance(2, false);
    advance(2, true);
    advance(2, false);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_released(&b),
        "a bouncing break must not report a release yet");

    hold(40, false);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_released(&b),
        "and exactly one release once it settles");
    TEST_ASSERT_FALSE(button_fsm_is_down(&b));
}

static void test_a_button_already_down_at_startup_does_not_fire(void)
{
    fixture();

    /* Held as the app starts - the BOOT button is easy to still be pressing
     * a moment after a reset. */
    advance(0, true);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_pressed(&b),
        "the first sample is the starting state, not an event");
    TEST_ASSERT_TRUE(button_fsm_is_down(&b));

    hold(40, false);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_released(&b),
        "letting go of it is a real release, though");
}

static void test_a_full_tap_gives_one_press_and_one_release(void)
{
    fixture();
    advance(0, false);

    hold(40, true);
    hold(80, true);
    hold(40, false);

    TEST_ASSERT_TRUE(button_fsm_take_pressed(&b));
    TEST_ASSERT_TRUE(button_fsm_take_released(&b));
    TEST_ASSERT_FALSE(button_fsm_take_pressed(&b));
    TEST_ASSERT_FALSE(button_fsm_take_released(&b));
}

void run_button_fsm_suite(void)
{
    RUN_TEST(test_a_held_press_is_reported_once);
    RUN_TEST(test_an_edge_is_consumed_by_whoever_reads_it);
    RUN_TEST(test_contact_bounce_produces_one_press_not_several);
    RUN_TEST(test_a_spike_shorter_than_the_debounce_is_ignored);
    RUN_TEST(test_release_is_debounced_too);
    RUN_TEST(test_a_button_already_down_at_startup_does_not_fire);
    RUN_TEST(test_a_full_tap_gives_one_press_and_one_release);
}

SUITE_REGISTER(run_button_fsm_suite);
