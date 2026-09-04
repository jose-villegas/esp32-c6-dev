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
#include "gfx/fonts/font_lmroman_40.h"

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

/* A plain identity matrix (small3dlib's own S3L_mat4Init()) and an
 * orthographic focal length (0 - see boot_anim.h's "The projection" section
 * on why 0 means that) - the simplest boot_anim_view_t there is, built
 * directly rather than through boot_anim_view()/the keyframe table, so
 * these tests can check boot_anim_project()'s own arithmetic in isolation
 * from whatever the CURRENT seed keyframes happen to say. */
static boot_anim_view_t identity_view(S3L_Unit focal)
{
    boot_anim_view_t v;
    S3L_mat4Init(v.matrix);
    v.focal = focal;
    return v;
}

/* The space's own local origin (0,0,0) has to land at the screen's centre
 * under an identity transform and an orthographic projection - no camera
 * offset, no rotation, no depth-dependent scale to reason about, just
 * small3dlib's own S3L_mapProjectionPlaneToScreen() centring a (0,0) point.
 * The most basic thing boot_anim_project() has to get right. */
static void test_identity_transform_leaves_the_origin_at_screen_centre(void)
{
    const boot_anim_view_t view = identity_view(0);
    int x, y;
    boot_anim_project(0, 0, 0, &view, &x, &y);

    TEST_ASSERT_EQUAL_INT_MESSAGE(PANEL_W / 2, x,
        "the origin should land exactly on screen centre under an "
        "identity transform");
    TEST_ASSERT_EQUAL_INT_MESSAGE(PANEL_H / 2, y,
        "the origin should land exactly on screen centre under an "
        "identity transform");
}

/* The one property that actually distinguishes perspective from the old
 * axonometric projection: a point further from the camera has to project
 * SMALLER (closer to screen centre) than the same point nearer the camera,
 * for a real focal length. re/im map to X/Z (see boot_anim_project()'s own
 * comment on the axis mapping) - im is depth here, re is the offset being
 * compared at two different depths. */
static void test_a_point_further_from_the_camera_projects_smaller(void)
{
    const boot_anim_view_t view = identity_view(S3L_F);
    const int32_t re = 2 * BOOT_ANIM_ONE;
    const int32_t near_im = 1 * BOOT_ANIM_ONE;
    const int32_t far_im  = 4 * BOOT_ANIM_ONE;
    int x_near, y_near, x_far, y_far;

    boot_anim_project(re, near_im, 0, &view, &x_near, &y_near);
    boot_anim_project(re, far_im, 0, &view, &x_far, &y_far);

    const int centre = PANEL_W / 2;
    const int near_offset = (x_near > centre) ? x_near - centre : centre - x_near;
    const int far_offset  = (x_far  > centre) ? x_far  - centre : centre - x_far;

    TEST_ASSERT_TRUE_MESSAGE(far_offset < near_offset,
        "a point further from the camera should project closer to screen "
        "centre than the same point nearer the camera");
}

/* boot_anim_project_point()'s entire reason to exist over plain
 * boot_anim_project() - see its own comment in boot_anim.h - is refusing
 * to write anything for a point at or behind the near plane, rather than
 * projecting it to an ordinary-looking but geometrically nonsense screen
 * position. Pinned right AT the boundary rather than somewhere clearly
 * behind it - `im_q12 = 408` is chosen so that, under an identity
 * transform, BOOT_ANIM_ZETA_TO_S3L(408) = 408 >> 3 = 51 = BOOT_ANIM_
 * NEAR_Z exactly - so this specifically exercises the `<=`, not just
 * "somewhere behind", which a `<` typo in the real check would still
 * pass at a point further back. */
static void test_project_point_rejects_a_point_at_the_near_plane(void)
{
    const boot_anim_view_t view = identity_view(S3L_F);
    int x = -1, y = -1;

    const bool ok = boot_anim_project_point(0, 408, 0, &view, &x, &y);

    TEST_ASSERT_FALSE_MESSAGE(ok,
        "a point exactly at the near plane must be rejected, not "
        "projected to a nonsense screen position");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, x,
        "a rejected point's output x must be left untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, y,
        "a rejected point's output y must be left untouched");
}

/* The OTHER early-out in boot_anim_project_segment_cs() besides the clip
 * itself (see test_project_segment_cs_clips_asymmetric_coordinates below
 * for that branch): a segment with BOTH endpoints at or behind the near
 * plane has nothing in front of the camera to draw at all, and must
 * return false outright rather than clip against itself. */
static void test_project_segment_cs_rejects_a_segment_entirely_behind(void)
{
    const boot_anim_view_t view = identity_view(S3L_F);
    const S3L_Vec4 p0 = { 100, 200, 0, S3L_F };
    const S3L_Vec4 p1 = { -100, -200, BOOT_ANIM_NEAR_Z, S3L_F };
    int ax, ay, bx, by;

    const bool ok =
        boot_anim_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by);

    TEST_ASSERT_FALSE_MESSAGE(ok,
        "a segment with both endpoints at or behind the near plane "
        "should be rejected entirely, not clipped against itself");
}

/* boot_anim_project_segment_cs()'s near-plane clip, at asymmetric,
 * deliberately-not-a-clean-fraction-of-512 coordinates - the exact case
 * the Q16 rewrite (see this function's own comment in boot_anim.h)
 * exists for: a clip fraction that lands nowhere near a multiple of
 * 1/S3L_F (1/512) is precisely where the OLD precision rounded the
 * worst. Verified against an independent double-precision reference
 * computed right here, not against whatever the function under test
 * happens to produce - worked out once BY RUNNING IT (temporarily
 * reverting the Q16 fix and forcing a zero-tolerance assertion to read
 * the exact numbers back - see this repo's own "watch it fail before it
 * passes" testing convention), not by hand, after an earlier hand
 * calculation here turned out to be wrong:
 *
 *   clip fraction = 86/496 = 0.17338...
 *   this test's tolerance (20px) comfortably contains the Q16 result's
 *   own error against the double-precision reference (7px, 0px) while
 *   still rejecting the OLD S3L_F(512)-precision result for the exact
 *   same inputs (3831px, 2352px off) - the actual regression this test
 *   protects against, even though the old code no longer exists to call. */
static void test_project_segment_cs_clips_asymmetric_coordinates(void)
{
    const boot_anim_view_t view = identity_view(S3L_F);
    const S3L_Vec4 p0 = { -300123,  250009, BOOT_ANIM_NEAR_Z - 86, S3L_F };
    const S3L_Vec4 p1 = {  401777, -180321, BOOT_ANIM_NEAR_Z + 410, S3L_F };

    int ax, ay, bx, by;
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by),
        "a segment with one endpoint in front of the near plane should "
        "always project");

    /* Double-precision reference for the clip itself: p0 is BEHIND, so
     * IT is what gets replaced by the near-plane crossing point. */
    const double frac =
        (double)(BOOT_ANIM_NEAR_Z - p0.z) / (double)(p1.z - p0.z);
    const double exact_x = p0.x + (p1.x - p0.x) * frac;
    const double exact_y = p0.y + (p1.y - p0.y) * frac;
    int ex, ey;
    const S3L_Vec4 exact_clip = {
        (S3L_Unit)exact_x, (S3L_Unit)exact_y, BOOT_ANIM_NEAR_Z, S3L_F };
    boot_anim_camera_to_screen(exact_clip, view.focal, &ex, &ey);

    const int tolerance = 20;
    TEST_ASSERT_INT_WITHIN_MESSAGE(tolerance, ex, ax,
        "the clipped endpoint's screen x should be close to a "
        "double-precision reference - a wide miss here is exactly the "
        "S3L_F-precision rounding this test guards against");
    TEST_ASSERT_INT_WITHIN_MESSAGE(tolerance, ey, ay,
        "the clipped endpoint's screen y should be close to a "
        "double-precision reference - a wide miss here is exactly the "
        "S3L_F-precision rounding this test guards against");

    /* p1 was already in front - not clipped at all, so it must match
     * projecting it directly, independent of whatever the clip branch
     * above did. */
    int fx, fy;
    S3L_Vec4 p1_copy = p1;
    boot_anim_camera_to_screen(p1_copy, view.focal, &fx, &fy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(fx, bx,
        "the untouched (already in front) endpoint should project "
        "identically whether reached through the clipping path or not");
    TEST_ASSERT_EQUAL_INT_MESSAGE(fy, by,
        "the untouched (already in front) endpoint should project "
        "identically whether reached through the clipping path or not");
}

/* boot_anim_spoke_reveal_target()'s own two guaranteed endpoints,
 * regardless of where its internal linear/reciprocal switchover (`r0`)
 * happens to fall for a given near/far pair: reach=0 must stay exactly at
 * the origin (nothing drawn yet), and reach=255 must land exactly on
 * `far` (a spoke actually finishes where it was told to, not asymptotic-
 * ally close). Both are relied on directly by draw_grid_spoke() in
 * boot_anim.c - see its own comment on `near_target`/`target`. */
static void test_spoke_reveal_target_hits_its_endpoints_exactly(void)
{
    const int32_t near = 10 * BOOT_ANIM_ONE;
    const int32_t far  = 500 * BOOT_ANIM_ONE;

    TEST_ASSERT_EQUAL_INT32(0, boot_anim_spoke_reveal_target(near, far, 0));
    TEST_ASSERT_EQUAL_INT32(far,
        boot_anim_spoke_reveal_target(near, far, 255));
}

/* The whole point of switching to reciprocal interpolation (see this
 * function's own comment in boot_anim.h) is keeping ON-SCREEN growth
 * roughly even - which this test cannot see a screen to check directly,
 * but 1/target advancing in roughly EQUAL steps as `reach` advances in
 * equal steps is the exact algebraic property that guarantees it (screen
 * position is itself roughly proportional to 1/target under a
 * perspective projection - see boot_anim_camera_to_screen()). Checked as
 * a difference-of-differences bound rather than exact equality: the
 * reach-to-r0 knot and integer rounding both perturb it slightly, so
 * "each step's own shrinkage is within a small tolerance of the next
 * step's" is the property that actually matters, not bit-exact evenness. */
static void test_spoke_reveal_target_advances_evenly_in_screen_space(void)
{
    const int32_t near = 10 * BOOT_ANIM_ONE;
    const int32_t far  = 500 * BOOT_ANIM_ONE;
    const int steps = 10;

    /* Starts at i=1 (reach>0), not 0 - reach=0's own target=0 is already
     * covered exactly by test_spoke_reveal_target_hits_its_endpoints_
     * exactly() above, and 1/0 has no meaningful "screen position" to
     * compare against the next step's here. */
    double prev_inv = -1.0;
    double prev_delta = 0.0;
    for (int i = 1; i <= steps; i++) {
        const uint8_t reach = (uint8_t)((255 * i) / steps);
        const int32_t target =
            boot_anim_spoke_reveal_target(near, far, reach);
        TEST_ASSERT_TRUE_MESSAGE(target > 0,
            "every non-zero reach should produce a positive target");
        const double inv = 1.0 / (double)target;
        if (prev_inv >= 0.0) {
            const double delta = prev_inv - inv;
            TEST_ASSERT_TRUE_MESSAGE(delta >= 0.0,
                "1/target should never increase as reach climbs - the "
                "target itself must be monotonically non-decreasing");
            if (i > 2) {
                /* prev_delta == 0 would mean the PRIOR step had zero
                 * shrinkage - a plateau, not something these constants
                 * produce, but an explicit message beats an inf/NaN
                 * ratio silently failing the comparison below for a
                 * reason this test's own output does not explain. */
                TEST_ASSERT_TRUE_MESSAGE(prev_delta > 0.0,
                    "the previous reach step had zero shrinkage in "
                    "1/target - a plateau these test constants should "
                    "never actually produce");
                /* Consecutive per-step shrinkages should stay within 25%
                 * of each other - loose on purpose (this is a fixed-point
                 * approximation, not exact reciprocal interpolation), but
                 * tight enough that the OLD linear-in-radius formula (whose
                 * first step alone covers ~90% of the total 1/near-to-
                 * 1/far span) would fail it outright. */
                const double ratio = delta / prev_delta;
                TEST_ASSERT_TRUE_MESSAGE(ratio > 0.75 && ratio < 1.25,
                    "consecutive reach steps should shrink 1/target by "
                    "roughly the same amount - a front-loaded reveal "
                    "would fail this");
            }
            prev_delta = delta;
        }
        prev_inv = inv;
    }
}

/* "Scale renders as 1,1,1 for default values" - the exact ask this table
 * exists to satisfy (see boot_anim_timeline.json's own comment): an
 * untouched keyframe's space scale has to read back as S3L_F (small3dlib's
 * own 1.0), on every axis, not some other number that happens to look
 * right today. */
static void test_an_untouched_keyframes_scale_reads_back_as_identity(void)
{
    const boot_anim_timeline_state_t st =
        boot_anim_timeline_sample(boot_anim_keyframes[0].ms);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(S3L_F, st.space.scale.x,
        "an unscaled keyframe's space.scale.x should read back as 1.0");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(S3L_F, st.space.scale.y,
        "an unscaled keyframe's space.scale.y should read back as 1.0");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(S3L_F, st.space.scale.z,
        "an unscaled keyframe's space.scale.z should read back as 1.0");
}

/*---------------------------------------------------------------------------
 * The seed keyframes
 *
 * Unlike the old fixed axonometric projection - three compile-time screen
 * directions, provably fitting the panel for any camera angle - a real,
 * freely keyframed 3D camera has no such blanket guarantee: point it the
 * wrong way and the scene is off-panel, which is a legitimate thing a
 * creative edit can do, not a bug in the projection. What is still worth
 * protecting is the SEED this repo ships - a sanity sweep against the
 * actual committed boot_anim_keyframes[], not a property of the projection
 * in general. A generous margin, not a tight fit: this catches "the seed
 * is now wildly broken", not "the seed could be tuned tighter".
 *-------------------------------------------------------------------------*/

#define BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE 3

static void test_the_seeds_curve_stays_near_the_panel_throughout(void)
{
    for (uint32_t t = 0; t <= BOOT_ANIM_MS; t += 100) {
        const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, t);
        const int32_t progress = boot_anim_pen(t);
        const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
        const int last = (int)(((int64_t)progress * span) >> BOOT_ANIM_Q);

        /* The head (furthest-drawn point) alone is enough to catch a
         * camera pointed somewhere absurd - checking every sample every
         * 100ms as well would be thousands of assertions for the same
         * signal. */
        if (last < 0 || last >= BOOT_ANIM_CURVE_POINTS) {
            continue;
        }
        const boot_anim_pt_t p = boot_anim_sample(last);
        int x, y;
        boot_anim_project(p.re, p.im, p.t, &view, &x, &y);

        TEST_ASSERT_TRUE_MESSAGE(
            x > -PANEL_W * BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE &&
            x <  PANEL_W * (BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE + 1),
            "the seed's camera has drifted wildly off in X - not just "
            "off-panel, off by panel-widths");
        TEST_ASSERT_TRUE_MESSAGE(
            y > -PANEL_H * BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE &&
            y <  PANEL_H * (BOOT_ANIM_TEST_MAX_PANEL_MULTIPLE + 1),
            "the seed's camera has drifted wildly off in Y - not just "
            "off-panel, off by panel-heights");
    }
}

/* The three axes, projected from the seed's own first keyframe, must not
 * collapse onto each other - a camera that happened to look straight down
 * one of them would still "work" in the sense of not crashing, but the
 * picture would read as two lines, not three. A weak, non-fragile check on
 * purpose: it is not this test's job to say WHERE the axes should point
 * (that is a creative choice made through the editor now), only that they
 * are not degenerate. */
static void test_the_seeds_three_axes_project_to_distinct_directions(void)
{
    const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, 0);
    const int32_t one = BOOT_ANIM_ONE;
    int ox, oy, rx, ry, ix, iy, tx, ty;

    boot_anim_project(0, 0, 0, &view, &ox, &oy);
    boot_anim_project(one, 0, 0, &view, &rx, &ry);
    boot_anim_project(0, one, 0, &view, &ix, &iy);
    boot_anim_project(0, 0, 1 << BOOT_ANIM_TQ, &view, &tx, &ty);

    TEST_ASSERT_FALSE_MESSAGE(rx == ix && ry == iy,
        "the real and imaginary axes should not project to the same point");
    TEST_ASSERT_FALSE_MESSAGE(rx == tx && ry == ty,
        "the real and t axes should not project to the same point");
    TEST_ASSERT_FALSE_MESSAGE(ix == tx && iy == ty,
        "the imaginary and t axes should not project to the same point");
    TEST_ASSERT_FALSE_MESSAGE(rx == ox && ry == oy,
        "the real axis should not collapse onto the origin");
}

/*---------------------------------------------------------------------------
 * The wave
 *-------------------------------------------------------------------------*/

/* `amp_q12`/`wavelength_q12`/`period_ms` are fabricated here, not read
 * from BOOT_ANIM_WAVE_HEIGHT_Q12/WAVELENGTH_Q12/PERIOD_MS - see
 * boot_anim_wave_height()'s own comment on why it takes all three as
 * parameters: the shipped seed's own height is 0 (off by default -
 * test_the_seeds_wave_is_coherently_authored() below is what actually
 * checks that), which at 0 would make every one of these assertions trivially
 * true for the wrong reason. */
static void test_wave_height_is_zero_when_the_amplitude_is_zero(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_wave_height(
        BOOT_ANIM_ONE, 0, 0, BOOT_ANIM_ONE, 1000));
}

static void test_wave_height_is_zero_when_the_wavelength_is_not_positive(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_wave_height(
        BOOT_ANIM_ONE, 0, BOOT_ANIM_ONE, 0, 1000));
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_wave_height(
        BOOT_ANIM_ONE, 0, BOOT_ANIM_ONE, -1, 1000));
}

/* sin() is exactly periodic in r for a fixed t - a vertex a full
 * wavelength further out should read back the identical height. This is
 * the structural fact that actually lets several rings show the ripple's
 * own crests/troughs at once, all from the one formula, rather than
 * needing a moving front to explain which rings are "lit" yet (see
 * boot_anim.h's "The wave" section). */
static void test_wave_height_is_periodic_in_radius(void)
{
    const int32_t wavelength = 3 * BOOT_ANIM_ONE;
    const int32_t amp_q12 = BOOT_ANIM_ONE;
    const int32_t r = 2 * BOOT_ANIM_ONE;

    const int32_t a = boot_anim_wave_height(r, 0, amp_q12, wavelength, 0);
    const int32_t b = boot_anim_wave_height(r + wavelength, 0, amp_q12,
                                            wavelength, 0);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(a, b,
        "a vertex a full wavelength further out should be at the exact "
        "same point in the same crest/trough cycle");
}

/* Also exactly periodic in TIME, for a fixed r - one full period_ms
 * brings the pattern back to where it started. */
static void test_wave_height_is_periodic_in_time(void)
{
    const int32_t wavelength = 3 * BOOT_ANIM_ONE;
    const int32_t amp_q12 = BOOT_ANIM_ONE;
    const int32_t r = 2 * BOOT_ANIM_ONE;
    const uint32_t period_ms = 1000;

    const int32_t a = boot_anim_wave_height(r, 250, amp_q12, wavelength,
                                            period_ms);
    const int32_t b = boot_anim_wave_height(r, 250 + period_ms, amp_q12,
                                            wavelength, period_ms);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(a, b,
        "a full period later, the same vertex should be back at the same "
        "height");
}

/* The travelling look itself: the same point in the crest/trough cycle
 * that is a quarter wavelength closer to the origin right now is exactly
 * where THIS vertex will be a quarter period from now - see
 * boot_anim_wave_height()'s own comment on why subtracting the time term
 * is what makes a crest's own radius grow with time, the pattern moving
 * outward rather than inward. */
static void test_wave_height_travels_outward_with_time(void)
{
    const int32_t wavelength = 4 * BOOT_ANIM_ONE;
    const int32_t amp_q12 = BOOT_ANIM_ONE;
    const uint32_t period_ms = 4000;
    const int32_t r = 10 * BOOT_ANIM_ONE;

    const int32_t at_r_a_quarter_period_later = boot_anim_wave_height(
        r, period_ms / 4, amp_q12, wavelength, period_ms);
    const int32_t a_quarter_wavelength_closer_right_now =
        boot_anim_wave_height(r - wavelength / 4, 0, amp_q12, wavelength,
                              period_ms);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(
        at_r_a_quarter_period_later, a_quarter_wavelength_closer_right_now,
        "a quarter period from now, this vertex should read the way a "
        "vertex a quarter wavelength closer to the origin reads right now "
        "- the pattern travels OUTWARD as time advances");
}

/* period_ms of 0 is a legitimate, if unusual, choice - a static ripple
 * that never travels - not a division by zero. */
static void test_wave_height_is_frozen_when_the_period_is_zero(void)
{
    const int32_t wavelength = 3 * BOOT_ANIM_ONE;
    const int32_t amp_q12 = BOOT_ANIM_ONE;
    const int32_t r = 2 * BOOT_ANIM_ONE;

    const int32_t at_t0 = boot_anim_wave_height(r, 0, amp_q12, wavelength, 0);
    const int32_t at_t_later =
        boot_anim_wave_height(r, 999999, amp_q12, wavelength, 0);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(at_t0, at_t_later,
        "a period of 0 should freeze the pattern's own time term rather "
        "than crash or drift");
}

/* The seed this repo ships has the ripple authored off (BOOT_ANIM_WAVE_
 * HEIGHT_Q12 == 0) until someone turns it up through the editor - not a
 * claim boot_anim_wave_height() itself makes (see its own comment), so
 * worth its own test the way test_the_seed_finishes_the_curve_before_
 * the_dissolve_starts() already checks a different seed-specific fact. */
/* This used to assert the seed's own wave height was 0 - true when the
 * ripple was a brand new knob nobody had authored yet, and false the
 * moment anyone tuned one in, which is exactly what happened. A seed
 * value an author is expected to change is not a fact worth pinning; what
 * IS worth pinning is that whatever they tune stays COHERENT, since
 * boot_anim_wave_height() reads all three numbers together and a nonzero
 * height against a zero wavelength or period is the combination that
 * silently draws nothing (see that function's own early-out). Holds
 * whether the ripple is authored on or off. */
static void test_the_seeds_wave_is_coherently_authored(void)
{
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_WAVE_HEIGHT_Q12 >= 0,
        "a negative wave height would flip every crest into a trough");
    if (BOOT_ANIM_WAVE_HEIGHT_Q12 != 0) {
        TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_WAVE_WAVELENGTH_Q12 > 0,
            "the seed authors a wave height but no wavelength - "
            "boot_anim_wave_height() would return flat and the ripple "
            "would silently never appear");
        TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_WAVE_PERIOD_MS > 0,
            "the seed authors a wave height but no period - the ripple "
            "would be frozen rather than travelling");
    }
}

/* boot_anim_wave_envelope() takes no parameters of its own to fabricate -
 * unlike boot_anim_wave_height() above, IT is the timeline, the same
 * reason boot_anim_grid_alpha() reads BOOT_ANIM_GRID_START_MS/RING_MS/
 * FADE_MS directly rather than taking them as arguments - so these test
 * against the seed's own real generated values (BOOT_ANIM_WAVE_IN_MS,
 * BOOT_ANIM_WAVE_OUT_MS, BOOT_ANIM_MS), the same way
 * test_wave_front_starts_at_the_origin... used to before this rewrite. */
static void test_wave_envelope_is_zero_at_the_very_start(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_wave_envelope(0));
}

/* The whole point of wave_in_ms being a MOMENT rather than a duration:
 * the ripple stays muted right up to and including that moment itself -
 * an author controls exactly when it is allowed to start, not merely how
 * long a ramp beginning at frame 0 takes. */
static void test_wave_envelope_is_still_muted_at_wave_in_ms_itself(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_wave_envelope(BOOT_ANIM_WAVE_IN_MS));
}

static void test_wave_envelope_reaches_full_strength_after_the_ramp(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_wave_envelope(
        BOOT_ANIM_WAVE_IN_MS + BOOT_ANIM_WAVE_ENVELOPE_RAMP_MS));
}

static void test_wave_envelope_plateaus_between_in_and_out(void)
{
    const uint32_t midpoint =
        (BOOT_ANIM_WAVE_IN_MS + BOOT_ANIM_WAVE_OUT_MS) / 2;

    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_wave_envelope(midpoint));
}

/* Symmetric with test_wave_envelope_is_still_muted_at_wave_in_ms_itself
 * above: still at FULL strength right at wave_out_ms itself - that moment
 * is when the fade-out ramp starts, not when it has already finished. */
static void test_wave_envelope_is_still_full_at_wave_out_ms_itself(void)
{
    TEST_ASSERT_EQUAL_UINT8(255,
        boot_anim_wave_envelope(BOOT_ANIM_WAVE_OUT_MS));
}

/* The title's idle wave calming down - a different wave entirely from the
 * floor ripple above (see boot_anim_title_wave()), but the same "255 minus
 * a ramp" envelope, so the same three moments are worth pinning: full
 * swing before it starts, still full AT the start (that instant is when
 * the ramp begins, not when it has finished), and nothing left once the
 * fade window has passed. */
static void test_the_title_wave_is_at_full_swing_before_it_calms(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_title_wave_reach(0));
    TEST_ASSERT_EQUAL_UINT8(255,
        boot_anim_title_wave_reach(BOOT_ANIM_TITLE_WAVE_OUT_MS));
}

static void test_the_title_wave_reaches_stillness_after_its_fade(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_title_wave_reach(
        BOOT_ANIM_TITLE_WAVE_OUT_MS + BOOT_ANIM_TITLE_WAVE_FADE_MS));
}

/* The point of scaling the AMPLITUDE rather than gating the wave off: the
 * motion has to shrink monotonically through the window, so a letter
 * caught mid-bob rides its own arc down instead of snapping straight. The
 * envelope is what carries that, so it is what gets swept - the wave
 * itself keeps oscillating underneath and would not be monotonic. */
static void test_the_title_wave_never_swings_wider_as_it_calms(void)
{
    const uint32_t out = BOOT_ANIM_TITLE_WAVE_OUT_MS;
    const uint32_t fade = BOOT_ANIM_TITLE_WAVE_FADE_MS;
    int prev = 256;

    for (uint32_t t = out; t <= out + fade; t += (fade / 32) + 1) {
        const int now = boot_anim_title_wave_reach(t);
        TEST_ASSERT_TRUE_MESSAGE(now <= prev,
            "the title wave's own envelope grew back while it was "
            "supposed to be calming");
        prev = now;
    }
}

/* Measured from BOOT_ANIM_WAVE_OUT_MS, not from BOOT_ANIM_MS: the
 * envelope's contract is that it reaches zero one ramp after the fade-out
 * STARTS, and whether that lands before the animation's own end is a
 * timeline choice, not a property of this function. The seed currently
 * parks wave_out_ms exactly at total_ms - so the ripple is still at full
 * strength on the last frame, which is fine because boot_anim_ink() has
 * been taking the whole picture to black since fade_start_ms long before
 * then. Asserting against BOOT_ANIM_MS, as this used to, made the test a
 * hostage to that one authoring decision. */
static void test_wave_envelope_fades_back_to_zero_after_its_ramp(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_wave_envelope(
        BOOT_ANIM_WAVE_OUT_MS + BOOT_ANIM_WAVE_ENVELOPE_RAMP_MS));
}

/* The reach's own endpoints, read off whatever the seed authors rather
 * than off the all-zero default it used to ship. That default ("every
 * spoke at full length instantly", the behaviour from before either field
 * existed - see gen_boot_anim_timeline.py's own comment) is still covered
 * here, because a start and draw of 0 make the two assertions below land
 * on 0ms and 1ms exactly as the old test did; a seed that animates its
 * spokes instead simply moves where they are sampled, which is the point.
 *
 * tween_ramp() is what makes the second one exact at the boundary rather
 * than merely close - see its own comment on reaching 255 AT
 * start + duration, not one millisecond after. */
static void test_the_seeds_spokes_reach_their_full_length(void)
{
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0,
        boot_anim_grid_spoke_reach(BOOT_ANIM_GRID_SPOKE_START_MS),
        "a spoke should have no length at all before its own draw begins");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(255,
        boot_anim_grid_spoke_reach(BOOT_ANIM_GRID_SPOKE_START_MS +
                                   BOOT_ANIM_GRID_SPOKE_DRAW_MS + 1),
        "a spoke should be at full length once its own draw window has "
        "passed");
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

/* boot_anim_spline_cs()'s whole reason to exist: transforming three control
 * points to camera space and THEN interpolating them must land on the same
 * point (modulo fixed-point rounding order, not a real difference) as
 * interpolating in world space and THEN transforming - draw_curve() relies
 * on this to skip a full matrix transform on every drawn sub-point. Checked
 * against a real, non-identity view (rotated, translated, perspective) -
 * an identity transform would not catch a bug that only shows up once
 * translation and rotation are actually mixed in. */
static void test_spline_cs_matches_transforming_the_world_space_spline(void)
{
    const boot_anim_view_t view = boot_anim_view(PANEL_W, PANEL_H, CURVE_DONE_MS);
    const boot_anim_pt_t a = pt(-3000, 1500, 200);
    const boot_anim_pt_t b = pt(2500, -1800, 900);
    const boot_anim_pt_t c = pt(600, 3200, 1600);

    const S3L_Vec4 ta = boot_anim_to_camera_space(a.re, a.im, a.t, &view);
    const S3L_Vec4 tb = boot_anim_to_camera_space(b.re, b.im, b.t, &view);
    const S3L_Vec4 tc = boot_anim_to_camera_space(c.re, c.im, c.t, &view);

    for (int32_t t = 0; t <= BOOT_ANIM_ONE; t += 197) {
        const boot_anim_pt_t world = boot_anim_spline(a, b, c, t);
        const S3L_Vec4 want =
            boot_anim_to_camera_space(world.re, world.im, world.t, &view);
        const S3L_Vec4 got = boot_anim_spline_cs(ta, tb, tc, t);

        TEST_ASSERT_INT32_WITHIN_MESSAGE(2, want.x, got.x,
            "transform-then-interpolate must match interpolate-then-"
            "transform, up to fixed-point rounding order");
        TEST_ASSERT_INT32_WITHIN_MESSAGE(2, want.y, got.y,
            "transform-then-interpolate must match interpolate-then-"
            "transform, up to fixed-point rounding order");
        TEST_ASSERT_INT32_WITHIN_MESSAGE(2, want.z, got.z,
            "transform-then-interpolate must match interpolate-then-"
            "transform, up to fixed-point rounding order");
    }
}

/*---------------------------------------------------------------------------
 * Basic level of detail
 *-------------------------------------------------------------------------*/

/* Two points far enough apart on screen that boot_anim_curve_lod_steps()
 * must not shortcut - an identity, orthographic view (focal 0) so the
 * points' own x/y ARE their screen offset from centre, no projection math
 * to work back through by hand. */
static void test_curve_lod_steps_keeps_full_detail_for_a_wide_chord(void)
{
    const boot_anim_view_t view = identity_view(0);
    const S3L_Vec4 a = { -100, 0, 5 * S3L_F, S3L_F };
    const S3L_Vec4 c = {  100, 0, 5 * S3L_F, S3L_F };

    TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_ANIM_SPLINE_STEPS,
        boot_anim_curve_lod_steps(a, c, &view),
        "a span whose two ends land well apart on screen must keep full "
        "detail, not be shortcut");
}

/* The actual point of the LOD shortcut: two points that already land on
 * (near enough) the same pixel cannot have a spline through them worth
 * subdividing - see boot_anim_curve_lod_steps()'s own comment on the
 * convex-hull argument for why the two OUTER points are enough to decide
 * this without looking at anything in between. */
static void test_curve_lod_steps_collapses_a_tiny_chord_to_one_step(void)
{
    const boot_anim_view_t view = identity_view(0);
    const S3L_Vec4 a = { 40, 40, 5 * S3L_F, S3L_F };
    const S3L_Vec4 c = { 41, 40, 5 * S3L_F, S3L_F };

    TEST_ASSERT_EQUAL_INT_MESSAGE(1,
        boot_anim_curve_lod_steps(a, c, &view),
        "two points landing within a pixel of each other should collapse "
        "to a single straight step");
}

/* boot_anim_screen_chord_lt()'s whole point over projecting the points for
 * real: the perspective cross-multiplication has to agree with what the
 * projection would say - the SAME camera-space pair reads as a wide chord
 * near the camera and a tiny one far from it, because apparent size falls
 * off with z. dx=100 S3L units with focal=S3L_F: at z of one unit it spans
 * ~36 screen px (well over the 3px bar); pushed a hundred units out it
 * spans well under one. */
static void test_screen_chord_shrinks_with_distance(void)
{
    const boot_anim_view_t view = identity_view(S3L_F);
    const S3L_Vec4 near_a = { 0, 0, S3L_F, S3L_F };
    const S3L_Vec4 near_c = { 100, 0, S3L_F, S3L_F };
    const S3L_Vec4 far_a = { 0, 0, 100 * S3L_F, S3L_F };
    const S3L_Vec4 far_c = { 100, 0, 100 * S3L_F, S3L_F };

    TEST_ASSERT_FALSE_MESSAGE(
        boot_anim_screen_chord_lt(near_a, near_c, &view, 3),
        "a pair spanning tens of pixels near the camera must not read as "
        "a tiny chord");
    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_screen_chord_lt(far_a, far_c, &view, 3),
        "the same pair a hundred units out spans under a pixel and must "
        "read as tiny - apparent size falls off with z");
}

/* The pure decision half of the whole-curve sample decimation - see
 * boot_anim_curve_stride()'s own comment. Full detail while the curve is
 * anywhere near panel-sized; samples only start dropping once the whole
 * thing has shrunk to a fraction of it. */
static void test_lod_stride_tiers_by_extent(void)
{
    TEST_ASSERT_EQUAL_INT(1, boot_anim_lod_stride_for_extent(500));
    TEST_ASSERT_EQUAL_INT(1, boot_anim_lod_stride_for_extent(
        BOOT_ANIM_LOD_STRIDE2_PX));
    TEST_ASSERT_EQUAL_INT(2, boot_anim_lod_stride_for_extent(
        BOOT_ANIM_LOD_STRIDE2_PX - 1));
    TEST_ASSERT_EQUAL_INT(2, boot_anim_lod_stride_for_extent(
        BOOT_ANIM_LOD_STRIDE4_PX));
    TEST_ASSERT_EQUAL_INT(4, boot_anim_lod_stride_for_extent(
        BOOT_ANIM_LOD_STRIDE4_PX - 1));
    TEST_ASSERT_EQUAL_INT(4, boot_anim_lod_stride_for_extent(0));
}

/* The safety fallback: a span the probe cannot even project (both ends at
 * or behind the near plane here) must NOT be reported as "tiny, skip it" -
 * boot_anim_curve_lod_steps() has no idea how big it actually is on
 * screen in that case, so it has to default to full detail rather than
 * guess low. */
static void test_curve_lod_steps_keeps_full_detail_when_the_probe_cannot_project(void)
{
    const boot_anim_view_t view = identity_view(S3L_F);
    const S3L_Vec4 a = { 40, 40, 0, S3L_F };
    const S3L_Vec4 c = { 41, 40, BOOT_ANIM_NEAR_Z - 1, S3L_F };

    TEST_ASSERT_EQUAL_INT_MESSAGE(BOOT_ANIM_SPLINE_STEPS,
        boot_anim_curve_lod_steps(a, c, &view),
        "a span the probe cannot project at all must default to full "
        "detail, not be assumed tiny");
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
 * BOOT_ANIM_PEN_MS now; test_the_curve_is_finished_by_pen_finish_ms()
 * covers phase 2's own end. */
static void test_the_pen_runs_from_nothing_to_phase_ones_end(void)
{
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(0));
    TEST_ASSERT_EQUAL_INT32(0, boot_anim_pen(BOOT_ANIM_PEN_START_MS));
    TEST_ASSERT_EQUAL_INT32(BOOT_ANIM_CURVE_PHASE1_FRACTION,
        boot_anim_pen(BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS));
}

/* Phase 2: continues past phase 1's end rather than sitting still, and
 * reaches the whole curve by BOOT_ANIM_PEN_FINISH_MS - an authored moment
 * of its own, independent of BOOT_ANIM_FADE_START_MS (see boot_anim_pen()'s
 * own "TWO PHASES" comment on why the two were split apart) - see
 * test_the_curve_is_finished_by_pen_finish_ms() for that half, kept as its
 * own test since it is really a claim about PEN_FINISH_MS, not about the
 * pen's climb in general. */
static void test_the_pen_keeps_climbing_through_phase_two(void)
{
    const uint32_t phase1_end_ms =
        BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS;
    const uint32_t mid_ms =
        (phase1_end_ms + BOOT_ANIM_PEN_FINISH_MS) / 2;

    TEST_ASSERT_TRUE_MESSAGE(
        boot_anim_pen(mid_ms) > BOOT_ANIM_CURVE_PHASE1_FRACTION,
        "the pen should have climbed past where phase 1 left it");
    TEST_ASSERT_TRUE_MESSAGE(boot_anim_pen(mid_ms) < BOOT_ANIM_ONE,
        "the pen should not have reached the end of the curve yet");
}

static void test_the_curve_is_finished_by_pen_finish_ms(void)
{
    TEST_ASSERT_EQUAL_INT32_MESSAGE(BOOT_ANIM_ONE,
        boot_anim_pen(BOOT_ANIM_PEN_FINISH_MS),
        "the curve should have reached its full extent by pen_finish_ms");
}

/* Not a claim boot_anim_pen() itself makes - see its own comment on why
 * PEN_FINISH_MS landing after FADE_START_MS is only ever a generator
 * warning, not a refusal - but true of the SEED this repo ships, and worth
 * catching if a future edit to the committed timeline quietly breaks it. */
static void test_the_seed_finishes_the_curve_before_the_dissolve_starts(void)
{
    TEST_ASSERT_TRUE_MESSAGE(BOOT_ANIM_PEN_FINISH_MS <= BOOT_ANIM_FADE_START_MS,
        "the shipped seed's curve is still being drawn when the picture "
        "begins fading");
}

static void test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(0));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(BOOT_ANIM_FADE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_ink(BOOT_ANIM_MS));
    TEST_ASSERT_TRUE(boot_anim_ink(BOOT_ANIM_MS - 100) < 255);
}

/* boot_anim_image_reveal()'s own two guaranteed endpoints - the crossfade
 * has not started at or before BOOT_ANIM_IMAGE_START_MS, and it is fully
 * arrived (and stays arrived) once BOOT_ANIM_IMAGE_FADE_MS has passed
 * since. */
static void test_the_photograph_arrives_over_its_own_window(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_image_reveal(0));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_image_reveal(BOOT_ANIM_IMAGE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_image_reveal(
        BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS));
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_image_reveal(
        BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS + 1000));
}

/* The whole point of the pair (see boot_anim_scene_reach()'s own comment
 * in boot_anim.h): one window, two halves that always sum to a whole
 * picture. If these ever stopped summing to 255 the crossfade would
 * visibly dip or bloom partway through - exactly the artefact plain, not
 * eased, tween_ramp() is chosen to avoid. */
static void test_the_scene_leaves_exactly_as_fast_as_the_photograph_arrives(void)
{
    const uint32_t from = BOOT_ANIM_IMAGE_START_MS > 200 ?
        BOOT_ANIM_IMAGE_START_MS - 200 : 0;
    const uint32_t to = BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS + 200;

    for (uint32_t ms = from; ms <= to; ms += 5) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(255,
            (uint16_t)boot_anim_image_reveal(ms) + boot_anim_scene_reach(ms),
            "the photograph and the scene did not sum to one whole "
            "picture - the crossfade would visibly dip or bloom");
    }

    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_scene_reach(BOOT_ANIM_IMAGE_START_MS));
    TEST_ASSERT_EQUAL_UINT8(0, boot_anim_scene_reach(
        BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS));
}

/* The two clocks (ink and the crossfade) are independent by design - see
 * boot_anim_scene_reach()'s own comment - but draw_image() still
 * multiplies them together, so the picture needs to be at full ink for
 * the whole time the photograph is arriving, or the mountain would fade
 * in already dimmed rather than arriving bright and only later
 * dissolving. Not true of every timeline the generator could author (it
 * only WARNS if the two windows overlap) - asserted here for the one
 * this repo actually ships. Ignored while the seed's own image_start_ms
 * sits at its inert, no-photograph-authored default (== BOOT_ANIM_MS) -
 * see gen_boot_anim_timeline.py's own setdefault comment. */
static void test_the_seed_finishes_the_crossfade_before_the_dissolve_starts(void)
{
    if (BOOT_ANIM_IMAGE_START_MS >= BOOT_ANIM_MS) {
        TEST_IGNORE_MESSAGE("this seed authors no photograph crossfade");
    }
    TEST_ASSERT_EQUAL_UINT8(255, boot_anim_ink(BOOT_ANIM_IMAGE_START_MS));
    TEST_ASSERT_TRUE_MESSAGE(
        BOOT_ANIM_IMAGE_START_MS + BOOT_ANIM_IMAGE_FADE_MS <=
            BOOT_ANIM_FADE_START_MS,
        "the shipped seed is still crossing to the photograph when the "
        "picture begins dissolving");
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
/* NON-increasing per ring, plus a real drop across any span, rather than
 * strictly dimmer at every single ring as this used to demand.
 *
 * Strict per-ring was only ever satisfiable while the alpha ceiling was
 * large next to the ring count: the falloff is left * ceiling / FADE, so
 * with the seed's current 128 rings against a ceiling of 64 each ring is
 * worth half a level and adjacent PAIRS land on the same integer (63, 63,
 * 62, 62, ...). That is arithmetic, not a regression - and it is invisible
 * on the panel, because what the eye reads as "a plane going away" is the
 * overall gradient, not whether ring 62 and ring 63 differ by one 255th.
 * The two assertions below are what the test's own name actually claims:
 * it never gets BRIGHTER with distance, and it genuinely falls rather than
 * plateauing into a tile with an edge. */
static void test_the_floor_fades_out_with_distance_rather_than_stopping(void)
{
    const uint32_t settled = BOOT_ANIM_MS;
    const int span = BOOT_ANIM_GRID_FADE / 8;

    for (int ring = 2; ring < BOOT_ANIM_GRID_FADE; ring++) {
        TEST_ASSERT_TRUE_MESSAGE(
            boot_anim_grid_alpha(settled, ring) <=
            boot_anim_grid_alpha(settled, ring - 1),
            "a floor ring was BRIGHTER than the one inside it - the grid "
            "would read as lit from the far edge inward");
    }
    for (int ring = span; ring < BOOT_ANIM_GRID_FADE; ring += span) {
        TEST_ASSERT_TRUE_MESSAGE(
            boot_anim_grid_alpha(settled, ring) <
            boot_anim_grid_alpha(settled, ring - span),
            "the floor stopped dimming across a whole eighth of its own "
            "reach - it would read as a tile with an edge rather than as "
            "a plane");
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
/* How far the floor's opacity ceiling travels is authored, not fixed:
 * boot_anim_grid_alpha() lerps it from BOOT_ANIM_GRID_MAX to
 * BOOT_ANIM_GRID_CEILING_MAX, so a seed that sets those two EQUAL - which
 * the current one does, both 64 - has deliberately asked for a floor that
 * holds one steady brightness while only its hue climbs. This used to
 * demand a strict climb and so failed the moment that was authored.
 *
 * What must hold either way is that the opacity never goes BACKWARDS: a
 * settled ring dimming again partway through would read as the floor
 * guttering, and would mean the ceiling lerp or the arrival ramp had been
 * wired the wrong way round. The strict-climb case is still asserted, but
 * only where the seed actually asks for one. */
static void test_the_floor_opacity_never_falls_back(void)
{
    /* Ring 1's own fade-in (BOOT_ANIM_GRID_RING_MS + BOOT_ANIM_GRID_FADE_MS
     * after BOOT_ANIM_GRID_START_MS) is long done by either of these, plus
     * a little slack - so the only thing left changing its alpha between
     * them is the ceiling itself climbing, not "arrived" still ramping. */
    const uint32_t early_ms = BOOT_ANIM_GRID_START_MS +
        BOOT_ANIM_GRID_RING_MS + BOOT_ANIM_GRID_FADE_MS + 50;
    const uint8_t early = boot_anim_grid_alpha(early_ms, 1);
    const uint8_t late  = boot_anim_grid_alpha(BOOT_ANIM_MS, 1);

    TEST_ASSERT_TRUE_MESSAGE(late >= early,
        "the floor's own opacity fell back over the animation - a settled "
        "ring should never gutter once it has arrived");

    if (BOOT_ANIM_GRID_CEILING_MAX > BOOT_ANIM_GRID_MAX) {
        TEST_ASSERT_TRUE_MESSAGE(late > early,
            "the seed authors a ceiling above the floor's starting max, "
            "so its opacity should visibly climb over the animation and "
            "not just its colour");
    }
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
 *
 * boot_anim_title_letter() now takes the font it is laying out - see its
 * own comment in boot_anim.h - so the layout tests below exercise the REAL
 * font the seed authors (see TITLE_FONT below) rather than a
 * synthetic stand-in: these are checks against the actual authored
 * geometry (BOOT_ANIM_TITLE_VIEW_X/Y, the real "Autana" advances), not
 * just the layout FORMULA in the abstract - suite_gfx_font.c already
 * covers gfx_font_text_width()/gfx_font_advance() themselves against a
 * synthetic proportional font, so there is no need to repeat that here.
 *-------------------------------------------------------------------------*/

/* Whichever font the timeline actually AUTHORS, resolved the same way
 * draw_title() resolves it (boot_anim.c) - not gfx_font_lmroman_40
 * hardcoded, as this was when that font was the only one the title could
 * use. These tests check the real authored geometry, so they have to ask
 * the same question the renderer does: title_font and title_scale are a
 * pair, and pinning the font here while the seed tunes the scale for the
 * OTHER one tests a combination that never ships - a 51px cell at 5x,
 * which duly ran off the panel and failed. */
#define TITLE_FONT ((BOOT_ANIM_TITLE_FONT == BOOT_ANIM_TITLE_FONT_8X8) \
                        ? &gfx_font_8x8 : &gfx_font_lmroman_40)

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
/* Counts sign changes in each HALF of the flight rather than comparing
 * gaps between four consecutive crossings, as this used to.
 *
 * The property under test is unchanged - the wobble is a chirp, packing
 * its oscillation into the early, far part of the flight and stretching
 * out as the letter settles - and so is the reason it holds:
 * boot_anim_title_wobble() drives its phase off d SQUARED, so the first
 * half of the approach (d from 1.0 to 0.5) sweeps three quarters of the
 * total phase and the second half only the remaining quarter. Comparing
 * the two halves measures exactly that, and needs just ONE crossing to
 * do it.
 *
 * Gap-comparison needed four, which is not something a seed owes anyone:
 * BOOT_ANIM_TITLE_TURNS_PHASE is authored, and the current seed's 92000
 * is 1.4 turns over the whole flight - a deliberately gentler wobble than
 * the 3.5 turns that was in the file when this test was written, and only
 * two crossings in total. The chirp was still there; the old test simply
 * could not see it. */
static void test_the_wobbles_oscillation_slows_as_it_lands(void)
{
    int early = 0, late = 0, prev_sign = 0;

    for (int32_t d = BOOT_ANIM_ONE; d >= 0; d -= 4) {
        const int y = boot_anim_title_wobble(d);
        const int sign = y > 0 ? 1 : (y < 0 ? -1 : 0);
        if (sign != 0 && prev_sign != 0 && sign != prev_sign) {
            if (d >= BOOT_ANIM_ONE / 2) {
                early++;
            } else {
                late++;
            }
        }
        if (sign != 0) {
            prev_sign = sign;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(early + late >= 1,
        "the seed authors no wobble oscillation at all - "
        "BOOT_ANIM_TITLE_TURNS_PHASE would have to be under half a turn");
    TEST_ASSERT_TRUE_MESSAGE(early > late,
        "the wobble should oscillate faster early in the flight than as "
        "it lands - a chirp, not a constant vibration fading out");
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
            boot_anim_title_letter(TITLE_FONT, i, arrived);
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
            boot_anim_title_letter(TITLE_FONT, i, never);
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
                boot_anim_title_letter(TITLE_FONT, i, mid);
            const boot_anim_title_pos_t prev_at =
                boot_anim_title_letter(TITLE_FONT, i - 1, mid);
            TEST_ASSERT_TRUE_MESSAGE(prev_at.x >= at.x,
                "an earlier letter should be at least as far along as a "
                "later one at the same moment");
        }
    }
}

static void test_a_letter_starts_off_panel_to_the_left(void)
{
    const boot_anim_title_pos_t p =
        boot_anim_title_letter(TITLE_FONT, 0, BOOT_ANIM_TITLE_START_MS);
    TEST_ASSERT_TRUE_MESSAGE(p.x < 0,
        "a letter should begin off the left edge of the panel, not merely "
        "at it");
}

/* Regression guard for the bug boot_anim_title_letter()'s own comment in
 * boot_anim.h describes: final_x used to be `i * (8 * SCALE + GAP)`, a
 * fixed per-letter cell, which is only correct for a MONOSPACE font. This
 * checks the real formula directly against gfx_font_text_width() - the
 * same pure sum-of-advances function gfx_font.h's own suite already pins
 * against a synthetic proportional font - rather than against a second,
 * hand-derived copy of the arithmetic that could make the same mistake
 * twice. Watching this fail against the pre-fix `i * (8*SCALE+GAP)`
 * formula (temporarily restore it to see) is what proves this test can
 * catch the bug it exists for, not just describe it. */
static void test_final_x_matches_the_advance_sum(void)
{
    /* Any moment past every letter's own flight is fine: only the FINAL
     * resting x matters here, and test_a_letter_lands_exactly_on_its_final_
     * position() already covers y/wobble/wave separately. */
    const uint32_t arrived = BOOT_ANIM_TITLE_START_MS +
                             (uint32_t)BOOT_ANIM_TITLE_LEN *
                                 BOOT_ANIM_TITLE_STAGGER_MS +
                             BOOT_ANIM_TITLE_FLIGHT_MS + 1000;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const boot_anim_title_pos_t p =
            boot_anim_title_letter(TITLE_FONT, i, arrived);
        const int prefix_w = gfx_font_text_width(TITLE_FONT, BOOT_ANIM_TITLE,
                                                 i, BOOT_ANIM_TITLE_SCALE);
        const int expected_x = BOOT_ANIM_TITLE_VIEW_X + prefix_w +
                               i * BOOT_ANIM_TITLE_GAP;
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected_x, p.x,
            "a landed letter's x must be VIEW_X plus the ADVANCE SUM of "
            "every letter before it (gfx_font_text_width(), proportional) "
            "plus i*GAP of tracking - not a fixed per-letter cell, which "
            "is only correct for a monospace font");
    }
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
     * stay on the panel. cell_w and cell_h separately, not one shared
     * `cell` - font_lmroman_40's cell is NOT square (51x58), unlike the
     * old gfx_font_8x8 this test used to size itself off; conflating the
     * two axes here would silently check the wrong bound on whichever axis
     * differs. */
    const int cell_w = TITLE_FONT->cell_w * BOOT_ANIM_TITLE_SCALE;
    const int cell_h = TITLE_FONT->cell_h * BOOT_ANIM_TITLE_SCALE;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const uint32_t start = BOOT_ANIM_TITLE_START_MS +
                               (uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS;
        for (uint32_t t = start; t <= start + BOOT_ANIM_TITLE_FLIGHT_MS;
             t += 15) {
            const boot_anim_title_pos_t p =
                boot_anim_title_letter(TITLE_FONT, i, t);
            if (p.x + cell_w < 0) {
                continue;   /* still off-panel to the left - not visible yet */
            }
            TEST_ASSERT_TRUE_MESSAGE(p.x + cell_w <= BOOT_ANIM_TITLE_VIEW_W,
                "a letter drifted off the right edge of the viewer's frame");
            TEST_ASSERT_TRUE_MESSAGE(
                p.y >= 0 && p.y + cell_h < BOOT_ANIM_TITLE_VIEW_H,
                "a letter's wobble carried it off the top or bottom of "
                "the viewer's frame");
        }
    }
}

/* boot_anim_title_shadow_offset() is the one piece of draw_title()'s
 * shadow logic this file can reach directly - draw_title() itself needs a
 * framebuffer and a panel (see this file's own top comment), so this is
 * also the only test standing between a future edit here and the exact
 * "up-right instead of down-right" bug this function was extracted to fix
 * (see its own comment in boot_anim.h for the panel-space derivation). */
static void test_title_shadow_offset_turns_reader_frame_into_panel_frame(void)
{
    int dx, dy;

    /* The shipped default: 1 right, 1 down in the reader's frame. */
    boot_anim_title_shadow_offset(1, 1, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, dx,
        "1 pixel right in the reader's frame must turn into -1 on the "
        "panel's own X - a +1 here is the up-right regression this "
        "function exists to prevent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy, "1 pixel down must turn into +1 "
        "on the panel's own Y");

    /* Pure right (no vertical component) must not move the panel X at
     * all - panel_x tracks -view_y, so it is only the DOWN component that
     * can touch it. Pure down, in turn, must not move the panel Y at all
     * - panel_y tracks view_x, only the RIGHT component reaches it. The
     * two axes are a plain 90-degree swap, not a general rotation that
     * would mix them. */
    boot_anim_title_shadow_offset(5, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dx);
    boot_anim_title_shadow_offset(0, 5, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dy);

    /* (0, 0) - the "shadow disabled" sentinel draw_title() checks for -
     * must stay the identity, not become a spurious offset. */
    boot_anim_title_shadow_offset(0, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dx);
    TEST_ASSERT_EQUAL_INT(0, dy);
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
    RUN_TEST(test_identity_transform_leaves_the_origin_at_screen_centre);
    RUN_TEST(test_a_point_further_from_the_camera_projects_smaller);
    RUN_TEST(test_project_point_rejects_a_point_at_the_near_plane);
    RUN_TEST(test_project_segment_cs_rejects_a_segment_entirely_behind);
    RUN_TEST(test_project_segment_cs_clips_asymmetric_coordinates);
    RUN_TEST(test_spoke_reveal_target_hits_its_endpoints_exactly);
    RUN_TEST(test_spoke_reveal_target_advances_evenly_in_screen_space);
    RUN_TEST(test_an_untouched_keyframes_scale_reads_back_as_identity);
    RUN_TEST(test_the_seeds_curve_stays_near_the_panel_throughout);
    RUN_TEST(test_the_seeds_three_axes_project_to_distinct_directions);

    RUN_TEST(test_wave_height_is_zero_when_the_amplitude_is_zero);
    RUN_TEST(test_wave_height_is_zero_when_the_wavelength_is_not_positive);
    RUN_TEST(test_wave_height_is_periodic_in_radius);
    RUN_TEST(test_wave_height_is_periodic_in_time);
    RUN_TEST(test_wave_height_travels_outward_with_time);
    RUN_TEST(test_wave_height_is_frozen_when_the_period_is_zero);
    RUN_TEST(test_the_seeds_wave_is_coherently_authored);
    RUN_TEST(test_wave_envelope_is_zero_at_the_very_start);
    RUN_TEST(test_wave_envelope_is_still_muted_at_wave_in_ms_itself);
    RUN_TEST(test_wave_envelope_reaches_full_strength_after_the_ramp);
    RUN_TEST(test_wave_envelope_plateaus_between_in_and_out);
    RUN_TEST(test_wave_envelope_is_still_full_at_wave_out_ms_itself);
    RUN_TEST(test_wave_envelope_fades_back_to_zero_after_its_ramp);
    RUN_TEST(test_the_title_wave_is_at_full_swing_before_it_calms);
    RUN_TEST(test_the_title_wave_reaches_stillness_after_its_fade);
    RUN_TEST(test_the_title_wave_never_swings_wider_as_it_calms);
    RUN_TEST(test_the_seeds_spokes_reach_their_full_length);

    RUN_TEST(test_a_span_starts_and_ends_halfway_between_its_points);
    RUN_TEST(test_a_repeated_point_pins_the_end_of_the_curve);
    RUN_TEST(test_a_span_never_leaves_its_control_points_behind);
    RUN_TEST(test_a_span_climbs_steadily_when_its_points_do);
    RUN_TEST(test_spline_cs_matches_transforming_the_world_space_spline);
    RUN_TEST(test_curve_lod_steps_keeps_full_detail_for_a_wide_chord);
    RUN_TEST(test_curve_lod_steps_collapses_a_tiny_chord_to_one_step);
    RUN_TEST(test_curve_lod_steps_keeps_full_detail_when_the_probe_cannot_project);
    RUN_TEST(test_screen_chord_shrinks_with_distance);
    RUN_TEST(test_lod_stride_tiers_by_extent);

    RUN_TEST(test_the_pen_runs_from_nothing_to_phase_ones_end);
    RUN_TEST(test_the_pen_keeps_climbing_through_phase_two);
    RUN_TEST(test_the_curve_is_finished_by_pen_finish_ms);
    RUN_TEST(test_the_seed_finishes_the_curve_before_the_dissolve_starts);
    RUN_TEST(test_the_picture_is_lit_until_the_dissolve_and_dark_at_the_end);
    RUN_TEST(test_the_photograph_arrives_over_its_own_window);
    RUN_TEST(test_the_scene_leaves_exactly_as_fast_as_the_photograph_arrives);
    RUN_TEST(test_the_seed_finishes_the_crossfade_before_the_dissolve_starts);
    RUN_TEST(test_the_floor_fades_in_from_the_origin_outward);
    RUN_TEST(test_the_floor_fades_out_with_distance_rather_than_stopping);
    RUN_TEST(test_the_floor_stays_dim_enough_to_be_a_backdrop);
    RUN_TEST(test_the_grid_climb_runs_the_whole_animation);
    RUN_TEST(test_the_floor_opacity_never_falls_back);
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
    RUN_TEST(test_final_x_matches_the_advance_sum);
    RUN_TEST(test_the_title_stays_on_the_panel_once_visible);
    RUN_TEST(test_title_shadow_offset_turns_reader_frame_into_panel_frame);
}

SUITE_REGISTER(run_boot_anim_suite);
