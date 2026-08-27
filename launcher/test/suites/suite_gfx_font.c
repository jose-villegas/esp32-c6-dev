/*=============================================================================
 * Portable suite: gfx_font - the pure metrics half of a font descriptor.
 *
 * gfx_font.h splits a font into pure metrics (gfx_font_advance(),
 * gfx_font_text_width(), gfx_font_height() - `static inline` in the header,
 * same reason icon_check_blocks() is in icons.h: it links on a host with no
 * gfx.h, no BSP, no drivers) and drawing (gfx_text_font() in gfx.c, which
 * calls gfx_fill_rect() and so cannot). This suite exercises only the
 * metrics, the same split suite_icons.c makes for icons.h.
 *
 * gfx.h is deliberately NOT included here - it pulls in bsp/esp-bsp.h, which
 * does not compile on a host. That means GFX_CHAR_W, GFX_CHAR_H and
 * GFX_GLYPH_SCALE (gfx.h) are not reachable from this file, so the few
 * assertions that need "the size the UI is laid out around" mirror
 * GFX_GLYPH_SCALE as a local constant instead of including it - see
 * MIRRORED_GLYPH_SCALE below. Everything else is derived from
 * gfx_font_8x8's own fields (cell_w, cell_h) rather than a second hardcoded
 * 8, so a change to the bitmap's cell size cannot silently drift out of
 * step with what this suite expects.
 *===========================================================================*/

#include <string.h>

#include "unity.h"
#include "suites.h"

#include "gfx_font.h"

/* Mirrors gfx.h's GFX_GLYPH_SCALE (8x8 glyphs drawn at 2x - see gfx.h's own
 * comment on GFX_CHAR_W/GFX_CHAR_H). Kept in one place, right where it is
 * used, rather than repeated as a magic 2 at every call site below. */
#define MIRRORED_GLYPH_SCALE 2

/*---------------------------------------------------------------------------
 * gfx_font_8x8 - the real, shipped font
 *-------------------------------------------------------------------------*/

static void test_default_font_width_matches_char_w_per_character(void)
{
    /* GFX_CHAR_W is 8 * GFX_GLYPH_SCALE (gfx.h) - i.e. cell_w * scale here. */
    const int char_w = gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE;

    TEST_ASSERT_EQUAL_INT(char_w,
        gfx_font_text_width(&gfx_font_8x8, "A", 1, MIRRORED_GLYPH_SCALE));
    TEST_ASSERT_EQUAL_INT(3 * char_w,
        gfx_font_text_width(&gfx_font_8x8, "ABC", 3, MIRRORED_GLYPH_SCALE));
}

static void test_width_scales_linearly_with_scale(void)
{
    const int one_char_at_1 = gfx_font_text_width(&gfx_font_8x8, "A", 1, 1);

    for (int scale = 1; scale <= 5; scale++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(one_char_at_1 * scale,
            gfx_font_text_width(&gfx_font_8x8, "A", 1, scale),
            "width did not scale linearly with `scale`");
    }
}

static void test_width_scales_linearly_with_length(void)
{
    const int one_char = gfx_font_text_width(&gfx_font_8x8, "A", 1,
                                             MIRRORED_GLYPH_SCALE);

    for (int len = 0; len <= 10; len++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(one_char * len,
            gfx_font_text_width(&gfx_font_8x8, "AAAAAAAAAA", len,
                                MIRRORED_GLYPH_SCALE),
            "width did not scale linearly with string length");
    }
}

/* The monospace shortcut (gfx_font_text_width's `advance == NULL` branch)
 * never inspects the string's content when `len >= 0` - matching the old
 * gfx_text_width(), which computed `len * GFX_CHAR_W` without touching the
 * string either. A `len` longer than what a literal actually holds must
 * therefore still be safe (and still just len * cell_w * scale), not a
 * newly-introduced out-of-bounds read. */
static void test_monospace_width_does_not_need_len_to_fit_the_string(void)
{
    const int expect = 20 * gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE;
    TEST_ASSERT_EQUAL_INT(expect,
        gfx_font_text_width(&gfx_font_8x8, "HI", 20, MIRRORED_GLYPH_SCALE));
}

static void test_len_negative_matches_nul_terminated_length(void)
{
    const char *s = "hello, world";
    TEST_ASSERT_EQUAL_INT(
        gfx_font_text_width(&gfx_font_8x8, s, (int)strlen(s), 1),
        gfx_font_text_width(&gfx_font_8x8, s, -1, 1));
}

static void test_zero_length_string_is_zero_wide(void)
{
    TEST_ASSERT_EQUAL_INT(0,
        gfx_font_text_width(&gfx_font_8x8, "", -1, MIRRORED_GLYPH_SCALE));
    TEST_ASSERT_EQUAL_INT(0,
        gfx_font_text_width(&gfx_font_8x8, "ignored", 0, MIRRORED_GLYPH_SCALE));
}

static void test_height_matches_char_h_at_default_scale(void)
{
    /* GFX_CHAR_H is 8 * GFX_GLYPH_SCALE (gfx.h) - i.e. cell_h * scale here. */
    const int char_h = gfx_font_8x8.cell_h * MIRRORED_GLYPH_SCALE;
    TEST_ASSERT_EQUAL_INT(char_h,
        gfx_font_height(&gfx_font_8x8, MIRRORED_GLYPH_SCALE));
}

/* Monospace: every glyph advances by cell_w * scale, whether or not the
 * codepoint is one the font actually covers - the same thing the old
 * gfx_text_turned() did, advancing by a fixed cell every character even
 * past one it declined to draw (see gfx.c's gfx_text_font()). */
static void test_monospace_advance_is_cell_w_times_scale_for_every_glyph(void)
{
    const unsigned char in_range[]  = { 0, 'A', 'z', 127 };
    const unsigned char out_of_range[] = { 128, 200, 255 };

    for (size_t i = 0; i < sizeof(in_range); i++) {
        TEST_ASSERT_EQUAL_INT(gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE,
            gfx_font_advance(&gfx_font_8x8, in_range[i], MIRRORED_GLYPH_SCALE));
    }
    for (size_t i = 0; i < sizeof(out_of_range); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(gfx_font_8x8.cell_w * MIRRORED_GLYPH_SCALE,
            gfx_font_advance(&gfx_font_8x8, out_of_range[i], MIRRORED_GLYPH_SCALE),
            "an out-of-range codepoint must still advance by a full cell, "
            "matching the old gfx_text_turned()'s unconditional advance");
    }
}

/*---------------------------------------------------------------------------
 * A synthetic proportional descriptor - the case with no real font behind
 * it yet, and so the easiest to get silently wrong (a monospace-only test
 * would still pass even if `advance` were never actually consulted).
 *-------------------------------------------------------------------------*/

/* Covers 'A'..'D' (4 glyphs) with deliberately distinct per-glyph advances,
 * so a bug that returns the wrong glyph's advance, or falls through to the
 * monospace default, shows up as a wrong number rather than an accident of
 * every glyph having the same width. The atlas is never read by these
 * tests (nothing here draws), so it is left zeroed. */
static const uint8_t synth_atlas[4 * 7] = { 0 };
static const uint8_t synth_advance[4] = { 3, 4, 5, 6 };
static const gfx_font_t synth_font = {
    .atlas   = synth_atlas,
    .bpp     = 1,
    .cell_w  = 5,
    .cell_h  = 7,
    .first   = (uint8_t)'A',
    .count   = 4,
    .advance = synth_advance,
};

static void test_proportional_advance_is_per_glyph(void)
{
    TEST_ASSERT_EQUAL_INT(3, gfx_font_advance(&synth_font, 'A', 1));
    TEST_ASSERT_EQUAL_INT(4, gfx_font_advance(&synth_font, 'B', 1));
    TEST_ASSERT_EQUAL_INT(5, gfx_font_advance(&synth_font, 'C', 1));
    TEST_ASSERT_EQUAL_INT(6, gfx_font_advance(&synth_font, 'D', 1));

    TEST_ASSERT_EQUAL_INT(6,  gfx_font_advance(&synth_font, 'A', 2));
    TEST_ASSERT_EQUAL_INT(12, gfx_font_advance(&synth_font, 'D', 2));
}

static void test_proportional_advance_falls_back_outside_its_range(void)
{
    /* 'Z' and the space before 'A' are both outside [first, first+count) -
     * neither has an entry in synth_advance, so both must fall back to the
     * monospace cell_w * scale rather than reading synth_advance
     * out-of-bounds. */
    TEST_ASSERT_EQUAL_INT(synth_font.cell_w,
        gfx_font_advance(&synth_font, 'Z', 1));
    TEST_ASSERT_EQUAL_INT(synth_font.cell_w,
        gfx_font_advance(&synth_font, ' ', 1));
    TEST_ASSERT_EQUAL_INT(synth_font.cell_w * 3,
        gfx_font_advance(&synth_font, 'Z', 3));
}

static void test_proportional_text_width_sums_per_glyph_advances(void)
{
    /* "ABCD" -> 3 + 4 + 5 + 6 = 18 at scale 1, 36 at scale 2. Unlike the
     * monospace shortcut, this path genuinely reads each of the `len`
     * characters, so - deliberately, unlike the monospace test above - `len`
     * here never exceeds what the literal actually holds. */
    TEST_ASSERT_EQUAL_INT(18,
        gfx_font_text_width(&synth_font, "ABCD", 4, 1));
    TEST_ASSERT_EQUAL_INT(36,
        gfx_font_text_width(&synth_font, "ABCD", 4, 2));

    /* A prefix stops summing where `len` says to, not at the string's own
     * end - "AB" worth of width out of the longer "ABCD" literal. */
    TEST_ASSERT_EQUAL_INT(3 + 4,
        gfx_font_text_width(&synth_font, "ABCD", 2, 1));
}

static void test_proportional_text_width_len_negative_is_nul_terminated(void)
{
    TEST_ASSERT_EQUAL_INT(3 + 4,
        gfx_font_text_width(&synth_font, "AB", -1, 1));
}

static void test_proportional_height_is_cell_h_times_scale(void)
{
    TEST_ASSERT_EQUAL_INT(7,  gfx_font_height(&synth_font, 1));
    TEST_ASSERT_EQUAL_INT(21, gfx_font_height(&synth_font, 3));
}

void run_gfx_font_suite(void)
{
    RUN_TEST(test_default_font_width_matches_char_w_per_character);
    RUN_TEST(test_width_scales_linearly_with_scale);
    RUN_TEST(test_width_scales_linearly_with_length);
    RUN_TEST(test_monospace_width_does_not_need_len_to_fit_the_string);
    RUN_TEST(test_len_negative_matches_nul_terminated_length);
    RUN_TEST(test_zero_length_string_is_zero_wide);
    RUN_TEST(test_height_matches_char_h_at_default_scale);
    RUN_TEST(test_monospace_advance_is_cell_w_times_scale_for_every_glyph);
    RUN_TEST(test_proportional_advance_is_per_glyph);
    RUN_TEST(test_proportional_advance_falls_back_outside_its_range);
    RUN_TEST(test_proportional_text_width_sums_per_glyph_advances);
    RUN_TEST(test_proportional_text_width_len_negative_is_nul_terminated);
    RUN_TEST(test_proportional_height_is_cell_h_times_scale);
}

SUITE_REGISTER(run_gfx_font_suite);
