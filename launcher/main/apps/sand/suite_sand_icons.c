/*=============================================================================
 * Portable suite: icons_sand - structural facts about the brush screen's
 * baked artwork.
 *
 * Same reasoning as test/suites/suite_icons_system.c and CLAUDE.md's
 * generated-files convention: the expected rows below are transcribed by
 * hand from the artwork this app shipped before it was baked (not read back
 * from icons_sand.h itself), so a match here proves gen_icons.py reproduced
 * known-good pixels rather than merely round-tripping its own packer. Once
 * that is established, the rest of this file checks facts a scan can pin
 * down - non-empty, in-bounds, small enough to draw, symmetric where the
 * artwork is meant to be.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"
#include "suites.h"

#include "gfx/icon.h"
#include "gfx/icons.h"
#include "icons_sand.h"

static bool baked_bit(const icon_t *icon, int x, int y)
{
    const uint8_t byte = icon_sand_rows[icon->offset + (unsigned)y * icon->stride + (unsigned)(x / 8)];
    return (byte & (0x80 >> (x % 8))) != 0;
}

typedef struct {
    const char             *name;
    icon_sand_id_t          id;
} named_icon_t;

static const named_icon_t ICONS[] = {
    { "pour",  ICON_SAND_POUR },
    { "erase", ICON_SAND_ERASE },
    { "boom",  ICON_SAND_BOOM },
    { "info",  ICON_SAND_INFO },
};
#define ICON_COUNT (sizeof(ICONS) / sizeof(ICONS[0]))

static void test_every_icon_is_non_empty(void)
{
    for (size_t i = 0; i < ICON_COUNT; i++) {
        const icon_t *icon = &icon_sand_table[ICONS[i].id];
        bool set = false;
        for (int y = 0; y < icon->h && !set; y++) {
            for (int x = 0; x < icon->w; x++) {
                if (baked_bit(icon, x, y)) {
                    set = true;
                    break;
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(set, ICONS[i].name);
    }
}

/* Erase's own baked run count (28) is the largest of the four - big enough
 * buffer for any of this atlas's icons at native size without pulling in a
 * general worst case that belongs to a different artwork. */
#define SAND_ICON_TEST_MAX_BLOCKS 32

typedef struct {
    icon_rect_t *blocks;
    int          count;
    int          cap;
} collect_ctx_t;

static void collect_emit(void *ctx, int x, int y, int w, int h)
{
    collect_ctx_t *cc = ctx;
    TEST_ASSERT_TRUE_MESSAGE(cc->count < cc->cap,
        "icon_walk_blocks emitted more runs than the test's own buffer expects");
    cc->blocks[cc->count] = (icon_rect_t){ x, y, w, h };
    cc->count++;
}

/* At native size (box == the icon's own w x h, scale 1) every run
 * icon_walk_blocks() emits must land inside that same box - only possible if
 * the artwork's own content lives inside its declared grid. */
static void test_every_icon_content_bbox_is_inside_16x16(void)
{
    for (size_t i = 0; i < ICON_COUNT; i++) {
        const icon_t *icon = &icon_sand_table[ICONS[i].id];
        icon_rect_t blocks[SAND_ICON_TEST_MAX_BLOCKS];
        collect_ctx_t cc = { .blocks = blocks, .count = 0, .cap = SAND_ICON_TEST_MAX_BLOCKS };
        icon_walk_blocks(icon_sand_rows + icon->offset, icon->w, icon->h,
                         icon->stride, icon->w, icon->h, collect_emit, &cc);

        TEST_ASSERT_TRUE_MESSAGE(cc.count > 0, ICONS[i].name);
        for (int b = 0; b < cc.count; b++) {
            TEST_ASSERT_TRUE_MESSAGE(
                blocks[b].x >= 0 && blocks[b].y >= 0 &&
                blocks[b].x + blocks[b].w <= icon->w &&
                blocks[b].y + blocks[b].h <= icon->h,
                ICONS[i].name);
        }
    }
}

/* Transcribed from the four bitmaps apps/sand/sand_icons.h used to carry
 * (icon_pour_bitmap etc.) before they were baked - see this file's own top
 * comment for why the comparison is against a hand copy, not the header. */
static const char *const pour_expected_rows[16] = {
    ".XXXXXXXXXXXXXX.", /* row 0 */
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    ".....XXXXXX.....",
    "......XXXX......", /* row 5 */
    ".......XX.......",
    ".......XX.......",
    ".......XX.......",
    ".......XX.......",
    "................", /* row 10 */
    ".......X........",
    "................",
    "........X.......",
    "................",
    ".......X........", /* row 15 */
};

static const char *const erase_expected_rows[16] = {
    "XX............XX", /* row 0 */
    ".XX..........XX.",
    "..XX........XX..",
    "...XX......XX...",
    "....XX....XX....",
    ".....XX..XX.....", /* row 5 */
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    ".....XX..XX.....", /* row 10 */
    "....XX....XX....",
    "...XX......XX...",
    "..XX........XX..",
    ".XX..........XX.",
    "XX............XX", /* row 15 */
};

static const char *const boom_expected_rows[16] = {
    ".......XX.......", /* row 0 */
    ".......XX.......",
    ".......XX.......",
    "......XXXX......",
    ".....XXXXXX.....",
    "....XXXXXXXX....", /* row 5 */
    "..XXXXXXXXXXXX..",
    "XXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXX",
    "..XXXXXXXXXXXX..",
    "....XXXXXXXX....", /* row 10 */
    ".....XXXXXX.....",
    "......XXXX......",
    ".......XX.......",
    ".......XX.......",
    ".......XX.......", /* row 15 */
};

static const char *const info_expected_rows[16] = {
    "................", /* row 0 */
    "................",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "................", /* row 5 */
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......", /* row 10 */
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "................", /* row 15 */
};

typedef struct {
    icon_sand_id_t          id;
    const char *const      *rows;
} expected_icon_t;

static const expected_icon_t EXPECTED[] = {
    { ICON_SAND_POUR,  pour_expected_rows },
    { ICON_SAND_ERASE, erase_expected_rows },
    { ICON_SAND_BOOM,  boom_expected_rows },
    { ICON_SAND_INFO,  info_expected_rows },
};

static void test_every_icon_matches_the_artwork_it_replaced(void)
{
    for (size_t i = 0; i < sizeof(EXPECTED) / sizeof(EXPECTED[0]); i++) {
        const icon_t *icon = &icon_sand_table[EXPECTED[i].id];
        TEST_ASSERT_EQUAL_INT(16, icon->w);
        TEST_ASSERT_EQUAL_INT(16, icon->h);
        for (int y = 0; y < 16; y++) {
            const char *row = EXPECTED[i].rows[y];
            for (int x = 0; x < 16; x++) {
                const bool want = row[x] == 'X';
                const bool got = baked_bit(icon, x, y);
                TEST_ASSERT_EQUAL_INT_MESSAGE(want, got,
                    "a baked icon diverges from the bitmap it replaced - see "
                    "the message above for which row/col TEST_ASSERT_EQUAL_INT "
                    "stopped at");
            }
        }
    }
}

/* Successor to the old cap test, which checked artwork against
 * UI_DRAW_BITMAP_MAX_BLOCKS - gone now that ui_draw_icon() streams runs
 * instead of collecting them (gfx/icon.h). A run-length walk of the
 * UNPACKED rows, independent of gen_icons.py's count_runs(), proves each
 * baked `blocks` against the actual bytes, not the generator's own tally. */
static void test_every_icon_blocks_matches_actual_run_length(void)
{
    for (size_t i = 0; i < ICON_COUNT; i++) {
        const icon_t *icon = &icon_sand_table[ICONS[i].id];
        int total = 0;
        for (int y = 0; y < icon->h; y++) {
            bool in_run = false;
            for (int x = 0; x < icon->w; x++) {
                const bool on = baked_bit(icon, x, y);
                if (on && !in_run) {
                    total++;
                }
                in_run = on;
            }
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(icon->blocks, total, ICONS[i].name);
    }
}

/* Row `y` mirrors itself across the icon's own width: column i and column
 * (w - 1 - i) agree for every i. True for an icon symmetric left-right,
 * independent of any other row. */
static bool icon_is_left_right_symmetric(const icon_t *icon)
{
    for (int y = 0; y < icon->h; y++) {
        for (int x = 0; x < icon->w / 2; x++) {
            if (baked_bit(icon, x, y) != baked_bit(icon, icon->w - 1 - x, y)) {
                return false;
            }
        }
    }
    return true;
}

static bool icon_is_top_bottom_symmetric(const icon_t *icon)
{
    for (int y = 0; y < icon->h / 2; y++) {
        for (int x = 0; x < icon->w; x++) {
            if (baked_bit(icon, x, y) != baked_bit(icon, x, icon->h - 1 - y)) {
                return false;
            }
        }
    }
    return true;
}

/* ERASE (an X) and BOOM (a burst) are drawn to be symmetric on both axes. A
 * hand-placed diagonal or ray that drifted off-centre in one quadrant would
 * still look plausible by eye but fail here. */
static void test_erase_is_symmetric_both_axes(void)
{
    const icon_t *icon = &icon_sand_table[ICON_SAND_ERASE];
    TEST_ASSERT_TRUE(icon_is_left_right_symmetric(icon));
    TEST_ASSERT_TRUE(icon_is_top_bottom_symmetric(icon));
}

static void test_boom_is_symmetric_both_axes(void)
{
    const icon_t *icon = &icon_sand_table[ICON_SAND_BOOM];
    TEST_ASSERT_TRUE(icon_is_left_right_symmetric(icon));
    TEST_ASSERT_TRUE(icon_is_top_bottom_symmetric(icon));
}

/* INFO (a lowercase i) is drawn symmetric left-right only - a dot centred
 * over its own stem - and deliberately NOT top-bottom, or it would stop
 * reading as a letter. */
static void test_info_is_left_right_symmetric_but_not_top_bottom(void)
{
    const icon_t *icon = &icon_sand_table[ICON_SAND_INFO];
    TEST_ASSERT_TRUE(icon_is_left_right_symmetric(icon));
    TEST_ASSERT_FALSE(icon_is_top_bottom_symmetric(icon));
}

/* POUR (the funnel) is deliberately NOT symmetric on either axis - its three
 * falling drops step right-left-right below a symmetric funnel body, which
 * is what reads as motion rather than a static tail. */
static void test_pour_is_not_symmetric(void)
{
    const icon_t *icon = &icon_sand_table[ICON_SAND_POUR];
    TEST_ASSERT_FALSE(icon_is_left_right_symmetric(icon));
    TEST_ASSERT_FALSE(icon_is_top_bottom_symmetric(icon));
}

void run_sand_icons_suite(void)
{
    RUN_TEST(test_every_icon_is_non_empty);
    RUN_TEST(test_every_icon_content_bbox_is_inside_16x16);
    RUN_TEST(test_every_icon_matches_the_artwork_it_replaced);
    RUN_TEST(test_every_icon_blocks_matches_actual_run_length);
    RUN_TEST(test_erase_is_symmetric_both_axes);
    RUN_TEST(test_boom_is_symmetric_both_axes);
    RUN_TEST(test_info_is_left_right_symmetric_but_not_top_bottom);
    RUN_TEST(test_pour_is_not_symmetric);
}

SUITE_REGISTER(run_sand_icons_suite);
