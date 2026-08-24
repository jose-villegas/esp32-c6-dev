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

__attribute__((always_inline))
static inline bool cell_occupied(const sand_t *s, int x, int y)
{
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
        return false;
    }
    return s->cells[(size_t)y * (size_t)s->w + (size_t)x] != SAND_EMPTY;
}

/* Whether reaching into block (nbx,nby) - to clear its settled bits -
 * is worth the read needed to decide, given the one cell (cx,cy) that
 * would justify it if occupied (see point_reach()'s own comment for why
 * a single cell is enough).
 *
 * Checks the CANDIDATE'S OWN state first: if it is already awake (its
 * settled bits already 0), reaching it costs nothing extra regardless -
 * the caller's settled-bit clear is already a no-op - so there is
 * nothing to gain by reading grid cells to decide, and this returns true
 * immediately without doing so. Only a genuinely SETTLED neighbour needs
 * the occupancy read, since that is the only case where waking it
 * unnecessarily would cost anything.
 *
 * This is what makes the busiest moment in the simulation cheap: right
 * after a jostle or gravity flip, compute_settled_bit() resets every
 * block's settled bits to 0 in one memset, so for the first several
 * steps afterwards - exactly the chaotic, everything-scattering phase a
 * pour-then-flip produces - almost every neighbour this function is
 * asked about is already awake, and the occupancy read underneath never
 * runs at all. */
__attribute__((always_inline))
static inline bool should_wake_neighbor(const sand_t *s, int nbx, int nby,
                                        int cx, int cy)
{
    if ((s->block_state[nby * s->block_cols + nbx] &
        (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) == 0) {
        return true;
    }
    return cell_occupied(s, cx, cy);
}

/* One axis' worth of point_reach()'s edge check - split out purely to
 * keep that function's own complexity down, not because this is reused
 * beyond its two call sites there (once for x, once for y). Forced
 * inline: this and its neighbours sit on the hottest path in the
 * simulation (every grain move), and a real call here - rather than the
 * compiler folding it into point_reach() - measured as a real cost on
 * top of the reads themselves. */
__attribute__((always_inline))
static inline void reach_axis(const sand_t *s, int nb_bx_lo, int nb_by_lo,
                              int nb_bx_hi, int nb_by_hi, int lo_cx,
                              int lo_cy, int hi_cx, int hi_cy, int b,
                              bool at_lo, bool at_hi, int *lo, int *hi)
{
    if (at_lo && should_wake_neighbor(s, nb_bx_lo, nb_by_lo, lo_cx, lo_cy)) {
        *lo = b - 1;
    }
    if (at_hi && should_wake_neighbor(s, nb_bx_hi, nb_by_hi, hi_cx, hi_cy)) {
        *hi = b + 1;
    }
}

/* The true-corner case of point_reach()'s edge check, split out for the
 * same reason as reach_axis() above. Forced inline, same reason too.
 *
 * A true corner - (x,y) on both axes' edges at once - is checked as one
 * unit against all three of the diagonal neighbour's justifying cells
 * (both orthogonal ones and the diagonal itself), not as two independent
 * per-axis checks: a grain can rest diagonally against a corner with
 * both orthogonal neighbours empty, so ANDing two independent axis
 * checks would miss exactly that case - see
 * test_a_block_wakes_when_disturbed_diagonally, which exists to catch
 * it. */
__attribute__((always_inline))
static inline void reach_corner(const sand_t *s, int x, int y, int bx,
                                int by, bool at_lo_x, bool at_lo_y,
                                int *lo_x, int *hi_x, int *lo_y, int *hi_y)
{
    const int nbx = at_lo_x ? bx - 1 : bx + 1;
    const int nby = at_lo_y ? by - 1 : by + 1;
    const int nx  = at_lo_x ? x - 1  : x + 1;
    const int ny  = at_lo_y ? y - 1  : y + 1;

    bool wake;
    if ((s->block_state[nby * s->block_cols + nbx] &
        (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) == 0) {
        wake = true;
    } else {
        wake = cell_occupied(s, nx, y) || cell_occupied(s, x, ny) ||
               cell_occupied(s, nx, ny);
    }
    if (!wake) {
        return;
    }
    if (at_lo_x) { *lo_x = bx - 1; } else { *hi_x = bx + 1; }
    if (at_lo_y) { *lo_y = by - 1; } else { *hi_y = by + 1; }
}

/* Which blocks touched cell (x,y) - in block (bx,by), local position
 * (lx,ly) within it - requires waking: its own block always (written into
 * the four out-parameters as [bx,bx] by [by,by] to start), widened to
 * include an edge-adjacent neighbour only if reaching it is worth doing -
 * see should_wake_neighbor() and reach_corner() above for what that
 * means.
 *
 * Forced inline, and load-bearing this time - see docs/Notes/Simulation-
 * Lessons.md's note on this. Plain `static inline` was NOT enough:
 * objdump showed wake_blocks_points() (this function's only caller) with
 * its own out-of-line `.text.wake_blocks_points` section, two real `jalr`
 * calls into a separately-compiled point_reach(), and a 96-byte stack
 * frame saving ten callee-saved registers - all of bx0/by0/bx1/by1/lx0/
 * ly0/lx1/ly1 forced to survive across the call boundary. Forcing this
 * inline collapsed that to zero calls and a 32-byte, two-register frame,
 * measured to matter directly: ~1.9ms off a 15.7ms worst case. */
__attribute__((always_inline))
static inline void point_reach(const sand_t *s, int x, int y, int bx, int by,
                               int lx, int ly, int *lo_x, int *hi_x,
                               int *lo_y, int *hi_y)
{
    *lo_x = bx; *hi_x = bx; *lo_y = by; *hi_y = by;

    const bool at_lo_x = lx == 0 && bx > 0;
    const bool at_hi_x = lx == SAND_BLOCK_W - 1 && bx + 1 < s->block_cols;
    const bool at_lo_y = ly == 0 && by > 0;
    const bool at_hi_y = ly == SAND_BLOCK_H - 1 && by + 1 < s->block_rows;
    const bool on_x_edge = at_lo_x || at_hi_x;
    const bool on_y_edge = at_lo_y || at_hi_y;

    if (on_x_edge && on_y_edge) {
        reach_corner(s, x, y, bx, by, at_lo_x, at_lo_y, lo_x, hi_x, lo_y, hi_y);
        return;
    }
    if (on_x_edge) {
        reach_axis(s, bx - 1, by, bx + 1, by, x - 1, y, x + 1, y, bx,
                  at_lo_x, at_hi_x, lo_x, hi_x);
    }
    if (on_y_edge) {
        reach_axis(s, bx, by - 1, bx, by + 1, x, y - 1, x, y + 1, by,
                  at_lo_y, at_hi_y, lo_y, hi_y);
    }
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
 * on the hot path every grain move goes through.
 *
 * Forced inline for the same reason as point_reach() above - this is
 * mark_move()'s only call to it, and mark_move() itself is inlined at
 * every one of its own ~10 call sites in sand.c/sand_liquid.c, so leaving
 * this one boundary real would have meant paying a call, from deep
 * inside those already-inlined sites, on every grain move.
 *
 * Do NOT chase this same fix one level further by also forcing
 * mark_move() inline - tried, and measured to make everything worse, not
 * better: full-occupancy went from passing to 10568us (over an 8000us
 * budget), and the flip test got slower too. Once wake_blocks_points()
 * folds in here, this function is already large; inlining it again at
 * every one of mark_move()'s ~10 call sites bloats the already-large
 * inlined sand_step() past whatever fits in the chip's 32 KiB instruction
 * cache, and the resulting cache misses cost more than the saved calls
 * were worth - including on code paths, like full-occupancy, that barely
 * touch this function. There is a real ceiling here, found by measuring
 * past it, not by reasoning about it - see docs/Notes/Simulation-
 * Lessons.md. */
__attribute__((always_inline))
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

    /* The common case, by far: an interior move that stays inside one
     * ALREADY-AWAKE block (most grain moves do - a fall or slide only
     * ever crosses one cell). Needs nothing at all: step_one_block()
     * already sets BLOCK_ACTIVE on its own block directly from
     * moved_here, independent of this call, and finalisation only ever
     * reads that bit - not the settled bits this function would
     * otherwise clear, which would be this same block's own, and so
     * already redundant with that direct write. The only thing that
     * could still matter is a NEIGHBOUR noticing, and that is only
     * possible if one of the two points sits ON this block's shared
     * edge.
     *
     * The already-awake check is not optional: a caller outside the
     * sweep entirely - sand_set()/sand_erase()/try_spawn_one() disturbing
     * a block that has been asleep for a while - has no other path that
     * would clear its settled bits, and skipping unconditionally there
     * left it asleep forever (caught by
     * test_sideways_tilt_wakes_only_the_disturbed_column erasing a
     * settled block's interior cell and expecting it to wake). If the
     * settled bits are already both 0, nothing needs clearing regardless
     * of who is calling or when - that is what makes the check safe. */
    if (bx0 == bx1 && by0 == by1 &&
        lx0 != 0 && lx0 != SAND_BLOCK_W - 1 &&
        ly0 != 0 && ly0 != SAND_BLOCK_H - 1 &&
        lx1 != 0 && lx1 != SAND_BLOCK_W - 1 &&
        ly1 != 0 && ly1 != SAND_BLOCK_H - 1 &&
        (s->block_state[by0 * s->block_cols + bx0] &
         (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) == 0) {
        return;
    }

    int lo_x0, hi_x0, lo_y0, hi_y0, lo_x1, hi_x1, lo_y1, hi_y1;
    point_reach(s, x0, y0, bx0, by0, lx0, ly0, &lo_x0, &hi_x0, &lo_y0, &hi_y0);
    point_reach(s, x1, y1, bx1, by1, lx1, ly1, &lo_x1, &hi_x1, &lo_y1, &hi_y1);

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
