/*=============================================================================
 * Portable suite: the startup animation's maths and choreography.
 *
 * boot_anim.h is the whole of what is tested here - the tables, the curve,
 * the plane and the timeline. boot_anim.c, which turns all that into gfx
 * calls, is not: it needs a framebuffer and a panel.
 *
 * NO libm, ON PURPOSE
 *
 * The obvious way to check a sine table is against sin(), and the obvious way
 * to check ln(n) is against log() - but test/run_tests.sh links no maths
 * library, and adding one would mean this suite proving that the host's libm
 * agrees with the host's libm. Everything below is checked by an IDENTITY the
 * table itself must satisfy instead:
 *
 *     sin^2 + cos^2 == 1              catches any wrong sine entry
 *     ln(a) + ln(b) == ln(a*b)        catches any wrong logarithm entry
 *     n * (1/sqrt(n))^2 == 1          catches any wrong length entry
 *
 * which is stronger than a spot check against a library, because a single
 * mistyped digit anywhere fails several of them at once.
 *===========================================================================*/

#include <stdbool.h>
#include <stdint.h>

#include "unity.h"
#include "suites.h"

#include "boot_anim.h"

/* The panel these numbers are laid out for. Named here rather than pulled
 * from gfx.h, which needs the BSP; gfx_dirty.h mirrors the same two numbers
 * for the same reason. */
#define PANEL_W 368
#define PANEL_H 448

/*---------------------------------------------------------------------------
 * The sine table
 *-------------------------------------------------------------------------*/

static void test_the_quarter_wave_starts_at_zero_and_ends_at_one(void)
{
    TEST_ASSERT_EQUAL_INT(0, boot_anim_sin_quarter[0]);
    TEST_ASSERT_EQUAL_INT(32767, boot_anim_sin_quarter[64]);
}

static void test_the_quarter_wave_rises_all_the_way(void)
{
    for (int i = 1; i < 65; i++) {
        TEST_ASSERT_TRUE_MESSAGE(boot_anim_sin_quarter[i] >
                                 boot_anim_sin_quarter[i - 1],
            "the quarter wave must increase at every step - a dip means a "
            "transposed or mistyped entry");
    }
}

/* The identity that pins every entry down at once. Q15 squares are summed in
 * 64-bit and compared against 1.0 in Q30. */
static void test_sin_squared_plus_cos_squared_is_one(void)
{
    for (uint32_t phase = 0; phase < 65536; phase += 7) {
        const int64_t s = boot_anim_sin((uint16_t)phase);
        const int64_t c = boot_anim_cos((uint16_t)phase);
        const int64_t sum = s * s + c * c;
        const int64_t one = (int64_t)32767 * 32767;

        /* The tolerance is the interpolation error between table points, not
         * slack for a bad table: a wrong entry is out by hundreds of times
         * this. */
        TEST_ASSERT_TRUE_MESSAGE(sum > one - one / 300 && sum < one + one / 300,
            "sin^2 + cos^2 left the neighbourhood of 1");
    }
}

static void test_sine_is_odd_about_the_origin(void)
{
    for (uint32_t phase = 1; phase < 32768; phase += 13) {
        const int32_t a = boot_anim_sin((uint16_t)phase);
        const int32_t b = boot_anim_sin((uint16_t)(65536u - phase));
        TEST_ASSERT_INT_WITHIN_MESSAGE(2, a, -b,
            "sin(-x) should be -sin(x) - the quadrant reflection is wrong");
    }
}

static void test_the_quarter_points_are_exact(void)
{
    TEST_ASSERT_EQUAL_INT(0, boot_anim_sin(0));
    TEST_ASSERT_EQUAL_INT(32767, boot_anim_sin(16384));
    TEST_ASSERT_EQUAL_INT(0, boot_anim_sin(32768));
    TEST_ASSERT_EQUAL_INT(-32767, boot_anim_sin(49152));
    TEST_ASSERT_EQUAL_INT(32767, boot_anim_cos(0));
}

/*---------------------------------------------------------------------------
 * The per-term tables
 *-------------------------------------------------------------------------*/

/* ln(a) + ln(b) == ln(a*b), over every pair whose product is still in the
 * table. Nothing here computes a logarithm; the table has to be consistent
 * with itself, and only a correct one can be. */
static void test_the_log_table_adds_up(void)
{
    for (int a = 2; a <= BOOT_ANIM_TERMS; a++) {
        for (int b = a; a * b <= BOOT_ANIM_TERMS; b++) {
            const int32_t sum = boot_anim_ln_phase[a] + boot_anim_ln_phase[b];
            TEST_ASSERT_INT32_WITHIN_MESSAGE(2, boot_anim_ln_phase[a * b], sum,
                "ln(a) + ln(b) did not match ln(a*b) - one of the three "
                "entries is wrong");
        }
    }
}

static void test_the_log_table_starts_at_zero_and_climbs(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_ln_phase[1]);
    for (int n = 2; n <= BOOT_ANIM_TERMS; n++) {
        TEST_ASSERT_TRUE_MESSAGE(boot_anim_ln_phase[n] >
                                 boot_anim_ln_phase[n - 1],
            "ln is increasing; this table is not");
    }
}

/* n * (1/sqrt(n))^2 == 1, in Q12. */
static void test_the_length_table_is_a_reciprocal_square_root(void)
{
    for (int n = 1; n <= BOOT_ANIM_TERMS; n++) {
        const int64_t len = boot_anim_inv_sqrt[n];
        const int64_t got = n * len * len;
        const int64_t one = (int64_t)BOOT_ANIM_ONE * BOOT_ANIM_ONE;

        TEST_ASSERT_TRUE_MESSAGE(got > one - one / 200 && got < one + one / 200,
            "n / n = 1 failed for some n - a length entry is wrong");
    }
}

static void test_the_arc_length_constant_matches_the_table(void)
{
    int32_t total = 0;
    for (int n = 1; n <= BOOT_ANIM_TERMS; n++) {
        total += boot_anim_inv_sqrt[n];
    }
    TEST_ASSERT_EQUAL_INT32_MESSAGE(BOOT_ANIM_ARC_Q12, total,
        "BOOT_ANIM_ARC_Q12 must be exactly the sum of the length table, or "
        "the colour gradient will not reach its end on the last term");
}

/*---------------------------------------------------------------------------
 * The curve
 *-------------------------------------------------------------------------*/

static void test_a_walk_starts_at_the_origin(void)
{
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        boot_anim_walk_t w;
        boot_anim_walk_begin(&w, k);
        TEST_ASSERT_EQUAL_INT32(0, w.z.re);
        TEST_ASSERT_EQUAL_INT32(0, w.z.im);
        TEST_ASSERT_EQUAL_INT32(0, w.arc);
        TEST_ASSERT_EQUAL_INT(0, w.n);
    }
}

/* Term one is 1^(-1/2) turned by t*ln(1) - that is, one unit along the real
 * axis, whatever t is. Every spiral therefore leaves the origin the same way,
 * which is visible in the picture and is the cheapest possible check that the
 * phase multiply is not scaled wrongly. */
static void test_the_first_term_is_one_along_the_real_axis(void)
{
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        boot_anim_walk_t w;
        boot_anim_walk_begin(&w, k);
        boot_anim_walk_step(&w);

        TEST_ASSERT_INT32_WITHIN(2, BOOT_ANIM_ONE, w.z.re);
        TEST_ASSERT_INT32_WITHIN(2, 0, w.z.im);
    }
}

/* Each step must be exactly as long as the table says, which is what proves
 * the sine and cosine of the term's phase were taken from the same angle.
 * Squared lengths, so no square root is needed. */
static void test_every_step_is_the_length_the_table_promises(void)
{
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        boot_anim_walk_t w;
        boot_anim_walk_begin(&w, k);

        for (int n = 1; n <= BOOT_ANIM_TERMS; n++) {
            const boot_anim_pt_t before = w.z;
            boot_anim_walk_step(&w);

            const int64_t dx = w.z.re - before.re;
            const int64_t dy = w.z.im - before.im;
            const int64_t got = dx * dx + dy * dy;
            const int64_t want = (int64_t)boot_anim_inv_sqrt[n] *
                                 boot_anim_inv_sqrt[n];

            TEST_ASSERT_TRUE_MESSAGE(got > want - want / 100 - 4 &&
                                     got < want + want / 100 + 4,
                "a term's step was not the length its table entry says");
        }
    }
}

static void test_a_walk_stops_at_the_last_term(void)
{
    boot_anim_walk_t w;
    boot_anim_walk_begin(&w, 0);
    for (int i = 0; i < BOOT_ANIM_TERMS + 20; i++) {
        boot_anim_walk_step(&w);
    }
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_TERMS, w.n);
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_ARC_Q12, w.arc);
}

static void test_the_gradient_reaches_its_end_exactly_at_the_last_term(void)
{
    boot_anim_walk_t w;
    boot_anim_walk_begin(&w, 0);
    for (int n = 1; n <= BOOT_ANIM_TERMS; n++) {
        boot_anim_walk_step(&w);
    }
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_ONE, boot_anim_along(&w));
}

/* The layout guard. Every point of every spiral has to land on the panel, at
 * this origin and this scale - so a change to either that pushes the picture
 * off the edge fails here rather than on the bench. */
static void test_the_whole_picture_fits_on_the_panel(void)
{
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        boot_anim_walk_t w;
        boot_anim_walk_begin(&w, k);

        for (int n = 0; n <= BOOT_ANIM_TERMS; n++) {
            const int x = boot_anim_screen_x(PANEL_W, w.z.re);
            const int y = boot_anim_screen_y(PANEL_H, w.z.im);

            TEST_ASSERT_TRUE_MESSAGE(x >= 0 && x < PANEL_W,
                "a spiral ran off the left or right of the panel");
            TEST_ASSERT_TRUE_MESSAGE(y >= 0 && y < PANEL_H,
                "a spiral ran off the top or bottom of the panel");

            boot_anim_walk_step(&w);
        }
    }
}

static void test_the_origin_sits_left_of_centre_but_not_at_the_edge(void)
{
    const int ox = boot_anim_origin_x(PANEL_W);

    TEST_ASSERT_TRUE_MESSAGE(ox < PANEL_W / 2,
        "the origin is meant to be offset to the LEFT of centre");
    TEST_ASSERT_TRUE_MESSAGE(ox > PANEL_W / 4,
        "offset to the left, not shoved against the edge");
    TEST_ASSERT_EQUAL_INT(PANEL_H / 2, boot_anim_origin_y(PANEL_H));
}

static void test_the_origin_maps_to_itself(void)
{
    TEST_ASSERT_EQUAL_INT(boot_anim_origin_x(PANEL_W),
                          boot_anim_screen_x(PANEL_W, 0));
    TEST_ASSERT_EQUAL_INT(boot_anim_origin_y(PANEL_H),
                          boot_anim_screen_y(PANEL_H, 0));
}

/* Positive imaginary parts must draw UPWARDS, which is the opposite of the
 * framebuffer's y and the easiest sign in the file to get backwards. */
static void test_the_imaginary_axis_points_up(void)
{
    TEST_ASSERT_TRUE(boot_anim_screen_y(PANEL_H, BOOT_ANIM_ONE) <
                     boot_anim_screen_y(PANEL_H, 0));
    TEST_ASSERT_TRUE(boot_anim_screen_x(PANEL_W, BOOT_ANIM_ONE) >
                     boot_anim_screen_x(PANEL_W, 0));
}

/*---------------------------------------------------------------------------
 * Pacing
 *-------------------------------------------------------------------------*/

static void test_a_ramp_is_flat_before_and_after(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ramp(0, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ramp(100, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ramp(300, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ramp(9999, 100, 200));
}

static void test_a_ramp_never_goes_backwards(void)
{
    uint8_t last = 0;
    for (uint32_t t = 0; t <= 400; t++) {
        const uint8_t v = boot_anim_ramp(t, 100, 200);
        TEST_ASSERT_TRUE_MESSAGE(v >= last, "a ramp went backwards");
        last = v;
    }
}

static void test_the_ease_keeps_its_endpoints_and_leads_in_the_middle(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ease_out(0));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ease_out(255));
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_ease_out(128) > 128,
        "an ease-out is ahead of linear part way through, not behind it");
}

/* The whole point of pacing by arc length: term one is a fifth of the screen
 * long, so the pen must still be on it well after it set off. */
static void test_the_pen_spends_real_time_on_the_first_term(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen_terms(0));
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_TERMS * BOOT_ANIM_ONE,
                            boot_anim_pen_terms(BOOT_ANIM_ONE));

    /* A tenth of the way along the curve is only one term in. */
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_pen_terms(BOOT_ANIM_ONE / 10) < 2 * BOOT_ANIM_ONE,
        "pacing looks linear in the term index, which would fling the pen "
        "across the screen and then leave it crawling");
}

static void test_the_pen_never_goes_backwards(void)
{
    int32_t last = -1;
    for (int32_t p = 0; p <= BOOT_ANIM_ONE; p += 8) {
        const int32_t terms = boot_anim_pen_terms(p);
        TEST_ASSERT_TRUE_MESSAGE(terms >= last, "the pen went backwards");
        last = terms;
    }
}

static void test_every_spiral_is_finished_before_the_dissolve_starts(void)
{
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(
            BOOT_ANIM_ONE, boot_anim_pen(BOOT_ANIM_FADE_START_MS, k),
            "a spiral was still being drawn when the picture began fading");
    }
}

static void test_the_spirals_do_not_all_set_off_at_once(void)
{
    const uint32_t t = BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_STAGGER_MS / 2;
    TEST_ASSERT_TRUE(boot_anim_pen(t, 0) > 0);
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(t, 1));
}

static void test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(0));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(BOOT_ANIM_FADE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ink(BOOT_ANIM_MS));
    TEST_ASSERT_TRUE(boot_anim_ink(BOOT_ANIM_MS - 100) < 255);
}

static void test_the_grid_fades_in_from_the_origin_outward(void)
{
    /* At the moment ring 2 is starting, ring 1 is already part way up and
     * ring 3 has not begun. */
    const uint32_t t = BOOT_ANIM_GRID_START_MS + 2 * BOOT_ANIM_GRID_RING_MS + 1;

    TEST_ASSERT_TRUE(boot_anim_grid_alpha(t, 1) > boot_anim_grid_alpha(t, 2));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_alpha(t, 3));
}

static void test_the_axes_are_drawn_before_anything_is_plotted_on_them(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_axis_reach(BOOT_ANIM_AXES_MS));
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_AXES_MS <= BOOT_ANIM_PEN_START_MS,
        "the plane should be there before the curve arrives on it");
}

/*---------------------------------------------------------------------------
 * Colour
 *-------------------------------------------------------------------------*/

/* Every colour on the wheel is fully saturated: one channel at the top, one
 * at the bottom, the third somewhere between. That is what makes it a hue
 * wheel rather than a set of pastels, and it is what the panel is being shown
 * off with. */
static void test_every_hue_is_fully_saturated(void)
{
    for (int hue = 0; hue < BOOT_ANIM_HUE_TURN; hue++) {
        const uint32_t rgb = boot_anim_hue_rgb(hue);
        const int r = (int)((rgb >> 16) & 0xFF);
        const int g = (int)((rgb >> 8) & 0xFF);
        const int b = (int)(rgb & 0xFF);
        const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);

        TEST_ASSERT_EQUAL_INT_MESSAGE(255, hi, "a hue had no full channel");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, lo, "a hue had no empty channel");
    }
}

static void test_the_hue_wheel_joins_up(void)
{
    TEST_ASSERT_EQUAL_UINT32(boot_anim_hue_rgb(0),
                             boot_anim_hue_rgb(BOOT_ANIM_HUE_TURN));
    TEST_ASSERT_EQUAL_UINT32(boot_anim_hue_rgb(5),
                             boot_anim_hue_rgb(-BOOT_ANIM_HUE_TURN + 5));
}

/* No step round the wheel may jump: a discontinuity at a sector boundary is
 * the classic mistake in this conversion, and it shows up as a hard band
 * across the middle of a gradient. */
static void test_the_hue_wheel_has_no_seams(void)
{
    for (int hue = 0; hue < BOOT_ANIM_HUE_TURN; hue++) {
        const uint32_t a = boot_anim_hue_rgb(hue);
        const uint32_t b = boot_anim_hue_rgb(hue + 1);

        for (int shift = 0; shift <= 16; shift += 8) {
            const int ca = (int)((a >> shift) & 0xFF);
            const int cb = (int)((b >> shift) & 0xFF);
            const int step = ca > cb ? ca - cb : cb - ca;
            TEST_ASSERT_TRUE_MESSAGE(step <= 1,
                "a channel jumped between neighbouring hues - the wheel has "
                "a seam at a sector boundary");
        }
    }
}

static void test_a_stroke_brightens_along_the_curve(void)
{
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        const boot_anim_stroke_t tail = boot_anim_stroke(k, 0);
        const boot_anim_stroke_t head = boot_anim_stroke(k, BOOT_ANIM_ONE);

        TEST_ASSERT_TRUE_MESSAGE(head.glow > tail.glow,
            "the newest part of the spiral should be the brightest");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, head.glow,
            "the head of the curve should reach full brightness");
        TEST_ASSERT_TRUE_MESSAGE(head.bloom > tail.bloom,
            "the core should wash toward white, the tail should not");
        TEST_ASSERT_TRUE_MESSAGE(head.hue > tail.hue,
            "a spiral's colour should travel round the wheel as it winds in");
    }
}

static void test_the_spirals_start_on_different_hues(void)
{
    for (int a = 0; a < BOOT_ANIM_SPIRALS; a++) {
        for (int b = a + 1; b < BOOT_ANIM_SPIRALS; b++) {
            const int gap = boot_anim_spiral_hue[a] - boot_anim_spiral_hue[b];
            TEST_ASSERT_TRUE_MESSAGE((gap > 128 || gap < -128),
                "two spirals set off close enough in hue to be mistaken for "
                "one another where they cross");
        }
    }
}

void run_boot_anim_suite(void)
{
    RUN_TEST(test_the_quarter_wave_starts_at_zero_and_ends_at_one);
    RUN_TEST(test_the_quarter_wave_rises_all_the_way);
    RUN_TEST(test_sin_squared_plus_cos_squared_is_one);
    RUN_TEST(test_sine_is_odd_about_the_origin);
    RUN_TEST(test_the_quarter_points_are_exact);

    RUN_TEST(test_the_log_table_adds_up);
    RUN_TEST(test_the_log_table_starts_at_zero_and_climbs);
    RUN_TEST(test_the_length_table_is_a_reciprocal_square_root);
    RUN_TEST(test_the_arc_length_constant_matches_the_table);

    RUN_TEST(test_a_walk_starts_at_the_origin);
    RUN_TEST(test_the_first_term_is_one_along_the_real_axis);
    RUN_TEST(test_every_step_is_the_length_the_table_promises);
    RUN_TEST(test_a_walk_stops_at_the_last_term);
    RUN_TEST(test_the_gradient_reaches_its_end_exactly_at_the_last_term);
    RUN_TEST(test_the_whole_picture_fits_on_the_panel);
    RUN_TEST(test_the_origin_sits_left_of_centre_but_not_at_the_edge);
    RUN_TEST(test_the_origin_maps_to_itself);
    RUN_TEST(test_the_imaginary_axis_points_up);

    RUN_TEST(test_a_ramp_is_flat_before_and_after);
    RUN_TEST(test_a_ramp_never_goes_backwards);
    RUN_TEST(test_the_ease_keeps_its_endpoints_and_leads_in_the_middle);
    RUN_TEST(test_the_pen_spends_real_time_on_the_first_term);
    RUN_TEST(test_the_pen_never_goes_backwards);
    RUN_TEST(test_every_spiral_is_finished_before_the_dissolve_starts);
    RUN_TEST(test_the_spirals_do_not_all_set_off_at_once);
    RUN_TEST(test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end);
    RUN_TEST(test_the_grid_fades_in_from_the_origin_outward);
    RUN_TEST(test_the_axes_are_drawn_before_anything_is_plotted_on_them);

    RUN_TEST(test_every_hue_is_fully_saturated);
    RUN_TEST(test_the_hue_wheel_joins_up);
    RUN_TEST(test_the_hue_wheel_has_no_seams);
    RUN_TEST(test_a_stroke_brightens_along_the_curve);
    RUN_TEST(test_the_spirals_start_on_different_hues);
}

SUITE_REGISTER(run_boot_anim_suite);
