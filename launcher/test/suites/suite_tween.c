/*=============================================================================
 * Portable suite: util/tween.h - timeline ramps, easing and lerps.
 *
 * These moved here from boot_anim.h, which had its own private copies and
 * its own tests for them under boot_anim-specific names. The tests moved
 * too, generalised away from anything boot_anim-shaped (a millisecond of
 * the boot animation, a letter's flight) into checks about the primitives
 * themselves - see suite_boot_anim.c's own history for the originals this
 * suite is closest to.
 *===========================================================================*/

#include <stdint.h>

#include "unity.h"
#include "suites.h"

#include "util/tween.h"

/*---------------------------------------------------------------------------
 * tween_ramp()
 *-------------------------------------------------------------------------*/

static void test_a_ramp_is_flat_before_and_after(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, tween_ramp(0, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(0, tween_ramp(100, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ramp(300, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ramp(9999, 100, 200));
}

static void test_a_ramp_never_goes_backwards(void)
{
    uint8_t last = 0;
    for (uint32_t t = 0; t <= 400; t += 5) {
        const uint8_t v = tween_ramp(t, 100, 200);
        TEST_ASSERT_TRUE_MESSAGE(v >= last,
            "a ramp must never step backwards as time moves forward");
        last = v;
    }
}

static void test_a_zero_duration_ramp_jumps_straight_to_the_end(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, tween_ramp(100, 100, 0));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ramp(101, 100, 0));
}

/*---------------------------------------------------------------------------
 * tween_ease_out()
 *-------------------------------------------------------------------------*/

static void test_the_ease_keeps_its_endpoints_and_leads_in_the_middle(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, tween_ease_out(0));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ease_out(255));
    TEST_ASSERT_TRUE_MESSAGE(tween_ease_out(128) > 128,
        "an ease-out is ahead of linear part way through, not behind it");
}

static void test_the_ease_never_goes_backwards(void)
{
    uint8_t last = 0;
    for (int v = 0; v <= 255; v++) {
        const uint8_t eased = tween_ease_out((uint8_t)v);
        TEST_ASSERT_TRUE_MESSAGE(eased >= last,
            "easing a monotonic input must stay monotonic");
        last = eased;
    }
}

/*---------------------------------------------------------------------------
 * tween_lerp_i32()
 *-------------------------------------------------------------------------*/

static void test_lerp_hits_its_endpoints_exactly(void)
{
    TEST_ASSERT_EQUAL_INT32(1000, tween_lerp_i32(1000, 5000, 0));
    TEST_ASSERT_EQUAL_INT32(5000, tween_lerp_i32(1000, 5000, 255));
}

static void test_lerp_is_between_its_endpoints_throughout(void)
{
    for (int u = 0; u <= 255; u++) {
        const int32_t v = tween_lerp_i32(1000, 5000, (uint8_t)u);
        TEST_ASSERT_TRUE_MESSAGE(v >= 1000 && v <= 5000,
            "a lerp must never overshoot either endpoint");
    }
}

static void test_lerp_works_the_same_shrinking_as_growing(void)
{
    /* b < a: the span is negative, and the lerp should still walk from a
     * down to b rather than assuming growth. */
    TEST_ASSERT_EQUAL_INT32(5000, tween_lerp_i32(5000, 1000, 0));
    TEST_ASSERT_EQUAL_INT32(1000, tween_lerp_i32(5000, 1000, 255));

    int32_t last = 6000;
    for (int u = 0; u <= 255; u++) {
        const int32_t v = tween_lerp_i32(5000, 1000, (uint8_t)u);
        TEST_ASSERT_TRUE_MESSAGE(v <= last,
            "lerping toward a smaller value must decrease monotonically");
        last = v;
    }
}

static void test_lerp_handles_a_span_too_wide_for_int32_untouched(void)
{
    /* (b - a) * 255 alone would already be close to overflowing int32_t for
     * a span in the tens of millions - not a shape any call site here
     * actually needs, but the (int64_t) widening exists specifically so a
     * future one does not have to rediscover that the hard way. */
    const int32_t a = -1000000000;
    const int32_t b  = 1000000000;
    TEST_ASSERT_EQUAL_INT32(a, tween_lerp_i32(a, b, 0));
    TEST_ASSERT_EQUAL_INT32(b, tween_lerp_i32(a, b, 255));
    const int32_t mid = tween_lerp_i32(a, b, 128);
    TEST_ASSERT_TRUE_MESSAGE(mid > a && mid < b,
        "a wide span must still land strictly between its endpoints, not "
        "wrap around from an overflowing intermediate");
}

void suite_tween(void)
{
    RUN_TEST(test_a_ramp_is_flat_before_and_after);
    RUN_TEST(test_a_ramp_never_goes_backwards);
    RUN_TEST(test_a_zero_duration_ramp_jumps_straight_to_the_end);

    RUN_TEST(test_the_ease_keeps_its_endpoints_and_leads_in_the_middle);
    RUN_TEST(test_the_ease_never_goes_backwards);

    RUN_TEST(test_lerp_hits_its_endpoints_exactly);
    RUN_TEST(test_lerp_is_between_its_endpoints_throughout);
    RUN_TEST(test_lerp_works_the_same_shrinking_as_growing);
    RUN_TEST(test_lerp_handles_a_span_too_wide_for_int32_untouched);
}

SUITE_REGISTER(suite_tween);
