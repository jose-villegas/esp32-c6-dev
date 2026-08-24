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
 * any_neighbor_active()/wake_blocks_range() below. Kept exactly as it was: a
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
 * pass in sand_step(), both in sand.c.
 *
 * BLOCK_STAGGER_HOLD: a block whose settled bits are already 0 (known to
 * need re-examining) but is deliberately not being walked yet - waiting
 * for its block-row's turn in the staggered release that follows a mass
 * wake (a jostle or gravity-direction change, which resets every block
 * at once). Spreads the single most expensive step after a big
 * disturbance across a few steps instead of paying it all at once - see
 * compute_settled_bit()'s own comment. All four bits fit in one byte
 * (block_state has no other bits to share with, unlike row_state).
 *
 * MEASURED TRADE-OFF, confirmed by direct interactive testing (not just
 * the synthetic device tests): flip's worst single step genuinely beats
 * the pre-staggering average, a real improvement. Water's worst step
 * does not improve, and a fully settled/idle screen gets measurably
 * worse (re-examining a whole released block-row of packed, maximally-
 * buried sand is expensive even when nothing moves) - net negative
 * there, with nothing to offset it. Kept on its own branch rather than
 * merged, for exactly that reason - not a blanket win. See docs/Notes/
 * Simulation-Lessons.md's "The fifth attempt" section for the full
 * numbers and the (unconfirmed) live-accelerometer risk this was
 * originally suspected of, before direct device testing did not
 * reproduce it. */
#define BLOCK_SETTLED_NEAREST 0x1
#define BLOCK_SETTLED_OTHER   0x2
#define BLOCK_ACTIVE          0x4
#define BLOCK_STAGGER_HOLD    0x8

static inline int block_of(const sand_t *s, int x, int y)
{
    return (y / SAND_BLOCK_H) * s->block_cols + (x / SAND_BLOCK_W);
}

/* Something changed at, or between, the two blocks covering (bx0,by0) and
 * (bx1,by1), given as BLOCK indices already. Used only by
 * equalise_one_row()'s deferred, already-block-shaped range wake, which
 * is called once per row rather than once per transfer. Clears
 * BLOCK_SETTLED_NEAREST|OTHER
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

/* Whether any of block (bx,by)'s up to 8 neighbours has BLOCK_ACTIVE set
 * this step - the pull-based replacement for the old per-move push
 * mechanism (wake_blocks_points()/point_reach()/reach_axis()/
 * reach_corner()/should_wake_neighbor()/cell_occupied(), all removed;
 * see docs/Notes/Simulation-Lessons.md for the two failed attempts at
 * cheapening that mechanism that led here instead).
 *
 * Called once per block per step, from sand_step()'s own finalisation
 * pass (sand.c) - not once per grain move, so unlike everything it
 * replaces, it can afford to check unconditionally rather than needing
 * edge-position/occupancy gating to stay cheap: the cost is already
 * bounded to O(block_count) regardless of how many grains moved inside
 * any of them. Not forced inline - it has exactly one caller, a loop
 * that already isn't inlined into anything hotter. */
static inline bool any_neighbor_active(const sand_t *s, int bx, int by)
{
    const int lo_x = (bx > 0) ? bx - 1 : bx;
    const int hi_x = (bx + 1 < s->block_cols) ? bx + 1 : bx;
    const int lo_y = (by > 0) ? by - 1 : by;
    const int hi_y = (by + 1 < s->block_rows) ? by + 1 : by;

    for (int ny = lo_y; ny <= hi_y; ny++) {
        for (int nx = lo_x; nx <= hi_x; nx++) {
            if (nx == bx && ny == by) {
                continue;
            }
            if (s->block_state[ny * s->block_cols + nx] & BLOCK_ACTIVE) {
                return true;
            }
        }
    }
    return false;
}

/* Marks block (bx,by) - the one containing (x,y) - and its up to 8
 * neighbours unsettled, unconditionally. Used only by touches that
 * happen OUTSIDE the gravity sweep (sand_set(), sand_erase(),
 * try_spawn_one(), and liquid's cross-flow/rebound passes in
 * sand_liquid.c), where there is no `moved_here`-style bookkeeping for
 * the pull-based any_neighbor_active() check above to observe on its
 * own next step.
 *
 * Sweep-internal moves need no equivalent: step_one_block() already
 * sets BLOCK_ACTIVE on its own (source) block directly from
 * `moved_here`, independent of any wake call, and a grain only ever
 * moves one cell - so a destination block, if different from the
 * source, is always that source block's immediate neighbour, which
 * any_neighbor_active() will find active on its own the moment the
 * finalisation pass runs. An external touch has no such source block
 * whose own activity a neighbour could observe, which is why it needs
 * to expand to neighbours itself, right here, instead.
 *
 * Unconditional 3x3 expansion, not edge-aware the way point_reach() had
 * to be: these calls are user-interaction/cross-flow rate, not once per
 * grain move, so the precision that mechanism needed to stay cheap on
 * the sweep's hot path is not needed here - see
 * test_undermining_a_sleeping_pile_collapses_it, which is what would
 * catch this being narrowed later: erasing a grain must wake whatever
 * was resting on it in a NEIGHBOURING block, not just the block the
 * erased cell itself was in, and there is no sweep-internal activity of
 * its own to fall back on for a block that never gets examined at all. */
static inline void wake_block_and_neighbors(sand_t *s, int x, int y)
{
    if (s->block_state == NULL) {
        return;
    }

    /* Unsigned cast for the same reason as elsewhere in this file (see
     * docs/Notes/Optimization-Playbook.md's division lesson) - x/y are
     * always non-negative grid coordinates, the compiler just cannot
     * prove it from a plain int parameter. Low-frequency call, so this
     * matters far less here than it did on the sweep's old hot path,
     * but there is no reason to leave it slower than free. */
    const int bx = (int)((unsigned)x / SAND_BLOCK_W);
    const int by = (int)((unsigned)y / SAND_BLOCK_H);

    const int lo_x = (bx > 0) ? bx - 1 : bx;
    const int hi_x = (bx + 1 < s->block_cols) ? bx + 1 : bx;
    const int lo_y = (by > 0) ? by - 1 : by;
    const int hi_y = (by + 1 < s->block_rows) ? by + 1 : by;

    for (int ny = lo_y; ny <= hi_y; ny++) {
        for (int nx = lo_x; nx <= hi_x; nx++) {
            s->block_state[ny * s->block_cols + nx] &=
                (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by * s->block_cols + bx] |= BLOCK_ACTIVE;
}

/* The row-shaped bookkeeping mark_rows() always did, plus waking the
 * touched blocks - see wake_block_and_neighbors() above for why this is
 * only used outside the sweep (sand_set()/sand_erase()/try_spawn_one(),
 * and liquid's equalise_one_cell()/rebound_one_cell()). The sweep's own
 * move-reporting call sites (sand.c's try_scatter()/try_fall_or_scatter()/
 * try_slide()/try_slide_pair(), and move_liquid_grain() in
 * sand_liquid.c) call mark_rows() directly instead - they need no block
 * wake of their own at all, since step_one_block()'s existing
 * `moved_here` bookkeeping already marks the source block active, and
 * any_neighbor_active() picks that up for its neighbours (including any
 * different block a move actually landed in, always one of them) on its
 * own the moment sand_step()'s finalisation pass runs. */
static inline void mark_move(sand_t *s, int x0, int y0, int x1, int y1)
{
    mark_rows(s, y0, y1);
    wake_block_and_neighbors(s, x0, y0);
    wake_block_and_neighbors(s, x1, y1);
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
