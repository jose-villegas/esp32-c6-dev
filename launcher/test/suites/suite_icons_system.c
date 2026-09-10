/*=============================================================================
 * Portable suite: gfx/icons_system.h - the baked system icon atlas.
 *
 * The check mark's own artwork is pinned independently in suite_icons.c
 * (hand-transcribed expected rows, not read back from this generated
 * header) - this file checks facts a scan can pin down across the whole
 * atlas instead: run counts, struct self-consistency, one SVG import traced
 * by hand against its source.
 *
 * Everything here works over EVERY baked icon by index, never assuming a
 * 16-wide/2-byte-stride shape - the atlas mixes the 16x16 PNG-sourced check
 * mark with 24x24 SVG-sourced imports, and icon_t's own w/h/stride fields
 * are what make that mixing safe.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "suites.h"

#include "gfx/icon.h"
#include "gfx/icons_system.h"

static bool baked_bit(const icon_t *icon, int x, int y)
{
    const uint8_t byte = icon_system_rows[icon->offset + (unsigned)y * icon->stride + (unsigned)(x / 8)];
    return (byte & (0x80 >> (x % 8))) != 0;
}

/* A run-length walk of the UNPACKED rows, independent of count_runs() in
 * gen_icons.py - proving every baked `blocks` field against what the bytes
 * actually contain, not against the generator's own count of what it
 * intended to write. Covers both the PNG-sourced check mark and every
 * SVG-sourced import, whatever their own w/h/stride happen to be. */
static void test_all_icons_blocks_match_actual_run_length(void)
{
    for (int id = 0; id < ICON_SYSTEM_COUNT; id++) {
        const icon_t *icon = &icon_system_table[id];
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
        TEST_ASSERT_EQUAL_INT_MESSAGE(icon->blocks, total,
            "icon_t.blocks does not match an actual run-length walk of the "
            "unpacked rows for some icon in icon_system_table - the baked "
            "count and the real bytes disagree");
    }
}

static void test_all_icons_struct_fields_are_self_consistent(void)
{
    for (int id = 0; id < ICON_SYSTEM_COUNT; id++) {
        const icon_t *icon = &icon_system_table[id];
        TEST_ASSERT_EQUAL_INT_MESSAGE((icon->w + 7) / 8, icon->stride,
            "stride is not (w + 7) / 8 for some icon in icon_system_table");
        TEST_ASSERT_TRUE_MESSAGE(
            (size_t)icon->offset + (size_t)icon->stride * icon->h <= sizeof(icon_system_rows),
            "an icon's rows run past the end of icon_system_rows[]");
    }
}

/* design/icons/system/chevron-left.svg's 7 rectangles, traced BY HAND from
 * its path data rather than by calling gen_icons.py's SVG reader - a green
 * result proves the baked bytes against the source file, not the parser's
 * own idea of what it read. */
static const char *const chevron_left_expected_rows[24] = {
    "........................", /* row 0 */
    "........................",
    "........................",
    "........................",
    "........................",
    "..............XX........", /* row 5 */
    "..............XX........",
    "............XX..........",
    "............XX..........",
    "..........XX............",
    "..........XX............", /* row 10 */
    "........XX..............",
    "........XX..............",
    "..........XX............",
    "..........XX............",
    "............XX..........", /* row 15 */
    "............XX..........",
    "..............XX........",
    "..............XX........",
    "........................",
    "........................", /* row 20 */
    "........................",
    "........................",
    "........................",
};

static void test_chevron_left_matches_source_svg_rectangles(void)
{
    const icon_t *icon = &icon_system_table[ICON_SYSTEM_CHEVRON_LEFT];
    TEST_ASSERT_EQUAL_INT(24, icon->w);
    TEST_ASSERT_EQUAL_INT(24, icon->h);

    for (int y = 0; y < 24; y++) {
        const char *row = chevron_left_expected_rows[y];
        TEST_ASSERT_EQUAL_INT_MESSAGE(24, (int)strlen(row),
            "a hand-transcribed expected row is not 24 characters");
        for (int x = 0; x < 24; x++) {
            const bool want = row[x] == 'X';
            const bool got = baked_bit(icon, x, y);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want, got,
                "baked chevron_left diverges from chevron-left.svg's own "
                "rectangles - see the message above for row/col");
        }
    }
}

/* A second, independent extraction of the same geometry - built on
 * baked_bit()/icon_system_table lookups rather than on icon_walk_blocks()
 * or its internals - so agreement with the streaming walker below proves
 * the reshape into a callback changed no geometry, not merely that the
 * walker agrees with itself. */
static int reference_blocks(const icon_t *icon, int box_w, int box_h,
                            icon_rect_t *out, int max)
{
    const int iw = icon->w, ih = icon->h;
    int scale = box_w / iw;
    const int scale_h = box_h / ih;
    if (scale_h < scale) {
        scale = scale_h;
    }
    if (scale < 1) {
        scale = 1;
    }

    int min_x = iw, max_x = -1, min_y = ih, max_y = -1;
    for (int y = 0; y < ih; y++) {
        for (int x = 0; x < iw; x++) {
            if (!baked_bit(icon, x, y)) {
                continue;
            }
            if (x < min_x) { min_x = x; }
            if (x > max_x) { max_x = x; }
            if (y < min_y) { min_y = y; }
            if (y > max_y) { max_y = y; }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(max_x >= 0,
        "reference extraction found no ink in a baked icon");

    const int cw = max_x - min_x + 1;
    const int ch = max_y - min_y + 1;
    const int ox = (box_w - cw * scale) / 2 - min_x * scale;
    const int oy = (box_h - ch * scale) / 2 - min_y * scale;

    int n = 0;
    for (int y = 0; y < ih; y++) {
        int x = 0;
        while (x < iw) {
            if (!baked_bit(icon, x, y)) {
                x++;
                continue;
            }
            const int start = x;
            while (x < iw && baked_bit(icon, x, y)) {
                x++;
            }
            TEST_ASSERT_TRUE_MESSAGE(n < max,
                "reference extraction overflowed the test's own buffer");
            out[n].x = ox + start * scale;
            out[n].y = oy + y * scale;
            out[n].w = (x - start) * scale;
            out[n].h = scale;
            n++;
        }
    }
    return n;
}

/* home's own 50 is the largest baked run count in the atlas, so this bounds
 * every buffer below - malloc'd, not on-stack, per check_stack_usage.py's
 * gate (see its own header on the two device panics that gate exists for). */
#define TEST_MAX_BLOCKS 64

typedef struct {
    icon_rect_t *blocks;
    int count;
    int cap;
} collect_ctx_t;

static void collect_emit(void *ctx, int x, int y, int w, int h)
{
    collect_ctx_t *cc = ctx;
    TEST_ASSERT_TRUE_MESSAGE(cc->count < cc->cap,
        "icon_walk_blocks emitted more runs than the test's own buffer expects");
    cc->blocks[cc->count] = (icon_rect_t){ x, y, w, h };
    cc->count++;
}

/* Proves the reshape from an array-collecting extraction to a streaming
 * callback changed no geometry: every baked icon, at its native size, at
 * 2x, and at a deliberately non-square box (exercises the scale_w != scale_h
 * clamp), must emit the exact same rects as reference_blocks() above. */
static void test_streaming_walker_matches_reference_extraction(void)
{
    icon_rect_t *cc_buf = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    icon_rect_t *ref = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    TEST_ASSERT_NOT_NULL(cc_buf);
    TEST_ASSERT_NOT_NULL(ref);

    for (int id = 0; id < ICON_SYSTEM_COUNT; id++) {
        const icon_t *icon = &icon_system_table[id];
        const int box_sizes[][2] = {
            { icon->w, icon->h },
            { icon->w * 2, icon->h * 2 },
            { icon->w * 3 + 5, icon->h * 2 },
        };
        for (size_t s = 0; s < sizeof(box_sizes) / sizeof(box_sizes[0]); s++) {
            const int box_w = box_sizes[s][0], box_h = box_sizes[s][1];

            collect_ctx_t cc = { .blocks = cc_buf, .count = 0, .cap = TEST_MAX_BLOCKS };
            icon_walk_blocks(icon_system_rows + icon->offset, icon->w, icon->h,
                             icon->stride, box_w, box_h, collect_emit, &cc);

            const int ref_n = reference_blocks(icon, box_w, box_h, ref, TEST_MAX_BLOCKS);

            TEST_ASSERT_EQUAL_INT_MESSAGE(ref_n, cc.count,
                "icon_walk_blocks emitted a different run count than the "
                "reference extraction for some icon/box size");
            for (int i = 0; i < ref_n; i++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].x, cc_buf[i].x, "run x mismatch");
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].y, cc_buf[i].y, "run y mismatch");
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].w, cc_buf[i].w, "run w mismatch");
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].h, cc_buf[i].h, "run h mismatch");
            }
        }
    }

    free(cc_buf);
    free(ref);
}

/* Every pixelarticons import is already centred in its own 24x24 grid, so
 * content-bbox and full-grid centring coincide for all of them - only
 * icon_check's hand-drawn ink is off-centre (1px top margin, 2px bottom),
 * which is what actually catches a caller centring on w x h instead. At a
 * 32x32 box its first run (row 1, col 13) lands at (26, 3), not (26, 2) -
 * hand-derived from the centring formula. */
static void test_content_bbox_centring_uses_a_specific_expected_origin(void)
{
    const icon_t *icon = &icon_system_table[ICON_SYSTEM_CHECK];

    icon_rect_t *cc_buf = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    TEST_ASSERT_NOT_NULL(cc_buf);
    collect_ctx_t cc = { .blocks = cc_buf, .count = 0, .cap = TEST_MAX_BLOCKS };
    icon_walk_blocks(icon_system_rows + icon->offset, icon->w, icon->h,
                     icon->stride, 32, 32, collect_emit, &cc);

    TEST_ASSERT_TRUE_MESSAGE(cc.count > 0, "check emitted no runs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(26, cc_buf[0].x, "first run x");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, cc_buf[0].y, "first run y");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cc_buf[0].w, "first run w");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cc_buf[0].h, "first run h");
    free(cc_buf);
}

/* home.svg was excluded by RUN_COUNT_CAP (gen_icons.py) while it still
 * tracked ui_draw_bitmap()'s 48-slot stack buffer - see bd esp32c6-k2u.
 * Baking and walking it at 50 runs is the case that cap made impossible. */
static void test_home_bakes_and_walks_at_fifty_runs(void)
{
    const icon_t *icon = &icon_system_table[ICON_SYSTEM_HOME];
    TEST_ASSERT_EQUAL_INT_MESSAGE(50, icon->blocks,
        "home was expected to bake at 50 runs");

    icon_rect_t *cc_buf = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    TEST_ASSERT_NOT_NULL(cc_buf);
    collect_ctx_t cc = { .blocks = cc_buf, .count = 0, .cap = TEST_MAX_BLOCKS };
    icon_walk_blocks(icon_system_rows + icon->offset, icon->w, icon->h,
                     icon->stride, icon->w, icon->h, collect_emit, &cc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(50, cc.count,
        "icon_walk_blocks did not emit 50 runs for home at its native size");
    free(cc_buf);
}

void run_icons_system_suite(void)
{
    RUN_TEST(test_all_icons_blocks_match_actual_run_length);
    RUN_TEST(test_all_icons_struct_fields_are_self_consistent);
    RUN_TEST(test_chevron_left_matches_source_svg_rectangles);
    RUN_TEST(test_streaming_walker_matches_reference_extraction);
    RUN_TEST(test_content_bbox_centring_uses_a_specific_expected_origin);
    RUN_TEST(test_home_bakes_and_walks_at_fifty_runs);
}

SUITE_REGISTER(run_icons_system_suite);
