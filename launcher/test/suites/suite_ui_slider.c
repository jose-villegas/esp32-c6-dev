/*=============================================================================
 * Portable suite: ui_slider - the geometry behind ui_slider_int(), the knob
 * you can drag rather than only tap-to-jump (see ui_pointer.h, Phase 1,
 * which is what makes a drag possible at all).
 *
 * Header-only, like ui_style.h, and tested the same way: nobody can eyeball
 * a knob rect landing one pixel off where a finger touched, so the geometry
 * is checked directly rather than by looking at a screenshot.
 *===========================================================================*/

#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "ui/ui_slider.h"

typedef struct {
    mu_Rect track;
    int     knob_w;
    int     lo, hi, step;
} slider_fixture_t;

static slider_fixture_t fixture(void)
{
    return (slider_fixture_t){
        .track  = (mu_Rect){ 40, 100, 200, 24 },
        .knob_w = 40,
        .lo = 1, .hi = 64, .step = 1,
    };
}

/*-----------------------------------------------------------------------------
 * The round trip - the property that stops a knob drifting a step every
 * time a finger touches it without moving.
 *---------------------------------------------------------------------------*/

static void test_round_trip_holds_for_every_value_in_range(void)
{
    const slider_fixture_t f = fixture();

    for (int v = f.lo; v <= f.hi; v++) {
        const int cx = ui_slider_knob_center_x(f.track, f.lo, f.hi, v, f.knob_w);
        const int back = ui_slider_value_at_x(f.track, f.lo, f.hi, f.knob_w, f.step, cx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(v, back,
            "value -> knob centre -> value must round-trip exactly, or a finger "
            "resting on the knob without moving would drift its value");
    }
}

static void test_round_trip_holds_at_a_coarser_step(void)
{
    const slider_fixture_t f = fixture();
    const int lo = 0, hi = 100, step = 5;

    for (int v = lo; v <= hi; v += step) {
        const int cx = ui_slider_knob_center_x(f.track, lo, hi, v, f.knob_w);
        const int back = ui_slider_value_at_x(f.track, lo, hi, f.knob_w, step, cx);
        TEST_ASSERT_EQUAL_INT(v, back);
    }
}

/* What "the knob follows your thumb" means precisely: of every value the
 * slider can hold, the one it picks puts its knob centre nearest the touch.
 * Asserted against all the candidates rather than a tolerance in pixels, so
 * it stays true whatever the track width and range are. */
static void test_a_touch_picks_the_value_whose_knob_centres_nearest_it(void)
{
    const slider_fixture_t f = fixture();

    for (int x = f.track.x - 20; x <= f.track.x + f.track.w + 20; x++) {
        const int v = ui_slider_value_at_x(f.track, f.lo, f.hi, f.knob_w, f.step, x);
        const int dv = abs(ui_slider_knob_center_x(f.track, f.lo, f.hi, v, f.knob_w) - x);

        for (int u = f.lo; u <= f.hi; u++) {
            const int du = abs(ui_slider_knob_center_x(f.track, f.lo, f.hi, u, f.knob_w) - x);
            TEST_ASSERT_TRUE_MESSAGE(dv <= du,
                "the knob must land under the finger, not half its width to "
                "one side of it");
        }
    }
}

/*-----------------------------------------------------------------------------
 * The knob never leaves the track.
 *---------------------------------------------------------------------------*/

static void test_knob_sits_flush_inside_the_track_at_lo_and_hi(void)
{
    const slider_fixture_t f = fixture();

    const mu_Rect at_lo = ui_slider_knob_rect(f.track, f.lo, f.hi, f.lo, f.knob_w);
    const mu_Rect at_hi = ui_slider_knob_rect(f.track, f.lo, f.hi, f.hi, f.knob_w);

    TEST_ASSERT_EQUAL_INT_MESSAGE(f.track.x, at_lo.x,
        "at the minimum the knob must sit flush against the track's start");
    TEST_ASSERT_EQUAL_INT_MESSAGE(f.track.x + f.track.w, at_hi.x + at_hi.w,
        "at the maximum the knob must sit flush against the track's end, "
        "not run past it or stop short");
}

static void test_knob_stays_inside_the_track_everywhere_between(void)
{
    const slider_fixture_t f = fixture();

    for (int v = f.lo; v <= f.hi; v++) {
        const mu_Rect k = ui_slider_knob_rect(f.track, f.lo, f.hi, v, f.knob_w);
        TEST_ASSERT_TRUE_MESSAGE(k.x >= f.track.x,
            "the knob must never start left of the track");
        TEST_ASSERT_TRUE_MESSAGE(k.x + k.w <= f.track.x + f.track.w,
            "the knob must never run past the track's right edge - its "
            "travel is track.w - knob_w, not track.w");
    }
}

/*-----------------------------------------------------------------------------
 * Quantization and clamping of a touch.
 *---------------------------------------------------------------------------*/

static void test_a_touch_quantizes_to_the_nearest_step(void)
{
    const slider_fixture_t f = fixture();
    const int lo = 0, hi = 100, step = 10;

    for (int x = f.track.x; x <= f.track.x + f.track.w; x++) {
        const int v = ui_slider_value_at_x(f.track, lo, hi, f.knob_w, step, x);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (v - lo) % step,
            "every value a touch produces must land exactly on a step "
            "multiple from lo");
    }
}

static void test_a_touch_beyond_either_end_clamps_not_wraps(void)
{
    const slider_fixture_t f = fixture();

    TEST_ASSERT_EQUAL_INT_MESSAGE(f.lo,
        ui_slider_value_at_x(f.track, f.lo, f.hi, f.knob_w, f.step, -5000),
        "a touch far to the left must clamp to lo, not wrap around");
    TEST_ASSERT_EQUAL_INT_MESSAGE(f.hi,
        ui_slider_value_at_x(f.track, f.lo, f.hi, f.knob_w, f.step, 5000),
        "a touch far to the right must clamp to hi, not wrap around");
}

/*-----------------------------------------------------------------------------
 * The filled portion.
 *---------------------------------------------------------------------------*/

static void test_fill_width_never_exceeds_the_track(void)
{
    const slider_fixture_t f = fixture();

    for (int v = f.lo; v <= f.hi; v++) {
        const mu_Rect fill = ui_slider_fill_rect(f.track, f.lo, f.hi, v, f.knob_w);
        TEST_ASSERT_TRUE_MESSAGE(fill.w >= 0 && fill.w <= f.track.w,
            "the fill must never run past the track it belongs to");
    }
}

static void test_fill_width_grows_with_value(void)
{
    const slider_fixture_t f = fixture();

    const mu_Rect low  = ui_slider_fill_rect(f.track, f.lo, f.hi, f.lo, f.knob_w);
    const mu_Rect mid  = ui_slider_fill_rect(f.track, f.lo, f.hi, (f.lo + f.hi) / 2, f.knob_w);
    const mu_Rect high = ui_slider_fill_rect(f.track, f.lo, f.hi, f.hi, f.knob_w);

    TEST_ASSERT_TRUE_MESSAGE(low.w <= mid.w && mid.w <= high.w,
        "a higher value must never fill less of the track than a lower one");
}

/*-----------------------------------------------------------------------------
 * Degenerate cases - none of these may divide by zero or hand back a knob
 * wider than its track.
 *---------------------------------------------------------------------------*/

static void test_lo_equals_hi_returns_lo_without_dividing_by_zero(void)
{
    const slider_fixture_t f = fixture();

    const mu_Rect k = ui_slider_knob_rect(f.track, 5, 5, 5, f.knob_w);
    TEST_ASSERT_EQUAL_INT(f.track.x, k.x);
    TEST_ASSERT_EQUAL_INT_MESSAGE(5,
        ui_slider_value_at_x(f.track, 5, 5, f.knob_w, f.step, f.track.x + 137),
        "with no range at all, every touch must resolve to the one value there is");
}

static void test_a_track_narrower_than_the_knob_shrinks_the_knob_to_fit(void)
{
    const mu_Rect narrow = { 0, 0, 10, 24 };

    const mu_Rect k = ui_slider_knob_rect(narrow, 0, 100, 50, 40);
    TEST_ASSERT_TRUE_MESSAGE(k.w <= narrow.w,
        "a knob must never be drawn wider than the track it lives on");
}

static void test_step_of_zero_falls_back_rather_than_dividing_by_zero(void)
{
    const slider_fixture_t f = fixture();

    /* Only asserting it returns something in range - step 0 has no
     * meaningful "correct" quantization, only a safe one. */
    const int v = ui_slider_value_at_x(f.track, f.lo, f.hi, f.knob_w, 0,
                                       f.track.x + f.track.w / 2);
    TEST_ASSERT_TRUE(v >= f.lo && v <= f.hi);
}

static void test_step_larger_than_the_whole_range_still_clamps(void)
{
    const slider_fixture_t f = fixture();

    const int v = ui_slider_value_at_x(f.track, 0, 10, f.knob_w, 1000,
                                       f.track.x + f.track.w);
    TEST_ASSERT_TRUE_MESSAGE(v == 0 || v == 10,
        "a step bigger than the whole range leaves only its two ends "
        "reachable");
}

static void test_zero_width_track_produces_no_oversized_or_negative_rect(void)
{
    const mu_Rect zero = { 0, 0, 0, 24 };

    const mu_Rect k = ui_slider_knob_rect(zero, 0, 100, 50, 40);
    TEST_ASSERT_TRUE(k.w >= 0 && k.w <= zero.w);

    const mu_Rect fill = ui_slider_fill_rect(zero, 0, 100, 50, 40);
    TEST_ASSERT_TRUE(fill.w >= 0 && fill.w <= zero.w);
}

void run_ui_slider_suite(void)
{
    RUN_TEST(test_round_trip_holds_for_every_value_in_range);
    RUN_TEST(test_round_trip_holds_at_a_coarser_step);
    RUN_TEST(test_a_touch_picks_the_value_whose_knob_centres_nearest_it);
    RUN_TEST(test_knob_sits_flush_inside_the_track_at_lo_and_hi);
    RUN_TEST(test_knob_stays_inside_the_track_everywhere_between);
    RUN_TEST(test_a_touch_quantizes_to_the_nearest_step);
    RUN_TEST(test_a_touch_beyond_either_end_clamps_not_wraps);
    RUN_TEST(test_fill_width_never_exceeds_the_track);
    RUN_TEST(test_fill_width_grows_with_value);
    RUN_TEST(test_lo_equals_hi_returns_lo_without_dividing_by_zero);
    RUN_TEST(test_a_track_narrower_than_the_knob_shrinks_the_knob_to_fit);
    RUN_TEST(test_step_of_zero_falls_back_rather_than_dividing_by_zero);
    RUN_TEST(test_step_larger_than_the_whole_range_still_clamps);
    RUN_TEST(test_zero_width_track_produces_no_oversized_or_negative_rect);
}

SUITE_REGISTER(run_ui_slider_suite);
