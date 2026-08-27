/*=============================================================================
 * palette - grid arithmetic and hit-testing for the material picker overlay.
 *
 * Pure logic, no gfx and no touch state: a tile index in, a rectangle out,
 * or a screen point in and a tile index out. No ESP-IDF or hardware header
 * may be pulled in here (not even gfx.h, which drags in bsp/esp-bsp.h) - the
 * whole reason this lives in its own file instead of a couple of static
 * functions in app_sand.c is so it links on the host and the geometry can be
 * tested there. See row_runs.h beside it for the same pattern.
 *
 * WHY FOUR COLUMNS
 *
 * The panel is 368 px wide on a 1.8" 368x448 screen, which works out to about
 * 322 ppi - so 1 mm is roughly 12.7 px and a fingertip's contact patch is
 * roughly 89 px across. Four columns puts a tile at 368 / 4 = 92 px = 7.2 mm,
 * right at the accepted minimum touch target; five columns would be 368 / 5 =
 * 74 px = 5.8 mm, under any guideline going. Four is the most columns that
 * still keeps a tile at or above a fingertip - this reasoning is the entire
 * justification for PALETTE_COLS/PALETTE_TILE below and cannot be recovered
 * from the numbers alone, which is why it is written out here rather than
 * left for the constants to speak for themselves.
 *
 * THE SCREEN SIZE IS DUPLICATED, NOT SHARED
 *
 * gfx.h has no business being included by a host-testable module, so its
 * GFX_WIDTH/GFX_HEIGHT cannot be reused here. PALETTE_SCREEN_W/H below are the
 * same 368x448 by a different name - if the panel is ever a different size,
 * both places need to agree, the same way app_sand.c already keeps its own
 * grid math in step with gfx.h's dimensions.
 *
 * THE LAST ROW IS CENTRED, NOT LEFT-ALIGNED
 *
 * When `count` does not fill a whole number of rows, the short last row sits
 * centred across the panel's width rather than flush against the left edge -
 * see palette_tile_rect()'s own comment for the arithmetic. Get this wrong
 * and the last few materials are either unreachable or answer to the wrong
 * index, which is why this module is host-tested rather than trusted by eye.
 *
 * TWO VIEWS OF ONE LAYOUT
 *
 * palette_tile_rect() and palette_hit() must never disagree: a point inside
 * tile i's rect has to hit i, and a point outside every tile's rect has to
 * hit -1. They are kept as two separate functions only because the caller
 * needs both a forward mapping (draw tile i where?) and a reverse one (what
 * did this touch land on?) - not because they are free to drift apart. A
 * change to one's arithmetic is a change to the other's.
 *===========================================================================*/
#pragma once

/* Duplicated from gfx.h's GFX_WIDTH/GFX_HEIGHT - see this file's own top
 * comment for why this module cannot simply include that header. */
#define PALETTE_SCREEN_W 368
#define PALETTE_SCREEN_H 448

#define PALETTE_COLS 4
#define PALETTE_TILE 92

/* Rows a `count`-brush palette needs, ceiling-divided so a partial last row
 * still gets counted. `count` is a compile-time constant at its only call
 * site (BRUSH_COUNT in app_sand.c), which is what makes PALETTE_FITS below
 * usable inside a _Static_assert. */
#define PALETTE_ROWS(count) (((count) + PALETTE_COLS - 1) / PALETTE_COLS)

/* The panel's pixel height for `count` brushes, derived rather than
 * hardcoded - see PALETTE_ROWS above. */
#define PALETTE_HEIGHT(count) (PALETTE_ROWS(count) * PALETTE_TILE)

/* Whether a `count`-brush panel fits the screen. The grid must never be
 * hand-synced to the brush list: assert this against the real brush count,
 * e.g.
 *
 *     _Static_assert(PALETTE_FITS(BRUSH_COUNT), "palette panel taller than "
 *                    "the screen - too many brushes for PALETTE_COLS");
 *
 * so a 17th brush fails the BUILD instead of silently drawing a row that
 * runs off the bottom of the screen. */
#define PALETTE_FITS(count) (PALETTE_HEIGHT(count) <= PALETTE_SCREEN_H)

/* Where tile `index` sits, in screen pixels - the grid is PALETTE_COLS wide
 * and PALETTE_ROWS(count) tall, centred on the screen. Horizontally a full
 * row exactly fills the panel (PALETTE_COLS * PALETTE_TILE == 368, so a full
 * row starts at x=0); vertically the whole block is offset down by
 * (PALETTE_SCREEN_H - rows * PALETTE_TILE) / 2 so the sand shows evenly
 * above and below it.
 *
 * A row short of PALETTE_COLS tiles - only ever the LAST row - is centred
 * the same way the whole block is centred vertically: its own row width is
 * `row_count * PALETTE_TILE`, and that block is offset by
 * (PALETTE_SCREEN_W - row_width) / 2 from the left edge. A full row's
 * row_count is PALETTE_COLS, which makes that same formula land back on
 * x=0 - so there is only one centring rule here, not a special case for the
 * last row and a separate one for every other. */
void palette_tile_rect(int index, int count, int *x, int *y, int *w, int *h);

/* Which tile contains (px, py), or -1 for none - including a point in the
 * empty part of a centred partial row, a point outside the panel entirely,
 * and negative coordinates. Must stay exactly consistent with
 * palette_tile_rect() - see this file's top comment. */
int palette_hit(int px, int py, int count);

/* The panel's own bounds - PALETTE_COLS * PALETTE_TILE wide,
 * PALETTE_HEIGHT(count) tall, centred on the screen - for the caller to
 * clear or restore. */
void palette_panel_rect(int count, int *x, int *y, int *w, int *h);
