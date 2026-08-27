#include <stdbool.h>

#include "palette.h"

/* Defined in terms of PALETTE_COLS_FOR() (palette.h) rather than
 * reimplementing floor+clamp here, so the runtime and the compile-time
 * _Static_assert path (PALETTE_FITS, also built from PALETTE_COLS_FOR())
 * share exactly one formula - see PALETTE_COLS_FOR()'s own comment for why
 * that matters. */
int palette_cols(int screen_w)
{
    return PALETTE_COLS_FOR(screen_w);
}

/* Row geometry shared by palette_tile_rect() and palette_hit() - see
 * palette.h's own "two views of one layout" comment for why these two
 * functions must never disagree. Both compute a row's tile count and its
 * left edge with this exact arithmetic rather than each rolling their own,
 * so there is only one place the centring rule can be wrong. */
static int row_tile_count(int row, int count, int cols)
{
    const int start = row * cols;
    const int remaining = count - start;
    return (remaining < cols) ? remaining : cols;
}

static int row_left_x(int row_count, int screen_w)
{
    return (screen_w - row_count * PALETTE_TILE) / 2;
}

static int panel_top_y(int count, int cols, int screen_h)
{
    const int rows = PALETTE_ROWS(count, cols);
    return (screen_h - rows * PALETTE_TILE) / 2;
}

void palette_tile_rect(int index, int count, int cols, int screen_w,
                       int screen_h, int *x, int *y, int *w, int *h)
{
    const int row = index / cols;
    const int col = index % cols;
    const int row_count = row_tile_count(row, count, cols);

    *x = row_left_x(row_count, screen_w) + col * PALETTE_TILE;
    *y = panel_top_y(count, cols, screen_h) + row * PALETTE_TILE;
    *w = PALETTE_TILE;
    *h = PALETTE_TILE;
}

int palette_hit(int px, int py, int count, int cols, int screen_w,
                int screen_h)
{
    const int top = panel_top_y(count, cols, screen_h);
    const int rows = PALETTE_ROWS(count, cols);

    if (py < top || py >= top + rows * PALETTE_TILE) {
        return -1;
    }
    const int row = (py - top) / PALETTE_TILE;

    const int row_count = row_tile_count(row, count, cols);
    const int left = row_left_x(row_count, screen_w);
    const int row_w = row_count * PALETTE_TILE;

    if (px < left || px >= left + row_w) {
        return -1;
    }
    const int col = (px - left) / PALETTE_TILE;

    const int index = row * cols + col;
    /* Cannot exceed `count`: row_count already clipped the last row to
     * however many tiles it actually holds, and col is bounded by row_w /
     * PALETTE_TILE == row_count above. */
    return index;
}

void palette_panel_rect(int count, int cols, int screen_w, int screen_h,
                        int *x, int *y, int *w, int *h)
{
    /* row_left_x(cols, ...) is exactly the panel's own left edge: a
     * full-width row (row_count == cols) IS the panel's own width, centred
     * the same way. This used to be hardcoded to 0, which only happened to
     * be right because PALETTE_COLS * PALETTE_TILE == PALETTE_SCREEN_W
     * (368 == 368) in the upright case - at screen_w == 448 (a quarter
     * turn), a panel narrower than the canvas no longer fills it and has to
     * be centred like everything else here. */
    *x = row_left_x(cols, screen_w);
    *y = panel_top_y(count, cols, screen_h);
    *w = cols * PALETTE_TILE;
    *h = PALETTE_HEIGHT(count, cols);
}

void palette_label_origin(int x, int y, int w, int h, int len, int turn,
                          int *out_x, int *out_y)
{
    /* The bounding box the string occupies, centred in (x, y, w, h) - one
     * box, computed the same way regardless of turn, which is what makes
     * turn 0 and turn 2 (and turn 1 and turn 3) share it below. Swapped at
     * turns 1/3 because the glyphs stack vertically instead of side by side
     * - see this function's declaration in palette.h. */
    const bool turned = (turn == 1) || (turn == 3);
    const int box_w = turned ? PALETTE_CHAR_H : len * PALETTE_CHAR_W;
    const int box_h = turned ? len * PALETTE_CHAR_W : PALETTE_CHAR_H;

    const int box_x = x + (w - box_w) / 2;
    const int box_y = y + (h - box_h) / 2;

    /* gfx_text_turned()'s origin is the FIRST GLYPH's cell, not a corner of
     * the box - for turn 0 and turn 1 that is the box's own top-left corner
     * (the string walks forward, +x or +y, away from it), but turn 2 and
     * turn 3 walk backward from the FAR end of the box, so their origin is
     * one glyph cell in from that far edge. */
    *out_x = box_x;
    *out_y = box_y;
    if (turn == 2) {
        *out_x = box_x + box_w - PALETTE_CHAR_W;
    } else if (turn == 3) {
        *out_y = box_y + box_h - PALETTE_CHAR_W;
    }
}
