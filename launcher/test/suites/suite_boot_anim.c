/*=============================================================================
 * Portable suite: the startup animation's projection, curve, smoothing and
 * timeline.
 *
 * boot_anim.h and the generated boot_anim_curve.h are the whole of what is
 * tested here. boot_anim.c, which turns all that into gfx calls, is not: it
 * needs a framebuffer and a panel.
 *
 * THE TABLE IS TESTED AGAINST THE MATHEMATICS, NOT AGAINST ITSELF
 *
 * tools/gen_zeta_curve.py already refuses to emit a table unless its own
 * evaluation of zeta checks out, but that proves nothing about the file
 * actually in the repo - which could be stale, hand-edited, or generated with
 * different constants than boot_anim.h now uses.
 *
 * So the shipped numbers are checked directly, and against the one thing that
 * makes this curve worth drawing: it must pass through zero at the five known
 * heights, and it must NOT come anywhere near zero anywhere else. Those are
 * the first five nontrivial zeros of the zeta function, and no table of
 * plausible-looking numbers passes both halves by accident.
 *===========================================================================*/

#include <stdbool.h>
#include <stdint.h>

#include "unity.h"
#include "suites.h"

#include "boot/boot_anim.h"

/* The panel these numbers are laid out for. Named here rather than pulled
 * from gfx.h, which needs the BSP; gfx_dirty.h mirrors the same two numbers
 * for the same reason. */
#define PANEL_W 368
#define PANEL_H 448

/* |zeta|^2 in Q24, so nothing here needs a square root. */
static int32_t mag_sq(const boot_anim_sample_t *s)
{
    return (int32_t)s->re * s->re + (int32_t)s->im * s->im;
}

/* A Q12 magnitude, squared - what mag_sq() is compared against. */
static int32_t threshold_sq(int32_t q12)
{
    return q12 * q12;
}

/*---------------------------------------------------------------------------
 * The curve
 *-------------------------------------------------------------------------*/

static void test_the_curve_climbs_from_zero_to_the_top(void)
{
    TEST_ASSERT_EQUAL_INT(0, boot_anim_curve[0].t);
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_T_MAX << BOOT_ANIM_TQ,
                          boot_anim_curve[BOOT_ANIM_CURVE_POINTS - 1].t);
}

/* t only ever increases. The picture is a climb, and a sample out of order
 * would draw a segment going back down through everything above it. */
static void test_the_curve_never_descends(void)
{
    for (int i = 1; i < BOOT_ANIM_CURVE_POINTS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(boot_anim_curve[i].t >=
                                 boot_anim_curve[i - 1].t,
            "the curve went back down - the table is out of order");
    }
}

/* THE test. At each of the five listed heights the curve must reach the t
 * axis, which is zeta vanishing there.
 *
 * 0.15 rather than 0 because the table is a sampling: the nearest sample to a
 * crossing sits a little to one side of it. The measured worst case is 0.084,
 * and the check below that this never happens away from a zero is what stops
 * the tolerance being meaningless. */
static void test_the_curve_meets_the_axis_at_every_known_zero(void)
{
    const int32_t window = 1 << BOOT_ANIM_TQ;    /* +/- 1.0 in t */
    const int32_t close  = threshold_sq((int32_t)(0.15 * BOOT_ANIM_ONE));

    for (int z = 0; z < BOOT_ANIM_ZEROS; z++) {
        int32_t best = INT32_MAX;
        for (int i = 0; i < BOOT_ANIM_CURVE_POINTS; i++) {
            const int32_t dt = boot_anim_curve[i].t - boot_anim_zero_t[z];
            if (dt >= -window && dt <= window) {
                const int32_t m = mag_sq(&boot_anim_curve[i]);
                if (m < best) {
                    best = m;
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(best <= close,
            "the curve does not reach the t axis at one of the heights where "
            "zeta is known to vanish");
    }
}

/* The other half, and the one that gives the test above its teeth: away from
 * a zero the curve must stay well clear of the axis. Measured minimum is
 * 0.527, so 0.35 leaves room without letting a flat or collapsed table pass. */
static void test_the_curve_keeps_away_from_the_axis_everywhere_else(void)
{
    const int32_t window = 2 << BOOT_ANIM_TQ;    /* +/- 2.0 in t */
    const int32_t clear  = threshold_sq((int32_t)(0.35 * BOOT_ANIM_ONE));

    for (int i = 0; i < BOOT_ANIM_CURVE_POINTS; i++) {
        bool near_a_zero = false;
        for (int z = 0; z < BOOT_ANIM_ZEROS; z++) {
            const int32_t dt = boot_anim_curve[i].t - boot_anim_zero_t[z];
            if (dt >= -window && dt <= window) {
                near_a_zero = true;
            }
        }
        if (near_a_zero) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(mag_sq(&boot_anim_curve[i]) > clear,
            "the curve came close to the t axis at a height where zeta does "
            "not vanish");
    }
}

static void test_every_listed_zero_is_on_the_climb_and_in_order(void)
{
    for (int z = 0; z < BOOT_ANIM_ZEROS; z++) {
        TEST_ASSERT_TRUE(boot_anim_zero_t[z] > 0);
        TEST_ASSERT_TRUE(boot_anim_zero_t[z] < (BOOT_ANIM_T_MAX << BOOT_ANIM_TQ));
        if (z > 0) {
            /* draw_zeros() stops at the first zero above the pen, which is
             * only correct while the table is sorted. */
            TEST_ASSERT_TRUE_MESSAGE(boot_anim_zero_t[z] > boot_anim_zero_t[z - 1],
                "the zeros must be in increasing order");
        }
    }
}

static void test_samples_are_clamped_rather_than_read_out_of_range(void)
{
    const boot_anim_pt_t first = boot_anim_sample(0);
    const boot_anim_pt_t last  = boot_anim_sample(BOOT_ANIM_CURVE_POINTS - 1);

    TEST_ASSERT_EQUAL_INT32(first.t, boot_anim_sample(-1).t);
    TEST_ASSERT_EQUAL_INT32(first.re, boot_anim_sample(-99).re);
    TEST_ASSERT_EQUAL_INT32(last.t, boot_anim_sample(BOOT_ANIM_CURVE_POINTS).t);
    TEST_ASSERT_EQUAL_INT32(last.im,
                            boot_anim_sample(BOOT_ANIM_CURVE_POINTS + 99).im);
}

/*---------------------------------------------------------------------------
 * The projection
 *-------------------------------------------------------------------------*/

static void test_the_origin_maps_to_the_origin(void)
{
    TEST_ASSERT_EQUAL_INT(boot_anim_origin_x(PANEL_W),
                          boot_anim_screen_x(PANEL_W, 0, 0));
    TEST_ASSERT_EQUAL_INT(boot_anim_origin_y(PANEL_H),
                          boot_anim_screen_y(PANEL_H, 0, 0, 0));
}

/* The three axes have to go in three different directions, and each of the
 * six signs below is one that would silently mirror the picture if it were
 * wrong. t especially: screen y grows downward and height does not. */
static void test_the_three_axes_point_the_way_they_are_supposed_to(void)
{
    const int32_t one = BOOT_ANIM_ONE;
    const int ox = boot_anim_origin_x(PANEL_W);
    const int oy = boot_anim_origin_y(PANEL_H);

    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_x(PANEL_W, one, 0) > ox,
        "the real axis should run to the right");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_y(PANEL_H, one, 0, 0) > oy,
        "the real axis should run downward, into the floor");

    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_x(PANEL_W, 0, one) < ox,
        "the imaginary axis should run to the left");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_y(PANEL_H, 0, one, 0) > oy,
        "the imaginary axis should run downward, into the floor");

    TEST_ASSERT_EQUAL_INT_MESSAGE(ox, boot_anim_screen_x(PANEL_W, 0, 0),
        "t should not move a point sideways at all");
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_screen_y(PANEL_H, 0, 0, 1 << BOOT_ANIM_TQ) < oy,
        "t should run UP the screen, which is downward in y");
}

/* The imaginary axis at 45 degrees, which is what makes the floor read as a
 * floor: equal steps left and down. */
static void test_the_imaginary_axis_is_at_forty_five_degrees(void)
{
    const int ox = boot_anim_origin_x(PANEL_W);
    const int oy = boot_anim_origin_y(PANEL_H);
    const int32_t three = 3 * BOOT_ANIM_ONE;

    const int dx = ox - boot_anim_screen_x(PANEL_W, 0, three);
    const int dy = boot_anim_screen_y(PANEL_H, 0, three, 0) - oy;

    TEST_ASSERT_INT_WITHIN_MESSAGE(1, dx, dy,
        "the imaginary axis should go as far left as it goes down");
}

/* The layout guard: the whole scene has to land on the panel. Curve, floor
 * and the top of the t axis, so that changing a scale, an angle or the height
 * of the climb fails here rather than on the bench. */
static void test_the_whole_scene_fits_on_the_panel(void)
{
    for (int i = 0; i < BOOT_ANIM_CURVE_POINTS; i++) {
        const boot_anim_pt_t p = boot_anim_sample(i);
        const int x = boot_anim_screen_x(PANEL_W, p.re, p.im);
        const int y = boot_anim_screen_y(PANEL_H, p.re, p.im, p.t);

        TEST_ASSERT_TRUE_MESSAGE(x >= 0 && x < PANEL_W,
            "the curve ran off the side of the panel");
        TEST_ASSERT_TRUE_MESSAGE(y >= 0 && y < PANEL_H,
            "the curve ran off the top or bottom of the panel");
    }

    /* The floor deliberately has no corners to check - its lines run off the
     * panel and are clipped, which is what makes it read as a plane rather
     * than a tile. What must still fit is the ends of the two axis arms,
     * because those carry labels. */
    const int32_t arm = 4 * BOOT_ANIM_ONE;
    const int re_x = boot_anim_screen_x(PANEL_W, arm, 0);
    const int re_y = boot_anim_screen_y(PANEL_H, arm, 0, 0);
    const int im_x = boot_anim_screen_x(PANEL_W, 0, arm);
    const int im_y = boot_anim_screen_y(PANEL_H, 0, arm, 0);

    TEST_ASSERT_TRUE_MESSAGE(re_x >= 0 && re_x < PANEL_W &&
                             re_y >= 0 && re_y < PANEL_H,
        "the end of the real axis is off the panel");
    TEST_ASSERT_TRUE_MESSAGE(im_x >= 0 && im_x < PANEL_W &&
                             im_y >= 0 && im_y < PANEL_H,
        "the end of the imaginary axis is off the panel");

    const int top = boot_anim_screen_y(PANEL_H, 0, 0,
                                       (BOOT_ANIM_T_MAX + 1) << BOOT_ANIM_TQ);
    TEST_ASSERT_TRUE_MESSAGE(top >= 0,
        "the top of the t axis is off the top of the panel");
}

/*---------------------------------------------------------------------------
 * Smoothing
 *-------------------------------------------------------------------------*/

static boot_anim_pt_t pt(int32_t re, int32_t im, int32_t t)
{
    boot_anim_pt_t p = { re, im, t };
    return p;
}

/* A span runs from the midpoint of the first two control points to the
 * midpoint of the last two, which is what makes consecutive spans join. */
static void test_a_span_starts_and_ends_halfway_between_its_points(void)
{
    const boot_anim_pt_t a = pt(0, 0, 0);
    const boot_anim_pt_t b = pt(4096, 2048, 512);
    const boot_anim_pt_t c = pt(8192, -2048, 1024);

    const boot_anim_pt_t start = boot_anim_spline(a, b, c, 0);
    const boot_anim_pt_t end   = boot_anim_spline(a, b, c, BOOT_ANIM_ONE);

    TEST_ASSERT_INT32_WITHIN(1, (a.re + b.re) / 2, start.re);
    TEST_ASSERT_INT32_WITHIN(1, (a.im + b.im) / 2, start.im);
    TEST_ASSERT_INT32_WITHIN(1, (a.t + b.t) / 2, start.t);

    TEST_ASSERT_INT32_WITHIN(1, (b.re + c.re) / 2, end.re);
    TEST_ASSERT_INT32_WITHIN(1, (b.im + c.im) / 2, end.im);
    TEST_ASSERT_INT32_WITHIN(1, (b.t + c.t) / 2, end.t);
}

/* Repeating a control point pins the curve to it, which is how the first and
 * last samples end up actually being drawn rather than half a span in. */
static void test_a_repeated_point_pins_the_end_of_the_curve(void)
{
    const boot_anim_pt_t a = pt(1000, -2000, 300);
    const boot_anim_pt_t b = pt(5000, 1500, 900);

    const boot_anim_pt_t start = boot_anim_spline(a, a, b, 0);
    TEST_ASSERT_INT32_WITHIN(1, a.re, start.re);
    TEST_ASSERT_INT32_WITHIN(1, a.im, start.im);
    TEST_ASSERT_INT32_WITHIN(1, a.t, start.t);
}

/* The convex hull property, which is the whole reason this is a B-spline and
 * not a Catmull-Rom: no point of the curve may leave the box its control
 * points span. An interpolating spline would overshoot here, and the picture
 * would bulge exactly where the curve turns hardest. */
static void test_a_span_never_leaves_its_control_points_behind(void)
{
    const boot_anim_pt_t a = pt(-3000, 500, 0);
    const boot_anim_pt_t b = pt(4000, -2500, 400);
    const boot_anim_pt_t c = pt(-2500, 3000, 800);

    const int32_t lo_re = -3000, hi_re = 4000;
    const int32_t lo_im = -2500, hi_im = 3000;

    for (int32_t t = 0; t <= BOOT_ANIM_ONE; t += 37) {
        const boot_anim_pt_t p = boot_anim_spline(a, b, c, t);
        TEST_ASSERT_TRUE_MESSAGE(p.re >= lo_re - 1 && p.re <= hi_re + 1,
            "the spline overshot its control points - that is a Catmull-Rom "
            "failure mode and this is supposed to be a B-spline");
        TEST_ASSERT_TRUE_MESSAGE(p.im >= lo_im - 1 && p.im <= hi_im + 1,
            "the spline overshot its control points");
    }
}

/* Height must not wobble across a span either: the whole curve climbs, so a
 * span between two rising samples has to rise all the way through. */
static void test_a_span_climbs_steadily_when_its_points_do(void)
{
    const boot_anim_pt_t a = pt(0, 0, 100);
    const boot_anim_pt_t b = pt(3000, 1000, 200);
    const boot_anim_pt_t c = pt(-1000, 2000, 300);

    int32_t last = boot_anim_spline(a, b, c, 0).t;
    for (int32_t t = 64; t <= BOOT_ANIM_ONE; t += 64) {
        const int32_t now = boot_anim_spline(a, b, c, t).t;
        TEST_ASSERT_TRUE_MESSAGE(now >= last, "a span dipped on its way up");
        last = now;
    }
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

static void test_the_pen_runs_from_nothing_to_the_whole_curve(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(0));
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(BOOT_ANIM_PEN_START_MS));
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_ONE,
        boot_anim_pen(BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS));
}

static void test_the_curve_is_finished_before_the_dissolve_starts(void)
{
    TEST_ASSERT_EQUAL_INT32_MESSAGE(BOOT_ANIM_ONE,
        boot_anim_pen(BOOT_ANIM_FADE_START_MS),
        "the curve was still being drawn when the picture began fading");
}

static void test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(0));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(BOOT_ANIM_FADE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ink(BOOT_ANIM_MS));
    TEST_ASSERT_TRUE(boot_anim_ink(BOOT_ANIM_MS - 100) < 255);
}

static void test_the_floor_fades_in_from_the_origin_outward(void)
{
    const uint32_t t = BOOT_ANIM_GRID_START_MS + 2 * BOOT_ANIM_GRID_RING_MS + 1;

    TEST_ASSERT_TRUE(boot_anim_grid_alpha(t, 1) > boot_anim_grid_alpha(t, 2));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_alpha(t, 3));
}

/* The floor has no edge: it fades out with distance instead of stopping.
 * Long after everything has arrived, each ring must still be dimmer than the
 * one inside it, all the way down to nothing. */
static void test_the_floor_fades_out_with_distance_rather_than_stopping(void)
{
    const uint32_t settled = BOOT_ANIM_MS;

    for (int ring = 2; ring < BOOT_ANIM_GRID_FADE; ring++) {
        TEST_ASSERT_TRUE_MESSAGE(
            boot_anim_grid_alpha(settled, ring) <
            boot_anim_grid_alpha(settled, ring - 1),
            "a floor ring was no dimmer than the one inside it - the grid "
            "would read as a tile with an edge rather than as a plane");
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0,
        boot_anim_grid_alpha(settled, BOOT_ANIM_GRID_FADE),
        "the floor should be completely gone by the end of its fade");
}

/* Backdrop, not subject. The floor covers far more of the screen than the
 * curve does, so if it is ever allowed near full brightness it wins the
 * picture - which is exactly what happened before this cap existed. */
static void test_the_floor_stays_dim_enough_to_be_a_backdrop(void)
{
    for (uint32_t t = 0; t <= BOOT_ANIM_MS; t += 25) {
        for (int ring = 1; ring <= BOOT_ANIM_GRID_RINGS; ring++) {
            TEST_ASSERT_TRUE_MESSAGE(
                boot_anim_grid_alpha(t, ring) <= BOOT_ANIM_GRID_MAX,
                "the floor got brighter than its cap");
        }
    }
}

static void test_the_floor_colour_travels_with_time_and_distance(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_grid_hue(BOOT_ANIM_GRID_HUE_MS / 4, 1) >
        boot_anim_grid_hue(0, 1),
        "the floor colour should move on as time passes");
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_grid_hue(0, 2) > boot_anim_grid_hue(0, 1),
        "rings should not all change together - the drift travels outward");

    /* A whole period brings it back round to where it started. */
    TEST_ASSERT_EQUAL_UINT32(
        boot_anim_hue_rgb(boot_anim_grid_hue(0, 1)),
        boot_anim_hue_rgb(boot_anim_grid_hue(BOOT_ANIM_GRID_HUE_MS, 1)));
}

static void test_the_axes_are_there_before_the_curve_starts_climbing(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_axis_reach(BOOT_ANIM_AXES_MS));
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_AXES_MS <= BOOT_ANIM_PEN_START_MS,
        "the axes should be drawn before anything is plotted against them");
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
                "a channel jumped between neighbouring hues - the wheel has a "
                "seam at a sector boundary");
        }
    }
}

static void test_height_changes_the_hue(void)
{
    const boot_anim_stroke_t foot = boot_anim_stroke(0, 0);
    const boot_anim_stroke_t top  = boot_anim_stroke(BOOT_ANIM_ONE, 0);

    TEST_ASSERT_TRUE_MESSAGE(top.hue - foot.hue > BOOT_ANIM_HUE_TURN / 2,
        "the climb should turn most of the way round the wheel, so that "
        "height reads as colour");
}

/* One pen's trail, sampled within the gap before the next pen contributes
 * anything - so this is that pen alone, fading. */
static void test_a_pens_trail_fades_behind_it(void)
{
    const int32_t pen = BOOT_ANIM_ONE / 2;
    const int32_t step = BOOT_ANIM_TRAIL_GAP / 4;

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, boot_anim_stroke(pen, pen).glow,
        "the stroke under the leading pen should be at full brightness");

    uint8_t last = 255;
    for (int32_t back = step; back < BOOT_ANIM_TRAIL_GAP; back += step) {
        const uint8_t glow = boot_anim_stroke(pen - back, pen).glow;
        TEST_ASSERT_TRUE_MESSAGE(glow < last,
            "the trail should fade with distance behind its pen");
        last = glow;
    }
}

/* Every pen is a bright point of its own, spaced back along the curve. That
 * is the whole difference from a single trail: several live bands at once. */
static void test_every_pen_is_lit_at_its_own_position(void)
{
    const int32_t pen = BOOT_ANIM_ONE;

    for (int k = 0; k < BOOT_ANIM_TRAILS; k++) {
        const int32_t at = boot_anim_trail_pos(pen, k);
        TEST_ASSERT_TRUE_MESSAGE(at > 0,
            "a pen never sets off before the curve is finished");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, boot_anim_stroke(at, pen).glow,
            "a pen was not at full brightness at its own position");
    }
}

/* And each carries a different part of the wheel, which is what puts most of
 * the gamut on screen at once. */
static void test_the_pens_carry_different_colours(void)
{
    for (int a = 0; a < BOOT_ANIM_TRAILS; a++) {
        for (int b = a + 1; b < BOOT_ANIM_TRAILS; b++) {
            TEST_ASSERT_TRUE_MESSAGE(
                boot_anim_trail_hue(a) != boot_anim_trail_hue(b),
                "two pens carry the same hue - they would read as one band");
        }
    }

    const int spread = boot_anim_trail_hue(BOOT_ANIM_TRAILS - 1) -
                       boot_anim_trail_hue(0);
    TEST_ASSERT_TRUE_MESSAGE(spread > BOOT_ANIM_HUE_TURN / 2,
        "the pens should be spread round the wheel, not bunched on one side");
}

/* A pen's hue is mixed in BY STRENGTH rather than switched to, so a piece of
 * curve part way behind a pen is part way toward that pen's colour. Without
 * this the bands snap between colours as the nearest pen changes. */
static void test_a_pens_colour_arrives_gradually(void)
{
    const int32_t pen = BOOT_ANIM_ONE / 2;

    const int under = boot_anim_stroke(pen, pen).hue;
    const int half  = boot_anim_stroke(pen - BOOT_ANIM_TRAIL_GAP / 2, pen).hue;
    const int base  = boot_anim_stroke(pen - BOOT_ANIM_TRAIL_GAP + 1, pen).hue;

    TEST_ASSERT_TRUE_MESSAGE(under > half && half > base,
        "a pen's hue should fade in with its trail rather than switch on");
}

/* Beyond the trail's reach, only the base is left - which is what the
 * finished picture is made of, and it must still be lit and still be
 * coloured. */
static void test_settled_curve_keeps_its_colour(void)
{
    const int32_t pen = BOOT_ANIM_ONE;
    const boot_anim_stroke_t settled = boot_anim_stroke(0, pen);

    TEST_ASSERT_TRUE_MESSAGE(settled.glow > 96,
        "the settled part of the curve should still be clearly lit");
    TEST_ASSERT_TRUE_MESSAGE(settled.bloom < 64,
        "the settled part should keep its hue rather than wash toward white");
    TEST_ASSERT_EQUAL_UINT8(1, settled.width);
}

/* The bloom is capped well short of white on purpose - see boot_anim.h. A
 * trail that reaches white has no colour left exactly where it is brightest. */
static void test_the_trail_never_washes_out_to_white(void)
{
    for (int32_t along = 0; along <= BOOT_ANIM_ONE; along += 64) {
        const boot_anim_stroke_t s = boot_anim_stroke(along, along);
        TEST_ASSERT_TRUE_MESSAGE(s.bloom <= 128,
            "the trail bloomed far enough toward white to lose its hue");
    }
}

static void test_the_live_end_of_the_curve_is_drawn_thicker(void)
{
    const int32_t pen = BOOT_ANIM_ONE / 2;

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(BOOT_ANIM_FAT_WIDTH,
        boot_anim_stroke(pen, pen).width,
        "the stroke at the pen should be the fat one");

    /* Just short of the next pen's position, where the leading pen's trail
     * has faded below the threshold and the next one has not arrived. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1,
        boot_anim_stroke(pen - BOOT_ANIM_TRAIL_GAP + 1, pen).width,
        "a stroke between two pens should be thin again");
}

void run_boot_anim_suite(void)
{
    RUN_TEST(test_the_curve_climbs_from_zero_to_the_top);
    RUN_TEST(test_the_curve_never_descends);
    RUN_TEST(test_the_curve_meets_the_axis_at_every_known_zero);
    RUN_TEST(test_the_curve_keeps_away_from_the_axis_everywhere_else);
    RUN_TEST(test_every_listed_zero_is_on_the_climb_and_in_order);
    RUN_TEST(test_samples_are_clamped_rather_than_read_out_of_range);

    RUN_TEST(test_the_origin_maps_to_the_origin);
    RUN_TEST(test_the_three_axes_point_the_way_they_are_supposed_to);
    RUN_TEST(test_the_imaginary_axis_is_at_forty_five_degrees);
    RUN_TEST(test_the_whole_scene_fits_on_the_panel);

    RUN_TEST(test_a_span_starts_and_ends_halfway_between_its_points);
    RUN_TEST(test_a_repeated_point_pins_the_end_of_the_curve);
    RUN_TEST(test_a_span_never_leaves_its_control_points_behind);
    RUN_TEST(test_a_span_climbs_steadily_when_its_points_do);

    RUN_TEST(test_a_ramp_is_flat_before_and_after);
    RUN_TEST(test_a_ramp_never_goes_backwards);
    RUN_TEST(test_the_ease_keeps_its_endpoints_and_leads_in_the_middle);
    RUN_TEST(test_the_pen_runs_from_nothing_to_the_whole_curve);
    RUN_TEST(test_the_curve_is_finished_before_the_dissolve_starts);
    RUN_TEST(test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end);
    RUN_TEST(test_the_floor_fades_in_from_the_origin_outward);
    RUN_TEST(test_the_floor_fades_out_with_distance_rather_than_stopping);
    RUN_TEST(test_the_floor_stays_dim_enough_to_be_a_backdrop);
    RUN_TEST(test_the_floor_colour_travels_with_time_and_distance);
    RUN_TEST(test_the_axes_are_there_before_the_curve_starts_climbing);

    RUN_TEST(test_every_hue_is_fully_saturated);
    RUN_TEST(test_the_hue_wheel_joins_up);
    RUN_TEST(test_the_hue_wheel_has_no_seams);
    RUN_TEST(test_height_changes_the_hue);
    RUN_TEST(test_a_pens_trail_fades_behind_it);
    RUN_TEST(test_every_pen_is_lit_at_its_own_position);
    RUN_TEST(test_the_pens_carry_different_colours);
    RUN_TEST(test_a_pens_colour_arrives_gradually);
    RUN_TEST(test_settled_curve_keeps_its_colour);
    RUN_TEST(test_the_trail_never_washes_out_to_white);
    RUN_TEST(test_the_live_end_of_the_curve_is_drawn_thicker);
}

SUITE_REGISTER(run_boot_anim_suite);
