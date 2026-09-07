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

/* WHY DERIVED, NOT FIXED: fixing the column count at compile time bakes
 * in the assumption the panel is always 368 px wide, which a quarter
 * turn already breaks elsewhere in this file. palette_cols() recomputes
 * it, applying the same 92 px touch-target floor to whatever width is
 * available. At both real widths this panel sees - 368 and 448 (a
 * quarter turn) - it still comes out to 4 by calculation, not
 * coincidence: floor(368/92)=4 exactly, floor(448/92)=4 with 80 px left
 * over. */
#define PALETTE_TILE 92

/* Upper bound on what palette_cols()/PALETTE_COLS_FOR() can return. Not
 * protecting any fixed-size array - there is none keyed by column count.
 * Exists purely so a caller passing a huge or garbage screen_w (a plain
 * int, nothing stops one) gets a capped, sane column count back instead
 * of dividing touch target across an absurd number of columns. 16 is far
 * above anything the touch-target reasoning above would justify - that
 * argument caps out at 4 - so this bound never actually limits a real
 * call. */
#define PALETTE_COLS_MAX 16

/* The formula palette_cols() computes, as a preprocessor constant
 * expression so PALETTE_FITS below can evaluate it inside a
 * _Static_assert - a plain function call cannot appear there.
 * palette_cols() is defined in palette.c IN TERMS OF this macro rather
 * than reimplementing the same arithmetic, so there is exactly one
 * formula and no way for the runtime and compile-time versions to drift
 * apart. floor(w / PALETTE_TILE), clamped to [1, PALETTE_COLS_MAX]. */
#define PALETTE_COLS_FOR(w) \
    ((((w) / PALETTE_TILE) < 1) ? 1 : \
     (((w) / PALETTE_TILE) > PALETTE_COLS_MAX ? PALETTE_COLS_MAX : \
      ((w) / PALETTE_TILE)))

/* How many columns a `screen_w`-wide canvas gets - see this file's own
 * "WHY DERIVED, NOT FIXED" comment above. Every caller that used to read
 * the constant PALETTE_COLS now calls this with the LOGICAL canvas width
 * (see ui.h's ui_width(), not GFX_WIDTH) and threads the result through
 * explicitly, the same way screen_w/screen_h are already threaded
 * through palette_tile_rect() and friends below - never a hidden
 * global. */
int palette_cols(int screen_w);

/* Duplicated from gfx.h's GFX_CHAR_W/GFX_CHAR_H - see this file's own top
 * comment ("THE SCREEN SIZE IS DUPLICATED, NOT SHARED") for why: gfx.h
 * drags in bsp/esp-bsp.h, which this host-testable module cannot
 * include. Both are 16 (an 8x8 font glyph at GFX_GLYPH_SCALE 2) and must
 * stay in step with gfx.h's own definitions by hand. */
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
 * Columns are recomputed per screen_w now, so orientations can derive
 * DIFFERENT column counts, each checked against its own paired dimension
 * - hence both pairings, not one shared minimum. At today's PALETTE_TILE
 * both derive 4 (92 divides 368/448 equally by coincidence), and the
 * turned pairing (368 tall == PALETTE_SCREEN_W) pins that fit with NO
 * margin. Assert against the real brush count so growth fails the
 * BUILD, not the screen. */
#define PALETTE_FITS(count) \
    (PALETTE_HEIGHT(count, PALETTE_COLS_FOR(PALETTE_SCREEN_W)) <= \
        PALETTE_SCREEN_H && \
     PALETTE_HEIGHT(count, PALETTE_COLS_FOR(PALETTE_SCREEN_H)) <= \
        PALETTE_SCREEN_W)

/* Where tile `index` sits, in screen pixels - grid is `cols` wide,
 * PALETTE_ROWS(count, cols) tall, centred on `screen_w` x `screen_h`.
 * `cols` is the caller's own palette_cols(screen_w), passed in rather
 * than recomputed, so this never guesses which canvas `cols` came from.
 * `screen_w`/`screen_h` is the LOGICAL canvas (ui_width()/ui_height()):
 * under a quarter turn the two swap. See palette.c's own comment above
 * the definition for the centring arithmetic. */
void palette_tile_rect(int index, int count, int cols, int screen_w,
                       int screen_h, int *x, int *y, int *w, int *h);

/* Which tile contains (px, py), or -1 for none - including a point in
 * the empty part of a centred partial row, a point outside the panel
 * entirely, and negative coordinates. Must stay exactly consistent with
 * palette_tile_rect() - see this file's top comment. `cols`, `screen_w`
 * and `screen_h` must be the same values passed to palette_tile_rect()
 * for the two to agree - see that function's own comment on why `cols`
 * is threaded through rather than recomputed. */
int palette_hit(int px, int py, int count, int cols, int screen_w,
                int screen_h);

/* The panel's own bounds - `cols * PALETTE_TILE` wide,
 * PALETTE_HEIGHT(count, cols) tall, centred on a `screen_w` x `screen_h`
 * canvas - for the caller to clear or restore. */
void palette_panel_rect(int count, int cols, int screen_w, int screen_h,
                        int *x, int *y, int *w, int *h);

/* Where to start drawing a `len`-character label so gfx_text_turned(), at
 * `turn` quarter turns, ends up centred in the rect (x, y, w, h).
 * gfx_text_turned()'s (x, y) is the FIRST GLYPH's cell, not a corner of
 * the string, and runs away in whichever direction the turn implies -
 * getting the corner/direction wrong by even one turn is an off-by-one
 * invisible until the board is turned, why this is host-tested (see
 * suite_palette.c). Generalises app_sand.c's draw_mode_label() to an
 * arbitrary rect. */
void palette_label_origin(int x, int y, int w, int h, int len, int turn,
                          int *out_x, int *out_y);
