/*=============================================================================
 * sand_priv - internals shared between sand.c and sand_liquid.c.
 *
 * Not a public header: nothing outside this module includes it, and nothing
 * in it is part of sand.h's API. It exists only because splitting the liquid
 * logic into its own file left a few things - marking a row dirty, finding
 * the row a move would land in - needed on both sides of that split.
 *
 * dest_row() and mark_rows() stay `static inline` here rather than becoming
 * ordinary functions defined once and declared extern: both sit on the
 * hottest path in the simulation - called per row, and per move,
 * respectively - and a call across translation units is not guaranteed to
 * inline the way a call within the same file is. A header of small inline
 * functions gives each .c file its own inlinable copy, which is what lets the
 * file split without also risking a performance regression for it - see the
 * frame-budget tests in suite_sand.c, which is exactly what would catch it if
 * this ever stopped being true.
 *===========================================================================*/
#pragma once

#include <stddef.h>

#include "sand.h"

/* The row a move would land in, or NULL if that is off the grid.
 *
 * Worked out once per row rather than once per grain: every grain in a row
 * shares the same three destination rows, so the vertical bounds check is
 * done 224 times per step instead of 41,216 times. */
static inline uint8_t *dest_row(const sand_t *s, int y)
{
    if (y < 0 || y >= s->h) {
        return NULL;
    }
    return s->cells + (size_t)y * (size_t)s->w;
}

/* Something changed across rows y0..y1, so none of them - nor the rows
 * touching them - can still be considered settled under any direction.
 * Clearing the neighbours is what makes a grain notice that its support has
 * moved.
 *
 * Only ROW_NO_LIQUID (sand_liquid.c) lives in row_state now - the settled
 * bits this used to also clear moved to block_state, see
 * wake_blocks_point()/wake_blocks_range() below. Kept exactly as it was: a
 * row's dry-of-liquid bit is still only
 * worth clearing this conservatively, and narrowing it is unrelated to why
 * settling moved to blocks. */
static inline void wake_span(sand_t *s, int y0, int y1)
{
    int lo = (y0 < y1 ? y0 : y1) - 1;
    int hi = (y0 > y1 ? y0 : y1) + 1;

    if (lo < 0) {
        lo = 0;
    }
    if (hi >= s->h) {
        hi = s->h - 1;
    }
    for (int y = lo; y <= hi; y++) {
        s->row_state[y] = 0;
    }
}

/* Marking is a pair because every move touches two rows: the one a grain left
 * and the one it arrived in. Both are guaranteed in range at every call
 * site - a move only happens once its destination row has been found to
 * exist.
 *
 * Row-shaped only: dirty_rows and row_state's ROW_NO_LIQUID bit. Kept
 * alongside mark_move() below for the one call site (equalise_one_row()'s
 * deferred cross-flow wake) that needs the row-shaped bookkeeping without
 * a single pair of points to wake blocks from - see the comment there. */
static inline void mark_rows(sand_t *s, int y0, int y1)
{
    if (s->dirty_rows != NULL) {
        s->dirty_rows[y0] = 1;
        s->dirty_rows[y1] = 1;
    }
    if (s->row_state != NULL) {
        wake_span(s, y0, y1);
    }
}

/* Settled-block bits, in block_state - the finer-grained sibling of
 * ROW_SETTLED_NEAREST/OTHER (which used to live in row_state; see
 * sand_enable_sleeping()'s comment in sand.h for why they moved). Two
 * settled bits for the same reason a row needed two: gravity direction is
 * dithered between two ring directions each step, and a block settled
 * under one may not be settled under the other. BLOCK_ACTIVE is transient,
 * cleared at the start of every sand_step() and finalised into the settled
 * bits at the very end - see compute_settled_bit() and the finalisation
 * pass in sand_step(), both in sand.c. All three fit in one byte
 * (block_state has no other bits to share with, unlike row_state). */
#define BLOCK_SETTLED_NEAREST 0x1
#define BLOCK_SETTLED_OTHER   0x2
#define BLOCK_ACTIVE          0x4

static inline int block_of(const sand_t *s, int x, int y)
{
    return (y / SAND_BLOCK_H) * s->block_cols + (x / SAND_BLOCK_W);
}

/* Something changed at, or between, the two blocks covering (bx0,by0) and
 * (bx1,by1), given as BLOCK indices already (see wake_blocks_point() below
 * for the cell-coordinate, single-point version used on the hot path -
 * this one is for equalise_one_row()'s deferred, already-block-shaped
 * range wake, which is called once per row rather than once per transfer
 * and so does not need to be as cheap). Clears BLOCK_SETTLED_NEAREST|OTHER
 * for every block in the bounding box of the two, expanded by one block in
 * every direction and clipped to the grid (mirroring wake_span()'s
 * y0-1..y1+1 buffer, now on both axes), so a block bordering the one
 * actually touched also notices - the same reason a neighbouring row
 * noticing a move is what lets undermining wake a settled pile above it.
 * Also sets BLOCK_ACTIVE, unexpanded, on exactly the two touched blocks
 * themselves: that bit is how compute_settled_bit() later knows not to put
 * a genuinely-active block back to sleep, and only a block a move actually
 * happened in counts, not its neighbours (which merely became worth
 * re-examining, not proven active). */
static inline void wake_blocks_range(sand_t *s, int bx0, int by0, int bx1,
                                     int by1)
{
    if (s->block_state == NULL) {
        return;
    }

    int lo_x = (bx0 < bx1 ? bx0 : bx1) - 1;
    int hi_x = (bx0 > bx1 ? bx0 : bx1) + 1;
    int lo_y = (by0 < by1 ? by0 : by1) - 1;
    int hi_y = (by0 > by1 ? by0 : by1) + 1;

    if (lo_x < 0) {
        lo_x = 0;
    }
    if (lo_y < 0) {
        lo_y = 0;
    }
    if (hi_x >= s->block_cols) {
        hi_x = s->block_cols - 1;
    }
    if (hi_y >= s->block_rows) {
        hi_y = s->block_rows - 1;
    }

    for (int by = lo_y; by <= hi_y; by++) {
        for (int bx = lo_x; bx <= hi_x; bx++) {
            s->block_state[by * s->block_cols + bx] &=
                (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by0 * s->block_cols + bx0] |= BLOCK_ACTIVE;
    s->block_state[by1 * s->block_cols + bx1] |= BLOCK_ACTIVE;
}

/* Edge-aware neighbour reach for one cell coordinate along one axis: `b` is
 * which block it falls in, `local` its position within that block (0-based),
 * `size` the block's extent on this axis, `count` how many blocks exist on
 * this axis. Returns b-1 if the cell sits on the low edge of its block (and
 * a lower block exists), b+1 if it sits on the high edge (and a higher one
 * exists), else just b - the same reasoning as wake_blocks_point() used to
 * have inline, factored out so wake_blocks_points() below can apply it to
 * both ends of a move in one pass without a second function call. */
static inline int edge_reach(int b, int local, int size, int count, bool low)
{
    if (low) {
        return (local == 0 && b > 0) ? b - 1 : b;
    }
    return (local == size - 1 && b + 1 < count) ? b + 1 : b;
}

/* Something changed at, or between, cells (x0,y0) and (x1,y1) - or a single
 * cell changed in place, called with the same point twice (sand_set,
 * try_spawn_one, sand_erase). Clears BLOCK_SETTLED_NEAREST|OTHER over the
 * bounding box of both points' blocks, edge-aware instead of always
 * touching a blanket 3x3 neighbourhood per point: a neighbour block only
 * genuinely needs re-examining if a touched cell sits ON the shared
 * boundary with it (checked via its position modulo SAND_BLOCK_W/H), since
 * nothing outside that block could otherwise have anywhere new to go. Most
 * moves touch no edge at all, so this is usually just the block(s) the two
 * points are already in. Sets BLOCK_ACTIVE, unexpanded, on exactly the two
 * touched blocks - that bit is how compute_settled_bit() later knows not
 * to put a genuinely-active block back to sleep, and only a block a move
 * actually happened in counts, not its neighbours (which merely became
 * worth re-examining, not proven active).
 *
 * One call doing both points together, not two separate one-point calls:
 * measured to matter - two calls to a smaller single-point version cost
 * more in call overhead than the blanket 3x3 expansion this replaced saved,
 * on the hot path every grain move goes through. */
static inline void wake_blocks_points(sand_t *s, int x0, int y0, int x1,
                                      int y1)
{
    if (s->block_state == NULL) {
        return;
    }

    const int bx0 = x0 / SAND_BLOCK_W, by0 = y0 / SAND_BLOCK_H;
    const int bx1 = x1 / SAND_BLOCK_W, by1 = y1 / SAND_BLOCK_H;
    const int lx0 = x0 - bx0 * SAND_BLOCK_W, ly0 = y0 - by0 * SAND_BLOCK_H;
    const int lx1 = x1 - bx1 * SAND_BLOCK_W, ly1 = y1 - by1 * SAND_BLOCK_H;

    const int lo_x0 = edge_reach(bx0, lx0, SAND_BLOCK_W, s->block_cols, true);
    const int lo_x1 = edge_reach(bx1, lx1, SAND_BLOCK_W, s->block_cols, true);
    const int hi_x0 = edge_reach(bx0, lx0, SAND_BLOCK_W, s->block_cols, false);
    const int hi_x1 = edge_reach(bx1, lx1, SAND_BLOCK_W, s->block_cols, false);
    const int lo_y0 = edge_reach(by0, ly0, SAND_BLOCK_H, s->block_rows, true);
    const int lo_y1 = edge_reach(by1, ly1, SAND_BLOCK_H, s->block_rows, true);
    const int hi_y0 = edge_reach(by0, ly0, SAND_BLOCK_H, s->block_rows, false);
    const int hi_y1 = edge_reach(by1, ly1, SAND_BLOCK_H, s->block_rows, false);

    const int lo_x = lo_x0 < lo_x1 ? lo_x0 : lo_x1;
    const int hi_x = hi_x0 > hi_x1 ? hi_x0 : hi_x1;
    const int lo_y = lo_y0 < lo_y1 ? lo_y0 : lo_y1;
    const int hi_y = hi_y0 > hi_y1 ? hi_y0 : hi_y1;

    for (int wy = lo_y; wy <= hi_y; wy++) {
        for (int wx = lo_x; wx <= hi_x; wx++) {
            s->block_state[wy * s->block_cols + wx] &=
                (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by0 * s->block_cols + bx0] |= BLOCK_ACTIVE;
    s->block_state[by1 * s->block_cols + bx1] |= BLOCK_ACTIVE;
}

/* The row-shaped bookkeeping mark_rows() always did, plus the block-shaped
 * wake above. Every move-reporting call site in sand.c and sand_liquid.c
 * uses this now, except equalise_one_row()'s deferred, batched cross-flow
 * wake, which has a whole row's worth of touched x values rather than one
 * pair of points - see the comment there, and mark_rows() above. */
static inline void mark_move(sand_t *s, int x0, int y0, int x1, int y1)
{
    mark_rows(s, y0, y1);
    wake_blocks_points(s, x0, y0, x1, y1);
}

/* Defined in sand_liquid.c, called from sand.c's per-cell sweep. The one
 * piece of liquid movement that has to live inside that sweep rather than in
 * its own pass: it is gravity-ward, so it shares that sweep's own guarantee
 * against double-moving a cell, the same way a grain's fall does. See
 * sand_liquid.c for down-then-slope and why cross-flow cannot join it there.
 * Returns whether it moved anything. */
bool move_liquid_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                       int x, int y, int dx, int dy,
                       const int *slide_a, const int *slide_b,
                       cell_t grain, uint8_t mat_id);

/* Defined in sand_liquid.c: the whole of a step's liquid work that does NOT
 * belong inside the main sweep - cross-flow levelling and the wall-rebound
 * splash. Called once from sand_step(), after that sweep finishes.
 * `perp_a`/`perp_b` are the two directions across the flow; `dx`/`dy` is
 * gravity's own dithered direction this step. */
void sand_step_liquids(sand_t *s, const int *perp_a, const int *perp_b,
                       int dx, int dy);
