/*=============================================================================
 * Portable suite: util/fixed.h - shift-based fixed-point arithmetic.
 *
 * The whole reason this header exists is a widening cast (int64_t) that must
 * never be dropped, and a floor-vs-round distinction that must never be
 * confused for the other - see fixed.h's own top comment. Both are easy to
 * get right by accident on a small hand-checked example and wrong on the
 * values that actually occur (a 32-bit product, a negative accumulator that
 * is not an exact multiple of the shift), so this suite is built around
 * exactly those two failure shapes rather than around "does multiplication
 * work".
 *===========================================================================*/

#include <stdint.h>

#include "unity.h"
#include "suites.h"

#include "util/fixed.h"

/*---------------------------------------------------------------------------
 * fx_mul_floor() against a hand-written ((int64_t)a * b) >> shift
 *-------------------------------------------------------------------------*/

static void test_mul_floor_matches_a_hand_written_widened_shift(void)
{
    static const struct {
        int32_t a, b;
        int     shift;
        const char *msg;
    } cases[] = {
        { 256,   256,   8, "positive, exact multiple" },
        { -256,  256,   8, "negative product, exact multiple" },
        { -1,    1,     8, "negative product, NOT an exact multiple" },
        { -300,  1,     8, "negative product, remainder near the boundary" },
        { 3,     5,     8, "small positive, not an exact multiple" },
        { -7,    -9,    8, "negative times negative (positive product)" },
        { 65536, 65536, 16, "Q16.16 identity multiply" },
        { -65536, 65536, 16, "Q16.16 negated identity multiply" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int64_t expect = ((int64_t)cases[i].a * (int64_t)cases[i].b) >>
                                cases[i].shift;
        const int32_t got = fx_mul_floor(cases[i].a, cases[i].b, cases[i].shift);
        TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)expect, got, cases[i].msg);
    }
}

/*---------------------------------------------------------------------------
 * floor vs round: the divergence that would change sand's physics
 *
 * mom_x_q8 in sand.c is signed and routinely negative. Its decay uses
 * fx_mul_floor(), matching a plain `>> 8` on a negative accumulator - which
 * FLOORS toward negative infinity, not toward zero. If a future reader swaps
 * that for fx_mul_round() because "round" sounds more correct, this is the
 * exact shape of value that would silently change: a negative product that
 * is not an exact multiple of the shift.
 *-------------------------------------------------------------------------*/

static void test_floor_and_round_diverge_on_an_inexact_negative_product(void)
{
    /* a * b = -1, shift 8: -1 is not a multiple of 256.
     * floor(-1 / 256)          = -1 (rounds DOWN, away from the origin)
     * round(-1 / 256), nearest =  0 (-1/256 is much closer to 0 than to -1) */
    TEST_ASSERT_EQUAL_INT32_MESSAGE(-1, fx_mul_floor(-1, 1, 8),
        "flooring a negative, inexact product must round toward -infinity");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, fx_mul_round(-1, 1, 8),
        "rounding the same product to nearest must land on 0, not -1 - this "
        "is the divergence that would change sand's momentum decay if "
        "fx_mul_round() were ever substituted for fx_mul_floor() there");

    /* A second case with a larger magnitude, so the divergence is not an
     * artifact of the smallest possible product. a * b = -300, shift 8:
     * floor(-300 / 256) = -2, round(-300 / 256) = -1. */
    TEST_ASSERT_EQUAL_INT32_MESSAGE(-2, fx_mul_floor(-300, 1, 8), "-300 >> 8 floors to -2");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(-1, fx_mul_round(-300, 1, 8),
        "-300/256 = -1.171875, nearest is -1");
}

/*---------------------------------------------------------------------------
 * fx_mul_round(): nearest for positives, and ui_fp_round()'s own tie rule
 * (ties away from zero) for negatives
 *-------------------------------------------------------------------------*/

static void test_mul_round_rounds_to_nearest_for_positives(void)
{
    /* 1 * 1, shift 1: exactly 0.5, ties away from zero -> 1. */
    TEST_ASSERT_EQUAL_INT32(1, fx_mul_round(1, 1, 1));
    /* 3 * 1, shift 1: 1.5, rounds up to 2. */
    TEST_ASSERT_EQUAL_INT32(2, fx_mul_round(3, 1, 1));
    /* 5 * 1, shift 2: 1.25, rounds down to 1. */
    TEST_ASSERT_EQUAL_INT32(1, fx_mul_round(5, 1, 2));
    /* 7 * 1, shift 2: 1.75, rounds up to 2. */
    TEST_ASSERT_EQUAL_INT32(2, fx_mul_round(7, 1, 2));
}

static void test_mul_round_ties_away_from_zero_for_negatives(void)
{
    /* -1 * 1, shift 1: exactly -0.5. Away-from-zero ties round to -1, not 0 -
     * the same rule ui_fp_round() has always used (splitting on sign rather
     * than a plain arithmetic shift, which would instead floor a negative
     * tie toward -infinity and bias every negative half-pixel the same way). */
    TEST_ASSERT_EQUAL_INT32(-1, fx_mul_round(-1, 1, 1));
    /* -3 * 1, shift 1: -1.5, away from zero -> -2. */
    TEST_ASSERT_EQUAL_INT32(-2, fx_mul_round(-3, 1, 1));
    /* -5 * 1, shift 2: -1.25, nearest is -1. */
    TEST_ASSERT_EQUAL_INT32(-1, fx_mul_round(-5, 1, 2));
    /* -7 * 1, shift 2: -1.75, nearest is -2. */
    TEST_ASSERT_EQUAL_INT32(-2, fx_mul_round(-7, 1, 2));
}

/*---------------------------------------------------------------------------
 * Exact multiples: floor and round must agree when there is nothing to
 * round or floor away
 *-------------------------------------------------------------------------*/

static void test_floor_and_round_agree_on_exact_multiples(void)
{
    static const struct { int32_t a, b; int shift; int32_t expect; } cases[] = {
        { 256,  2, 8, 2 },
        { -256, 2, 8, -2 },
        { 0,    1, 8, 0 },
        { 65536, 3, 16, 3 },
        { -65536, 3, 16, -3 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int32_t f = fx_mul_floor(cases[i].a, cases[i].b, cases[i].shift);
        const int32_t r = fx_mul_round(cases[i].a, cases[i].b, cases[i].shift);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(cases[i].expect, f,
            "an exact multiple has nothing to floor away");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(cases[i].expect, r,
            "an exact multiple has nothing to round away");
    }
}

/*---------------------------------------------------------------------------
 * The widening cast: a 32-bit product that would overflow int32_t must
 * still give the correct answer
 *-------------------------------------------------------------------------*/

static void test_mul_floor_survives_a_32_bit_overflowing_product(void)
{
    /* 46341 * 46341 = 2,147,488,281, which is already past INT32_MAX
     * (2,147,483,647) - a plain `int32_t * int32_t` here would overflow
     * before any shift ever ran. The final answer, after shifting right by
     * 8, comfortably fits back in int32_t (8,388,626), which is exactly the
     * shape of bug the (int64_t) widening in fx_mul_floor() exists to
     * prevent: an intermediate that cannot fit in 32 bits even though both
     * the inputs and the final result do. */
    const int64_t product = (int64_t)46341 * (int64_t)46341;
    /* Plain TEST_ASSERT_TRUE rather than a *_INT64 comparison macro: the
     * device build runs with CONFIG_UNITY_ENABLE_64BIT off (see sdkconfig),
     * where Unity's 64-bit-typed assertions are compiled-out stubs that fail
     * unconditionally. A boolean expression has no such restriction - only
     * the numeric-comparison macros need 64-bit formatting support. */
    TEST_ASSERT_TRUE_MESSAGE(product > (int64_t)INT32_MAX,
        "the test itself is only meaningful if this product overflows int32_t");

    const int32_t expect = (int32_t)(product >> 8);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(8388626, expect,
        "sanity-check the hand-computed expectation before trusting it");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(expect, fx_mul_floor(46341, 46341, 8),
        "fx_mul_floor must widen to int64_t internally, or this overflows "
        "silently and returns garbage");
}

static void test_mul_round_survives_a_32_bit_overflowing_product(void)
{
    /* Same overflowing product, run through the rounding path instead. */
    const int64_t product = (int64_t)46341 * (int64_t)46341;
    const int64_t half     = (int64_t)1 << 7;
    const int32_t expect   = (int32_t)((product + half) >> 8);
    TEST_ASSERT_EQUAL_INT32(expect, fx_mul_round(46341, 46341, 8));
}

/*---------------------------------------------------------------------------
 * Both shifts the tree actually uses: Q8 (sand, tilt) and Q16.16 (the UI
 * transform) are the same helper, parameterised
 *-------------------------------------------------------------------------*/

static void test_the_same_helpers_serve_both_shift_8_and_shift_16(void)
{
    /* Q8 "one times one is one", as sand's fixed point would need. */
    TEST_ASSERT_EQUAL_INT32(256, fx_mul_floor(256, 256, 8));
    TEST_ASSERT_EQUAL_INT32(256, fx_mul_round(256, 256, 8));

    /* Q16.16 "one times one is one", as ui_transform.h's UI_FP_ONE would
     * need - the same function, just given a different shift. */
    TEST_ASSERT_EQUAL_INT32(65536, fx_mul_floor(65536, 65536, 16));
    TEST_ASSERT_EQUAL_INT32(65536, fx_mul_round(65536, 65536, 16));

    /* And fx_div_round() at both scales too - `x` divided by itself is
     * exactly one, at either shift. */
    TEST_ASSERT_EQUAL_INT32(256, fx_div_round(100, 100, 8));
    TEST_ASSERT_EQUAL_INT32(65536, fx_div_round(100, 100, 16));
}

void suite_fixed(void)
{
    RUN_TEST(test_mul_floor_matches_a_hand_written_widened_shift);
    RUN_TEST(test_floor_and_round_diverge_on_an_inexact_negative_product);
    RUN_TEST(test_mul_round_rounds_to_nearest_for_positives);
    RUN_TEST(test_mul_round_ties_away_from_zero_for_negatives);
    RUN_TEST(test_floor_and_round_agree_on_exact_multiples);
    RUN_TEST(test_mul_floor_survives_a_32_bit_overflowing_product);
    RUN_TEST(test_mul_round_survives_a_32_bit_overflowing_product);
    RUN_TEST(test_the_same_helpers_serve_both_shift_8_and_shift_16);
}

SUITE_REGISTER(suite_fixed);
