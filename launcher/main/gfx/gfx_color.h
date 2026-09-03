/*=============================================================================
 * gfx_color - what a pixel is, separately from how the panel works.
 *
 * Split out of gfx.h because this part is pure arithmetic and nothing else:
 * no BSP, no drivers, no hardware headers. That lets code which only needs to
 * describe colours - a material table, say - be compiled and tested on a host,
 * while gfx.h keeps everything that genuinely needs the board.
 *
 * The macros matter for more than tidiness. A colour table built from them is
 * a compile-time constant, so it lands in .rodata and is memory-mapped from
 * flash at zero cost in RAM - which on this board is the resource that actually
 * runs out. Computing the same table at startup would cost real bytes of the
 * scarcest thing there is.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A packed, panel-ready pixel. Produced by GFX_RGB or gfx_rgb(), stored in the
 * framebuffer, never inspected by callers. */
typedef uint16_t gfx_color_t;

/* 0xRRGGBB to RGB565. */
#define GFX_RGB565(rgb)                        \
    ((((uint32_t)(rgb) >> 8) & 0xF800u) |      \
     (((uint32_t)(rgb) >> 5) & 0x07E0u) |      \
     (((uint32_t)(rgb) >> 3) & 0x001Fu))

/* 0xRRGGBB to the panel's format: RGB565 with the bytes swapped, which is what
 * this QSPI controller expects - the opposite order to the chip's native
 * layout. Usable in a constant expression. */
#define GFX_RGB(rgb)                           \
    ((gfx_color_t)((GFX_RGB565(rgb) >> 8) |    \
                   (GFX_RGB565(rgb) << 8)))

/* Blend `a` toward `b`. t is 0..255, where 0 is all `a` and 255 is all `b`.
 *
 * A gfx_color_t is RGB565 with the bytes swapped (see GFX_RGB above), not
 * RGB565 itself - so blending it channel-by-channel means swapping the bytes
 * back to native RGB565 first, unpacking R5/G6/B5, blending each channel,
 * repacking to RGB565, and swapping the bytes again on the way out. Skipping
 * either swap does not fail loudly: it still produces a plausible-looking
 * colour, just the wrong one, which is exactly why this is tested against
 * GFX_RGB(...) constants rather than against its own round-trip.
 *
 * The three channels are NOT the same width - red and blue are 5 bits, green
 * is 6 - so each is blended in its own width. Treating all three as 8-bit
 * (e.g. by blending the swapped 16-bit halves directly) is the obvious
 * mistake, and it does not crash: it shifts the hue, because a 5-bit channel
 * blended with 8-bit arithmetic gets weighted wrong relative to the 6-bit
 * one. */
/* v / 255, without a divide.
 *
 * EXACT, not approximate, for every v these callers can produce. The widest
 * channel here is green's six bits, so the largest numerator gfx_color_mix()
 * builds is 63*255 + 63*255 + 127 = 32257, and this identity was checked
 * against integer division across the whole of 0..32257 rather than spot
 * tested. Outside that range it is not claimed to hold, which is why it is
 * static and lives next to its one caller instead of in intmath.h.
 *
 * Worth having because a divide is the slowest integer instruction on this
 * chip, and gfx_color_mix() is on the path of every outlined glyph, every
 * bezelled button and every colour the startup animation fades. */
static inline uint32_t div255(uint32_t v)
{
    return (v + (v >> 8) + 1) >> 8;
}

/* The 0xRRGGBB a packed panel colour came from - the inverse of GFX_RGB().
 *
 * Same byte-swap-then-unpack as gfx_color_mix() above, but expanding each
 * channel back to 8 bits by BIT REPLICATION (v << 3 | v >> 2 for a 5-bit
 * channel, v << 2 | v >> 4 for 6-bit) rather than a plain left shift.
 *
 * Replication is what makes GFX_RGB(gfx_color_rgb888(c)) == c for every c: a
 * plain shift (v << 3, say) leaves the low bits zero, so packing that value
 * back down with GFX_RGB565's own truncating >> 3 recovers v exactly only by
 * accident of rounding - most values land one bucket short and the colour
 * reads slightly darker each time it makes the round trip. Replication fills
 * those low bits with the channel's own high bits instead, which are exactly
 * what a truncating right-shift throws away first: for a 5-bit v, v8 = (v <<
 * 3) | (v >> 2) has v in bits 7..3 and v's own top 3 bits in bits 2..0, so
 * v8 >> 3 is v again whatever v was - not just for the values a shift would
 * have gotten right anyway. This is what any UI code that reads a panel
 * colour into microui's 8-bit mu_Color needs: a value that survives being
 * carried through this layer and mapped back down, not merely one that looks
 * close. */
static inline uint32_t gfx_color_rgb888(gfx_color_t c)
{
    /* Undo the byte swap to get back to native-endian RGB565 - see
     * gfx_color_mix() above for the same first step. */
    const uint16_t native = (uint16_t)((c >> 8) | (c << 8));

    const uint8_t r5 = (uint8_t)((native >> 11) & 0x1Fu); /* 5 bits */
    const uint8_t g6 = (uint8_t)((native >> 5)  & 0x3Fu); /* 6 bits */
    const uint8_t b5 = (uint8_t)( native        & 0x1Fu); /* 5 bits */

    const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

    return ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

static inline gfx_color_t gfx_color_mix(gfx_color_t a, gfx_color_t b, uint8_t t)
{
    /* Undo the byte swap to get back to native-endian RGB565. */
    const uint16_t na = (uint16_t)((a >> 8) | (a << 8));
    const uint16_t nb = (uint16_t)((b >> 8) | (b << 8));

    const uint8_t ar = (uint8_t)((na >> 11) & 0x1Fu); /* 5 bits */
    const uint8_t ag = (uint8_t)((na >> 5)  & 0x3Fu); /* 6 bits */
    const uint8_t ab = (uint8_t)( na        & 0x1Fu); /* 5 bits */

    const uint8_t br = (uint8_t)((nb >> 11) & 0x1Fu);
    const uint8_t bg = (uint8_t)((nb >> 5)  & 0x3Fu);
    const uint8_t bb = (uint8_t)( nb        & 0x1Fu);

    /* (channel * (255 - t) + channel * t) / 255, rounded rather than
     * truncated so t=255 lands exactly on `b` and t=0 exactly on `a`.
     *
     * The divide is done as div255() below - not an approximation, an exact
     * identity over the range these numerators can reach. This function is on
     * the per-pixel path of every antialiased line (see gfx_line_ex()), and
     * three hardware divides per pixel was measurably the most expensive
     * thing in the startup animation: turning smoothing on cost 6.7 fps, and
     * most of that was here rather than in the extra pixels. */
    const uint8_t mr = (uint8_t)div255(ar * (255 - t) + br * t + 127);
    const uint8_t mg = (uint8_t)div255(ag * (255 - t) + bg * t + 127);
    const uint8_t mb = (uint8_t)div255(ab * (255 - t) + bb * t + 127);

    const uint16_t nm = (uint16_t)((mr << 11) | (mg << 5) | mb);

    /* Swap back to the panel's byte order. */
    return (gfx_color_t)((nm >> 8) | (nm << 8));
}

/* The standard order-4 Bayer matrix, values 0..15 rather than pre-scaled to
 * 0..255 - gfx_dither_covers() below scales the ONE side that actually
 * needs to be a byte (alpha, an input this file does not control), not
 * the table it is compared against.
 *
 * Indexed by each pixel's own ABSOLUTE panel row/col (y & 3, x & 3), not a
 * position local to whatever shape is being dithered. That is what keeps
 * two dithered shapes that overlap or sit edge to edge in phase with each
 * other - a local index would have every dithered shape restart the
 * pattern at its own corner, which reads as a seam where two of them meet
 * rather than one continuous texture. */
static const uint8_t gfx_dither4x4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* Whether an ordered (Bayer) dither at `alpha` (0 nothing, 255 everything,
 * 16 graduated levels between - see gfx_fill_rect_dither()'s own comment
 * in gfx.c for the full reasoning behind this trade) covers absolute
 * panel pixel (x, y). This is gfx_fill_rect_dither()'s own per-pixel
 * decision, pulled out here so a caller with its own pixel loop - not a
 * plain filled rect - can reuse the identical table and rounding rule
 * rather than keeping a second copy that could drift out of phase with
 * it. `static inline`, not a regular function: this is meant to inline
 * into a hot per-pixel loop the same way gfx_color_mix() above does, and
 * a real call per pixel across a whole framebuffer would cost more than
 * the branch it replaces.
 *
 * Rounded, not truncated: a plain `alpha >> 4` maps every alpha in 1..15
 * to level 0, which then never compares greater than any table cell and
 * covers nothing at all - an author-visible value meant to read as
 * "barely there" would instead be indistinguishable from alpha 0. +8
 * rounds to the nearest of the 16 levels instead of always flooring. */

/* The alpha -> Bayer-level scaling gfx_dither_covers() below compares
 * against the table, exposed on its own so a caller with a whole ROW (or
 * frame) of pixels at one alpha can compute the level once and test cells
 * directly, instead of re-deriving it per pixel - see boot_anim.c's
 * draw_image() for the loop this exists for. 17, not 16: alpha 255 maps
 * to a level strictly above every table cell (the table's own max is 15),
 * which is what keeps "fully solid" exactly solid without the separate
 * >= 255 branch the per-pixel wrapper below still keeps for clarity. */
static inline int gfx_dither_level(uint8_t alpha)
{
    if (alpha == 0) {
        return 0;
    }
    if (alpha >= 255) {
        return 16;
    }
    return (alpha + 8) >> 4;
}

static inline bool gfx_dither_covers(int x, int y, uint8_t alpha)
{
    return gfx_dither_level(alpha) > gfx_dither4x4[y & 3][x & 3];
}

/* Add `b` to `a`, saturating each channel at its own maximum.
 *
 * What overlapping light does: two strokes crossing on a black field make a
 * brighter, mixed colour rather than whichever was drawn second. That is the
 * whole reason this exists - see boot_anim.c, where several hundred curve
 * segments cross each other and flat writes made the picture look like
 * stacked wires instead of one lit object.
 *
 * Same byte-swap dance as gfx_color_mix() below, and the same trap: the three
 * channels are NOT the same width, so each saturates at its own ceiling.
 * Clamping all three at 31 dims green by half; clamping all three at 63
 * wraps red and blue round to nearly black at the exact moment they are
 * brightest, which looks like holes punched in the picture.
 */
static inline gfx_color_t gfx_color_add(gfx_color_t a, gfx_color_t b)
{
    const uint16_t na = (uint16_t)((a >> 8) | (a << 8));
    const uint16_t nb = (uint16_t)((b >> 8) | (b << 8));

    uint16_t r = (uint16_t)(((na >> 11) & 0x1Fu) + ((nb >> 11) & 0x1Fu));
    uint16_t g = (uint16_t)(((na >> 5)  & 0x3Fu) + ((nb >> 5)  & 0x3Fu));
    uint16_t bl = (uint16_t)((na & 0x1Fu) + (nb & 0x1Fu));

    if (r  > 0x1Fu) { r  = 0x1Fu; }
    if (g  > 0x3Fu) { g  = 0x3Fu; }
    if (bl > 0x1Fu) { bl = 0x1Fu; }

    const uint16_t nm = (uint16_t)((r << 11) | (g << 5) | bl);
    return (gfx_color_t)((nm >> 8) | (nm << 8));
}
