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
     * truncated so t=255 lands exactly on `b` and t=0 exactly on `a`. */
    const uint8_t mr = (uint8_t)((ar * (255 - t) + br * t + 127) / 255);
    const uint8_t mg = (uint8_t)((ag * (255 - t) + bg * t + 127) / 255);
    const uint8_t mb = (uint8_t)((ab * (255 - t) + bb * t + 127) / 255);

    const uint16_t nm = (uint16_t)((mr << 11) | (mg << 5) | mb);

    /* Swap back to the panel's byte order. */
    return (gfx_color_t)((nm >> 8) | (nm << 8));
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
