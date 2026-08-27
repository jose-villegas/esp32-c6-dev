/*=============================================================================
 * Portable suite: button debouncing.
 *
 * Every one of these would need a stopwatch and a very steady hand on real
 * hardware. Passing time in as a parameter makes them instant.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "input/button_fsm.h"

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

    /* Priming sets hold_fired = true for a button already down, so a hold
     * never phantom-fires for it (see the held tests below) - and that same
     * flag is what a real press-that-became-a-hold uses to swallow its
     * release. The two share one flag, so they share one consequence: a
     * press we never witnessed the start of now delivers no synthetic edge
     * on the way out either, matching the "no phantom press" reasoning this
     * test is named for rather than contradicting it. */
    hold(40, false);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_released(&b),
        "an untracked press's end is as synthetic as its start would have been");
    TEST_ASSERT_FALSE(button_fsm_is_down(&b));
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

/*-----------------------------------------------------------------------------
 * held: the short-press/long-press split.
 *---------------------------------------------------------------------------*/

static void test_a_short_press_gives_pressed_then_released_and_never_held(void)
{
    fixture();
    advance(0, false);

    hold(40, true);
    TEST_ASSERT_TRUE(button_fsm_take_pressed(&b));

    /* Let go well short of BUTTON_HOLD_US. */
    hold(40, false);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_released(&b),
        "a press let go well before the hold threshold must release normally");
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_held(&b),
        "a press this short must never fire held");
}

static void test_a_long_press_fires_held_once_with_no_release(void)
{
    fixture();
    advance(0, false);

    hold(40, true);
    TEST_ASSERT_TRUE(button_fsm_take_pressed(&b));

    /* Comfortably past BUTTON_HOLD_US, counted from the debounced press. */
    hold(600, true);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_held(&b),
        "a press held past BUTTON_HOLD_US must fire held");

    /* Finally let go. */
    hold(40, false);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_released(&b),
        "a press that became a hold must deliver no release edge at all - "
        "held already told the caller everything about this press");
}

static void test_held_does_not_fire_twice_while_still_down(void)
{
    fixture();
    advance(0, false);

    hold(40, true);
    (void)button_fsm_take_pressed(&b);

    hold(600, true);
    TEST_ASSERT_TRUE(button_fsm_take_held(&b));

    /* Keep holding well past the threshold; held must not repeat. */
    hold(200, true);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_held(&b),
        "held must fire exactly once per press, not once per poll while held");
}

static void test_hold_is_timed_from_the_debounced_press_not_the_first_raw_sample(void)
{
    fixture();
    advance(0, false);

    /* Bounce on make, as in the contact-bounce test above - every one of
     * these transitions is well inside the debounce window, so none of them
     * is the real, debounced press. */
    advance(2, true);
    advance(2, false);
    advance(2, true);
    advance(2, false);
    advance(3, true);

    advance(30, true);   /* now it settles - this is the debounced press */
    TEST_ASSERT_TRUE(button_fsm_take_pressed(&b));

    /* If the hold clock had started at the very first raw sample (11 ms
     * into the test) rather than the debounced edge, it would already have
     * crossed BUTTON_HOLD_US by now. It must not have. */
    advance(500, true);
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_held(&b),
        "the hold must not fire before a full BUTTON_HOLD_US after the "
        "debounced press, not the first raw sample");

    advance(150, true);
    TEST_ASSERT_TRUE_MESSAGE(button_fsm_take_held(&b),
        "and must fire once a full BUTTON_HOLD_US has elapsed from the "
        "debounced press");
}

static void test_a_button_already_down_at_startup_fires_no_pressed_or_held(void)
{
    fixture();

    /* Held as the app starts, same setup as the plain startup test above -
     * but held long enough here to also rule out a phantom `held`. */
    advance(0, true);
    TEST_ASSERT_FALSE(button_fsm_take_pressed(&b));
    TEST_ASSERT_TRUE(button_fsm_is_down(&b));

    hold(700, true);   /* comfortably past BUTTON_HOLD_US */
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_held(&b),
        "a button already down at startup must not fire a phantom hold, "
        "for the same reason it must not fire a phantom press");
}

static void test_take_held_consumes_the_edge(void)
{
    fixture();
    advance(0, false);
    hold(40, true);
    (void)button_fsm_take_pressed(&b);
    hold(600, true);

    TEST_ASSERT_TRUE(button_fsm_take_held(&b));
    TEST_ASSERT_FALSE_MESSAGE(button_fsm_take_held(&b),
        "reading held must clear it, or two readers both act on one hold");
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
    RUN_TEST(test_a_short_press_gives_pressed_then_released_and_never_held);
    RUN_TEST(test_a_long_press_fires_held_once_with_no_release);
    RUN_TEST(test_held_does_not_fire_twice_while_still_down);
    RUN_TEST(test_hold_is_timed_from_the_debounced_press_not_the_first_raw_sample);
    RUN_TEST(test_a_button_already_down_at_startup_fires_no_pressed_or_held);
    RUN_TEST(test_take_held_consumes_the_edge);
}

SUITE_REGISTER(run_button_fsm_suite);
