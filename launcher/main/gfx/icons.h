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

typedef struct {
    int x, y, w, h;
} icon_rect_t;

/* Every hand-drawn icon in this module shares this format: one row of bits
 * per scanline, MSB is column 0. Not a property of the check mark
 * specifically - see icon_bitmap_blocks() below, which works on any bitmap
 * in this shape. */
#define ICON_BITMAP_SIZE 16

/* icon_bitmap_blocks() never returns more than this many blocks for ANY
 * ICON_BITMAP_SIZE-wide bitmap: a row packs at most ICON_BITMAP_SIZE/2 runs
 * (a run needs at least one clear bit after it to end), times
 * ICON_BITMAP_SIZE rows. A caller sizing a buffer for an arbitrary bitmap in
 * this format - one it has not hand-counted the runs of - uses this, not
 * ICON_CHECK_MAX_BLOCKS below, which is a fact about the check mark's own
 * artwork, not about bitmaps in general. */
#define ICON_BITMAP_MAX_BLOCKS ((ICON_BITMAP_SIZE / 2) * ICON_BITMAP_SIZE)

/* icon_check_blocks() never returns more than this many blocks - tighter
 * than ICON_BITMAP_MAX_BLOCKS because it is a fact about icon_check_bitmap's
 * own run count, not about the format in general - see
 * suite_icons.c's test_max_blocks_matches_the_bitmaps_actual_worst_case. */
#define ICON_CHECK_MAX_BLOCKS 16

#define ICON_CHECK_BITMAP_SIZE 16

/* The check mark, hand-drawn at 16x16 - one row of bits per line, same
 * convention font8x8_basic.h uses. Bit 15 (MSB) is column 0 (left); bit
 * 0 (LSB) is column 15 (right). A short limb descends left-to-right to
 * a vertex, then a long limb rises past it about a third again as far,
 * well above where the short limb started - reads as a check, not a V
 * or a plain diagonal tick. The long limb's free tip tapers to a single
 * pixel rather than ending square, keeping a small check from reading
 * as a blunt slab. */
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

/* Generic geometry behind every hand-drawn icon in this format: run-length
 * extraction, scaled by the largest INTEGER factor that fits box (0, 0, w,
 * h)'s smaller side (minimum 1, same rule gfx_text_scaled() uses - a
 * fractional scale needs interpolation this device has no budget for), then
 * centred. None of it is specific to the check mark, which is why
 * icon_check_blocks() below is now a thin wrapper over this. */
static inline int icon_bitmap_blocks(const uint16_t *bitmap, int w, int h,
                                     icon_rect_t *out, int max)
{
    const int side  = (w < h) ? w : h;
    int scale = side / ICON_BITMAP_SIZE;
    if (scale < 1) {
        scale = 1;
    }

    /* A glyph rarely fills its whole ICON_BITMAP_SIZE square, so scaling
     * and centring the full box would off-centre the visible content by
     * however wide that margin is. Scans the bitmap's own CONTENT
     * bounding box fresh each call, rather than hand-computed, so it
     * can never drift out of step with `bitmap` if that changes -
     * inclusive on both ends (a set bit at column max_x, row max_row
     * counts as inside it). */
    int min_x = ICON_BITMAP_SIZE, max_x = -1;
    int min_row = ICON_BITMAP_SIZE, max_row = -1;
    for (int y = 0; y < ICON_BITMAP_SIZE; y++) {
        const uint16_t row = bitmap[y];
        if (row == 0) {
            continue;
        }
        if (y < min_row) { min_row = y; }
        if (y > max_row) { max_row = y; }
        for (int x = 0; x < ICON_BITMAP_SIZE; x++) {
            if (row & (uint16_t)(1u << (ICON_BITMAP_SIZE - 1 - x))) {
                if (x < min_x) { min_x = x; }
                if (x > max_x) { max_x = x; }
            }
        }
    }

    const int content_w = max_x - min_x + 1;
    const int content_h = max_row - min_row + 1;

    /* Where native (0, 0) lands once the content box above is centred
     * in (0, 0, w, h) at `scale`. Never negative: `scale` was chosen so
     * ICON_BITMAP_SIZE * scale <= both w and h, and content_w/content_h
     * are each at most ICON_BITMAP_SIZE, so content_w * scale <= w and
     * content_h * scale <= h follow directly. */
    const int origin_x = (w - content_w * scale) / 2 - min_x * scale;
    const int origin_y = (h - content_h * scale) / 2 - min_row * scale;

    /* One output rect per contiguous run, not per pixel or row - scaling
     * changes a run's size, never how many runs there are, so the block
     * count is bounded by the bitmap's own run count regardless of
     * scale. ICON_BITMAP_MAX_BLOCKS bounds any bitmap in this format; a
     * specific bitmap's real worst case is usually far smaller (see
     * ICON_CHECK_MAX_BLOCKS). */
    int n = 0;
    for (int y = 0; y < ICON_BITMAP_SIZE && n < max; y++) {
        const uint16_t row = bitmap[y];
        int x = 0;
        while (x < ICON_BITMAP_SIZE && n < max) {
            if (!(row & (uint16_t)(1u << (ICON_BITMAP_SIZE - 1 - x)))) {
                x++;
                continue;
            }
            const int run_start = x;
            while (x < ICON_BITMAP_SIZE &&
                  (row & (uint16_t)(1u << (ICON_BITMAP_SIZE - 1 - x)))) {
                x++;
            }
            out[n].x = origin_x + run_start * scale;
            out[n].y = origin_y + y * scale;
            out[n].w = (x - run_start) * scale;
            out[n].h = scale;
            n++;
        }
    }
    /* Returns how many blocks were written to `out`, never more than the
     * bitmap's own run count and never more than `max` - a `max` smaller
     * than the shape needs truncates it rather than overflowing `out`. */
    return n;
}

/* icon_check_bitmap's own geometry - see icon_bitmap_blocks() above for the
 * logic itself. Kept as its own entry point (rather than callers reaching
 * for icon_bitmap_blocks() directly) so ICON_CHECK_MAX_BLOCKS stays the one
 * place that promises the check mark's tighter, artwork-specific bound. */
static inline int icon_check_blocks(int w, int h, icon_rect_t *out, int max)
{
    return icon_bitmap_blocks(icon_check_bitmap, w, h, out, max);
}

/* Draws icon_check_blocks()'s shape filling (x, y, w, h) in `color`. Lives in
 * icons.c, not here, because it calls gfx_fill_rect() - see this header's own
 * top comment on the split. */
void icon_check(int x, int y, int w, int h, gfx_color_t color);
