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
 * The camera
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

/* The identity that pins every entry down at once, checked across the full
 * turn rather than just the arc the camera actually uses - a general-purpose
 * table should hold everywhere, and this is what makes boot_anim_sin()/cos()
 * safe to reuse for anything else that turns up needing one. */
static void test_sin_squared_plus_cos_squared_is_one(void)
{
    for (uint32_t phase = 0; phase < 65536; phase += 7) {
        const int64_t s = boot_anim_sin((uint16_t)phase);
        const int64_t c = boot_anim_cos((uint16_t)phase);
        const int64_t sum = s * s + c * c;
        const int64_t one = (int64_t)32767 * 32767;

        TEST_ASSERT_TRUE_MESSAGE(sum > one - one / 300 && sum < one + one / 300,
            "sin^2 + cos^2 left the neighbourhood of 1");
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

/* A moment safely after the curve finishes (pen saturates at 2500ms) but
 * before the finale starts (2700ms) - the view here is what the old,
 * pre-finale tests meant by "full progress". */
#define CURVE_DONE_MS 2600

/* At t=0 the view must reproduce the ORIGINAL fixed camera - the one this
 * whole file described before the orbit existed - to within a couple of Q8
 * units. Not bit-exact: D+C reconstructs A (see boot_anim.h's derivation
 * comment) through two independently-rounded compile-time constants rather
 * than the single rounding the old RE_Y/IM_Y had, so a unit or two of drift
 * is the honest cost of that, not a bug. */
static void test_the_view_starts_at_the_original_fixed_camera(void)
{
    const boot_anim_view_t v = boot_anim_view(PANEL_W, PANEL_H, 0);

    TEST_ASSERT_INT32_WITHIN_MESSAGE(2, 3064, v.re_y,
        "at t=0 the real axis' weight should match the original "
        "sin(20) * Z_PX * 256");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(2, 6336, v.im_y,
        "at t=0 the imaginary axis' weight should match the "
        "original sin(45) * Z_PX * 256");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(2, -(BOOT_ANIM_T_PX << 8), v.t_y,
        "at t=0 the t axis should still be at its full, unforeshortened "
        "length - T_PX per unit, Q8");
    TEST_ASSERT_EQUAL_INT_MESSAGE(boot_anim_origin_y(PANEL_H), v.oy,
        "at t=0 the origin should sit exactly where it always did");
    TEST_ASSERT_EQUAL_INT_MESSAGE(boot_anim_origin_x(PANEL_W), v.ox,
        "at t=0 the origin should not have slid sideways yet");
}

/* Once the curve finishes, t must have foreshortened a long way from where
 * it started - the whole point of the first half of the orbit - while
 * staying short of collapsing to nothing, which is the "with a tilt to be
 * observable" half of the ask. */
static void test_the_view_foreshortens_t_by_the_end_of_the_curve(void)
{
    const boot_anim_view_t v0 = boot_anim_view(PANEL_W, PANEL_H, 0);
    const boot_anim_view_t v1 = boot_anim_view(PANEL_W, PANEL_H, CURVE_DONE_MS);

    const int32_t t0 = v0.t_y < 0 ? -v0.t_y : v0.t_y;
    const int32_t t1 = v1.t_y < 0 ? -v1.t_y : v1.t_y;

    TEST_ASSERT_TRUE_MESSAGE(t1 < t0 / 2,
        "t should have foreshortened to well under half its starting length "
        "by the end of the climb");
    TEST_ASSERT_TRUE_MESSAGE(t1 > 0,
        "t should not have foreshortened all the way to a point - some tilt "
        "must stay observable");
}

/* THE FINALE'S turn, past where the curve-drawing phase alone stops: by the
 * time the last letter has landed, t must have grown back toward roughly
 * its STARTING magnitude - "the spiral ends up pointing up as it was from
 * start" - while its SIGN has flipped, since continuing past the point
 * where it fully foreshortens necessarily means passing through zero. */
/* "By the end of the finale" now means BOOT_ANIM_MS, not
 * BOOT_ANIM_FINALE_END_MS - see BOOT_ANIM_COLLAPSE_MS's own comment: the
 * turn itself does not even START until FINALE_END_MS, once every letter
 * has landed, and runs for BOOT_ANIM_COLLAPSE_MS after that. */
static void test_the_finale_turns_t_back_toward_vertical(void)
{
    const boot_anim_view_t v0 = boot_anim_view(PANEL_W, PANEL_H, 0);
    const boot_anim_view_t vf = boot_anim_view(PANEL_W, PANEL_H, BOOT_ANIM_MS);

    const int32_t t0 = v0.t_y < 0 ? -v0.t_y : v0.t_y;
    const int32_t tf = vf.t_y < 0 ? -vf.t_y : vf.t_y;

    TEST_ASSERT_TRUE_MESSAGE(tf > t0 / 2,
        "by the end of the finale t should have grown back to at least half "
        "its starting magnitude, not stayed foreshortened");
}

/* The finale settles the origin at BOOT_ANIM_FINALE_ORIGIN_VIEW_X/Y, a VIEW
 * point - see that constant's own comment in boot_anim.h - which is why
 * this checks ox/oy against PANEL_H - VIEW_X and PANEL_W - VIEW_Y rather
 * than against VIEW_X/Y directly: that translation is the exact thing under
 * test, the same reasoning test_a_letter_starts_off_panel_to_the_left() and
 * the rest of "The title"'s tests apply on the other side of it.
 *
 * Checked at BOOT_ANIM_MS, not BOOT_ANIM_FINALE_END_MS - see
 * BOOT_ANIM_COLLAPSE_MS's own comment: the drift does not start until
 * every letter has landed and takes BOOT_ANIM_COLLAPSE_MS from there. */
static void test_the_finale_settles_the_origin_at_its_view_target(void)
{
    const boot_anim_view_t before =
        boot_anim_view(PANEL_W, PANEL_H, CURVE_DONE_MS);
    const boot_anim_view_t after =
        boot_anim_view(PANEL_W, PANEL_H, BOOT_ANIM_MS);

    TEST_ASSERT_TRUE_MESSAGE(after.ox != before.ox || after.oy != before.oy,
        "the origin should have moved during the finale");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        BOOT_ANIM_FINALE_ORIGIN_VIEW_X, after.oy,
        "oy should land exactly on the view target's x by the time the "
        "finale ends");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        PANEL_W - BOOT_ANIM_FINALE_ORIGIN_VIEW_Y, after.ox,
        "ox should land exactly on the view target's y by the time the "
        "finale ends");
}

/* The mirror named in boot_anim.h's "THE CAMERA ORBITS": the origin ends up
 * (once the CURVE finishes, before the finale's own further slide) as far
 * below the top of the screen as it started above the bottom. */
static void test_the_origin_ends_at_the_mirror_of_where_it_started(void)
{
    const int oy_start = boot_anim_view(PANEL_W, PANEL_H, 0).oy;
    const int oy_end = boot_anim_view(PANEL_W, PANEL_H, CURVE_DONE_MS).oy;

    TEST_ASSERT_EQUAL_INT_MESSAGE(PANEL_H - oy_start, oy_end,
        "the origin should finish exactly as far from the top as it began "
        "from the bottom");
    TEST_ASSERT_TRUE_MESSAGE(oy_end < oy_start,
        "the origin should have drifted upward, not down or sideways only");
}

static void test_the_origin_drifts_upward_without_doubling_back(void)
{
    int last_oy = boot_anim_view(PANEL_W, PANEL_H, 0).oy;
    for (uint32_t t = 0; t <= CURVE_DONE_MS; t += 40) {
        const int oy = boot_anim_view(PANEL_W, PANEL_H, t).oy;
        TEST_ASSERT_TRUE_MESSAGE(oy <= last_oy,
            "the origin should drift steadily upward, never back down");
        last_oy = oy;
    }
}

/*---------------------------------------------------------------------------
 * The projection
 *-------------------------------------------------------------------------*/

static void test_the_origin_maps_to_the_origin(void)
{
    const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, 0);

    TEST_ASSERT_EQUAL_INT(boot_anim_origin_x(PANEL_W),
                          boot_anim_screen_x(0, 0, &view));
    TEST_ASSERT_EQUAL_INT(boot_anim_origin_y(PANEL_H),
                          boot_anim_screen_y(PANEL_H, 0, 0, 0, &view));
}

/* The three axes have to go in three different directions, and each of the
 * six signs below is one that would silently mirror the picture if it were
 * wrong. t especially: screen y grows downward and height does not.
 *
 * Checked at t=0, the camera's starting orientation - see the camera
 * section above for what changes as it orbits, and
 * test_the_view_foreshortens_t_by_the_end_of_the_curve() for the
 * corresponding claim once it has. */
static void test_the_three_axes_point_the_way_they_are_supposed_to(void)
{
    const int32_t one = BOOT_ANIM_ONE;
    const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, 0);
    const int ox = view.ox;
    const int oy = boot_anim_origin_y(PANEL_H);

    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_x(one, 0, &view) > ox,
        "the real axis should run to the right");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_y(PANEL_H, one, 0, 0, &view) > oy,
        "the real axis should run downward, into the floor");

    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_x(0, one, &view) < ox,
        "the imaginary axis should run to the left");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_screen_y(PANEL_H, 0, one, 0, &view) > oy,
        "the imaginary axis should run downward, into the floor");

    TEST_ASSERT_EQUAL_INT_MESSAGE(ox, boot_anim_screen_x(0, 0, &view),
        "t should not move a point sideways at all");
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_screen_y(PANEL_H, 0, 0, 1 << BOOT_ANIM_TQ, &view) < oy,
        "t should run UP the screen, which is downward in y");
}

/* The imaginary axis at 45 degrees, which is what makes the floor read as a
 * floor: equal steps left and down. Checked at t=0, same reasoning as the
 * test above. */
static void test_the_imaginary_axis_is_at_forty_five_degrees(void)
{
    const int32_t three = 3 * BOOT_ANIM_ONE;
    const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, 0);
    const int ox = view.ox;
    const int oy = boot_anim_origin_y(PANEL_H);

    const int dx = ox - boot_anim_screen_x(0, three, &view);
    const int dy = boot_anim_screen_y(PANEL_H, 0, three, 0, &view) - oy;

    TEST_ASSERT_INT_WITHIN_MESSAGE(1, dx, dy,
        "the imaginary axis should go as far left as it goes down");
}

/* The layout guard: the whole CURVE has to land on the panel, at every
 * moment from power-on up to where the motif starts growing, and again once
 * it has fully settled back down - not "always", any more. Swept across
 * time and, before the curve is fully drawn, restricted to the part
 * actually drawn by then, matching what boot_anim.c really shows: a point
 * at the far top of the curve is not on screen yet while the pen has not
 * reached it, so checking it there would be checking something the
 * animation never shows.
 *
 * This is the test that would have caught the sign error the fixed-point
 * derivation had during development - see boot_anim.h's derivation comment
 * on how thoroughly this was cross-checked before being trusted.
 *
 * GROW and SETTLE are deliberately NOT held to zero overflow here, the
 * same way the floor and the unbounded axes never were -
 * see boot_anim_motif_shrink_q8()'s own top comment and draw_floor()'s: a
 * motif big enough to swell past its own full size for a moment is going to
 * run off the panel a little at its peak, on purpose, the same "let clipping
 * do the work" reasoning the floor's own reach already leans on. What this
 * sweep still guards against is a MUCH larger blowout than that - a broken
 * projection running the curve off by hundreds of pixels rather than a
 * clipped loop tip - and it still demands EXACT fit before the grow starts
 * and after the settle finishes, which is the part that actually has to
 * look tidy. */
#define BOOT_ANIM_TEST_GROW_START_MS  BOOT_ANIM_TITLE_START_MS
#define BOOT_ANIM_TEST_SETTLED_MS \
    (BOOT_ANIM_TITLE_START_MS + BOOT_ANIM_SHRINK_GROW_MS + \
     BOOT_ANIM_SHRINK_SETTLE_MS)
#define BOOT_ANIM_TEST_MAX_TRANSIENT_OVERFLOW_PX 260

static void test_the_whole_scene_fits_on_the_panel_throughout_the_orbit(void)
{
    for (uint32_t t = 0; t <= BOOT_ANIM_MS; t += 60) {
        const bool settled = t <= BOOT_ANIM_TEST_GROW_START_MS ||
                             t >= BOOT_ANIM_TEST_SETTLED_MS;
        const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, t);
        const int32_t progress = boot_anim_pen(t);
        const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
        const int last = (int)(((int64_t)progress * span) >> BOOT_ANIM_Q);
        /* The finale shrinks the drawn curve toward the origin - see
         * boot_anim_motif_shrink_q8()'s own comment - so what actually has
         * to stay on panel is the SHRUNK picture boot_anim.c's csx()/csy()
         * produce, not the raw projection alone. */
        const int shrink_q8 = boot_anim_motif_shrink_q8(t);

        for (int i = 0; i <= last && i < BOOT_ANIM_CURVE_POINTS; i++) {
            const boot_anim_pt_t p = boot_anim_sample(i);
            const int rawx = boot_anim_screen_x(p.re, p.im, &view);
            const int rawy = boot_anim_screen_y(PANEL_H, p.re, p.im, p.t, &view);
            const int x = view.ox + (((rawx - view.ox) * shrink_q8) >> 8);
            const int y = view.oy + (((rawy - view.oy) * shrink_q8) >> 8);

            if (settled) {
                TEST_ASSERT_TRUE_MESSAGE(x >= 0 && x < PANEL_W,
                    "the drawn part of the curve ran off the side of the "
                    "panel outside the grow/settle transition");
                TEST_ASSERT_TRUE_MESSAGE(y >= 0 && y < PANEL_H,
                    "the drawn part of the curve ran off the top or bottom "
                    "of the panel outside the grow/settle transition");
                continue;
            }

            const int over_x = x < 0 ? -x : (x >= PANEL_W ? x - PANEL_W + 1 : 0);
            const int over_y = y < 0 ? -y : (y >= PANEL_H ? y - PANEL_H + 1 : 0);
            TEST_ASSERT_TRUE_MESSAGE(
                over_x <= BOOT_ANIM_TEST_MAX_TRANSIENT_OVERFLOW_PX,
                "the grown motif ran off the side of the panel by far more "
                "than a clipped loop tip");
            TEST_ASSERT_TRUE_MESSAGE(
                over_y <= BOOT_ANIM_TEST_MAX_TRANSIENT_OVERFLOW_PX,
                "the grown motif ran off the top or bottom of the panel by "
                "far more than a clipped loop tip");
        }
    }
}

/* The grid's own shrink shares its shape with boot_anim_motif_shrink_q8()
 * (same GROW/SETTLE timing - see boot_anim_shrink_to_floor_q8()) but
 * rests at a much bigger floor - see BOOT_ANIM_GRID_SHRINK_FLOOR_Q8's own
 * comment for why that floor has no panel-fit ceiling to check against
 * the way BOOT_ANIM_SHRINK_PEAK_Q8 does. What is worth checking here is
 * just the shape: full size before the grow starts, and settled at its
 * own floor - not the curve/axes' smaller one - once SETTLE finishes. */
static void test_the_grid_settles_at_its_own_bigger_floor(void)
{
    TEST_ASSERT_EQUAL_INT(256, boot_anim_grid_shrink_q8(0));
    TEST_ASSERT_EQUAL_INT(256, boot_anim_grid_shrink_q8(BOOT_ANIM_TITLE_START_MS));

    const uint32_t settled_ms = BOOT_ANIM_TITLE_START_MS +
        BOOT_ANIM_SHRINK_GROW_MS + BOOT_ANIM_SHRINK_SETTLE_MS;
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_GRID_SHRINK_FLOOR_Q8,
                          boot_anim_grid_shrink_q8(settled_ms));
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_GRID_SHRINK_FLOOR_Q8,
                          boot_anim_grid_shrink_q8(BOOT_ANIM_MS));
    TEST_ASSERT_TRUE_MESSAGE(
        BOOT_ANIM_GRID_SHRINK_FLOOR_Q8 > BOOT_ANIM_SHRINK_FLOOR_Q8,
        "the grid should settle noticeably bigger than the curve and axes");
}

/* The axis labels, checked only at t=0: that is where the axes are drawn at
 * their short, labelled length (see boot_anim.c's draw_axes(), which only
 * labels them before the finale starts turning them into unbounded lines),
 * and it is also the widest the short arms ever are. */
static void test_the_axis_labels_fit_on_the_panel(void)
{
    const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, 0);
    const int32_t arm = 4 * BOOT_ANIM_ONE;
    const int re_x = boot_anim_screen_x(arm, 0, &view);
    const int re_y = boot_anim_screen_y(PANEL_H, arm, 0, 0, &view);
    const int im_x = boot_anim_screen_x(0, arm, &view);
    const int im_y = boot_anim_screen_y(PANEL_H, 0, arm, 0, &view);

    TEST_ASSERT_TRUE_MESSAGE(re_x >= 0 && re_x < PANEL_W &&
                             re_y >= 0 && re_y < PANEL_H,
        "the end of the real axis is off the panel");
    TEST_ASSERT_TRUE_MESSAGE(im_x >= 0 && im_x < PANEL_W &&
                             im_y >= 0 && im_y < PANEL_H,
        "the end of the imaginary axis is off the panel");

    const int top = boot_anim_screen_y(
        PANEL_H, 0, 0, (BOOT_ANIM_T_MAX_PHASE1 + 1) << BOOT_ANIM_TQ, &view);
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
 *
 * tween_ramp()/tween_ease_out() themselves are tested in suite_tween.c now -
 * this file used to have its own copies under boot_anim_ramp()/
 * boot_anim_ease_out(), with their own tests, before both moved to
 * util/tween.h as shared vocabulary. What is left here is specific to how
 * boot_anim.h USES them, not the primitives themselves.
 *-------------------------------------------------------------------------*/

/* Phase 1 only - see boot_anim_pen()'s own "TWO PHASES" comment. It reaches
 * BOOT_ANIM_CURVE_PHASE1_FRACTION, not BOOT_ANIM_ONE, at the end of
 * BOOT_ANIM_PEN_MS now; test_the_pen_reaches_the_whole_curve_by_the_fade()
 * covers phase 2's own end. */
static void test_the_pen_runs_from_nothing_to_phase_ones_end(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(0));
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(BOOT_ANIM_PEN_START_MS));
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_CURVE_PHASE1_FRACTION,
        boot_anim_pen(BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS));
}

/* Phase 2: continues past phase 1's end rather than sitting still, and
 * reaches the whole curve by the time the fade begins - see
 * test_the_curve_is_finished_before_the_dissolve_starts() for that half,
 * kept as its own test since it is really a claim about the fade, not
 * about the pen. */
static void test_the_pen_keeps_climbing_through_phase_two(void)
{
    const uint32_t phase1_end_ms =
        BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS;
    const uint32_t mid_ms =
        (phase1_end_ms + BOOT_ANIM_FADE_START_MS) / 2;

    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_pen(mid_ms) > BOOT_ANIM_CURVE_PHASE1_FRACTION,
        "the pen should have climbed past where phase 1 left it");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_pen(mid_ms) < BOOT_ANIM_ONE,
        "the pen should not have reached the end of the curve yet");
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

/* Backdrop, not subject - the floor covers far more of the screen than the
 * curve does, so it is never let all the way up to the axes' own full 255.
 * The ceiling itself climbs though (see boot_anim_grid_climb()'s own
 * comment for why alpha has to climb alongside the whitening, not stay
 * fixed while only the colour moves) - so the bound checked here is the
 * per-moment BOOT_ANIM_GRID_CEILING_MAX, not the starting BOOT_ANIM_GRID_MAX. */
static void test_the_floor_stays_dim_enough_to_be_a_backdrop(void)
{
    for (uint32_t t = 0; t <= BOOT_ANIM_MS; t += 25) {
        for (int ring = 1; ring <= BOOT_ANIM_GRID_RINGS; ring++) {
            TEST_ASSERT_TRUE_MESSAGE(
                boot_anim_grid_alpha(t, ring) <= BOOT_ANIM_GRID_CEILING_MAX,
                "the floor got brighter than its cap");
        }
    }
}

/* The clock boot_anim_grid_alpha()'s own ceiling and boot_anim_grid_whiten()
 * both ride - starts flat at 0 before the floor appears, climbs steadily
 * and monotonically, and reaches its top exactly at BOOT_ANIM_MS. */
static void test_the_grid_climb_runs_the_whole_animation(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_climb(0));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_climb(BOOT_ANIM_GRID_START_MS));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_grid_climb(BOOT_ANIM_MS));

    uint8_t last = 0;
    for (uint32_t t = 0; t <= BOOT_ANIM_MS; t += 25) {
        const uint8_t v = boot_anim_grid_climb(t);
        TEST_ASSERT_TRUE_MESSAGE(v >= last,
            "the climb must never step backwards as time moves forward");
        last = v;
    }
}

/* The floor's own opacity ceiling starts at BOOT_ANIM_GRID_MAX and climbs
 * to BOOT_ANIM_GRID_CEILING_MAX alongside the whitening, rather than
 * sitting fixed while only the colour moves - see boot_anim_grid_climb()'s
 * own comment for why raising the colour alone was not enough. Checked
 * through the innermost ring's own alpha, since the ceiling itself is not
 * a separate exposed function. */
static void test_the_floor_opacity_climbs_alongside_its_colour(void)
{
    /* Ring 1's own fade-in (BOOT_ANIM_GRID_RING_MS + BOOT_ANIM_GRID_FADE_MS
     * after BOOT_ANIM_GRID_START_MS) is long done by either of these, plus
     * a little slack - so the only thing left changing its alpha between
     * them is the ceiling itself climbing, not "arrived" still ramping. */
    const uint32_t early_ms = BOOT_ANIM_GRID_START_MS +
        BOOT_ANIM_GRID_RING_MS + BOOT_ANIM_GRID_FADE_MS + 50;
    const uint8_t early = boot_anim_grid_alpha(early_ms, 1);
    const uint8_t late  = boot_anim_grid_alpha(BOOT_ANIM_MS, 1);
    TEST_ASSERT_TRUE_MESSAGE(late > early,
        "the floor's own opacity should climb over the animation, not "
        "just its colour");
}

/* Starts at 0 the moment the floor itself appears, climbs steadily, never
 * doubles back, and reaches its cap by the time everything else has faded
 * away. */
static void test_the_floor_whitens_steadily_from_its_own_start_to_a_cap(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_grid_whiten(BOOT_ANIM_GRID_START_MS));
    TEST_ASSERT_EQUAL_UINT8(BOOT_ANIM_GRID_WHITEN_MAX,
                            boot_anim_grid_whiten(BOOT_ANIM_MS));

    uint8_t last = 0;
    for (uint32_t t = BOOT_ANIM_GRID_START_MS; t <= BOOT_ANIM_MS; t += 25) {
        const uint8_t w = boot_anim_grid_whiten(t);
        TEST_ASSERT_TRUE_MESSAGE(w >= last,
            "whitening must never step backwards as time moves forward");
        last = w;
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

/*---------------------------------------------------------------------------
 * The title
 *-------------------------------------------------------------------------*/

static void test_the_wobble_is_exactly_flat_once_a_letter_has_arrived(void)
{
    TEST_ASSERT_EQUAL_INT(0, boot_anim_title_wobble(0));
    TEST_ASSERT_EQUAL_INT(0, boot_anim_title_wobble(-100));
}

/* THE test. Walk d from 1 (just setting off) down to 0 (arrived) and record
 * where the wobble crosses zero; the gaps between successive crossings must
 * strictly grow. A constant-frequency wobble whose AMPLITUDE merely shrinks
 * - the mistake this is checking was not made - would pass every other test
 * here and still be wrong: it would vibrate to a stop instead of settling. */
static void test_the_wobbles_oscillation_slows_as_it_lands(void)
{
    int32_t crossings[8];
    int n = 0;
    int prev_sign = 0;

    for (int32_t d = BOOT_ANIM_ONE; d >= 0 && n < 8; d -= 4) {
        const int y = boot_anim_title_wobble(d);
        const int sign = y > 0 ? 1 : (y < 0 ? -1 : 0);
        if (sign != 0 && prev_sign != 0 && sign != prev_sign) {
            crossings[n++] = d;
        }
        if (sign != 0) {
            prev_sign = sign;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(n >= 4,
        "expected several oscillations early in the flight to compare");

    /* crossings[] was recorded walking d DOWNWARD (1 toward 0), so a later
     * entry is a smaller d - the gap between consecutive entries is the
     * distance-in-d between crossings, and it must grow as d shrinks. */
    for (int i = 0; i < n - 2; i++) {
        const int32_t gap_earlier = crossings[i] - crossings[i + 1];
        const int32_t gap_later   = crossings[i + 1] - crossings[i + 2];
        TEST_ASSERT_TRUE_MESSAGE(gap_later > gap_earlier,
            "the gap between oscillations should grow as the letter "
            "approaches - a chirp, not a constant vibration fading out");
    }
}

/* "on its final position" now means the ARRIVAL wobble (see
 * boot_anim_title_wobble()) has fully decayed, not that the letter sits
 * dead still - boot_anim_title_wave() keeps a small idle motion going
 * forever, by design, so a landed letter's y is BOOT_ANIM_TITLE_VIEW_Y plus
 * whatever that wave says at this instant, not VIEW_Y alone. */
static void test_a_letter_lands_exactly_on_its_final_position(void)
{
    const uint32_t arrived = BOOT_ANIM_TITLE_START_MS +
                             BOOT_ANIM_TITLE_FLIGHT_MS + 1000;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const boot_anim_title_pos_t p =
            boot_anim_title_letter(i, arrived);
        const int expected_y = BOOT_ANIM_TITLE_VIEW_Y +
                               boot_anim_title_wave(i, arrived);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected_y, p.y,
            "a fully arrived letter should sit on the baseline plus "
            "whatever the idle wave says, with no residual ARRIVAL wobble "
            "left over");
    }
}

/* Each letter starts later than the one before it - "one after another",
 * not all six arriving as a block. */
static void test_letters_are_staggered_left_to_right(void)
{
    const uint32_t never = 0;   /* well before any letter has set off */

    /* Not -1: an off-panel starting position is legitimately a large
     * negative number (BOOT_ANIM_TITLE_ENTRY_PX or so to the left of the
     * panel), which a sentinel of -1 would already be greater than on the
     * very first letter. */
    int last_x = -100000;
    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const boot_anim_title_pos_t here =
            boot_anim_title_letter(i, never);
        TEST_ASSERT_TRUE_MESSAGE(here.x > last_x,
            "letters should be laid out left to right in their resting "
            "row, whatever moment they are drawn at");
        last_x = here.x;

        if (i > 0) {
            /* Halfway between letter (i-1)'s start and letter i's: (i-1)
             * has been moving for half a stagger interval, i has not moved
             * at all yet (tween_ramp() is exactly 0 until STRICTLY past
             * its own start, so checking AT i's start catches neither
             * letter moving - checking here is what actually exercises "an
             * earlier letter is further along"). */
            const uint32_t mid = BOOT_ANIM_TITLE_START_MS +
                                 (uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS -
                                 BOOT_ANIM_TITLE_STAGGER_MS / 2;
            const boot_anim_title_pos_t at =
                boot_anim_title_letter(i, mid);
            const boot_anim_title_pos_t prev_at =
                boot_anim_title_letter(i - 1, mid);
            TEST_ASSERT_TRUE_MESSAGE(prev_at.x >= at.x,
                "an earlier letter should be at least as far along as a "
                "later one at the same moment");
        }
    }
}

static void test_a_letter_starts_off_panel_to_the_left(void)
{
    const boot_anim_title_pos_t p =
        boot_anim_title_letter(0, BOOT_ANIM_TITLE_START_MS);
    TEST_ASSERT_TRUE_MESSAGE(p.x < 0,
        "a letter should begin off the left edge of the panel, not merely "
        "at it");
}

/* The layout guard, in the same spirit as
 * test_the_whole_scene_fits_on_the_panel_throughout_the_orbit(): every
 * letter, at every moment of its own flight including the wildest part of
 * the wobble, must land within the VIEWER's frame once it is actually
 * visible - BOOT_ANIM_TITLE_VIEW_W/H, not PANEL_W/PANEL_H, because
 * boot_anim_title_letter() lays the word out in that frame now (see its own
 * comment in boot_anim.h) and boot_anim.c's draw_title() is what turns it
 * into a panel coordinate afterward - a step this test does not need to
 * repeat, since a letter kept inside its own frame here stays on the panel
 * there by construction. */
static void test_the_title_stays_on_the_panel_once_visible(void)
{
    /* The full glyph cell, not just its anchor corner - (x, y) is where a
     * glyph's cell BEGINS, so the cell's far edge is what actually has to
     * stay on the panel. */
    const int cell = 8 * BOOT_ANIM_TITLE_SCALE;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const uint32_t start = BOOT_ANIM_TITLE_START_MS +
                               (uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS;
        for (uint32_t t = start; t <= start + BOOT_ANIM_TITLE_FLIGHT_MS;
             t += 15) {
            const boot_anim_title_pos_t p =
                boot_anim_title_letter(i, t);
            if (p.x + cell < 0) {
                continue;   /* still off-panel to the left - not visible yet */
            }
            TEST_ASSERT_TRUE_MESSAGE(p.x + cell <= BOOT_ANIM_TITLE_VIEW_W,
                "a letter drifted off the right edge of the viewer's frame");
            TEST_ASSERT_TRUE_MESSAGE(
                p.y >= 0 && p.y + cell < BOOT_ANIM_TITLE_VIEW_H,
                "a letter's wobble carried it off the top or bottom of "
                "the viewer's frame");
        }
    }
}

void run_boot_anim_suite(void)
{
    RUN_TEST(test_the_curve_climbs_from_zero_to_the_top);
    RUN_TEST(test_the_curve_never_descends);
    RUN_TEST(test_the_curve_meets_the_axis_at_every_known_zero);
    RUN_TEST(test_the_curve_keeps_away_from_the_axis_everywhere_else);
    RUN_TEST(test_every_listed_zero_is_on_the_climb_and_in_order);
    RUN_TEST(test_samples_are_clamped_rather_than_read_out_of_range);

    RUN_TEST(test_the_quarter_wave_starts_at_zero_and_ends_at_one);
    RUN_TEST(test_the_quarter_wave_rises_all_the_way);
    RUN_TEST(test_sin_squared_plus_cos_squared_is_one);
    RUN_TEST(test_the_quarter_points_are_exact);
    RUN_TEST(test_the_view_starts_at_the_original_fixed_camera);
    RUN_TEST(test_the_view_foreshortens_t_by_the_end_of_the_curve);
    RUN_TEST(test_the_finale_turns_t_back_toward_vertical);
    RUN_TEST(test_the_finale_settles_the_origin_at_its_view_target);
    RUN_TEST(test_the_origin_ends_at_the_mirror_of_where_it_started);
    RUN_TEST(test_the_origin_drifts_upward_without_doubling_back);

    RUN_TEST(test_the_origin_maps_to_the_origin);
    RUN_TEST(test_the_three_axes_point_the_way_they_are_supposed_to);
    RUN_TEST(test_the_imaginary_axis_is_at_forty_five_degrees);
    RUN_TEST(test_the_whole_scene_fits_on_the_panel_throughout_the_orbit);
    RUN_TEST(test_the_grid_settles_at_its_own_bigger_floor);
    RUN_TEST(test_the_axis_labels_fit_on_the_panel);

    RUN_TEST(test_a_span_starts_and_ends_halfway_between_its_points);
    RUN_TEST(test_a_repeated_point_pins_the_end_of_the_curve);
    RUN_TEST(test_a_span_never_leaves_its_control_points_behind);
    RUN_TEST(test_a_span_climbs_steadily_when_its_points_do);

    RUN_TEST(test_the_pen_runs_from_nothing_to_phase_ones_end);
    RUN_TEST(test_the_pen_keeps_climbing_through_phase_two);
    RUN_TEST(test_the_curve_is_finished_before_the_dissolve_starts);
    RUN_TEST(test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end);
    RUN_TEST(test_the_floor_fades_in_from_the_origin_outward);
    RUN_TEST(test_the_floor_fades_out_with_distance_rather_than_stopping);
    RUN_TEST(test_the_floor_stays_dim_enough_to_be_a_backdrop);
    RUN_TEST(test_the_grid_climb_runs_the_whole_animation);
    RUN_TEST(test_the_floor_opacity_climbs_alongside_its_colour);
    RUN_TEST(test_the_floor_whitens_steadily_from_its_own_start_to_a_cap);
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

    RUN_TEST(test_the_wobble_is_exactly_flat_once_a_letter_has_arrived);
    RUN_TEST(test_the_wobbles_oscillation_slows_as_it_lands);
    RUN_TEST(test_a_letter_lands_exactly_on_its_final_position);
    RUN_TEST(test_letters_are_staggered_left_to_right);
    RUN_TEST(test_a_letter_starts_off_panel_to_the_left);
    RUN_TEST(test_the_title_stays_on_the_panel_once_visible);
}

SUITE_REGISTER(run_boot_anim_suite);
