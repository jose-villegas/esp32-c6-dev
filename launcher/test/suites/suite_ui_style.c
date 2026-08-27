/*=============================================================================
 * Portable suite: ui_bezel_spans - the bezel's geometry and shading.
 *
 * ui_style.h splits a style into geometry (ui_bezel_spans() and ui_shade(),
 * `static inline` in the header so they link on a host with neither gfx.c nor
 * microui.c) and painting (styled_draw_frame() in ui.c, which turns the spans
 * into mu_draw_rect() calls and so cannot). This suite exercises only the
 * geometry - the same reasoning as suite_icons.c: nobody can eyeball five
 * overlapping rectangles reliably, so the shape is checked here rather than by
 * looking at the screen.
 *
 * The rect used throughout is the one the launcher actually draws: a
 * full-width row at UI_ROW_HEIGHT (64), inset by UI_MARGIN. Where a size
 * matters to the assertion it is stated inline instead.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>

#include "unity.h"
#include "suites.h"

#include "ui_style.h"

#define ROW_X   16
#define ROW_Y   80
#define ROW_W   336
#define ROW_H   64

/* The launcher's own button face - see ui_init() in ui.c. */
static const mu_Color FACE = { 0x16, 0x1A, 0x28, 255 };

static mu_Rect row(void)
{
    return (mu_Rect){ ROW_X, ROW_Y, ROW_W, ROW_H };
}

static int spans_for(mu_Rect r, bool sunken, ui_span_t *out)
{
    return ui_bezel_spans(r, FACE, sunken, out, UI_BEZEL_MAX_SPANS);
}

static bool same_color(mu_Color a, mu_Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Rough perceived brightness, only ever used to compare two shades of one
 * colour - which is lighter is the whole question a bezel turns on. */
static int luma(mu_Color c)
{
    return c.r * 2 + c.g * 5 + c.b;
}

/*---------------------------------------------------------------------------
 * Shading
 *-------------------------------------------------------------------------*/

static void test_shading_by_zero_changes_nothing(void)
{
    TEST_ASSERT_TRUE_MESSAGE(same_color(ui_shade(FACE, 0), FACE),
        "a mix of zero must land exactly on the input, not near it");
}

static void test_full_shading_reaches_white_and_black(void)
{
    const mu_Color lit  = ui_shade(FACE, 255);
    const mu_Color dark = ui_shade(FACE, -255);

    TEST_ASSERT_TRUE_MESSAGE(same_color(lit, (mu_Color){ 255, 255, 255, 255 }),
        "a full mix toward white must reach white exactly - rounding that "
        "stops one short is what makes a ramp's end look wrong");
    TEST_ASSERT_TRUE_MESSAGE(same_color(dark, (mu_Color){ 0, 0, 0, 255 }),
        "a full mix toward black must reach black exactly");
}

static void test_shading_leaves_alpha_alone(void)
{
    const mu_Color translucent = { 0x40, 0x40, 0x40, 128 };

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(128, ui_shade(translucent, 96).a,
        "an edge must be exactly as opaque as the face it belongs to - "
        "ui.c drops fully transparent rects, so a shaded alpha could make "
        "edges appear or vanish on their own");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(128, ui_shade(translucent, -96).a,
        "shading toward black must not touch alpha either");
}

/*---------------------------------------------------------------------------
 * Geometry
 *-------------------------------------------------------------------------*/

static void test_the_face_is_drawn_first_and_covers_the_whole_rect(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];
    const int n = spans_for(row(), false, s);

    TEST_ASSERT_EQUAL_INT_MESSAGE(UI_BEZEL_MAX_SPANS, n,
        "a bezel is a face plus four edges");
    TEST_ASSERT_EQUAL_INT(ROW_X, s[0].rect.x);
    TEST_ASSERT_EQUAL_INT(ROW_Y, s[0].rect.y);
    TEST_ASSERT_EQUAL_INT(ROW_W, s[0].rect.w);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ROW_H, s[0].rect.h,
        "the face must cover the whole control before the edges go over it - "
        "insetting it would leave the background showing through the corners");
    TEST_ASSERT_TRUE_MESSAGE(same_color(s[0].color, FACE),
        "the face keeps the colour microui asked for; only edges are shaded");
}

static void test_every_span_stays_inside_the_control(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];
    const int n = spans_for(row(), false, s);

    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(s[i].rect.x >= ROW_X && s[i].rect.y >= ROW_Y,
            "a span started outside the control's rect");
        TEST_ASSERT_TRUE_MESSAGE(
            s[i].rect.x + s[i].rect.w <= ROW_X + ROW_W &&
            s[i].rect.y + s[i].rect.h <= ROW_Y + ROW_H,
            "a span ran past the control's rect - microui lays the rows out "
            "edge to edge, so overspill lands on the neighbouring button");
    }
}

static void test_a_raised_button_is_lit_from_the_top_left(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];
    spans_for(row(), false, s);

    TEST_ASSERT_TRUE_MESSAGE(luma(s[1].color) > luma(s[0].color),
        "the top edge of a raised button must be lighter than its face");
    TEST_ASSERT_TRUE_MESSAGE(luma(s[2].color) > luma(s[0].color),
        "so must the left edge");
    TEST_ASSERT_TRUE_MESSAGE(luma(s[3].color) < luma(s[0].color),
        "the bottom edge must be darker than the face");
    TEST_ASSERT_TRUE_MESSAGE(luma(s[4].color) < luma(s[0].color),
        "so must the right edge");
}

static void test_a_sunken_button_swaps_which_edges_are_lit(void)
{
    ui_span_t raised[UI_BEZEL_MAX_SPANS];
    ui_span_t sunken[UI_BEZEL_MAX_SPANS];
    spans_for(row(), false, raised);
    spans_for(row(), true,  sunken);

    for (int i = 1; i < UI_BEZEL_MAX_SPANS; i++) {
        const int opposite = (i <= 2) ? i + 2 : i - 2;
        TEST_ASSERT_TRUE_MESSAGE(
            same_color(sunken[i].color, raised[opposite].color),
            "pressing must move the light to the opposite pair of edges and "
            "change nothing else - that swap IS the pressed look on a palette "
            "this dark, where the shadow is nearly the background colour");
        TEST_ASSERT_EQUAL_INT_MESSAGE(raised[i].rect.x, sunken[i].rect.x,
            "the edges must not move when a button is pressed");
        TEST_ASSERT_EQUAL_INT(raised[i].rect.y, sunken[i].rect.y);
        TEST_ASSERT_EQUAL_INT(raised[i].rect.w, sunken[i].rect.w);
        TEST_ASSERT_EQUAL_INT(raised[i].rect.h, sunken[i].rect.h);
    }
}

static void test_the_shadowed_edges_are_drawn_over_the_lit_ones(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];
    spans_for(row(), false, s);

    /* The bottom span is full width and comes after the left span, which is
     * full height, so it owns the bottom-left corner; the right span owns the
     * top-right the same way. Both corners therefore come out shadowed, which
     * is the classic bevel - see ui_bezel_spans, which explains why the edges
     * overlap rather than being mitred. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ROW_W, s[3].rect.w,
        "the bottom edge must span the full width to reach the corner");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ROW_H, s[4].rect.h,
        "the right edge must span the full height to reach the corner");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ROW_H, s[2].rect.h,
        "the left edge must span the full height too, or the shadowed bottom "
        "edge would not be painting over anything at the corner");
}

/*---------------------------------------------------------------------------
 * Degenerate sizes
 *-------------------------------------------------------------------------*/

static void test_a_control_too_small_for_a_bezel_gets_a_flat_face(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans((mu_Rect){ 0, 0, 2, 2 }, FACE, false,
                                 s, UI_BEZEL_MAX_SPANS);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "with no room for even a one-pixel edge and a pixel of face between, "
        "a bezel degrades to a plain fill rather than drawing edges that meet");
    TEST_ASSERT_TRUE_MESSAGE(same_color(s[0].color, FACE),
        "and that fill is the face colour, unshaded");
}

static void test_a_thin_control_keeps_a_pixel_of_face_between_its_edges(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];
    /* 7px tall: room for two 3px edges is exactly what it does NOT have. */
    const int n = ui_bezel_spans((mu_Rect){ 0, 0, 200, 7 }, FACE, false,
                                 s, UI_BEZEL_MAX_SPANS);

    TEST_ASSERT_EQUAL_INT(UI_BEZEL_MAX_SPANS, n);
    TEST_ASSERT_TRUE_MESSAGE(s[1].rect.h + s[3].rect.h < 7,
        "the top and bottom edges must not meet or cross - a control shorter "
        "than twice the bezel thickness has to thin its edges, not overlap "
        "them into a solid bar of edge colour");
}

static void test_a_zero_sized_control_produces_nothing(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];

    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        ui_bezel_spans((mu_Rect){ 4, 4, 0, 20 }, FACE, false, s,
                       UI_BEZEL_MAX_SPANS),
        "a rect with no width must produce no spans at all, not a face of "
        "zero width that still costs a command in the list");
    TEST_ASSERT_EQUAL_INT(0,
        ui_bezel_spans((mu_Rect){ 4, 4, 20, 0 }, FACE, false, s,
                       UI_BEZEL_MAX_SPANS));
}

static void test_a_buffer_too_small_produces_nothing(void)
{
    ui_span_t s[UI_BEZEL_MAX_SPANS];

    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        ui_bezel_spans(row(), FACE, false, s, UI_BEZEL_MAX_SPANS - 1),
        "a half-written bezel - a face with some edges missing - looks like a "
        "bug, so refuse rather than fill what fits");
}

void suite_ui_style(void)
{
    RUN_TEST(test_shading_by_zero_changes_nothing);
    RUN_TEST(test_full_shading_reaches_white_and_black);
    RUN_TEST(test_shading_leaves_alpha_alone);
    RUN_TEST(test_the_face_is_drawn_first_and_covers_the_whole_rect);
    RUN_TEST(test_every_span_stays_inside_the_control);
    RUN_TEST(test_a_raised_button_is_lit_from_the_top_left);
    RUN_TEST(test_a_sunken_button_swaps_which_edges_are_lit);
    RUN_TEST(test_the_shadowed_edges_are_drawn_over_the_lit_ones);
    RUN_TEST(test_a_control_too_small_for_a_bezel_gets_a_flat_face);
    RUN_TEST(test_a_thin_control_keeps_a_pixel_of_face_between_its_edges);
    RUN_TEST(test_a_zero_sized_control_produces_nothing);
    RUN_TEST(test_a_buffer_too_small_produces_nothing);
}

SUITE_REGISTER(suite_ui_style);
