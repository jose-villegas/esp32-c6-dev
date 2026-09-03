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


/* --- gfx_color_add ------------------------------------------------------ */

static void test_adding_black_changes_nothing(void)
{
    const gfx_color_t c = GFX_RGB(0x3D7FA2);
    TEST_ASSERT_EQUAL_HEX16(c, gfx_color_add(c, GFX_RGB(0x000000)));
    TEST_ASSERT_EQUAL_HEX16(c, gfx_color_add(GFX_RGB(0x000000), c));
}

static void test_adding_two_primaries_gives_their_combination(void)
{
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(GFX_RGB(0xFFFF00),
        gfx_color_add(GFX_RGB(0xFF0000), GFX_RGB(0x00FF00)),
        "red plus green is yellow - this is light, not paint");
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF),
        gfx_color_add(GFX_RGB(0xFFFF00), GFX_RGB(0x0000FF)));
}

/* The trap this function exists to avoid: the three channels are not the same
 * width, so each has to saturate at its own ceiling. Clamping all three at 31
 * would halve green; clamping all three at 63 would wrap red and blue round
 * to nearly nothing at the moment they are brightest. */
static void test_each_channel_saturates_at_its_own_ceiling(void)
{
    const gfx_color_t white = GFX_RGB(0xFFFFFF);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(white, gfx_color_add(white, white),
        "white plus white is white, not a wrapped-round mess");
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000),
        gfx_color_add(GFX_RGB(0xFF0000), GFX_RGB(0xFF0000)));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x00FF00),
        gfx_color_add(GFX_RGB(0x00FF00), GFX_RGB(0x00FF00)));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF),
        gfx_color_add(GFX_RGB(0x0000FF), GFX_RGB(0x0000FF)));
}

static void test_adding_never_makes_a_channel_darker(void)
{
    const gfx_color_t base = GFX_RGB(0x402080);

    for (uint32_t rgb = 0; rgb <= 0xFFFFFFu; rgb += 0x0103F7u) {
        const gfx_color_t sum = gfx_color_add(base, GFX_RGB(rgb));
        const uint16_t nb = (uint16_t)((base >> 8) | (base << 8));
        const uint16_t ns = (uint16_t)((sum >> 8) | (sum << 8));

        TEST_ASSERT_TRUE_MESSAGE(((ns >> 11) & 0x1Fu) >= ((nb >> 11) & 0x1Fu),
            "adding light made the red channel darker");
        TEST_ASSERT_TRUE_MESSAGE(((ns >> 5) & 0x3Fu) >= ((nb >> 5) & 0x3Fu),
            "adding light made the green channel darker");
        TEST_ASSERT_TRUE_MESSAGE((ns & 0x1Fu) >= (nb & 0x1Fu),
            "adding light made the blue channel darker");
    }
}

/*-----------------------------------------------------------------------------
 * gfx_color_rgb888 - unpacking a panel colour back to 0xRRGGBB.
 *
 * The property under test is GFX_RGB(gfx_color_rgb888(c)) == c for every c -
 * see gfx_color_rgb888()'s own comment in gfx_color.h for why bit replication
 * is what makes that hold exactly rather than approximately. A spread of
 * colours is used rather than an exhaustive sweep of all 65536 gfx_color_t
 * values, since the replication argument is per-channel and does not depend
 * on the other two channels' values - so a handful of colours that between
 * them exercise every channel at 0 and at its own maximum is as convincing as
 * the full sweep and a great deal cheaper.
 *---------------------------------------------------------------------------*/

/* 0xRRGGBB constants covering pure black, pure white, each channel alone at
 * its own maximum (0xF80000/0x00FC00/0x0000F8 - the largest 0xRRGGBB value
 * GFX_RGB565 truncates to R5=31/G6=63/B5=31 respectively), and a few
 * arbitrary colours that mix all three channels at once. */
static const uint32_t rgb888_spread[] = {
    0x000000, 0xFFFFFF,
    0xF80000, 0x00FC00, 0x0000F8,
    0x336699, 0x8899AA, 0x5C7AAF, 0x1A2B3C, 0xE0D0C0,
};
#define RGB888_SPREAD_N (sizeof(rgb888_spread) / sizeof(rgb888_spread[0]))

static void test_round_trip_is_exact_for_a_spread_of_colours(void)
{
    for (size_t i = 0; i < RGB888_SPREAD_N; i++) {
        const gfx_color_t c = GFX_RGB(rgb888_spread[i]);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(c, GFX_RGB(gfx_color_rgb888(c)),
            "GFX_RGB(gfx_color_rgb888(c)) must reproduce c exactly - a "
            "plain shift instead of bit replication would land one bucket "
            "short and the colour would read darker every time it crosses "
            "this layer");
    }
}

static void test_expanding_a_colour_twice_is_idempotent(void)
{
    /* Feeding an already-expanded colour back through GFX_RGB() and
     * gfx_color_rgb888() a second time must land on exactly the same
     * 0xRRGGBB, not merely one that happens to look right once - the
     * expansion has to be a fixed point of the round trip, since the UI
     * layer reads a panel colour into an 8-bit mu_Color once and nothing
     * downstream of that is allowed to keep drifting it. */
    for (size_t i = 0; i < RGB888_SPREAD_N; i++) {
        const gfx_color_t c = GFX_RGB(rgb888_spread[i]);
        const uint32_t once  = gfx_color_rgb888(c);
        const uint32_t twice = gfx_color_rgb888(GFX_RGB(once));
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(once, twice,
            "a second pass through GFX_RGB()/gfx_color_rgb888() must not "
            "change the already-expanded colour any further");
    }
}

/*-----------------------------------------------------------------------------
 * gfx_dither_covers - the per-pixel ordered-dither coverage test shared by
 * gfx_fill_rect_dither() (device suite: test_dither_* in suite_gfx.c, which
 * exercises this function only indirectly, through a real framebuffer fill)
 * and boot_anim.c's draw_image(), which calls it directly on two separate
 * alphas per pixel (`reveal`, `ink`) and ANDs the results together. These
 * tests are what boot_anim.c's own comment on that AND - "equivalent to
 * testing the lower of the two against one cell" - is checked against,
 * rather than merely asserted in a comment.
 *---------------------------------------------------------------------------*/

static void test_alpha_zero_never_covers(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            TEST_ASSERT_FALSE_MESSAGE(gfx_dither_covers(x, y, 0),
                "alpha 0 must cover nothing at any cell, or a caller "
                "skipping a zero-alpha draw entirely would disagree with "
                "one that called through anyway");
        }
    }
}

static void test_alpha_255_always_covers(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            TEST_ASSERT_TRUE_MESSAGE(gfx_dither_covers(x, y, 255),
                "alpha 255 must cover every cell - its own unconditional "
                "path, not the >  test, so it is never one Bayer level "
                "short of solid");
        }
    }
}

static void test_coverage_is_monotonic_in_alpha_at_every_cell(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            bool was_covered = false;
            for (int a = 0; a <= 255; a++) {
                const bool covered = gfx_dither_covers(x, y, (uint8_t)a);
                if (was_covered) {
                    TEST_ASSERT_TRUE_MESSAGE(covered,
                        "coverage at a fixed cell must never go from "
                        "covered back to uncovered as alpha rises, or a "
                        "caller ramping alpha upward could see a pixel "
                        "flicker off partway through");
                }
                was_covered = covered;
            }
        }
    }
}

/* The exact property boot_anim.c's draw_image() leans on to fold two
 * gfx_dither_covers() calls into one: since coverage is monotonic in alpha
 * at a fixed cell (proved above), testing against the lower of two alphas
 * must agree with testing against each and ANDing the results, for every
 * cell and every pair. Swept in steps of 17 (255 is not divisible by 17,
 * so the sweep still lands on both 0 and 255) rather than exhaustively -
 * a monotonic step function that agrees at every 17th value and at both
 * endpoints cannot disagree in between without a jump the coarser sweep
 * would itself have caught at a neighbouring point. */
static void test_covers_both_equals_covers_the_lower_alpha(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            for (int a = 0; a <= 255; a += 17) {
                for (int b = 0; b <= 255; b += 17) {
                    const uint8_t lower = (uint8_t)(a < b ? a : b);
                    const bool both = gfx_dither_covers(x, y, (uint8_t)a) &&
                                       gfx_dither_covers(x, y, (uint8_t)b);
                    const bool via_min = gfx_dither_covers(x, y, lower);
                    TEST_ASSERT_EQUAL_MESSAGE(via_min, both,
                        "covers(a) && covers(b) must equal covers(min(a, "
                        "b)) at every cell - this is what lets draw_image() "
                        "test the lower alpha once instead of testing both "
                        "and ANDing");
                }
            }
        }
    }
}

void run_gfx_color_suite(void)
{
    RUN_TEST(test_t_zero_returns_a_exactly);
    RUN_TEST(test_t_255_returns_b_exactly);
    RUN_TEST(test_mixing_a_colour_with_itself_is_identity_at_any_t);
    RUN_TEST(test_black_toward_white_at_half_gives_mid_grey);
    RUN_TEST(test_a_known_pair_blends_to_a_known_result);
    RUN_TEST(test_channels_blend_independently_green_stays_put);
    RUN_TEST(test_adding_black_changes_nothing);
    RUN_TEST(test_adding_two_primaries_gives_their_combination);
    RUN_TEST(test_each_channel_saturates_at_its_own_ceiling);
    RUN_TEST(test_adding_never_makes_a_channel_darker);
    RUN_TEST(test_round_trip_is_exact_for_a_spread_of_colours);
    RUN_TEST(test_expanding_a_colour_twice_is_idempotent);
    RUN_TEST(test_alpha_zero_never_covers);
    RUN_TEST(test_alpha_255_always_covers);
    RUN_TEST(test_coverage_is_monotonic_in_alpha_at_every_cell);
    RUN_TEST(test_covers_both_equals_covers_the_lower_alpha);
}

SUITE_REGISTER(run_gfx_color_suite);
