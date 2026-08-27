/*=============================================================================
 * Portable suite: icon_check_blocks - the check mark's geometry.
 *
 * icons.h splits the check mark into geometry (icon_check_blocks(), a
 * `static inline` in the header so it links on a host with no gfx.h) and
 * drawing (icon_check(), in icons.c, which calls gfx_fill_rect() and so
 * cannot). This suite exercises only the geometry - nobody can eyeball a
 * handful of small rectangles reliably, so the shape is checked here instead
 * of by looking at the screen.
 *
 * The two sizes exercised throughout are the module's two real callers:
 *   - 18px, app_sand.c's PALETTE_BADGE_SIZE (scale 1 - see icon_check_blocks'
 *     own comment on why the badge gets no room for 2x);
 *   - 64px, the icon rect mu_checkbox() draws at (scale 4). mu_checkbox() in
 *     components/microui/src/microui.c builds its box as
 *     mu_rect(r.x, r.y, r.h, r.h), and r.h is UI_ROW_HEIGHT (64 - see ui.h),
 *     the row height app_diagnostics.c's toggle screen lays its two
 *     mu_checkbox() rows out at.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "unity.h"
#include "suites.h"

#include "icons.h"

static int blocks_at(int w, int h, icon_rect_t *out)
{
    return icon_check_blocks(w, h, out, ICON_CHECK_MAX_BLOCKS);
}

static void assert_all_inside_box(int w, int h)
{
    icon_rect_t blocks[ICON_CHECK_MAX_BLOCKS];
    const int n = blocks_at(w, h, blocks);

    TEST_ASSERT_TRUE_MESSAGE(n > 0, "produced no blocks at all");
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(blocks[i].x >= 0 && blocks[i].y >= 0,
            "a block's origin fell outside the (0,0,w,h) box");
        TEST_ASSERT_TRUE_MESSAGE(blocks[i].x + blocks[i].w <= w &&
                                 blocks[i].y + blocks[i].h <= h,
            "a block's far edge fell outside the (0,0,w,h) box");
    }
}

static void test_18px_badge_fits_inside_its_box(void)
{
    assert_all_inside_box(18, 18);
}

static void test_64px_checkbox_fits_inside_its_box(void)
{
    assert_all_inside_box(64, 64);
}

/* Sweeps every size from the module's floor (16px - see icon_check_blocks'
 * own comment on why a box smaller than the bitmap still gets scale 1) up
 * past both real call sites, so containment is proven as a property rather
 * than only at the two sizes anything happens to call with today. */
static void test_fits_inside_its_box_across_the_supported_range(void)
{
    for (int side = 16; side <= 80; side++) {
        assert_all_inside_box(side, side);
    }
}

static void test_return_never_exceeds_max(void)
{
    /* Sized to the largest `max` tried below (20), bigger than
     * ICON_CHECK_MAX_BLOCKS on purpose - a `max` more generous than the
     * shape needs must still cap at ICON_CHECK_MAX_BLOCKS, not overrun into
     * whatever the caller left past the shape's own runs. */
    icon_rect_t blocks[20];
    const int maxes[] = { 0, 1, 2, 3, 8, 16, 20 };

    for (size_t i = 0; i < sizeof(maxes) / sizeof(maxes[0]); i++) {
        const int max = maxes[i];
        const int n = icon_check_blocks(64, 64, blocks, max);
        TEST_ASSERT_TRUE_MESSAGE(n <= max,
            "returned more blocks than the caller's buffer can hold");
        TEST_ASSERT_TRUE_MESSAGE(n <= ICON_CHECK_MAX_BLOCKS,
            "returned more blocks than ICON_CHECK_MAX_BLOCKS promises");
    }
}

static void test_does_not_write_past_a_smaller_buffer(void)
{
    /* One extra slot past what the function is told it may use, poisoned
     * with a sentinel no real block would ever produce. If
     * icon_check_blocks() ignores `max` and keeps writing, this slot
     * changes and the test catches it; a correct implementation never
     * touches index `max` at all. */
    icon_rect_t out[ICON_CHECK_MAX_BLOCKS + 1];
    const icon_rect_t sentinel = { -999, -999, -999, -999 };
    const int max = 5;

    out[max] = sentinel;
    const int n = icon_check_blocks(64, 64, out, max);

    TEST_ASSERT_TRUE_MESSAGE(n <= max, "wrote more blocks than max allowed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(sentinel.x, out[max].x,
        "wrote past the caller's buffer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(sentinel.y, out[max].y,
        "wrote past the caller's buffer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(sentinel.w, out[max].w,
        "wrote past the caller's buffer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(sentinel.h, out[max].h,
        "wrote past the caller's buffer");
}

/* The bounding box of every returned block, at whatever scale, must be
 * centred within (0, 0, w, h) to within a pixel on both axes - the whole
 * point of icon_check_blocks() scanning the bitmap's own content box rather
 * than centring the full (mostly empty) 16x16 bitmap. */
static void assert_centred_within_a_pixel(int w, int h)
{
    icon_rect_t blocks[ICON_CHECK_MAX_BLOCKS];
    const int n = blocks_at(w, h, blocks);
    TEST_ASSERT_TRUE(n > 0);

    int min_x = blocks[0].x, max_x = blocks[0].x + blocks[0].w;
    int min_y = blocks[0].y, max_y = blocks[0].y + blocks[0].h;
    for (int i = 1; i < n; i++) {
        if (blocks[i].x < min_x) { min_x = blocks[i].x; }
        if (blocks[i].y < min_y) { min_y = blocks[i].y; }
        const int right  = blocks[i].x + blocks[i].w;
        const int bottom = blocks[i].y + blocks[i].h;
        if (right  > max_x) { max_x = right; }
        if (bottom > max_y) { max_y = bottom; }
    }

    const int left_margin   = min_x;
    const int right_margin  = w - max_x;
    const int top_margin    = min_y;
    const int bottom_margin = h - max_y;

    TEST_ASSERT_TRUE_MESSAGE(abs(left_margin - right_margin) <= 1,
        "left/right margins around the mark differ by more than a pixel");
    TEST_ASSERT_TRUE_MESSAGE(abs(top_margin - bottom_margin) <= 1,
        "top/bottom margins around the mark differ by more than a pixel");
}

static void test_centred_at_18px(void)
{
    assert_centred_within_a_pixel(18, 18);
}

static void test_centred_at_64px(void)
{
    assert_centred_within_a_pixel(64, 64);
}

static void test_centred_at_32px(void)
{
    /* A size neither real caller uses (scale 2), so centring is checked at
     * more than just the two sizes anything happens to draw at today. */
    assert_centred_within_a_pixel(32, 32);
}

/* Pins the hand-drawn bitmap itself (icons.h's icon_check_bitmap), not just
 * whatever icon_check_blocks() does with it - a shape that regressed to
 * empty, solid, or wildly different from the reviewed picture in icons.h's
 * own comment should fail here even if the scaling/centring math above it
 * is still correct. */
static int popcount16(uint16_t v)
{
    int c = 0;
    while (v) {
        c += (int)(v & 1u);
        v = (uint16_t)(v >> 1);
    }
    return c;
}

static void test_bitmap_is_neither_empty_nor_full(void)
{
    int set = 0;
    for (int y = 0; y < ICON_CHECK_BITMAP_SIZE; y++) {
        set += popcount16(icon_check_bitmap[y]);
    }
    /* 256 native pixels total. A recognisable check mark's stroke covers a
     * clear minority of its box - well under half - but is not a handful of
     * stray pixels either. 20..100 is generous either side of this bitmap's
     * actual count while still catching "went empty" or "went solid". */
    TEST_ASSERT_TRUE_MESSAGE(set >= 20 && set <= 100,
        "the bitmap's set-pixel count is outside a sane range for a check "
        "mark - it may have gone empty, solid, or been redrawn into "
        "something unrecognisable");
}

/* Every row of the bitmap has at most two separate runs (the three rows
 * where both limbs are visible at once - see icons.h's own picture) and
 * ICON_CHECK_MAX_BLOCKS (16) is exactly the sum of every row's run count -
 * the worst case the module actually needs, not a round number picked for
 * headroom. A bitmap edit that adds a run anywhere must fail this test
 * before it can silently overflow a caller's buffer sized to the old
 * constant. */
static void test_max_blocks_matches_the_bitmaps_actual_worst_case(void)
{
    int total_runs = 0;
    for (int y = 0; y < ICON_CHECK_BITMAP_SIZE; y++) {
        const uint16_t row = icon_check_bitmap[y];
        bool in_run = false;
        for (int x = 0; x < ICON_CHECK_BITMAP_SIZE; x++) {
            const bool set = (row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x))) != 0;
            if (set && !in_run) {
                total_runs++;
            }
            in_run = set;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ICON_CHECK_MAX_BLOCKS, total_runs,
        "ICON_CHECK_MAX_BLOCKS must equal the bitmap's actual total run "
        "count, or a caller sized to the constant can overflow");
}

/* The stroke is 2-3 native px through the body - see icons.h's own comment.
 * Exactly one run is allowed to be thinner than that: the long limb's
 * free-end taper (icons.h's own picture, row 1, a single pixel) - every
 * other run must meet the 2px floor, or the mark is reading as scattered
 * dots rather than a stroke. */
static void test_stroke_is_at_least_2px_native_except_the_one_tapered_tip(void)
{
    int thin_runs = 0;
    for (int y = 0; y < ICON_CHECK_BITMAP_SIZE; y++) {
        const uint16_t row = icon_check_bitmap[y];
        int x = 0;
        while (x < ICON_CHECK_BITMAP_SIZE) {
            if (!(row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x)))) {
                x++;
                continue;
            }
            const int run_start = x;
            while (x < ICON_CHECK_BITMAP_SIZE &&
                  (row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x)))) {
                x++;
            }
            if (x - run_start < 2) {
                thin_runs++;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(thin_runs <= 1,
        "more than one run in the bitmap is thinner than 2px - the stroke "
        "should taper at exactly one free end, not read as dots throughout");
}

void run_icons_suite(void)
{
    RUN_TEST(test_18px_badge_fits_inside_its_box);
    RUN_TEST(test_64px_checkbox_fits_inside_its_box);
    RUN_TEST(test_fits_inside_its_box_across_the_supported_range);
    RUN_TEST(test_return_never_exceeds_max);
    RUN_TEST(test_does_not_write_past_a_smaller_buffer);
    RUN_TEST(test_centred_at_18px);
    RUN_TEST(test_centred_at_64px);
    RUN_TEST(test_centred_at_32px);
    RUN_TEST(test_bitmap_is_neither_empty_nor_full);
    RUN_TEST(test_max_blocks_matches_the_bitmaps_actual_worst_case);
    RUN_TEST(test_stroke_is_at_least_2px_native_except_the_one_tapered_tip);
}

SUITE_REGISTER(run_icons_suite);
