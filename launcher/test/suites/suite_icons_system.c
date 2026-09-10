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
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * gen_icons.py - proving the baked `blocks` field against what the bytes
 * actually contain, not against the generator's own count of what it
 * intended to write. */
static void test_check_run_count_matches_baked_blocks(void)
{
    const icon_t *icon = &icon_system_table[ICON_SYSTEM_CHECK];
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
        "unpacked rows - the baked count and the real bytes disagree");
}

static void test_check_struct_fields_are_self_consistent(void)
{
    const icon_t *icon = &icon_system_table[ICON_SYSTEM_CHECK];
    TEST_ASSERT_EQUAL_INT_MESSAGE((icon->w + 7) / 8, icon->stride,
        "stride is not (w + 7) / 8");
    TEST_ASSERT_TRUE_MESSAGE(
        (size_t)icon->offset + (size_t)icon->stride * icon->h <= sizeof(icon_system_rows),
        "icon's rows run past the end of icon_system_rows[]");
}

void run_icons_system_suite(void)
{
    RUN_TEST(test_check_matches_icon_check_bitmap_exactly);
    RUN_TEST(test_check_run_count_matches_baked_blocks);
    RUN_TEST(test_check_struct_fields_are_self_consistent);
}

SUITE_REGISTER(run_icons_system_suite);
