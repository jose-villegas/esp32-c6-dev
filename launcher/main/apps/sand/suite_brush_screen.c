/*
 * Portable suite: brush_screen - the three-panel layout for the sand app's
 * brush screen.
 *
 * Every invariant is checked at BOTH real canvases - 368x448 portrait and
 * its quarter turn, 448x368 - since a layout that only holds upright is
 * exactly the bug this suite exists to catch (see brush_screen.c's own
 * BLOCK_H comment on why 368, the shorter height, is the binding one).
 */

#include <stdbool.h>
#include <stddef.h>

#include "unity.h"
#include "suites.h"

#include "brush_screen.h"
#include "gfx/gfx_font_roles.h"

#define PORTRAIT_W 368
#define PORTRAIT_H 448
#define LANDSCAPE_W 448
#define LANDSCAPE_H 368

/* Smallest touch target this design accepts - see palette.h's own "WHY FOUR
 * COLUMNS" for the same 44px floor derived from a fingertip's contact patch. */
#define MIN_TAP 44

static brush_screen_layout_t fixture(int screen_w, int screen_h)
{
    brush_screen_layout_t layout;
    brush_screen_layout(screen_w, screen_h, &layout);
    return layout;
}

static bool inside_canvas(mu_Rect r, int screen_w, int screen_h)
{
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
           r.x + r.w <= screen_w && r.y + r.h <= screen_h;
}

static bool rects_overlap(mu_Rect a, mu_Rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static int min_dim(mu_Rect r)
{
    return (r.w < r.h) ? r.w : r.h;
}

/* Every rect stays on the canvas */

static void assert_all_rects_inside(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);

    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.header_panel, screen_w, screen_h), "header_panel");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.mode_panel, screen_w, screen_h), "mode_panel");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.size_panel, screen_w, screen_h), "size_panel");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.swatch, screen_w, screen_h), "swatch");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.material_caption, screen_w, screen_h), "material_caption");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.material_name, screen_w, screen_h), "material_name");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.info_button, screen_w, screen_h), "info_button");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.mode_caption, screen_w, screen_h), "mode_caption");
    for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.segments[i], screen_w, screen_h), "segments[i]");
    }
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.size_caption, screen_w, screen_h), "size_caption");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.size_value, screen_w, screen_h), "size_value");
    TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.slider_track, screen_w, screen_h), "slider_track");
}

static void test_every_rect_inside_canvas_portrait(void)
{
    assert_all_rects_inside(PORTRAIT_W, PORTRAIT_H);
}

static void test_every_rect_inside_canvas_landscape(void)
{
    assert_all_rects_inside(LANDSCAPE_W, LANDSCAPE_H);
}

/* Panels: no overlap, top-to-bottom order */

static void assert_panels_stacked(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);

    TEST_ASSERT_FALSE_MESSAGE(rects_overlap(l.header_panel, l.mode_panel),
        "header and mode panels overlap");
    TEST_ASSERT_FALSE_MESSAGE(rects_overlap(l.mode_panel, l.size_panel),
        "mode and size panels overlap");
    TEST_ASSERT_FALSE_MESSAGE(rects_overlap(l.header_panel, l.size_panel),
        "header and size panels overlap");

    TEST_ASSERT_TRUE_MESSAGE(l.header_panel.y + l.header_panel.h <= l.mode_panel.y,
        "header does not sit above mode");
    TEST_ASSERT_TRUE_MESSAGE(l.mode_panel.y + l.mode_panel.h <= l.size_panel.y,
        "mode does not sit above size");
}

static void test_panels_stacked_top_to_bottom_portrait(void)
{
    assert_panels_stacked(PORTRAIT_W, PORTRAIT_H);
}

static void test_panels_stacked_top_to_bottom_landscape(void)
{
    assert_panels_stacked(LANDSCAPE_W, LANDSCAPE_H);
}

/* Segments: equal width, equal gaps, inside the mode panel */

static void assert_segments_equal_and_contained(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);

    for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(inside_canvas(l.segments[i], l.mode_panel.x + l.mode_panel.w,
                                                l.mode_panel.y + l.mode_panel.h) &&
                                  l.segments[i].x >= l.mode_panel.x &&
                                  l.segments[i].y >= l.mode_panel.y,
            "a segment leaves the mode panel");
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(l.segments[0].w, l.segments[1].w,
        "segment 0 and 1 widths differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(l.segments[1].w, l.segments[2].w,
        "segment 1 and 2 widths differ");

    const int gap01 = l.segments[1].x - (l.segments[0].x + l.segments[0].w);
    const int gap12 = l.segments[2].x - (l.segments[1].x + l.segments[1].w);
    TEST_ASSERT_EQUAL_INT_MESSAGE(gap01, gap12, "gap between segments 0/1 and 1/2 differ");
}

static void test_segments_equal_and_contained_portrait(void)
{
    assert_segments_equal_and_contained(PORTRAIT_W, PORTRAIT_H);
}

static void test_segments_equal_and_contained_landscape(void)
{
    assert_segments_equal_and_contained(LANDSCAPE_W, LANDSCAPE_H);
}

/* Header: swatch is square, info button is square and clear of the name */

static void assert_header_shapes(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);

    TEST_ASSERT_EQUAL_INT_MESSAGE(l.swatch.w, l.swatch.h, "swatch is not square");
    TEST_ASSERT_EQUAL_INT_MESSAGE(l.info_button.w, l.info_button.h, "info button is not square");
    TEST_ASSERT_FALSE_MESSAGE(rects_overlap(l.info_button, l.material_name),
        "info button overlaps the material name");
}

static void test_header_shapes_portrait(void)
{
    assert_header_shapes(PORTRAIT_W, PORTRAIT_H);
}

static void test_header_shapes_landscape(void)
{
    assert_header_shapes(LANDSCAPE_W, LANDSCAPE_H);
}

/* Size row: value sits right of the caption, no overlap */

static void assert_size_row(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);

    TEST_ASSERT_TRUE_MESSAGE(l.size_caption.x + l.size_caption.w <= l.size_value.x,
        "size value is not to the right of the size caption");
    TEST_ASSERT_FALSE_MESSAGE(rects_overlap(l.size_caption, l.size_value),
        "size caption and value overlap");
}

static void test_size_row_portrait(void)
{
    assert_size_row(PORTRAIT_W, PORTRAIT_H);
}

static void test_size_row_landscape(void)
{
    assert_size_row(LANDSCAPE_W, LANDSCAPE_H);
}

/*
 * Tap targets: segments, info button, slider track >= MIN_TAP in the
 * smaller dimension
 */

static void assert_tap_targets(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);

    for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(min_dim(l.segments[i]) >= MIN_TAP,
            "a brush-mode segment is smaller than a fingertip");
    }
    TEST_ASSERT_TRUE_MESSAGE(min_dim(l.info_button) >= MIN_TAP,
        "the info button is smaller than a fingertip");
    TEST_ASSERT_TRUE_MESSAGE(min_dim(l.slider_track) >= MIN_TAP,
        "the slider track is smaller than a fingertip");
}

static void test_tap_targets_portrait(void)
{
    assert_tap_targets(PORTRAIT_W, PORTRAIT_H);
}

static void test_tap_targets_landscape(void)
{
    assert_tap_targets(LANDSCAPE_W, LANDSCAPE_H);
}


/* Every fixed string on this screen has to FIT the rect the layout gave it.
 * Twice now a caption was sized by eye and clipped its own tail on the
 * narrower canvas - "06 PX" against a 64px value box, then the size row's
 * caption against a 232px one. Measuring the real glyph metrics against the
 * real rect is the only check that catches it without a device. */
static void assert_captions_fit(int screen_w, int screen_h)
{
    const brush_screen_layout_t l = fixture(screen_w, screen_h);
    const int scale = BRUSH_SCREEN_CAPTION_SCALE;

    struct { const char *str; mu_Rect r; } fixed[] = {
        { BRUSH_SCREEN_MATERIAL_CAPTION, l.material_caption },
        { BRUSH_SCREEN_MODE_CAPTION,     l.mode_caption },
    };
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            gfx_font_text_width(gfx_font_ui(), fixed[i].str, -1, scale) <= fixed[i].r.w,
            fixed[i].str);
    }

    /* Whichever mode is selected drives the size caption and each segment
     * carries its own label, so the longest of each has to fit, not just
     * the one that happens to show first. */
    for (int seg = 0; seg < BRUSH_SCREEN_SEGMENT_COUNT; seg++) {
        const char *caption = brush_screen_size_caption((brush_screen_segment_t)seg);
        TEST_ASSERT_TRUE_MESSAGE(
            gfx_font_text_width(gfx_font_ui(), caption, -1, scale) <= l.size_caption.w,
            caption);

        const char *label = brush_screen_segment_label((brush_screen_segment_t)seg);
        TEST_ASSERT_TRUE_MESSAGE(
            gfx_font_text_width(gfx_font_ui(), label, -1, scale) <= l.segments[seg].w,
            label);
    }

    /* The widest the value ever gets: two digits, a space and "PX". */
    TEST_ASSERT_TRUE_MESSAGE(
        gfx_font_text_width(gfx_font_ui(), "00 PX", -1, scale) <= l.size_value.w,
        "the size value must fit its own box at every radius");
}

static void test_captions_fit_their_rects_portrait(void)
{
    assert_captions_fit(368, 448);
}

static void test_captions_fit_their_rects_landscape(void)
{
    assert_captions_fit(448, 368);
}

void run_brush_screen_suite(void)
{
    RUN_TEST(test_captions_fit_their_rects_portrait);
    RUN_TEST(test_captions_fit_their_rects_landscape);
    RUN_TEST(test_every_rect_inside_canvas_portrait);
    RUN_TEST(test_every_rect_inside_canvas_landscape);
    RUN_TEST(test_panels_stacked_top_to_bottom_portrait);
    RUN_TEST(test_panels_stacked_top_to_bottom_landscape);
    RUN_TEST(test_segments_equal_and_contained_portrait);
    RUN_TEST(test_segments_equal_and_contained_landscape);
    RUN_TEST(test_header_shapes_portrait);
    RUN_TEST(test_header_shapes_landscape);
    RUN_TEST(test_size_row_portrait);
    RUN_TEST(test_size_row_landscape);
    RUN_TEST(test_tap_targets_portrait);
    RUN_TEST(test_tap_targets_landscape);
}

SUITE_REGISTER(run_brush_screen_suite);
