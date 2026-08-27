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
 * justification for PALETTE_TILE below and for palette_cols()'s floor(width /
 * PALETTE_TILE), and cannot be recovered from the numbers alone, which is why
 * it is written out here rather than left for the arithmetic to speak for
 * itself. Four is not hardcoded any more - palette_cols() derives it from
 * whichever width the panel is actually filling - but it is still the answer
 * this reasoning gives at both real widths this panel is ever drawn at (368
 * and, at a quarter turn, 448); see palette_cols()'s own comment.
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

/* WHY THE COLUMN COUNT IS DERIVED, NOT FIXED
 *
 * A grid is a description of how content fills a rect, so it has to be
 * recomputed for the rect it is actually filling - fixing it at compile time
 * baked in an assumption (the panel is always 368 px wide) that a quarter
 * turn already breaks elsewhere in this file (see palette_tile_rect()'s own
 * comment on screen_w/screen_h being the LOGICAL, not physical, canvas).
 * palette_cols() below is that recomputation: the same 92 px touch-target
 * floor this file's top comment justifies, applied to whatever width is
 * actually available rather than assumed. At both real screen widths this
 * panel ever sees - 368 (upright) and 448 (a quarter turn) - it still comes
 * out to 4, by calculation rather than coincidence: floor(368/92) = 4 exactly
 * (368 is a multiple of 92), and floor(448/92) = 4 with 80 px left over
 * (448 = 4*92 + 80). See suite_palette.c's own test of this across a range of
 * widths, including both. */
#define PALETTE_TILE 92

/* Upper bound on what palette_cols()/PALETTE_COLS_FOR() below can return.
 *
 * Not protecting any fixed-size array - there is none in this file or in
 * app_sand.c's callers keyed by column count; PALETTE_COLS used to be a
 * plain constant read directly wherever a column count was needed, never an
 * array dimension. This exists purely so a caller passing a huge or garbage
 * screen_w (the function takes a plain int, so nothing stops one) gets a
 * capped, sane column count back instead of one dividing 368 px of touch
 * target across an absurd number of columns. 16 is far above anything the
 * touch-target reasoning above would ever justify - the whole argument
 * caps out at 4 - so this bound is never the thing actually limiting a real
 * call. */
#define PALETTE_COLS_MAX 16

/* The formula palette_cols() computes, as a preprocessor constant expression
 * so PALETTE_FITS below can evaluate it inside a _Static_assert - a plain
 * function call cannot appear there. palette_cols() is defined in palette.c
 * IN TERMS OF this macro rather than reimplementing the same arithmetic, so
 * there is exactly one formula and no way for the runtime and compile-time
 * versions to drift apart. floor(w / PALETTE_TILE), clamped to
 * [1, PALETTE_COLS_MAX]. */
#define PALETTE_COLS_FOR(w) \
    ((((w) / PALETTE_TILE) < 1) ? 1 : \
     (((w) / PALETTE_TILE) > PALETTE_COLS_MAX ? PALETTE_COLS_MAX : \
      ((w) / PALETTE_TILE)))

/* How many columns a `screen_w`-wide canvas gets - see this file's own
 * "WHY THE COLUMN COUNT IS DERIVED, NOT FIXED" comment above. Every caller
 * that used to read the constant PALETTE_COLS now calls this with the
 * LOGICAL canvas width (see ui.h's ui_width(), not GFX_WIDTH) and threads
 * the result through explicitly, the same way screen_w/screen_h are already
 * threaded through palette_tile_rect() and friends below - never a hidden
 * global. */
int palette_cols(int screen_w);

/* Duplicated from gfx.h's GFX_CHAR_W/GFX_CHAR_H - see this file's own top
 * comment ("THE SCREEN SIZE IS DUPLICATED, NOT SHARED") for why: gfx.h drags
 * in bsp/esp-bsp.h, which this host-testable module cannot include. Both are
 * 16 (an 8x8 font glyph at GFX_GLYPH_SCALE 2) and must stay in step with
 * gfx.h's own definitions by hand. */
#define PALETTE_CHAR_W 16
#define PALETTE_CHAR_H 16

/* Rows a `count`-brush palette needs at `cols` columns, ceiling-divided so a
 * partial last row still gets counted. Both are compile-time constants at
 * PALETTE_FITS's only call site (BRUSH_COUNT and PALETTE_COLS_FOR(...) in
 * app_sand.c), which is what keeps PALETTE_FITS usable inside a
 * _Static_assert. */
#define PALETTE_ROWS(count, cols) (((count) + (cols) - 1) / (cols))

/* The panel's pixel height for `count` brushes at `cols` columns, derived
 * rather than hardcoded - see PALETTE_ROWS above. */
#define PALETTE_HEIGHT(count, cols) (PALETTE_ROWS(count, cols) * PALETTE_TILE)

/* Whether a `count`-brush panel fits the screen AT EVERY QUARTER TURN.
 *
 * Columns are no longer one fixed number the panel carries into every
 * orientation - they are recomputed per screen_w (see palette_cols() above),
 * so "fits" can no longer be answered by checking one derived height against
 * whichever screen dimension is shorter the way it used to be: the two real
 * orientations this panel is ever drawn at now potentially compute DIFFERENT
 * column counts, and each has its own height to check against its own
 * paired screen dimension. So this checks both pairings explicitly -
 * (screen_w=PALETTE_SCREEN_W, screen_h=PALETTE_SCREEN_H) and the swapped
 * pair a quarter turn produces - rather than reusing one shared height
 * against a shared minimum. (At today's PALETTE_TILE, both pairings happen
 * to derive the same column count - 4 - so this is currently equivalent to
 * the old shared-minimum check; it is written the more general way because
 * that equivalence is a coincidence of 92 dividing both 368 and 448 the same
 * number of times, not a property PALETTE_TILE is guaranteed to keep.)
 *
 * Today's panel is 368 px tall in the upright pairing and exactly 368 px
 * tall (== PALETTE_SCREEN_W) in the turned one - so the turned pairing is
 * pinning a property that currently holds with NO margin at all, not fixing
 * a panel that was actually overflowing. The grid must never be hand-synced
 * to the brush list: assert this against the real brush count, e.g.
 *
 *     _Static_assert(PALETTE_FITS(BRUSH_COUNT), "palette panel taller than "
 *                    "the screen - too many brushes for the panel's derived "
 *                    "column count");
 *
 * so a 17th brush fails the BUILD instead of silently drawing a row that
 * runs off the bottom of the screen at some turns and not others. */
#define PALETTE_FITS(count) \
    (PALETTE_HEIGHT(count, PALETTE_COLS_FOR(PALETTE_SCREEN_W)) <= \
        PALETTE_SCREEN_H && \
     PALETTE_HEIGHT(count, PALETTE_COLS_FOR(PALETTE_SCREEN_H)) <= \
        PALETTE_SCREEN_W)

/* Where tile `index` sits, in screen pixels - the grid is `cols` wide and
 * PALETTE_ROWS(count, cols) tall, centred on a `screen_w` x `screen_h`
 * canvas. `cols` is the caller's own palette_cols(screen_w) - passed in
 * explicitly rather than recomputed here, the same way screen_w/screen_h are
 * already threaded through rather than assumed, so this function never has
 * to guess which canvas `cols` was derived from. `screen_w`/`screen_h` is
 * the LOGICAL canvas (see ui.h's ui_width()/ui_height()), not always
 * PALETTE_SCREEN_W/PALETTE_SCREEN_H: under a quarter turn the two swap, and
 * this centres against whichever is current rather than assuming the
 * upright pair. Horizontally a full row exactly fills the panel when
 * `cols * PALETTE_TILE` equals screen_w - which palette_cols() makes true
 * up to the leftover PALETTE_TILE cannot fill, so a full row starts at
 * x=0 or close to it; vertically the whole block is offset down by
 * (screen_h - rows * PALETTE_TILE) / 2 so the sand shows evenly above and
 * below it.
 *
 * A row short of `cols` tiles - only ever the LAST row - is centred the
 * same way the whole block is centred vertically: its own row width is
 * `row_count * PALETTE_TILE`, and that block is offset by
 * (screen_w - row_width) / 2 from the left edge. A full row's row_count is
 * `cols`, which makes that same formula land back on the panel's own left
 * edge - so there is only one centring rule here, not a special case for
 * the last row and a separate one for every other. */
void palette_tile_rect(int index, int count, int cols, int screen_w,
                       int screen_h, int *x, int *y, int *w, int *h);

/* Which tile contains (px, py), or -1 for none - including a point in the
 * empty part of a centred partial row, a point outside the panel entirely,
 * and negative coordinates. Must stay exactly consistent with
 * palette_tile_rect() - see this file's top comment. `cols`, `screen_w` and
 * `screen_h` must be the same values passed to palette_tile_rect() for the
 * two to agree - see that function's own comment on why `cols` is threaded
 * through rather than recomputed, and on why screen_w/screen_h are not
 * always PALETTE_SCREEN_W/PALETTE_SCREEN_H. */
int palette_hit(int px, int py, int count, int cols, int screen_w,
                int screen_h);

/* The panel's own bounds - `cols * PALETTE_TILE` wide,
 * PALETTE_HEIGHT(count, cols) tall, centred on a `screen_w` x `screen_h`
 * canvas - for the caller to clear or restore. */
void palette_panel_rect(int count, int cols, int screen_w, int screen_h,
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
