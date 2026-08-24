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
 * see docs/Sand/Simulation-Lessons.md for the two failed attempts at
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

/* Defined in sand_gas.c: a gas grain's whole step - rising (reusing
 * try_fall_or_scatter()/try_slide() below, direction-inverted) then
 * perpendicular spread (mirroring sand_step_liquids()'s cross-flow, but
 * whole-grain instead of mass-based). Called once from sand_step(), after
 * sand_step_liquids() and before finalize_settling() - same slot, same
 * reason: BLOCK_ACTIVE has to reflect the whole step. Every argument here
 * is something sand_step() already computed for the main sweep/liquid
 * pass; this negates whatever needs negating internally rather than
 * recomputing from scratch. */
void sand_step_gas(sand_t *s, int gx, int gy, int dx, int dy,
                   const int *slide_a, const int *slide_b,
                   const int *perp_a, const int *perp_b,
                   int load_dx, int load_dy, int x_step, int jostle);

/* The whole grain-movement primitive stack - try_fall_or_scatter() and
 * try_slide(), and everything they call - moved here from sand.c, still
 * `static inline`, for the same reason dest_row()/mark_rows() above are:
 * both sit on the hottest path there is, called once per grain per step
 * for every powder cell.
 *
 * Getting this right took three attempts, each one measured on device,
 * not assumed - see docs/Sand/Simulation-Lessons.md for the full numbers:
 *
 * 1. Just remove `static` from try_fall_or_scatter()/try_slide() so
 *    sand_gas.c could call them, leaving everything else static in
 *    sand.c. Regressed the flip/water frame-budget tests by ~26%,
 *    exactly reproducible, even though neither test ever places a gas
 *    cell: turning a `static` function called once per grain into an
 *    ordinary extern one is enough, on its own, to stop the compiler
 *    inlining it into step_one_grain()'s dispatch, which that call site
 *    had been relying on.
 * 2. Move the WHOLE chain here as `static inline`, so both sand.c and
 *    sand_gas.c get their own independently inlinable copy - exactly the
 *    pattern this header already uses for dest_row()/mark_rows(). Fixed
 *    flip/water back to baseline, but grew sand_step_gas() to ~3.9 KB
 *    (bigger than sand_step() itself, from carrying a full second copy
 *    of this chain) and THAT regressed the worst-case, sleeping-off
 *    test_a_full_size_step_fits_in_the_frame_budget from ~7000us to over
 *    15000us - exactly reproducible too, and again without that test
 *    ever placing a gas cell. Flash footprint, not runtime gas activity,
 *    was the cost both times.
 * 3. What actually shipped: try_fall_or_scatter_impl()/try_slide_impl()
 *    stay `static inline` here, but only step_one_grain() in sand.c
 *    calls them directly - keeping the main sweep's hot path fully
 *    inlined, exactly as it always was. sand_gas.c instead calls the
 *    ordinary, non-inline try_fall_or_scatter()/try_slide() defined once
 *    in sand.c (declared below) - genuine functions that each wrap one
 *    of the _impl versions exactly once, so the shared logic exists in
 *    flash as at most two copies (the inlined one in sand_step(), and
 *    the one real out-of-line copy sand_gas.c calls into) rather than a
 *    third, duplicated one growing inside sand_step_gas() itself. Fixed
 *    both regressions at once. */

/* Whether `mover` may occupy the cell currently holding `target`.
 *
 * Empty space always yields. Anything else yields only to something denser,
 * which is how sand sinks through water while water cannot push its way back
 * up through sand. Static materials never yield whatever the arithmetic says,
 * so a wall stays a wall. */
static inline bool can_enter(uint8_t mover_density, cell_t target)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }

    const material_t *t = material_of(target);

    return t->kind != KIND_STATIC && mover_density > t->density;
}

/* Whether a cell exists and can be entered, without moving anything into it.
 *
 * Needed so scatter can be decided ONLY for cells that could actually fall.
 * Drawing a random number for every cell regardless would undo the single
 * biggest saving in this loop - see the note on the common path below. */
static inline bool cell_open(const uint8_t *row, int nx, int w, uint8_t density)
{
    return row != NULL && (unsigned)nx < (unsigned)w &&
           can_enter(density, row[nx]);
}

/* Move a grain into `to_row` at column `nx`, if that cell exists and is free.
 *
 * A NULL row or an out-of-range column is a wall, so both simply fail. The
 * single unsigned comparison catches nx < 0 as well, by wrapping it to a huge
 * value - the usual trick, worth it in a loop this hot. */
static inline bool move_to(uint8_t *from_row, uint8_t *to_row,
                           int x, int nx, int w, cell_t mover, uint8_t density)
{
    if (to_row == NULL || (unsigned)nx >= (unsigned)w ||
        !can_enter(density, to_row[nx])) {
        return false;
    }

    /* A SWAP, not an overwrite. Where the target was empty this is exactly the
     * old behaviour; where it held something lighter, that lighter thing takes
     * the vacated cell and is displaced upward.
     *
     * Safe with respect to sweep order: the displaced cell lands in the very
     * cell being processed, which the sweep has just finished with, so it
     * cannot move a second time this step. */
    const cell_t displaced = to_row[nx];

    to_row[nx]  = mover;
    from_row[x] = displaced;
    return true;
}

/* Chance in 256 that a loaded grain may still slide sideways.
 *
 * A surface grain is free. Each grain above halves the chance, and past the cap
 * it is nil. Shaking overrides the lot - a shaken pile flows regardless of how
 * deeply buried its grains are, which is the whole reason shaking a jar of
 * sand levels it. */
static inline int slide_chance(const material_t *m, int load, int jostle)
{
    /* A material whose slip is 255 is never held by load at all. That is most
     * of what separates a liquid from a powder: water at the bottom of a deep
     * pool carries just as much weight as sand at the bottom of a dune, and
     * flows anyway. */
    if (load == 0 || m->slip >= 255) {
        return 256;
    }

    const int chance = (load >= SAND_LOAD_CAP) ? 0 : (m->slip >> (load - 1));

    return chance > jostle ? chance : jostle;
}

/* The common case by a wide margin - on a screen of falling sand almost every
 * grain simply moves the way gravity points. Taking it before drawing a
 * random number matters: the generator was the single most expensive thing
 * in this loop, and most grains never needed it.
 *
 * Only called when jostle == 0: scatter only applies to a grain that could
 * fall, and a shaken grain has to reach the slide logic in try_slide()
 * regardless of whether the plain fall is open.
 *
 * Whether a scattering grain drifted, lagged, or the scatter roll simply did
 * not apply - in every one of those cases the grain is spoken for, so the
 * caller must not also attempt a plain fall on it. */
static inline bool try_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                               uint8_t *arow, uint8_t *brow, int x, int y,
                               int w, int dx, const int *slide_a,
                               const int *slide_b, cell_t grain,
                               uint8_t density, int scatter)
{
    if (scatter == 0 || !cell_open(prow, x + dx, w, density)) {
        return false;
    }

    const uint32_t r = rng_next(&s->rng);
    if ((int)(r & 0xFF) >= scatter) {
        return false;
    }

    /* Drift: sideways as well as down, if that way is open. Spreads the
     * stream horizontally. Blocked or not chosen, it lags instead: nothing
     * at all this step, so the grains around it pull ahead and the stream
     * spreads vertically.
     *
     * Either way still counts as activity. A grain that CHOSE not to move is
     * not a settled grain, and letting the row sleep here would strand it in
     * mid-air. */
    if ((r & 0x100) == 0) {
        const bool pick_a = (r & 0x200) != 0;
        uint8_t  *drow = pick_a ? arow : brow;
        const int ddx  = pick_a ? slide_a[0] : slide_b[0];
        const int ddy  = pick_a ? slide_a[1] : slide_b[1];

        if (move_to(row, drow, x, x + ddx, w, grain, density)) {
            mark_rows(s, y, y + ddy);
        }
    }
    return true;
}

/* Named _impl, not called directly outside this header: step_one_grain()
 * in sand.c calls this inline version straight, so the main sweep's own
 * call site stays fully inlined. sand_gas.c instead calls the real,
 * ordinary try_fall_or_scatter()/try_slide() defined in sand.c (declared
 * further down) - a single genuine function, wrapping this same inline
 * body ONCE, rather than sand_gas.c inlining a second full copy of it.
 * Measured why this split exists: giving sand_step_gas() its own fully
 * inlined copy of the whole chain (the first version of this fix) grew it
 * to ~3.9 KB - bigger than sand_step() itself - and that alone was enough
 * to regress test_a_full_size_step_fits_in_the_frame_budget from ~7000us
 * to over 15000us, reproduced exactly across captures, even though that
 * test never places a single gas cell. One ordinary function call from
 * sand_gas.c costs far less than carrying a second copy of this code in
 * flash at all. See docs/Sand/Simulation-Lessons.md for the full story. */
static inline bool try_fall_or_scatter_impl(sand_t *s, uint8_t *row,
                                            uint8_t *prow, uint8_t *arow,
                                            uint8_t *brow, int x, int y,
                                            int w, int dx, int dy,
                                            const int *slide_a,
                                            const int *slide_b, cell_t grain,
                                            uint8_t density, int scatter)
{
    if (try_scatter(s, row, prow, arow, brow, x, y, w, dx, slide_a, slide_b,
                    grain, density, scatter)) {
        return true;
    }

    if (move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }
    return false;
}

/* Which of the two slides to try first, and which second - without
 * randomising it the sand develops a visible grain with everything leaning
 * the same way. */
static inline void pick_slide_order(uint32_t r, uint8_t *arow, uint8_t *brow,
                                    const int *slide_a, const int *slide_b,
                                    uint8_t mat_id, bool driven[MATERIAL_MAX][2],
                                    uint8_t **first_row, int *first_dx,
                                    int *first_dy, bool *first_driven,
                                    uint8_t **second_row, int *second_dx,
                                    int *second_dy, bool *second_driven)
{
    if (r & 1) {
        *first_row  = arow; *first_dx  = slide_a[0]; *first_dy  = slide_a[1];
        *first_driven = driven[mat_id][0];
        *second_row = brow; *second_dx = slide_b[0]; *second_dy = slide_b[1];
        *second_driven = driven[mat_id][1];
    } else {
        *first_row  = brow; *first_dx  = slide_b[0]; *first_dy  = slide_b[1];
        *first_driven = driven[mat_id][1];
        *second_row = arow; *second_dx = slide_a[0]; *second_dy = slide_a[1];
        *second_driven = driven[mat_id][0];
    }
}

/* Friction, and only on the slides. The grain could not fall, so whether it
 * may SHUFFLE depends on what is sitting on it. Reached only once the
 * gravity-ward move has already failed, so a grain in open air never pays
 * for this. */
static inline bool try_slide_pair(sand_t *s, uint8_t *row, int x, int y, int w,
                                  cell_t grain, uint8_t density,
                                  const material_t *mat, int load_dx,
                                  int load_dy, int jostle, uint32_t r,
                                  uint8_t *first_row, int first_dx,
                                  int first_dy, bool first_driven,
                                  uint8_t *second_row, int second_dx,
                                  int second_dy, bool second_driven)
{
    const int load = sand_load_above(s, x, y, load_dx, load_dy);
    const int allowance = slide_chance(mat, load, jostle);
    if (allowance < 256 && (int)((r >> 16) & 0xFF) >= allowance) {
        return false;
    }

    if (first_driven &&
        move_to(row, first_row, x, x + first_dx, w, grain, density)) {
        mark_rows(s, y, y + first_dy);
        return true;
    }
    if (second_driven &&
        move_to(row, second_row, x, x + second_dx, w, grain, density)) {
        mark_rows(s, y, y + second_dy);
        return true;
    }
    return false;
}

/* Blocked, or being shaken. _impl for the same reason
 * try_fall_or_scatter_impl() is - see its own comment above. */
static inline bool try_slide_impl(sand_t *s, uint8_t *row, uint8_t *prow,
                                  uint8_t *arow, uint8_t *brow, int x, int y,
                                  int w, int dx, int dy, const int *slide_a,
                                  const int *slide_b, int load_dx,
                                  int load_dy, int jostle, cell_t grain,
                                  uint8_t mat_id, uint8_t density,
                                  const material_t *mat,
                                  bool driven[MATERIAL_MAX][2])
{
    const uint32_t r = rng_next(&s->rng);

    uint8_t *first_row,  *second_row;
    int      first_dx,    second_dx;
    int      first_dy,    second_dy;
    bool     first_driven, second_driven;
    pick_slide_order(r, arow, brow, slide_a, slide_b, mat_id, driven,
                     &first_row, &first_dx, &first_dy, &first_driven,
                     &second_row, &second_dx, &second_dy, &second_driven);

    /* Shaking reorders the attempts rather than adding a new move: a shaken
     * grain prefers to spread sideways before it drops. Every destination
     * stays inside the already-swept half, so the no-double-move guarantee
     * above still holds. */
    const bool shaken = jostle > 0 && (int)((r >> 8) & 0xFF) < jostle;

    if (!shaken && jostle > 0 &&
        move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    if (try_slide_pair(s, row, x, y, w, grain, density, mat, load_dx,
                       load_dy, jostle, r, first_row, first_dx, first_dy,
                       first_driven, second_row, second_dx, second_dy,
                       second_driven)) {
        return true;
    }

    if (shaken && move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    return false;
}

/* Ordinary, non-inline functions, defined once in sand.c, each wrapping
 * one of the _impl versions above exactly once - what sand_gas.c calls
 * instead of the inline versions directly. See this header's own top
 * comment (above try_fall_or_scatter_impl()) for why: giving
 * sand_step_gas() its own fully inlined copy of this chain measured a
 * real, exactly-reproducible regression on a worst-case frame-budget
 * test that never even places a gas cell - flash footprint, not runtime
 * gas activity, was the cost. One ordinary function call from sand_gas.c
 * is far cheaper than that second copy. */
bool try_fall_or_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                         uint8_t *arow, uint8_t *brow, int x, int y,
                         int w, int dx, int dy, const int *slide_a,
                         const int *slide_b, cell_t grain,
                         uint8_t density, int scatter);

bool try_slide(sand_t *s, uint8_t *row, uint8_t *prow, uint8_t *arow,
               uint8_t *brow, int x, int y, int w, int dx, int dy,
               const int *slide_a, const int *slide_b, int load_dx,
               int load_dy, int jostle, cell_t grain, uint8_t mat_id,
               uint8_t density, const material_t *mat,
               bool driven[MATERIAL_MAX][2]);

/* Whether a grain may slide in direction (mx, my) at all, given gravity
 * (gx, gy) and its material's angle of repose. Moved here from sand.c
 * (still `static inline`, so it stays free to call inside the main
 * sweep's own hot loop) because sand_gas.c needs it too, to build its own
 * driven[][] table against the REVERSED gravity vector - reusing the
 * main sweep's own driven[][] (built against the forward vector) would
 * make every gas slide's descent dot-product come out negative, since
 * gas's slide vectors point away from real gravity by construction. See
 * sand_gas.c's own comment for the full reasoning. */
static inline bool driven_by_gravity(int mx, int my, int gx, int gy,
                                     int repose)
{
    const int descent = mx * gx + my * gy;
    if (descent <= 0) {
        return false;              /* uphill, or across a level slope */
    }
    if (repose == 0) {
        return true;               /* no friction angle at all - a liquid */
    }

    int lateral = mx * gy - my * gx;
    if (lateral < 0) {
        lateral = -lateral;
    }

    /* `repose` is mu times ten, so 7 is the ~35 degrees of dry sand. Kept as
     * a ratio so this stays in integers. */
    return (int64_t)descent * 10 > (int64_t)lateral * repose;
}
