/*=============================================================================
 * icons - small glyph-like artwork microui's widgets ask for that no font
 * provides.
 *
 * This is widget vocabulary, not framebuffer or panel concern, so it does not
 * belong in gfx.h - see gfx.h's own top comment on what that module owns.
 * It is its own small module for the same reason row_runs.c, gesture.c and
 * button_fsm.c are: tiny, single-purpose, easy to find.
 *
 * NO GLYPH EXISTS FOR THIS
 *
 * font8x8_basic.h covers U+0000-U+007F (basic Latin) only, and a check mark
 * is not in that range even in principle - Unicode's closest, U+2713 CHECK
 * MARK, is far outside it. Reaching for the font's unused control-code slots
 * (U+0000-U+001F) will not work either: font8x8_basic.h zeroes every one of
 * them (see its own file, e.g. the U+0000 "nul" entry), so there is nothing
 * there to repurpose.
 *
 * A HAND-DRAWN BITMAP, NOT A GENERATED SHAPE
 *
 * icon_check_bitmap below is drawn pixel-by-pixel, the same way
 * font8x8_basic.h's glyphs are, rather than computed from a step formula -
 * a small diagonal glyph reads as a stroke when a person places every pixel
 * and reads as a staircase when steps are generated arithmetically, which is
 * exactly what an earlier version of this file did and had to be redrawn
 * over. See the bitmap's own comment for the picture and the reasoning
 * behind its proportions.
 *
 * PURE GEOMETRY, SEPARATE FROM DRAWING
 *
 * icon_check_blocks() below is the same split palette.c makes between layout
 * and rendering: it returns WHERE the blocks go and touches nothing else, so
 * a host test can check the shape (see suite_icons.c) without linking gfx.c
 * or anything hardware-facing. It is a `static inline` here rather than a
 * separate .c, for the same reason gfx_color_mix() is inline in gfx_color.h
 * rather than a gfx_color.c: this header must stay host-compilable (no BSP,
 * no drivers, no panel - see gfx_color.h's own top comment on that split),
 * and icon_check() itself is hardware-facing (it calls gfx_fill_rect()), so
 * it cannot live here - see icons.c.
 *
 * ONLY MU_ICON_CHECK IS BUILT
 *
 * microui also defines MU_ICON_CLOSE, MU_ICON_COLLAPSED and MU_ICON_EXPANDED,
 * but nothing in this shell draws a closable window or a collapsible tree, so
 * nothing asks for them. They are the natural neighbours of icon_check() if
 * something ever does - do not add them speculatively before then.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#include "gfx/gfx_color.h"

/* icon_check_blocks() never returns more than this many blocks - see its own
 * comment for where 16 comes from. */
#define ICON_CHECK_MAX_BLOCKS 16

typedef struct {
    int x, y, w, h;
} icon_rect_t;

#define ICON_CHECK_BITMAP_SIZE 16

/* The check mark, hand-drawn at 16x16 - one row of bits per line, the same
 * convention font8x8_basic.h uses for its glyphs, so the shape is readable
 * directly in the array below rather than only on screen. Bit 15 (MSB) is
 * column 0 (left); bit 0 (LSB) is column 15 (right) - read each literal
 * left-to-right and it matches this picture exactly:
 *
 *     ................
 *     .............#..
 *     ............##..
 *     ...........##...
 *     ...........##...
 *     ..........##....
 *     .........###....
 *     ..##.....##.....
 *     ...##...##......
 *     ...###.###......
 *     ....#####.......
 *     .....####.......
 *     .....###........
 *     ......##........
 *     ................
 *     ................
 *
 * A short limb (rows 7-9) descends left-to-right to a vertex around row 10,
 * then a long limb rises from there past row 1 - about a third the length of
 * the long limb, and finishing well above where the short limb started (row
 * 1 against row 7), which is what reads as a check rather than a V or a
 * plain diagonal tick. The stroke is 2-3px through the body; the one free
 * end that is not anchored at the vertex (the long limb's top-right tip,
 * row 1) tapers to a single pixel rather than ending square, which is what
 * keeps a small check from reading as a blunt slab at its point. 1-2px of
 * margin surrounds the shape on every side so it does not run into whatever
 * border is drawn around its box - app_sand.c's badge draws this box right
 * up against its own 2px border, which is exactly where an edge-to-edge
 * glyph would have collided with it. */
static const uint16_t icon_check_bitmap[ICON_CHECK_BITMAP_SIZE] = {
    0b0000000000000000,
    0b0000000000000100,
    0b0000000000001100,
    0b0000000000011000,
    0b0000000000011000,
    0b0000000000110000,
    0b0000000001110000,
    0b0011000001100000,
    0b0001100011000000,
    0b0001110111000000,
    0b0000111110000000,
    0b0000011110000000,
    0b0000011100000000,
    0b0000001100000000,
    0b0000000000000000,
    0b0000000000000000,
};

/* The blocks that make up icon_check_bitmap, scaled to fill and centred
 * within the box (0, 0, w, h).
 *
 * SCALE
 *
 * The bitmap is scaled by the largest INTEGER factor that fits the box's
 * smaller side, minimum 1 - the same rule gfx_text_scaled() uses for the 8x8
 * font, and for the same reason: a fractional scale needs interpolation this
 * device has no budget for, and would blur a glyph this small into mush
 * anyway. At app_sand.c's 18px badge (PALETTE_BADGE_SIZE) that is scale 1 -
 * the badge is barely bigger than the bitmap's own 16px, so there is no room
 * for 2x. At the checkbox icon rect mu_checkbox() draws (64x64 - see
 * suite_icons.c's own comment on where that number comes from) it is scale
 * 4, landing exactly on 64 with nothing left over.
 *
 * A box smaller than 16px (scale would otherwise be 0) still gets scale 1
 * rather than nothing, so this module's effective floor is 16px - smaller
 * than that and the glyph will not fit, though nothing here has ever asked
 * for smaller than the 18px badge.
 *
 * CENTRING
 *
 * The glyph does not fill its 16x16 bitmap - see the margin the picture
 * above already leaves - so scaling and centring the full 16x16 box would
 * off-centre the visible content by however wide that margin is. Instead
 * this scans the bitmap's own CONTENT bounding box (scanned fresh each call
 * rather than hand-computed, so it can never drift out of step with
 * icon_check_bitmap if that ever changes) and centres THAT within (0, 0, w,
 * h), which is what keeps the drawn mark centred in its box rather than the
 * mark's invisible margin being centred instead.
 *
 * RECTS, NOT PIXELS
 *
 * Each bitmap row is run-length encoded once, at native (unscaled)
 * resolution, into however many contiguous horizontal runs it has - almost
 * always one, twice in the three rows where both limbs are present at once
 * (see the picture above, rows 7-9). Each run becomes exactly one output
 * rect, scaled and positioned, rather than one rect per output pixel or per
 * output row - scaling changes a run's size, never how many runs there are,
 * so the block count is bounded by the native bitmap's own 16 runs
 * regardless of scale. That bound is ICON_CHECK_MAX_BLOCKS.
 *
 * Returns how many blocks were written to `out`, never more than
 * ICON_CHECK_MAX_BLOCKS and never more than `max` - a `max` smaller than
 * ICON_CHECK_MAX_BLOCKS truncates the shape rather than overflowing `out`. */
static inline int icon_check_blocks(int w, int h, icon_rect_t *out, int max)
{
    const int side  = (w < h) ? w : h;
    int scale = side / ICON_CHECK_BITMAP_SIZE;
    if (scale < 1) {
        scale = 1;
    }

    /* The glyph's content bounding box in native pixels - inclusive on both
     * ends (a set bit at column max_x, row max_row counts as inside it). */
    int min_x = ICON_CHECK_BITMAP_SIZE, max_x = -1;
    int min_row = ICON_CHECK_BITMAP_SIZE, max_row = -1;
    for (int y = 0; y < ICON_CHECK_BITMAP_SIZE; y++) {
        const uint16_t row = icon_check_bitmap[y];
        if (row == 0) {
            continue;
        }
        if (y < min_row) { min_row = y; }
        if (y > max_row) { max_row = y; }
        for (int x = 0; x < ICON_CHECK_BITMAP_SIZE; x++) {
            if (row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x))) {
                if (x < min_x) { min_x = x; }
                if (x > max_x) { max_x = x; }
            }
        }
    }

    const int content_w = max_x - min_x + 1;
    const int content_h = max_row - min_row + 1;

    /* Where native (0, 0) lands once the content box above is centred in
     * (0, 0, w, h) at `scale`. Never negative: `scale` was chosen so
     * ICON_CHECK_BITMAP_SIZE * scale <= both w and h, and content_w/content_h
     * are each at most ICON_CHECK_BITMAP_SIZE, so content_w * scale <= w and
     * content_h * scale <= h follow directly. */
    const int origin_x = (w - content_w * scale) / 2 - min_x * scale;
    const int origin_y = (h - content_h * scale) / 2 - min_row * scale;

    int n = 0;
    for (int y = 0; y < ICON_CHECK_BITMAP_SIZE && n < max; y++) {
        const uint16_t row = icon_check_bitmap[y];
        int x = 0;
        while (x < ICON_CHECK_BITMAP_SIZE && n < max) {
            if (!(row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x)))) {
                x++;
                continue;
            }
            const int run_start = x;
            while (x < ICON_CHECK_BITMAP_SIZE &&
                  (row & (uint16_t)(1u << (ICON_CHECK_BITMAP_SIZE - 1 - x)))) {
                x++;
            }
            out[n].x = origin_x + run_start * scale;
            out[n].y = origin_y + y * scale;
            out[n].w = (x - run_start) * scale;
            out[n].h = scale;
            n++;
        }
    }
    return n;
}

/* Draws icon_check_blocks()'s shape filling (x, y, w, h) in `color`. Lives in
 * icons.c, not here, because it calls gfx_fill_rect() - see this header's own
 * top comment on the split. */
void icon_check(int x, int y, int w, int h, gfx_color_t color);
