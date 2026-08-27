/*=============================================================================
 * Portable suite: ui_transform_t - the UI layer's affine map.
 *
 * ui_transform.h is header-only and pure geometry, same reasoning as
 * ui_style.h (see that header's own top comment and suite_ui_style.c): the
 * shape of a mapping is not something anyone can eyeball reliably, especially
 * once rounding is involved, so it is checked here in fixed-point arithmetic
 * rather than by looking at a rotated screen.
 *
 * The viewport used throughout is the real panel's, so a wrong constant here
 * cannot quietly agree with a wrong constant in gfx.h - VIEW_W/VIEW_H are
 * spelled out as their own numbers instead of pulled from GFX_WIDTH/HEIGHT,
 * which this suite cannot include (gfx.h needs the BSP headers - see
 * ui_transform.h's own note on why it only takes microui.h).
 *===========================================================================*/

#include <stdbool.h>

#include "unity.h"
#include "suites.h"

#include "ui_transform.h"

#define VIEW_W 368
#define VIEW_H 448

/*---------------------------------------------------------------------------
 * Identity
 *-------------------------------------------------------------------------*/

static void test_identity_maps_every_point_to_itself(void)
{
    const ui_transform_t id = ui_transform_identity();
    const int xs[] = { 0, 1, -1, 100, VIEW_W, -VIEW_H, 12345 };
    const int ys[] = { 0, -1, 1, 200, VIEW_H, -VIEW_W, -6789 };

    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        int ox, oy;
        ui_transform_point(id, xs[i], ys[i], &ox, &oy);
        TEST_ASSERT_EQUAL_INT_MESSAGE(xs[i], ox,
            "identity must not move a point's x");
        TEST_ASSERT_EQUAL_INT_MESSAGE(ys[i], oy,
            "identity must not move a point's y");
    }
}

static void test_identity_maps_every_rect_to_itself(void)
{
    const ui_transform_t id = ui_transform_identity();
    const mu_Rect r = { 16, 80, 336, 64 };
    const mu_Rect out = ui_transform_rect(id, r);

    TEST_ASSERT_EQUAL_INT(r.x, out.x);
    TEST_ASSERT_EQUAL_INT(r.y, out.y);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.w, out.w,
        "identity must not resize a rect");
    TEST_ASSERT_EQUAL_INT(r.h, out.h);
}

/*---------------------------------------------------------------------------
 * Round trips: point -> transform -> inverse -> back to the start
 *-------------------------------------------------------------------------*/

static void assert_round_trips(ui_transform_t t, const char *msg)
{
    ui_transform_t inv;
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_invert(t, &inv),
        "a quarter turn is never singular");

    const int xs[] = { 0, 1, 37, VIEW_W / 2, VIEW_W, VIEW_H, 5, 200 };
    const int ys[] = { 0, 1, 200, VIEW_H / 2, VIEW_H, VIEW_W, 400, 9 };

    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        int mx, my, bx, by;
        ui_transform_point(t, xs[i], ys[i], &mx, &my);
        ui_transform_point(inv, mx, my, &bx, &by);
        TEST_ASSERT_EQUAL_INT_MESSAGE(xs[i], bx, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ys[i], by, msg);
    }
}

static void test_a_point_round_trips_through_every_quarter_turn(void)
{
    assert_round_trips(ui_transform_identity(),
        "turn 0: a point mapped and inverse-mapped must return to itself");
    assert_round_trips(ui_transform_quarter_turn(1, VIEW_W, VIEW_H),
        "turn 1: a point mapped and inverse-mapped must return to itself");
    assert_round_trips(ui_transform_quarter_turn(2, VIEW_W, VIEW_H),
        "turn 2: a point mapped and inverse-mapped must return to itself");
    assert_round_trips(ui_transform_quarter_turn(3, VIEW_W, VIEW_H),
        "turn 3: a point mapped and inverse-mapped must return to itself");
}

/*---------------------------------------------------------------------------
 * Quarter turns: where the viewport's own corners land
 *
 * The domain each turn expects is the LOGICAL canvas - VIEW_H x VIEW_W for
 * an odd turn, VIEW_W x VIEW_H for an even one, exactly what ui_width()/
 * ui_height() would report (see ui.h). Expected corners are the ones
 * ui_transform_quarter_turn()'s own doc comment claims: each corner moves to
 * the next one clockwise, once per quarter turn.
 *-------------------------------------------------------------------------*/

static void check_point(ui_transform_t t, int x, int y, int ex, int ey,
                        const char *msg)
{
    int ox, oy;
    ui_transform_point(t, x, y, &ox, &oy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ex, ox, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ey, oy, msg);
}

static void test_turn_0_leaves_corners_where_they_are(void)
{
    const ui_transform_t t = ui_transform_quarter_turn(0, VIEW_W, VIEW_H);
    check_point(t, 0,      0,      0,      0,      "turn 0: top-left");
    check_point(t, VIEW_W, 0,      VIEW_W, 0,      "turn 0: top-right");
    check_point(t, 0,      VIEW_H, 0,      VIEW_H, "turn 0: bottom-left");
    check_point(t, VIEW_W, VIEW_H, VIEW_W, VIEW_H, "turn 0: bottom-right");
}

static void test_turn_1_rotates_corners_one_step_clockwise(void)
{
    /* Domain is VIEW_H wide, VIEW_W tall - the swap odd turns imply. */
    const ui_transform_t t = ui_transform_quarter_turn(1, VIEW_W, VIEW_H);
    check_point(t, 0,      0,      VIEW_W, 0,      "turn 1: logical top-left -> physical top-right");
    check_point(t, VIEW_H, 0,      VIEW_W, VIEW_H, "turn 1: logical top-right -> physical bottom-right");
    check_point(t, 0,      VIEW_W, 0,      0,      "turn 1: logical bottom-left -> physical top-left");
    check_point(t, VIEW_H, VIEW_W, 0,      VIEW_H, "turn 1: logical bottom-right -> physical bottom-left");
}

static void test_turn_2_rotates_corners_two_steps(void)
{
    const ui_transform_t t = ui_transform_quarter_turn(2, VIEW_W, VIEW_H);
    check_point(t, 0,      0,      VIEW_W, VIEW_H, "turn 2: top-left -> bottom-right");
    check_point(t, VIEW_W, 0,      0,      VIEW_H, "turn 2: top-right -> bottom-left");
    check_point(t, 0,      VIEW_H, VIEW_W, 0,      "turn 2: bottom-left -> top-right");
    check_point(t, VIEW_W, VIEW_H, 0,      0,      "turn 2: bottom-right -> top-left");
}

static void test_turn_3_rotates_corners_three_steps_clockwise(void)
{
    const ui_transform_t t = ui_transform_quarter_turn(3, VIEW_W, VIEW_H);
    check_point(t, 0,      0,      0,      VIEW_H, "turn 3: logical top-left -> physical bottom-left");
    check_point(t, VIEW_H, 0,      0,      0,      "turn 3: logical top-right -> physical top-left");
    check_point(t, 0,      VIEW_W, VIEW_W, VIEW_H, "turn 3: logical bottom-left -> physical bottom-right");
    check_point(t, VIEW_H, VIEW_W, VIEW_W, 0,      "turn 3: logical bottom-right -> physical top-right");
}

/*---------------------------------------------------------------------------
 * A rect stays axis-aligned and inside the viewport after any quarter turn
 *-------------------------------------------------------------------------*/

static void assert_rect_fits(ui_transform_t t, mu_Rect r, const char *msg)
{
    const mu_Rect out = ui_transform_rect(t, r);

    TEST_ASSERT_TRUE_MESSAGE(out.w > 0 && out.h > 0, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.x >= 0 && out.y >= 0, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.x + out.w <= VIEW_W, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.y + out.h <= VIEW_H, msg);
}

static void test_a_mapped_rect_stays_axis_aligned_and_inside_the_viewport(void)
{
    const mu_Rect even_domain_rect = { 16, 80, 100, 64 };
    const mu_Rect odd_domain_rect  = { 16, 80, 100, 64 }; /* fits both domains */

    assert_rect_fits(ui_transform_quarter_turn(0, VIEW_W, VIEW_H),
                     even_domain_rect, "turn 0 rect must land inside the viewport");
    assert_rect_fits(ui_transform_quarter_turn(1, VIEW_W, VIEW_H),
                     odd_domain_rect, "turn 1 rect must land inside the viewport");
    assert_rect_fits(ui_transform_quarter_turn(2, VIEW_W, VIEW_H),
                     even_domain_rect, "turn 2 rect must land inside the viewport");
    assert_rect_fits(ui_transform_quarter_turn(3, VIEW_W, VIEW_H),
                     odd_domain_rect, "turn 3 rect must land inside the viewport");
}

static void test_a_mapped_rect_swaps_width_and_height_on_an_odd_turn(void)
{
    const mu_Rect r = { 16, 80, 100, 64 };
    const mu_Rect t1 = ui_transform_rect(ui_transform_quarter_turn(1, VIEW_W, VIEW_H), r);
    const mu_Rect t3 = ui_transform_rect(ui_transform_quarter_turn(3, VIEW_W, VIEW_H), r);

    TEST_ASSERT_EQUAL_INT_MESSAGE(r.h, t1.w,
        "a quarter turn is a pure rotation - it must swap w and h, not scale them");
    TEST_ASSERT_EQUAL_INT(r.w, t1.h);
    TEST_ASSERT_EQUAL_INT(r.h, t3.w);
    TEST_ASSERT_EQUAL_INT(r.w, t3.h);
}

/*---------------------------------------------------------------------------
 * Composition and the quarter turn a transform represents
 *-------------------------------------------------------------------------*/

static void test_quarter_reports_the_turn_each_matrix_represents(void)
{
    TEST_ASSERT_EQUAL_INT(0, ui_transform_quarter(ui_transform_identity()));
    TEST_ASSERT_EQUAL_INT(0,
        ui_transform_quarter(ui_transform_quarter_turn(0, VIEW_W, VIEW_H)));
    TEST_ASSERT_EQUAL_INT(1,
        ui_transform_quarter(ui_transform_quarter_turn(1, VIEW_W, VIEW_H)));
    TEST_ASSERT_EQUAL_INT(2,
        ui_transform_quarter(ui_transform_quarter_turn(2, VIEW_W, VIEW_H)));
    TEST_ASSERT_EQUAL_INT(3,
        ui_transform_quarter(ui_transform_quarter_turn(3, VIEW_W, VIEW_H)));
}

static void test_composing_two_quarter_turns_sums_mod_4(void)
{
    const ui_transform_t q1 = ui_transform_quarter_turn(1, VIEW_W, VIEW_H);
    const ui_transform_t q2 = ui_transform_quarter_turn(2, VIEW_W, VIEW_H);
    const ui_transform_t q3 = ui_transform_quarter_turn(3, VIEW_W, VIEW_H);

    /* 1 + 1 = 2 */
    TEST_ASSERT_EQUAL_INT(2, ui_transform_quarter(ui_transform_compose(q1, q1)));
    /* 1 + 2 = 3 */
    TEST_ASSERT_EQUAL_INT(3, ui_transform_quarter(ui_transform_compose(q1, q2)));
    /* 2 + 2 = 0 (mod 4) */
    TEST_ASSERT_EQUAL_INT(0, ui_transform_quarter(ui_transform_compose(q2, q2)));
    /* 1 + 3 = 0 (mod 4) */
    TEST_ASSERT_EQUAL_INT(0, ui_transform_quarter(ui_transform_compose(q1, q3)));
    /* 3 + 3 = 2 (mod 4) */
    TEST_ASSERT_EQUAL_INT(2, ui_transform_quarter(ui_transform_compose(q3, q3)));
}

/*---------------------------------------------------------------------------
 * The backend-vs-type boundary
 *-------------------------------------------------------------------------*/

static void test_axis_preserving_accepts_identity_and_every_quarter_turn(void)
{
    TEST_ASSERT_TRUE(ui_transform_is_axis_preserving(ui_transform_identity()));
    for (int turn = 0; turn < 4; turn++) {
        TEST_ASSERT_TRUE_MESSAGE(
            ui_transform_is_axis_preserving(
                ui_transform_quarter_turn(turn, VIEW_W, VIEW_H)),
            "every quarter turn is exactly what this backend can draw");
    }
}

static void test_axis_preserving_accepts_translation(void)
{
    const ui_transform_t translate =
        { UI_FP_ONE, 0, 0, UI_FP_ONE, 40 * UI_FP_ONE, -12 * UI_FP_ONE };
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(translate),
        "a pure translation keeps every rect axis-aligned");
}

static void test_axis_preserving_accepts_integer_scale(void)
{
    const ui_transform_t scale_up =
        { 2 * UI_FP_ONE, 0, 0, 2 * UI_FP_ONE, 0, 0 };
    const ui_transform_t scale_and_swap = /* an integer-scaled quarter turn */
        { 0, 3 * UI_FP_ONE, -3 * UI_FP_ONE, 0, 100, 0 };
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(scale_up),
        "an integer scale keeps every rect axis-aligned");
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(scale_and_swap),
        "a scaled quarter turn is still axis-preserving - it is the "
        "magnitude of a/b/c/d that carries the scale, the SIGN pattern "
        "that carries the rotation, and this checks only the pattern");
}

static void test_axis_preserving_rejects_a_shear(void)
{
    /* c and d unrotated, but b is nonzero too: the y-axis now has a
     * component along x as well as y, which is exactly a shear. */
    const ui_transform_t shear =
        { UI_FP_ONE, UI_FP_ONE / 2, 0, UI_FP_ONE, 0, 0 };
    TEST_ASSERT_FALSE_MESSAGE(ui_transform_is_axis_preserving(shear),
        "a shear must be rejected - it turns an axis-aligned rect into a "
        "parallelogram, which gfx_fill_rect() cannot draw");
}

static void test_axis_preserving_rejects_a_non_90_degree_rotation(void)
{
    /* All four entries nonzero: neither axis maps cleanly onto x or y. This
     * is not a real rotation matrix (its rows are not unit length), but the
     * classifier only needs to see that both columns are mixed to reject
     * it, which is exactly what a genuine 45 degree rotation would also
     * present. */
    const ui_transform_t skew_rotation =
        { UI_FP_ONE, UI_FP_ONE / 2, UI_FP_ONE / 2, UI_FP_ONE, 0, 0 };
    TEST_ASSERT_FALSE_MESSAGE(
        ui_transform_is_axis_preserving(skew_rotation),
        "a rotation that is not a multiple of 90 degrees must be rejected - "
        "gfx_text_turned() only has four quarters to offer it");
}

/*---------------------------------------------------------------------------
 * Inversion of a singular matrix
 *-------------------------------------------------------------------------*/

static void test_invert_fails_on_a_singular_matrix(void)
{
    const ui_transform_t zero_scale = { UI_FP_ONE, 0, 0, 0, 0, 0 };
    const ui_transform_t all_zero   = { 0, 0, 0, 0, 0, 0 };
    ui_transform_t out;

    TEST_ASSERT_FALSE_MESSAGE(ui_transform_invert(zero_scale, &out),
        "a matrix with a zeroed axis has no inverse - determinant is zero");
    TEST_ASSERT_FALSE_MESSAGE(ui_transform_invert(all_zero, &out),
        "the zero matrix is singular");
}

void suite_ui_transform(void)
{
    RUN_TEST(test_identity_maps_every_point_to_itself);
    RUN_TEST(test_identity_maps_every_rect_to_itself);
    RUN_TEST(test_a_point_round_trips_through_every_quarter_turn);
    RUN_TEST(test_turn_0_leaves_corners_where_they_are);
    RUN_TEST(test_turn_1_rotates_corners_one_step_clockwise);
    RUN_TEST(test_turn_2_rotates_corners_two_steps);
    RUN_TEST(test_turn_3_rotates_corners_three_steps_clockwise);
    RUN_TEST(test_a_mapped_rect_stays_axis_aligned_and_inside_the_viewport);
    RUN_TEST(test_a_mapped_rect_swaps_width_and_height_on_an_odd_turn);
    RUN_TEST(test_quarter_reports_the_turn_each_matrix_represents);
    RUN_TEST(test_composing_two_quarter_turns_sums_mod_4);
    RUN_TEST(test_axis_preserving_accepts_identity_and_every_quarter_turn);
    RUN_TEST(test_axis_preserving_accepts_translation);
    RUN_TEST(test_axis_preserving_accepts_integer_scale);
    RUN_TEST(test_axis_preserving_rejects_a_shear);
    RUN_TEST(test_axis_preserving_rejects_a_non_90_degree_rotation);
    RUN_TEST(test_invert_fails_on_a_singular_matrix);
}

SUITE_REGISTER(suite_ui_transform);
