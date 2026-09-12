/*
 * palette - grid arithmetic and hit-testing for the material picker overlay.
 *
 * Pure logic, no gfx and no touch state: a tile index in, a rectangle out, or
 * a screen point in and a tile index out. No ESP-IDF or hardware header may
 * be included here - not even gfx.h, which drags in bsp/esp-bsp.h - and that
 * is what lets the geometry be tested on a host. See row_runs.h beside it for
 * the same pattern.
 *
 * The tile size is a touch target rather than a taste. At this panel's ~322
 * ppi a fingertip's contact patch is about 89 px, so four columns give a
 * 92 px (7.2 mm) tile and five would give 74 px (5.8 mm), under any guideline
 * going. That is what PALETTE_TILE is for; palette_cols() derives the count
 * from whichever width is actually being filled rather than fixing it at four.
 *
 * PALETTE_SCREEN_W/H duplicate gfx.h's dimensions under different names
 * deliberately, since gfx.h cannot be included here. A panel of a different
 * size needs both places changed.
 *
 * palette_tile_rect() and palette_hit() must never disagree - a point inside
 * tile i's rect hits i, a point outside every rect hits -1. They are two
 * functions because the caller needs both directions, not because they are
 * free to drift; a change to one's arithmetic is a change to the other's.
 */
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
#define PALETTE_TILE     92

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
#define PALETTE_COLS_FOR(w)                                                                                            \
    ((((w) / PALETTE_TILE) < 1) ? 1                                                                                    \
                                : (((w) / PALETTE_TILE) > PALETTE_COLS_MAX ? PALETTE_COLS_MAX : ((w) / PALETTE_TILE)))

/* How many columns a `screen_w`-wide canvas gets - see this file's own
 * "WHY DERIVED, NOT FIXED" comment above. Callers pass the LOGICAL
 * canvas width (see ui.h's ui_width(), not GFX_WIDTH) and thread the
 * result through explicitly, the same way screen_w/screen_h are already
 * threaded through palette_tile_rect() and friends below - never a
 * hidden global. */
int palette_cols(int screen_w);

/* Duplicated from gfx.h's GFX_CHAR_W/GFX_CHAR_H - see this file's own top
 * comment ("THE SCREEN SIZE IS DUPLICATED, NOT SHARED") for why: gfx.h
 * drags in bsp/esp-bsp.h, which this host-testable module cannot
 * include. Both are 16 (an 8x8 font glyph at GFX_GLYPH_SCALE 2) and must
 * stay in step with gfx.h's own definitions by hand. */
#define PALETTE_CHAR_W              16
#define PALETTE_CHAR_H              16

/* Rows a `count`-brush palette needs at `cols` columns, ceiling-divided so a
 * partial last row still gets counted. Both are compile-time constants at
 * PALETTE_FITS's only call site (BRUSH_COUNT and PALETTE_COLS_FOR(...) in
 * app_sand.c), which is what keeps PALETTE_FITS usable inside a
 * _Static_assert. */
#define PALETTE_ROWS(count, cols)   (((count) + (cols) - 1) / (cols))

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
#define PALETTE_FITS(count)                                                                                            \
    (PALETTE_HEIGHT(count, PALETTE_COLS_FOR(PALETTE_SCREEN_W)) <= PALETTE_SCREEN_H                                     \
     && PALETTE_HEIGHT(count, PALETTE_COLS_FOR(PALETTE_SCREEN_H)) <= PALETTE_SCREEN_W)

/* Where tile `index` sits, in screen pixels - grid is `cols` wide,
 * PALETTE_ROWS(count, cols) tall, centred on `screen_w` x `screen_h`.
 * `cols` is the caller's own palette_cols(screen_w), passed in rather
 * than recomputed, so this never guesses which canvas `cols` came from.
 * `screen_w`/`screen_h` is the LOGICAL canvas (ui_width()/ui_height()):
 * under a quarter turn the two swap. See palette.c's own comment above
 * the definition for the centring arithmetic. */
void palette_tile_rect(int index, int count, int cols, int screen_w, int screen_h, int* x, int* y, int* w, int* h);

/* Which tile contains (px, py), or -1 for none - including a point in
 * the empty part of a centred partial row, a point outside the panel
 * entirely, and negative coordinates. Must stay exactly consistent with
 * palette_tile_rect() - see this file's top comment. `cols`, `screen_w`
 * and `screen_h` must be the same values passed to palette_tile_rect()
 * for the two to agree - see that function's own comment on why `cols`
 * is threaded through rather than recomputed. */
int palette_hit(int px, int py, int count, int cols, int screen_w, int screen_h);

/* The panel's own bounds - `cols * PALETTE_TILE` wide,
 * PALETTE_HEIGHT(count, cols) tall, centred on a `screen_w` x `screen_h`
 * canvas - for the caller to clear or restore. */
void palette_panel_rect(int count, int cols, int screen_w, int screen_h, int* x, int* y, int* w, int* h);

/* Where to start drawing a `len`-character label so gfx_text_turned(), at
 * `turn` quarter turns, ends up centred in the rect (x, y, w, h).
 * gfx_text_turned()'s (x, y) is the FIRST GLYPH's cell, not a corner of
 * the string, and runs away in whichever direction the turn implies -
 * getting the corner/direction wrong by even one turn is an off-by-one
 * invisible until the board is turned, why this is host-tested (see
 * suite_palette.c). Generalises app_sand.c's draw_mode_label() to an
 * arbitrary rect. */
void palette_label_origin(int x, int y, int w, int h, int len, int turn, int* out_x, int* out_y);
