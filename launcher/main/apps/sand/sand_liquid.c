/*=============================================================================
 * sand_liquid - everything about a liquid that is not the powder sweep.
 *
 * sand_step(), in sand.c, moves every grain the same way whatever it is made
 * of: try to fall, then try the two slides. A liquid needs that exact
 * treatment too - it is still gravity-ward, still bound by the sweep's
 * no-double-move guarantee - so move_liquid_grain() is called FROM inside
 * that sweep rather than living here as a separate pass.
 *
 * Everything else in a liquid's behaviour is NOT gravity-ward, and so cannot
 * safely live in that sweep at all - see the comment above equalise_liquids()
 * for why. sand_step_liquids() is the one thing sand_step() calls after its
 * sweep finishes: cross-flow levelling, and the wall-rebound splash on top of
 * it.
 *===========================================================================*/

#include "sand_priv.h"

/* liquid_mask() - which materials are liquid, as a bitmask over the nibble -
 * moved to sand_priv.h (still static inline) now that sand.c's own sweep needs
 * it too, to maintain BLOCK_HAS_LIQUID. See its comment there. */

/* Add `amount` of `id` to a cell that is either empty or already that same
 * material. Mass is only ever moved, never made: every caller subtracts the
 * same figure from somewhere else in the same breath. */
static inline void pour_into(cell_t *dst, uint8_t id, int amount)
{
    const int had = CELL_IS_EMPTY(*dst) ? 0 : CELL_VARIANT(*dst);

    *dst = CELL_MAKE(id, had + amount);
}

/* How much of `id` a cell will accept, 0 if it holds something else. */
static inline int room_in(cell_t c, uint8_t id)
{
    if (CELL_IS_EMPTY(c)) {
        return MASS_MAX;
    }
    if (CELL_MATERIAL(c) != id) {
        return 0;
    }
    return MASS_MAX - CELL_VARIANT(c);
}

/*---------------------------------------------------------------------------
 * The one piece of liquid movement inside the main sweep.
 *-------------------------------------------------------------------------*/

/* Give up to `mass` of `mat_id` to the cell at column `tx` of `to_row`, if it
 * exists and has room. Returns how much it actually took.
 *
 * Row-shaped bookkeeping only (mark_rows()) - no block wake at all, per
 * transfer or otherwise. move_liquid_grain() below can call this up to
 * three times for one grain (down, then both slides), and doing a
 * per-transfer block-wake here was most of a measured ~10ms/step
 * regression on a screen-wide water collapse back when this called
 * wake_blocks_points(); that call is gone entirely now (see
 * move_liquid_grain()'s own comment for why nothing replaced it), so the
 * old worry does not even apply any more - mark_rows() is now two byte
 * writes into dirty_rows and nothing else, so calling it up to three times
 * per grain costs almost nothing. It did NOT always cost almost nothing:
 * until the ninth attempt it also wiped three bytes of row_state per call
 * to invalidate a ROW_NO_LIQUID cache, and this one call site was 99.9% of
 * that traffic on a screen of water - 11,130 of the 11,142 calls a step
 * made. Removing the cache is what made this cheap. */
static inline int give_mass(sand_t *s, uint8_t *to_row, int tx, int w,
                            int mass, uint8_t mat_id, int y, int ty)
{
    if (to_row == NULL || (unsigned)tx >= (unsigned)w) {
        return 0;
    }
    const int room = room_in(to_row[tx], mat_id);
    const int give = mass < room ? mass : room;
    if (give > 0) {
        pour_into(&to_row[tx], mat_id, give);
        mark_rows(s, y, ty);
    }
    return give;
}

/* Liquids: move an AMOUNT between touching cells.
 *
 * Nothing here looks further than one cell in any direction, and that is the
 * whole point. A cell that is either full or empty cannot split, so a full
 * one beside an empty one has no legal move and a wide pool sets into a
 * staircase - a real fixed point of any local rule, which is why
 * equalise_liquids() below has to go hunting further than one cell for
 * somewhere lower. Carrying an amount instead, the same pair becomes 15 and
 * 0, settles to 8 and 7, and the difference spreads outward a neighbour at a
 * time until the surface is level.
 *
 * Two rules, in order: fill the cell below, then share what is left with the
 * one beside it. Called from sand_step()'s own sweep - not a pass of its
 * own - because both of those are gravity-ward, and only the main sweep's
 * order guarantees a destination has not been visited yet.
 *
 * No block wake of its own to do: give_mass() already calls mark_rows()
 * per transfer, and this grain's own (source) block gets BLOCK_ACTIVE set
 * by step_one_block()'s `moved_here` from this function's return value,
 * same as every other kind of grain - see mark_move()'s comment in
 * sand_priv.h for why nothing further is needed here. */
bool move_liquid_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                       int x, int y, int dx, int dy,
                       const int *slide_a, const int *slide_b,
                       cell_t grain, uint8_t mat_id)
{
    const int w = s->w;
    int mass = CELL_VARIANT(grain);
    bool moved = false;

    /* DOWN first, so a liquid falls before it spreads. */
    const int tx0 = x + dx, ty0 = y + dy;
    const int down = give_mass(s, prow, tx0, w, mass, mat_id, y, ty0);
    mass -= down;
    if (down > 0) {
        moved = true;
    }

    /* Then DOWN THE SLOPE, both ways.
     *
     * Still only immediate neighbours, and still gravity-ward, so they carry
     * the same guarantee the fall does. Leaving these out was a real
     * mistake: without them water on a slope can only shuffle sideways and
     * then fall, so a mound collapses at the speed of diffusion - a dome was
     * still 7 cells proud after five thousand steps. Running down the slope
     * directly is what makes it settle in a moment instead. */
    for (int d = 0; d < 2 && mass > 0; d++) {
        const int *slide = (d == 0) ? slide_a : slide_b;
        uint8_t *srow = dest_row(s, y + slide[1]);
        const int tx = x + slide[0], ty = y + slide[1];
        const int given = give_mass(s, srow, tx, w, mass, mat_id, y, ty);
        mass -= given;
        if (given > 0) {
            moved = true;
        }
    }

    /* Nothing left means no cell left. Written as CELL_EMPTY rather than a
     * zero variant, which would leave the material nibble claiming an
     * occupied cell holding nothing. */
    row[x] = (mass > 0) ? CELL_MAKE(mat_id, mass) : CELL_EMPTY;

    return moved;
}

/*---------------------------------------------------------------------------
 * Everything that is NOT gravity-ward, and so cannot live in that sweep.
 *-------------------------------------------------------------------------*/

/* Liquid cross-flow, as a second sweep with its own direction.
 *
 * WHY IT CANNOT LIVE IN THE MAIN LOOP
 *
 * Every move in the main sweep is gravity-ward, so sweeping against gravity
 * guarantees a cell's destination has already been visited and nothing can be
 * moved twice. Once gravity is tilted that pins the sweep on BOTH axes - and
 * then, of the two directions across the flow, only the one the sweep has
 * already passed is ever safe to use.
 *
 * Water could therefore cross a slope one way and never back. A tilted pool
 * did not level at all: it walked into the low corner and set there in steps.
 * Measured, a 45-degree pool sat at a spread of 525 units and had not improved
 * three thousand steps later. Tilting the board back releases it, which reads
 * convincingly like stored momentum and is nothing of the sort - it is the
 * gravity direction changing, and with it which way water is allowed to move.
 *
 * A separate pass has its own order, so it can be swept whichever way suits
 * the direction it is using, and that direction alternates every step. It only
 * ever moves liquid across the flow, so it cannot disturb anything the first
 * pass concluded.
 */
/* Falling water does not spread: if this cell has somewhere to fall THIS
 * step, that fall will happen in the main sweep and cross-flow has nothing
 * to decide. One comparison, and it is what makes an avalanche affordable -
 * while a body of water is collapsing almost every cell has room beneath
 * it, so almost every cell leaves right here instead of searching along a
 * surface it does not yet have. */
static inline bool has_room_below(const sand_t *s, int x, int y, int dx,
                                  int dy, uint8_t id)
{
    const int fx = x + dx;
    const int fy = y + dy;
    if ((unsigned)fx >= (unsigned)s->w || (unsigned)fy >= (unsigned)s->h) {
        return false;
    }
    const cell_t below = s->cells[(size_t)fy * (size_t)s->w + (size_t)fx];
    return CELL_IS_EMPTY(below) ||
           (CELL_MATERIAL(below) == id && CELL_VARIANT(below) < MASS_MAX);
}

/* Whether the immediate neighbour along (px, py) is lower. This is what
 * keeps the search off the bill: inside a body of water every neighbour
 * holds the same amount, so the overwhelming majority of cells answer here
 * in one comparison instead of walking the full sight distance. Only the
 * cells along a real imbalance look further, and those are the only ones
 * with anywhere to send anything. */
static inline bool neighbour_is_lower(const sand_t *s, int x, int y, int px,
                                      int py, uint8_t id, int mass)
{
    const int nx = x + px;
    const int ny = y + py;
    if ((unsigned)nx >= (unsigned)s->w || (unsigned)ny >= (unsigned)s->h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)s->w + (size_t)nx];
    return CELL_IS_EMPTY(n) || (CELL_MATERIAL(n) == id && CELL_VARIANT(n) < mass);
}

/* The shallowest place this liquid can reach along (px, py), within `sight`
 * cells, and how many steps away it is. Flow stops at anything that is not
 * the same liquid, so it cannot reach through a wall. */
static inline void find_shallowest(const sand_t *s, int x, int y, int px,
                                   int py, int sight, uint8_t id, int mass,
                                   int *lowest, int *at)
{
    *lowest = mass;
    *at = 0;

    for (int k = 1; k <= sight; k++) {
        const int sx = x + px * k;
        const int sy = y + py * k;

        if ((unsigned)sx >= (unsigned)s->w || (unsigned)sy >= (unsigned)s->h) {
            break;
        }
        const cell_t o = s->cells[(size_t)sy * (size_t)s->w + (size_t)sx];
        int there;

        if (CELL_IS_EMPTY(o)) {
            there = 0;
        } else if (CELL_MATERIAL(o) == id) {
            there = CELL_VARIANT(o);
        } else {
            break;
        }

        if (there < *lowest) {
            *lowest = there;
            *at = k;
            if (there == 0) {
                break;      /* nothing is lower than dry */
            }
        }
    }
}

/* One cell's share of cross-flow. Returns whether it gave anything, and
 * whether that transfer stayed inside this row (so the caller can defer
 * marking it, rather than paying for a full mark_rows() per transfer).
 * When it did stay in the row, `*touched_x` is set to the destination
 * column - the caller accumulates these into a range so the deferred wake
 * can still narrow which blocks it touches, rather than waking the row's
 * whole width. */
static inline bool equalise_one_cell(sand_t *s, uint8_t *row, int x, int y,
                                     int px, int py, int dx, int dy,
                                     int sight, uint8_t id, int mass,
                                     bool *stayed_in_row, int *touched_x)
{
    if (has_room_below(s, x, y, dx, dy, id)) {
        return false;
    }
    if (!neighbour_is_lower(s, x, y, px, py, id, mass)) {
        return false;
    }

    int lowest, at;
    find_shallowest(s, x, y, px, py, sight, id, mass, &lowest, &at);

    /* Half the difference. Handing over everything would only move the
     * imbalance rather than settle it - the two would trade places for
     * ever. */
    const int give = (mass - lowest) / 2;
    if (at == 0 || give <= 0) {
        return false;
    }

    const int tx = x + px * at;
    const int ty = y + py * at;
    const int w  = s->w;

    pour_into(&s->cells[(size_t)ty * (size_t)w + (size_t)tx], id, give);
    row[x] = (mass - give > 0) ? CELL_MAKE(id, mass - give) : CELL_EMPTY;

    *stayed_in_row = (py == 0);
    if (*stayed_in_row) {
        *touched_x = tx;
    } else {
        mark_move(s, x, y, tx, ty);
    }
    return true;
}

/* Widens [*x0,*x1] to also cover [lo,hi], or adopts it as the first range
 * seen if `*touched` was not already set - split out of
 * equalise_one_row()'s loop below purely to keep that loop's own
 * complexity down, not because this is reused elsewhere. */
static inline void union_touched_x(bool *touched, int *x0, int *x1,
                                   int lo, int hi)
{
    if (!*touched || lo < *x0) {
        *x0 = lo;
    }
    if (!*touched || hi > *x1) {
        *x1 = hi;
    }
    *touched = true;
}

/* One cell's contribution to a row's cross-flow pass: whether it holds a
 * tracked liquid, and folding any same-row transfer into the touched-x
 * range the row is accumulating (see union_touched_x() and the comment on
 * deferred marking below). Split out of equalise_one_row()'s loop purely
 * to keep that loop's own complexity down, not because this is reused
 * elsewhere. */
static inline bool equalise_one_row_cell(sand_t *s, uint8_t *row, int x, int y,
                                         int px, int py, int dx, int dy,
                                         int sight, uint16_t is_liquid,
                                         bool *touched, int *touched_x0,
                                         int *touched_x1)
{
    const cell_t c = row[x];
    if (CELL_IS_EMPTY(c)) {
        return false;
    }
    const uint8_t id = CELL_MATERIAL(c);
    if (((is_liquid >> id) & 1u) == 0) {
        return false;
    }

    bool stayed_in_row = false;
    int  tx = 0;
    if (equalise_one_cell(s, row, x, y, px, py, dx, dy, sight, id,
                          CELL_VARIANT(c), &stayed_in_row, &tx) &&
        stayed_in_row) {
        /* Marking is deferred when the flow stays inside one row, which is
         * every orientation where gravity has no sideways component - much
         * the commonest case. mark_rows() rewrites several bytes of row
         * state, and doing that per transfer rather than per row was most
         * of what this pass cost. The touched x range is kept so the
         * deferred block wake in equalise_one_row() can still narrow to
         * where the row's flow actually happened, rather than waking the
         * whole row's width of blocks - both the source column x and the
         * destination tx changed, so both bound the range. */
        union_touched_x(touched, touched_x0, touched_x1,
                       x < tx ? x : tx, x > tx ? x : tx);
    }
    return true;
}

/* One block-column's x-span within one row of the cross-flow pass, mirroring
 * step_one_block() in sand.c - the unit this pass can now skip whole. */
static inline bool equalise_one_block(sand_t *s, uint8_t *row, int y,
                                      int cx_from, int cx_to, int x_step,
                                      int px, int py, int dx, int dy,
                                      int sight, uint16_t is_liquid,
                                      bool *touched, int *touched_x0,
                                      int *touched_x1)
{
    bool any_liquid = false;

    for (int x = cx_from; x != cx_to; x += x_step) {
        if (equalise_one_row_cell(s, row, x, y, px, py, dx, dy, sight,
                                  is_liquid, touched, touched_x0,
                                  touched_x1)) {
            any_liquid = true;
        }
    }
    return any_liquid;
}

/* One row's share of cross-flow. Returns whether it held any liquid, which
 * the caller needs in order to know whether the whole pass found anything -
 * that is what arms may_have_liquid, and it is now this return value's only
 * job. It used to also decide whether the row earned a ROW_NO_LIQUID bit; see
 * equalise_liquids() below for where that went.
 *
 * Walked by BLOCK-COLUMN rather than by cell - exactly the shape
 * step_one_row() in sand.c has always used for the main sweep, and for the
 * same reason: a block with no liquid in it or beside it has nothing here to
 * decide, so none of its cells need to be read at all. The skip is
 * BLOCK_LIQUID_NEAR; see sand_priv.h for the invariant that makes it sound,
 * and equalise_liquids() below for where the bit is computed.
 *
 * When sleeping is disabled (block_state is NULL) there are no bits to read,
 * so no block is ever skipped and this walks the same cells in the same order
 * the flat per-cell version did. Written as a NULL `brow` rather than a
 * separate whole-row branch on purpose, and it is worth saying why, because
 * the obvious spelling costs 8% of the water benchmark: a second call site for
 * equalise_one_block() is enough to lose the inlining a single one gets
 * unconditionally (`-finline-functions-called-once`), which is the same trap
 * that silently un-inlined try_slide_impl() in the eighth attempt - see
 * docs/Sand/Performance-Tuning-Attempts.md. One call site, one branch on a
 * pointer that is loop-invariant, and the cost is nothing. */
static bool equalise_one_row(sand_t *s, int y, int w, int x_step, int px,
                             int py, int dx, int dy, int sight,
                             uint16_t is_liquid)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;
    bool any_liquid = false;
    bool touched = false;
    int  touched_x0 = 0, touched_x1 = 0;

    /* Unsigned cast for the division-by-power-of-two reason documented in
     * docs/Notes/Optimization-Playbook.md, the same as further down. */
    const uint8_t *brow = (s->block_state != NULL)
        ? s->block_state + (size_t)((unsigned)y / SAND_BLOCK_H) *
                           (size_t)s->block_cols
        : NULL;
    const int bx_from = (x_step > 0) ? 0 : s->block_cols - 1;
    const int bx_to   = (x_step > 0) ? s->block_cols : -1;

    for (int bx = bx_from; bx != bx_to; bx += x_step) {
        if (brow != NULL && (brow[bx] & BLOCK_LIQUID_NEAR) == 0) {
            continue;
        }
        const int lo = bx * SAND_BLOCK_W;
        const int hi = (lo + SAND_BLOCK_W < w) ? lo + SAND_BLOCK_W : w;
        if (equalise_one_block(s, row, y,
                               (x_step > 0) ? lo : hi - 1,
                               (x_step > 0) ? hi : lo - 1,
                               x_step, px, py, dx, dy, sight, is_liquid,
                               &touched, &touched_x0, &touched_x1)) {
            any_liquid = true;
        }
    }

    if (touched) {
        mark_rows(s, y, y);
        /* Unsigned cast for the same division-by-power-of-two reason
         * documented in docs/Notes/Optimization-Playbook.md -
         * touched_x0/touched_x1/y are always non-negative, but as plain
         * int the compiler cannot prove that and falls back to a
         * signed-division correction sequence instead of a shift. */
        const int by = (int)((unsigned)y / SAND_BLOCK_H);
        wake_blocks_range(s, (int)((unsigned)touched_x0 / SAND_BLOCK_W), by,
                   (int)((unsigned)touched_x1 / SAND_BLOCK_W), by);
    }

    return any_liquid;
}

/* Turn the sweep's per-block BLOCK_HAS_LIQUID observation into the
 * BLOCK_LIQUID_NEAR bit equalise_one_row() skips on: a block is NEAR if it or
 * any of its up to 8 neighbours holds liquid. One pass over the blocks, 24 of
 * them at the shipped grid size, run once per step - O(blocks), not O(moves),
 * which is the whole reason this skip structure is affordable where
 * ROW_NO_LIQUID was not (ninth attempt, Performance-Tuning-Attempts.md).
 *
 * Expanded by one block for the same reason every other wake in this codebase
 * is: liquid moves at most one cell in the sweep and at most SAND_LIQUID_SIGHT
 * (8, well inside a 32-wide block) in this pass, so anywhere it can arrive
 * after its own block was walked is a neighbour of the block that was seen
 * holding it. See sand_priv.h's comment above BLOCK_HAS_LIQUID for the full
 * invariant. */
static void mark_liquid_neighbourhoods(sand_t *s)
{
    for (int by = 0; by < s->block_rows; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            uint8_t *slot = &s->block_state[by * s->block_cols + bx];
            *slot = block_or_neighbour_has_liquid(s, bx, by)
                  ? (uint8_t)(*slot | BLOCK_LIQUID_NEAR)
                  : (uint8_t)(*slot & ~BLOCK_LIQUID_NEAR);
        }
    }
}

static void equalise_liquids(sand_t *s, const int *perp, int sight,
                             int dx, int dy)
{
    bool found_any = false;

    if (s->block_state != NULL) {
        mark_liquid_neighbourhoods(s);
    }

    const int px = perp[0];
    const int py = perp[1];
    const int w  = s->w;
    const int h  = s->h;

    const uint16_t is_liquid = liquid_mask();

    /* Swept so that whatever is being given to has already been visited. */
    const int y_from = (py > 0) ? h - 1 : 0;
    const int y_to   = (py > 0) ? -1    : h;
    const int y_step = (py > 0) ? -1    : 1;

    /* Only the direction now, not a from/to range: since each row walks
     * block-columns (see equalise_one_row()), every span works out its own
     * cell range from x_step and its own bounds - the same shape sweep_x_order()
     * in sand.c ended up with for the main sweep, for the same reason. */
    const int x_step = (px > 0) ? -1 : 1;

    /* Every row, every step - but not every CELL of every row: each row skips
     * the block-columns whose BLOCK_LIQUID_NEAR is clear (see
     * equalise_one_row()).
     *
     * There used to be a per-row "proved dry" cache (ROW_NO_LIQUID) letting
     * this skip whole rows. It was removed in the ninth attempt: keeping it
     * honest meant wiping three bytes of row_state on every liquid transfer
     * anywhere on the grid - 33,426 byte writes a step on the water benchmark
     * - to save scanning about 104 of 224 rows, and the device measured the
     * bookkeeping as costing far more than the scans it avoided (water 17860
     * -> 13130 us just from deleting it). The block-shaped skip that replaced
     * it is the same idea with the maintenance bill the old one failed:
     * BLOCK_HAS_LIQUID is established by a sweep that was going to read those
     * cells anyway, and turned into BLOCK_LIQUID_NEAR by one pass over 24
     * blocks. Nothing is charged per move. See
     * docs/Sand/Performance-Tuning-Attempts.md. */
    for (int y = y_from; y != y_to; y += y_step) {
        if (equalise_one_row(s, y, w, x_step, px, py, dx, dy,
                             sight, is_liquid)) {
            found_any = true;
        }
    }

    /* Looked everywhere and found none, so stop looking until some is placed.
     * Sound despite the block skipping above, and this is the one place where
     * that needs saying: a skipped block has BLOCK_LIQUID_NEAR clear, and the
     * invariant in sand_priv.h is that every liquid cell sits in a block whose
     * NEAR bit is set - so a skipped block provably held nothing to find. */
    if (!found_any) {
        s->may_have_liquid = false;
    }
}

/* One wall's share of the rebound splash - see SAND_REBOUND_GAIN.
 *
 * `edge` is the column (for a left/right wall) or row (top/bottom) that
 * touches the wall; `count` is how many cells run along it; `to` is the
 * one-cell step into the grid, away from the wall. `push_q8` is how hard
 * momentum is currently driving INTO this particular wall - already resolved
 * to a single non-negative number by the caller, since each wall only cares
 * about one sign of one axis.
 *
 * Every source cell here is unique to this wall and every destination is one
 * fixed step inward, so no two transfers this call can ever touch the same
 * cell - unlike equalise_liquids, this needs no sweep order at all. */
/* How much mass one cell's share of the splash moves, or 0 if the push is
 * not hard enough to count as a flick at all. */
static inline int rebound_kick(int push_q8)
{
    if (push_q8 <= SAND_REBOUND_THRESHOLD) {
        return 0;
    }
    const int raw = ((push_q8 - SAND_REBOUND_THRESHOLD) * SAND_REBOUND_GAIN) >> 8;
    return raw < SAND_REBOUND_MAX ? raw : SAND_REBOUND_MAX;
}

/* One cell's share of the splash: kick up to `kick` mass from (x, y) into
 * the cell `to` steps away from the wall, if (x, y) holds a liquid and that
 * destination exists. */
static inline void rebound_one_cell(sand_t *s, int x, int y, int to,
                                    bool vertical, int kick,
                                    uint16_t is_liquid)
{
    const int w = s->w;
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const cell_t c = s->cells[at];
    if (CELL_IS_EMPTY(c)) {
        return;
    }
    const uint8_t id = CELL_MATERIAL(c);
    if (((is_liquid >> id) & 1u) == 0) {
        return;
    }

    const int nx = vertical ? x + to : x;
    const int ny = vertical ? y      : y + to;
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)s->h) {
        return;
    }

    const int mass = CELL_VARIANT(c);
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    const int room = room_in(s->cells[nat], id);
    const int give = kick < mass ? kick : mass;
    const int moved = give < room ? give : room;
    if (moved <= 0) {
        return;
    }

    pour_into(&s->cells[nat], id, moved);
    s->cells[at] = (mass - moved > 0) ? CELL_MAKE(id, mass - moved)
                                      : CELL_EMPTY;
    mark_move(s, x, y, nx, ny);
}

static void rebound_wall(sand_t *s, int edge, int count, int to,
                         bool vertical, int push_q8, uint16_t is_liquid)
{
    const int kick = rebound_kick(push_q8);
    if (kick <= 0) {
        return;
    }

    for (int i = 0; i < count; i++) {
        const int x = vertical ? edge : i;
        const int y = vertical ? i : edge;
        rebound_one_cell(s, x, y, to, vertical, kick, is_liquid);
    }
}

void sand_step_liquids(sand_t *s, const int *perp_a, const int *perp_b,
                       int dx, int dy)
{
    if (!s->may_have_liquid) {
        return;
    }

    /* Cross-flow, in its own pass and alternating which way each step - so a
     * tilted pool levels both ways instead of walking into one corner. See
     * equalise_liquids(). */
    equalise_liquids(s, s->liquid_flip ? perp_a : perp_b,
                     SAND_LIQUID_SIGHT, dx, dy);
    s->liquid_flip = !s->liquid_flip;

    /* And let a hard flick splash liquid back off whichever wall it just
     * turned into. Four walls, but each is a no-op below its own threshold,
     * so this costs nothing on the vastly more common calm step - see
     * SAND_REBOUND_GAIN. */
    if (SAND_REBOUND_GAIN != 0) {
        const uint16_t is_liquid = liquid_mask();

        rebound_wall(s, 0,        s->h, 1,  true,
                    (int)-s->mom_x_q8, is_liquid);   /* left wall   */
        rebound_wall(s, s->w - 1, s->h, -1, true,
                    (int) s->mom_x_q8, is_liquid);   /* right wall  */
        rebound_wall(s, 0,        s->w, 1,  false,
                    (int)-s->mom_y_q8, is_liquid);   /* top wall    */
        rebound_wall(s, s->h - 1, s->w, -1, false,
                    (int) s->mom_y_q8, is_liquid);   /* bottom wall */
    }
}

/*---------------------------------------------------------------------------
 * Momentum accessors - see SAND_REBOUND_GAIN.
 *-------------------------------------------------------------------------*/

void sand_momentum(const sand_t *s, int32_t *mx_q8, int32_t *my_q8)
{
    *mx_q8 = s->mom_x_q8;
    *my_q8 = s->mom_y_q8;
}

void sand_set_flick(sand_t *s, int flick)
{
    s->flick = flick < 0 ? 0 : (flick > 255 ? 255 : flick);
}
