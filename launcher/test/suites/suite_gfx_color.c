/*=============================================================================
 * Portable suite: gfx_color_mix - blending two panel-packed pixels.
 *
 * gfx_color_t is RGB565 with the bytes swapped (see gfx_color.h's own top
 * comment on GFX_RGB), so mixing it means undoing that swap, blending each
 * of R5/G6/B5 in its own width, repacking, and swapping again. Getting the
 * swap wrong still produces a plausible-looking colour rather than an
 * obviously broken one, so every expected value here is checked against
 * GFX_RGB(...) built from a hand-picked 0xRRGGBB constant, never against
 * gfx_color_mix's own round-trip - a test that reused the implementation's
 * swap logic could carry the same bug and still pass.
 *
 * The 0xRRGGBB constants below are chosen so each 8-bit channel lands
 * squarely inside one 5- or 6-bit bucket (a multiple of 8 for R/B, of 4 for
 * G), which is what makes GFX_RGB565's R8>>3 / G8>>2 / B8>>3 truncation
 * land on an exact, by-hand-predictable value instead of something that
 * itself needs rounding to check.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "gfx/gfx_color.h"

static void test_t_zero_returns_a_exactly(void)
{
    const gfx_color_t a = GFX_RGB(0x336699);
    const gfx_color_t b = GFX_RGB(0x8899AA);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(a, gfx_color_mix(a, b, 0),
        "t=0 must be all `a` - a caller ramping a transition from 0 relies "
        "on the first frame matching the untouched colour exactly");
}

static void test_t_255_returns_b_exactly(void)
{
    const gfx_color_t a = GFX_RGB(0x336699);
    const gfx_color_t b = GFX_RGB(0x8899AA);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(b, gfx_color_mix(a, b, 255),
        "t=255 must be all `b` - a caller ramping a transition to 255 "
        "relies on the last frame matching the target colour exactly");
}

static void test_mixing_a_colour_with_itself_is_identity_at_any_t(void)
{
    /* Not black or white - both would pass an identity test even with a
     * broken blend, since 0 and 0xFFFF are fixed points of most channel
     * arithmetic. */
    const gfx_color_t c = GFX_RGB(0x5C7AAF);
    const int ts[] = { 0, 1, 64, 100, 128, 200, 254, 255 };

    for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(c, gfx_color_mix(c, c, (uint8_t)ts[i]),
            "blending a colour toward itself must be a no-op at every t, "
            "or the bezel's face colour would drift under its own highlight");
    }
}

static void test_black_toward_white_at_half_gives_mid_grey(void)
{
    /* Hand-derived: mr = (0*127 + 31*128 + 127) / 255 = 16, mg =
     * (0*127 + 63*128 + 127) / 255 = 32, mb = 16 - i.e. R8=G8=B8=128
     * (0x808080) once each is scaled back up by its own bucket width
     * (16*8=128, 32*4=128, 16*8=128). All three land near, not exactly on,
     * half of their own max (15.5, 31.5, 15.5) - which is the point: a
     * blend that used 8-bit weights for every channel would not land here. */
    const gfx_color_t black = GFX_RGB(0x000000);
    const gfx_color_t white = GFX_RGB(0xFFFFFF);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(GFX_RGB(0x808080),
        gfx_color_mix(black, white, 128),
        "black toward white at t=128 must land on a mid grey with every "
        "channel near its own half, not shifted by unequal channel widths");
}

static void test_a_known_pair_blends_to_a_known_result(void)
{
    /* Pure red toward pure blue at t=128. Hand-derived the same way as the
     * grey test above: mr = (31*127 + 127) / 255 = 15, mb =
     * (31*128 + 127) / 255 = 16, scaled back up (15*8=120, 16*8=128) gives
     * 0x780080. */
    const gfx_color_t red  = GFX_RGB(0xFF0000);
    const gfx_color_t blue = GFX_RGB(0x0000FF);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(GFX_RGB(0x780080),
        gfx_color_mix(red, blue, 128),
        "a known red/blue pair at a known t must land on the hand-derived "
        "packed value, byte swap included");
}

static void test_channels_blend_independently_green_stays_put(void)
{
    /* Same red-toward-blue pair as above, at several more t. Neither input
     * carries any green, so if channels were blended independently the
     * result must not either at ANY t - every expected constant below has
     * a green byte of 0x00. Picked to also sweep R and B away from the
     * t=128 case already covered: t=64 gives mr=(31*191+127)/255=23,
     * mb=(31*64+127)/255=8 (0xB80040); t=192 gives mr=(31*63+127)/255=8,
     * mb=(31*192+127)/255=23 (0x4000B8). */
    const gfx_color_t red  = GFX_RGB(0xFF0000);
    const gfx_color_t blue = GFX_RGB(0x0000FF);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(GFX_RGB(0xB80040),
        gfx_color_mix(red, blue, 64),
        "green must stay at zero while red and blue move independently");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(GFX_RGB(0x4000B8),
        gfx_color_mix(red, blue, 192),
        "green must stay at zero while red and blue move independently");
}

void run_gfx_color_suite(void)
{
    RUN_TEST(test_t_zero_returns_a_exactly);
    RUN_TEST(test_t_255_returns_b_exactly);
    RUN_TEST(test_mixing_a_colour_with_itself_is_identity_at_any_t);
    RUN_TEST(test_black_toward_white_at_half_gives_mid_grey);
    RUN_TEST(test_a_known_pair_blends_to_a_known_result);
    RUN_TEST(test_channels_blend_independently_green_stays_put);
}

SUITE_REGISTER(run_gfx_color_suite);
