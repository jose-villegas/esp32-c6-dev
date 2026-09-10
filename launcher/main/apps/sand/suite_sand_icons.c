/*=============================================================================
 * Portable suite: sand_icons - structural facts about the brush screen's
 * hand-drawn artwork.
 *
 * Same reasoning as test/suites/suite_icons.c and CLAUDE.md's generated-files
 * convention: there is no underlying math to check a hand-placed bitmap
 * against, so this checks facts a _Static_assert or a simple scan can pin
 * down - non-empty, in-bounds, small enough to draw, symmetric where the
 * artwork is meant to be - never the actual pixel shape against the code
 * that draws it.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"
#include "suites.h"

#include "gfx/icons.h"
#include "sand_icons.h"
#include "ui/ui.h"

typedef struct {
    const char       *name;
    const uint16_t    *bitmap;
} named_bitmap_t;

static const named_bitmap_t ICONS[] = {
    { "pour",  icon_pour_bitmap },
    { "erase", icon_erase_bitmap },
    { "boom",  icon_boom_bitmap },
    { "info",  icon_info_bitmap },
};
#define ICON_COUNT (sizeof(ICONS) / sizeof(ICONS[0]))

static int popcount16(uint16_t v)
{
    int c = 0;
    while (v) {
        c += (int)(v & 1u);
        v = (uint16_t)(v >> 1);
    }
    return c;
}

static void test_every_icon_is_non_empty(void)
{
    for (size_t i = 0; i < ICON_COUNT; i++) {
        int set = 0;
        for (int y = 0; y < ICON_BITMAP_SIZE; y++) {
            set += popcount16(ICONS[i].bitmap[y]);
        }
        TEST_ASSERT_TRUE_MESSAGE(set > 0, ICONS[i].name);
    }
}

/* ICON_BITMAP_MAX_BLOCKS (128) is the general worst case, not this artwork's
 * - a buffer that size overflows the device's small test-task stack (see
 * check_stack_usage's ceiling). None of these four bitmaps comes close to
 * 32 runs, so that is buffer enough without pulling in the general bound. */
#define SAND_ICON_TEST_MAX_BLOCKS 32

/* Same technique suite_icons.c's assert_all_inside_box() uses: the blocks
 * icon_bitmap_blocks() returns for a box exactly ICON_BITMAP_SIZE square
 * (scale 1, no centring margin to eat into) must stay within it, which is
 * only possible if the artwork's own content lives inside its 16x16 grid. */
static void test_every_icon_content_bbox_is_inside_16x16(void)
{
    for (size_t i = 0; i < ICON_COUNT; i++) {
        icon_rect_t blocks[SAND_ICON_TEST_MAX_BLOCKS];
        const int n = icon_bitmap_blocks(ICONS[i].bitmap, ICON_BITMAP_SIZE,
                                         ICON_BITMAP_SIZE, blocks,
                                         SAND_ICON_TEST_MAX_BLOCKS);
        TEST_ASSERT_TRUE_MESSAGE(n > 0, ICONS[i].name);
        for (int b = 0; b < n; b++) {
            TEST_ASSERT_TRUE_MESSAGE(
                blocks[b].x >= 0 && blocks[b].y >= 0 &&
                blocks[b].x + blocks[b].w <= ICON_BITMAP_SIZE &&
                blocks[b].y + blocks[b].h <= ICON_BITMAP_SIZE,
                ICONS[i].name);
        }
    }
}

/* Sweeps every box size from the module's floor (ICON_BITMAP_SIZE, below
 * which icon_bitmap_blocks() still clamps to scale 1) well past anything the
 * brush screen is likely to draw at, since Phase 5 has not fixed exact
 * on-screen sizes yet. A run's count is scale-invariant (icons.h's own
 * comment), so this is the test that keeps a future layout change from
 * silently truncating an icon under ui_draw_bitmap()'s stack cap. */
static void test_every_icon_fits_ui_draw_bitmap_cap_at_every_size(void)
{
    for (int side = ICON_BITMAP_SIZE; side <= 96; side++) {
        for (size_t i = 0; i < ICON_COUNT; i++) {
            icon_rect_t blocks[UI_DRAW_BITMAP_MAX_BLOCKS + 1];
            const int n = icon_bitmap_blocks(ICONS[i].bitmap, side, side,
                                             blocks, UI_DRAW_BITMAP_MAX_BLOCKS + 1);
            TEST_ASSERT_TRUE_MESSAGE(n <= UI_DRAW_BITMAP_MAX_BLOCKS,
                ICONS[i].name);
        }
    }
}

/* Row `y` read as a 16-bit value mirrors itself: bit i and bit
 * (ICON_BITMAP_SIZE - 1 - i) agree for every i. True for a bitmap that is
 * symmetric left-right, independent of any other row. */
static bool row_is_mirror_symmetric(uint16_t row)
{
    for (int i = 0; i < ICON_BITMAP_SIZE / 2; i++) {
        const bool left  = (row & (uint16_t)(1u << (ICON_BITMAP_SIZE - 1 - i))) != 0;
        const bool right = (row & (uint16_t)(1u << i)) != 0;
        if (left != right) {
            return false;
        }
    }
    return true;
}

static bool bitmap_is_left_right_symmetric(const uint16_t *bitmap)
{
    for (int y = 0; y < ICON_BITMAP_SIZE; y++) {
        if (!row_is_mirror_symmetric(bitmap[y])) {
            return false;
        }
    }
    return true;
}

static bool bitmap_is_top_bottom_symmetric(const uint16_t *bitmap)
{
    for (int y = 0; y < ICON_BITMAP_SIZE / 2; y++) {
        if (bitmap[y] != bitmap[ICON_BITMAP_SIZE - 1 - y]) {
            return false;
        }
    }
    return true;
}

/* ERASE (an X) and BOOM (a burst) are drawn to be symmetric on both axes -
 * see sand_icons.h's own comments on each. A hand-placed diagonal or ray
 * that drifted off-centre in one quadrant would still look plausible by eye
 * but fail here. */
static void test_erase_is_symmetric_both_axes(void)
{
    TEST_ASSERT_TRUE(bitmap_is_left_right_symmetric(icon_erase_bitmap));
    TEST_ASSERT_TRUE(bitmap_is_top_bottom_symmetric(icon_erase_bitmap));
}

static void test_boom_is_symmetric_both_axes(void)
{
    TEST_ASSERT_TRUE(bitmap_is_left_right_symmetric(icon_boom_bitmap));
    TEST_ASSERT_TRUE(bitmap_is_top_bottom_symmetric(icon_boom_bitmap));
}

/* INFO (a lowercase i) is drawn symmetric left-right only - a dot centred
 * over its own stem - and deliberately NOT top-bottom, or it would stop
 * reading as a letter. */
static void test_info_is_left_right_symmetric_but_not_top_bottom(void)
{
    TEST_ASSERT_TRUE(bitmap_is_left_right_symmetric(icon_info_bitmap));
    TEST_ASSERT_FALSE(bitmap_is_top_bottom_symmetric(icon_info_bitmap));
}

/* POUR (the funnel) is deliberately NOT symmetric on either axis - its three
 * falling drops step right-left-right below a symmetric funnel body, which
 * is what reads as motion rather than a static tail. See sand_icons.h. */
static void test_pour_is_not_symmetric(void)
{
    TEST_ASSERT_FALSE(bitmap_is_left_right_symmetric(icon_pour_bitmap));
    TEST_ASSERT_FALSE(bitmap_is_top_bottom_symmetric(icon_pour_bitmap));
}

void run_sand_icons_suite(void)
{
    RUN_TEST(test_every_icon_is_non_empty);
    RUN_TEST(test_every_icon_content_bbox_is_inside_16x16);
    RUN_TEST(test_every_icon_fits_ui_draw_bitmap_cap_at_every_size);
    RUN_TEST(test_erase_is_symmetric_both_axes);
    RUN_TEST(test_boom_is_symmetric_both_axes);
    RUN_TEST(test_info_is_left_right_symmetric_but_not_top_bottom);
    RUN_TEST(test_pour_is_not_symmetric);
}

SUITE_REGISTER(run_sand_icons_suite);
