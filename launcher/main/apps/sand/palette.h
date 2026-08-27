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

/* Duplicated from gfx.h's GFX_CHAR_W/GFX_CHAR_H - see this file's own top
 * comment ("THE SCREEN SIZE IS DUPLICATED, NOT SHARED") for why: gfx.h drags
 * in bsp/esp-bsp.h, which this host-testable module cannot include. Both are
 * 16 (an 8x8 font glyph at GFX_GLYPH_SCALE 2) and must stay in step with
 * gfx.h's own definitions by hand. */
#define PALETTE_CHAR_W 16
#define PALETTE_CHAR_H 16

/* Rows a `count`-brush palette needs, ceiling-divided so a partial last row
 * still gets counted. `count` is a compile-time constant at its only call
 * site (BRUSH_COUNT in app_sand.c), which is what makes PALETTE_FITS below
 * usable inside a _Static_assert. */
#define PALETTE_ROWS(count) (((count) + PALETTE_COLS - 1) / PALETTE_COLS)

/* The panel's pixel height for `count` brushes, derived rather than
 * hardcoded - see PALETTE_ROWS above. */
#define PALETTE_HEIGHT(count) (PALETTE_ROWS(count) * PALETTE_TILE)

/* Whether a `count`-brush panel fits the screen AT EVERY QUARTER TURN. The
 * panel is drawn upright on a 368x448 screen, but the board can be held any
 * of four ways and the panel is redrawn turned to match (see app_sand.c's
 * draw_palette()) rather than moved or resized - so "fits" cannot mean fits
 * the tall 448 px dimension alone, or a panel that is fine upright would run
 * off the edge the moment the board is turned 90 degrees and the same 448 px
 * of panel height has only 368 px of screen width to land in. The real
 * constraint is the SHORTER of the two screen dimensions, since that is what
 * every quarter turn's long axis eventually has to fit inside.
 *
 * Today's panel is PALETTE_HEIGHT(BRUSH_COUNT) == 368, which passes against
 * either dimension - so this is pinning a property that currently holds by
 * luck (nothing before this exercised the narrow-side case), not fixing a
 * panel that was actually overflowing. The grid must never be hand-synced to
 * the brush list: assert this against the real brush count, e.g.
 *
 *     _Static_assert(PALETTE_FITS(BRUSH_COUNT), "palette panel taller than "
 *                    "the screen - too many brushes for PALETTE_COLS");
 *
 * so a 17th brush fails the BUILD instead of silently drawing a row that
 * runs off the bottom of the screen at some turns and not others. */
#define PALETTE_FITS(count) \
    (PALETTE_HEIGHT(count) <= \
     ((PALETTE_SCREEN_W < PALETTE_SCREEN_H) ? PALETTE_SCREEN_W : PALETTE_SCREEN_H))

/* Where tile `index` sits, in screen pixels - the grid is PALETTE_COLS wide
 * and PALETTE_ROWS(count) tall, centred on a `screen_w` x `screen_h` canvas.
 * `screen_w`/`screen_h` is the LOGICAL canvas (see ui.h's ui_width()/
 * ui_height()), not always PALETTE_SCREEN_W/PALETTE_SCREEN_H: under a
 * quarter turn the two swap, and this centres against whichever is current
 * rather than assuming the upright pair. Horizontally a full row exactly
 * fills the panel when screen_w is 368 (PALETTE_COLS * PALETTE_TILE == 368,
 * so a full row starts at x=0); vertically the whole block is offset down by
 * (screen_h - rows * PALETTE_TILE) / 2 so the sand shows evenly above and
 * below it.
 *
 * A row short of PALETTE_COLS tiles - only ever the LAST row - is centred
 * the same way the whole block is centred vertically: its own row width is
 * `row_count * PALETTE_TILE`, and that block is offset by
 * (screen_w - row_width) / 2 from the left edge. A full row's row_count is
 * PALETTE_COLS, which makes that same formula land back on x=0 - so there
 * is only one centring rule here, not a special case for the last row and a
 * separate one for every other. */
void palette_tile_rect(int index, int count, int screen_w, int screen_h,
                       int *x, int *y, int *w, int *h);

/* Which tile contains (px, py), or -1 for none - including a point in the
 * empty part of a centred partial row, a point outside the panel entirely,
 * and negative coordinates. Must stay exactly consistent with
 * palette_tile_rect() - see this file's top comment. `screen_w`/`screen_h`
 * must be the same logical canvas passed to palette_tile_rect() for the two
 * to agree - see that function's own comment on why this is not always
 * PALETTE_SCREEN_W/PALETTE_SCREEN_H. */
int palette_hit(int px, int py, int count, int screen_w, int screen_h);

/* The panel's own bounds - PALETTE_COLS * PALETTE_TILE wide,
 * PALETTE_HEIGHT(count) tall, centred on a `screen_w` x `screen_h` canvas -
 * for the caller to clear or restore. */
void palette_panel_rect(int count, int screen_w, int screen_h,
                        int *x, int *y, int *w, int *h);

/* Where to start drawing a `len`-character label so gfx_text_turned(), at
 * `turn` quarter turns, ends up centred in the rect (x, y, w, h).
 *
 * gfx_text_turned()'s (x, y) is the FIRST GLYPH's cell, not any corner of
 * the string as a whole, and the string runs away from it in whichever
 * direction the turn implies - four different directions, one per turn (see
 * gfx.h's own comment on gfx_text_turned). Getting the corner and the run
 * direction wrong by even one turn is exactly the off-by-one that would go
 * unnoticed until the board is actually turned, which is why this is pulled
 * out here where it can be host-tested instead of trusted by eye inline in
 * draw_palette() - see suite_palette.c.
 *
 * Unrotated (turn 0 or 2) the string occupies `len * PALETTE_CHAR_W` by
 * PALETTE_CHAR_H; at turn 1 or 3 those two swap, because the glyphs are
 * stacked vertically instead of side by side. Either way there is one
 * bounding box, centred in the rect the same way regardless of turn -
 * turn 0 and turn 2 share the same box, only the corner of it that
 * gfx_text_turned() is told to start from (and the direction it walks from
 * there) differs, and likewise for turn 1 and turn 3:
 *
 *   turn 0 (upright, left-to-right):     box's top-left,  walks +x
 *   turn 1 (top-to-bottom):              box's top-left,  walks +y
 *   turn 2 (upside down, right-to-left): box's top-right, walks -x
 *   turn 3 (bottom-to-top):              box's bottom-left, walks -y
 *
 * This is not a fresh derivation - it is app_sand.c's draw_mode_label()'s
 * existing per-turn positioning, which already solves this same problem for
 * one string, generalised from "centred on the screen" to "centred in an
 * arbitrary rect" and factored out so both callers read off the same
 * arithmetic instead of two hand-written copies quietly drifting apart. */
void palette_label_origin(int x, int y, int w, int h, int len, int turn,
                          int *out_x, int *out_y);
