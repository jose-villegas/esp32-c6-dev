#include "palette.h"

/* Row geometry shared by palette_tile_rect() and palette_hit() - see
 * palette.h's own "two views of one layout" comment for why these two
 * functions must never disagree. Both compute a row's tile count and its
 * left edge with this exact arithmetic rather than each rolling their own,
 * so there is only one place the centring rule can be wrong. */
static int row_tile_count(int row, int count)
{
    const int start = row * PALETTE_COLS;
    const int remaining = count - start;
    return (remaining < PALETTE_COLS) ? remaining : PALETTE_COLS;
}

static int row_left_x(int row_count)
{
    return (PALETTE_SCREEN_W - row_count * PALETTE_TILE) / 2;
}

static int panel_top_y(int count)
{
    const int rows = PALETTE_ROWS(count);
    return (PALETTE_SCREEN_H - rows * PALETTE_TILE) / 2;
}

void palette_tile_rect(int index, int count, int *x, int *y, int *w, int *h)
{
    const int row = index / PALETTE_COLS;
    const int col = index % PALETTE_COLS;
    const int row_count = row_tile_count(row, count);

    *x = row_left_x(row_count) + col * PALETTE_TILE;
    *y = panel_top_y(count) + row * PALETTE_TILE;
    *w = PALETTE_TILE;
    *h = PALETTE_TILE;
}

int palette_hit(int px, int py, int count)
{
    const int top = panel_top_y(count);
    const int rows = PALETTE_ROWS(count);

    if (py < top || py >= top + rows * PALETTE_TILE) {
        return -1;
    }
    const int row = (py - top) / PALETTE_TILE;

    const int row_count = row_tile_count(row, count);
    const int left = row_left_x(row_count);
    const int row_w = row_count * PALETTE_TILE;

    if (px < left || px >= left + row_w) {
        return -1;
    }
    const int col = (px - left) / PALETTE_TILE;

    const int index = row * PALETTE_COLS + col;
    /* Cannot exceed `count`: row_count already clipped the last row to
     * however many tiles it actually holds, and col is bounded by row_w /
     * PALETTE_TILE == row_count above. */
    return index;
}

void palette_panel_rect(int count, int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = panel_top_y(count);
    *w = PALETTE_COLS * PALETTE_TILE;
    *h = PALETTE_HEIGHT(count);
}
