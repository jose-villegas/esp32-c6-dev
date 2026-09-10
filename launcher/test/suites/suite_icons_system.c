/*=============================================================================
 * Portable suite: gfx/icons_system.h - the baked system icon atlas.
 *
 * The test that matters is test_check_matches_icon_check_bitmap_exactly():
 * unpacks the baked check mark and compares it, bit for bit, against
 * icons.h's own hand-drawn icon_check_bitmap - artwork suite_icons.c already
 * pins independently, so agreement here proves gen_icons.py's PNG decode and
 * bit packing reproduce known-good artwork exactly, not merely that they
 * round-trip against themselves. suite_icons.c is untouched by this file and
 * stays the independent witness.
 *
 * Everything else in this file works over EVERY baked icon by index, never
 * assuming a 16-wide/2-byte-stride shape - the atlas mixes the 16x16
 * PNG-sourced check mark with 24x24 SVG-sourced imports, and icon_t's own
 * w/h/stride fields are what make that mixing safe.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "suites.h"

#include "gfx/icons.h"
#include "gfx/icons_system.h"

static bool baked_bit(const icon_t *icon, int x, int y)
{
    const uint8_t byte = icon_system_rows[icon->offset + (unsigned)y * icon->stride + (unsigned)(x / 8)];
    return (byte & (0x80 >> (x % 8))) != 0;
}

static void test_check_matches_icon_check_bitmap_exactly(void)
{
    const icon_t *icon = &icon_system_table[ICON_SYSTEM_CHECK];
    TEST_ASSERT_EQUAL_INT(ICON_CHECK_BITMAP_SIZE, icon->w);
    TEST_ASSERT_EQUAL_INT(ICON_CHECK_BITMAP_SIZE, icon->h);

    for (int y = 0; y < ICON_CHECK_BITMAP_SIZE; y++) {
        const uint16_t row = icon_check_bitmap[y];
        for (int x = 0; x < ICON_CHECK_BITMAP_SIZE; x++) {
            const bool want = (row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x))) != 0;
            const bool got = baked_bit(icon, x, y);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want, got,
                "baked check mark diverges from icons.h's icon_check_bitmap "
                "at some pixel - see the message above for which row/col "
                "TEST_ASSERT_EQUAL_INT stopped at");
        }
    }
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
        TEST_ASSERT_EQUAL_INT_MESSAGE(24, (int)strnlen(row, 25),
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

void run_icons_system_suite(void)
{
    RUN_TEST(test_check_matches_icon_check_bitmap_exactly);
    RUN_TEST(test_all_icons_blocks_match_actual_run_length);
    RUN_TEST(test_all_icons_struct_fields_are_self_consistent);
    RUN_TEST(test_chevron_left_matches_source_svg_rectangles);
}

SUITE_REGISTER(run_icons_system_suite);
