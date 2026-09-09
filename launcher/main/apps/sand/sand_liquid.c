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
 * sweep finishes: cross-flow levelling.
 *===========================================================================*/

#include "sand_priv.h"

#include "sand_liquid_move.h"
#include "util/fixed.h"

/* See liquid_mask() in sand_priv.h */

/*---------------------------------------------------------------------------
 * Everything that is NOT gravity-ward, and so cannot live in that sweep.
 *-------------------------------------------------------------------------*/

/* WHY A SEPARATE PASS: the main sweep guarantees no double-move sweeping
 * gravity-ward; tilted gravity pins both axes, so only ONE cross-flow
 * direction is safe. Without this, water crosses a slope one way and
 * never back - a tilted pool walks into the low corner and stays. A
 * separate pass alternates direction each step, only moving liquid
 * ACROSS flow, so it cannot disturb the main sweep. Falling water does
 * not spread: a cell able to fall THIS step leaves cross-flow nothing
 * to decide. */
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

/* `there < MASS_MAX` must stay part of the test, not just the level
 * comparison: once a bias is in play an EQUAL-mass neighbour can
 * legitimately read as lower, so "is it lower" alone would send every
 * interior cell of a full pool on a full sight walk - the exact cost
 * this function exists to avoid. */
static inline bool neighbour_is_lower(const sand_t *s, int x, int y, int px,
                                      int py, uint8_t id, int mass,
                                      int bias_q8)
{
    const int nx = x + px;
    const int ny = y + py;
    if ((unsigned)nx >= (unsigned)s->w || (unsigned)ny >= (unsigned)s->h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)s->w + (size_t)nx];
    /* Rejects OTHER material as full using same comparison. One branch for
     * both. */
    const int there = CELL_IS_EMPTY(n) ? 0
                    : (CELL_MATERIAL(n) == id ? CELL_VARIANT(n) : MASS_MAX);
    return there < MASS_MAX && (there << 8) - bias_q8 < (mass << 8);
}

/* Shallowest cell by level, steps away, 1/256 mass drop. Flow stops at
 * different liquid. */
static inline int find_shallowest(const sand_t *s, int x, int y, int px,
                                  int py, int sight, uint8_t id, int mass,
                                  int bias_q8, int *lowest, int *at)
{
    const int mine = mass << 8;
    int best = mine;
    int carried = 0;
    int low = mass;
    int k_at = 0;

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

        carried -= bias_q8;
        const int level = (there << 8) + carried;
        if (level < best) {
            best = level;
            low  = there;
            k_at = k;
        }
        if (there == 0) {
            break;          /* nothing is lower than dry */
        }
    }

    *lowest = low;
    *at = k_at;
    return mine - best;
}

/* Returns transfer status and row confinement; sets `*touched_x` if confined. */
static inline bool equalise_one_cell(sand_t *s, uint8_t *row, int x, int y,
                                     int px, int py, int dx, int dy,
                                     int sight, uint8_t id, int mass,
                                     int bias_q8,
                                     bool *stayed_in_row, int *touched_x)
{
    if (has_room_below(s, x, y, dx, dy, id)) {
        return false;
    }
    if (!neighbour_is_lower(s, x, y, px, py, id, mass, bias_q8)) {
        return false;
    }
    if (!liquid_may_move(s, id)) {
        return false;   /* viscosity affects levelling; syrupy liquid would
                         * level instantly sideways, resembling "runny" */
    }

    int lowest, at;
    const int drop_q8 = find_shallowest(s, x, y, px, py, sight, id, mass,
                                        bias_q8, &lowest, &at);

    /* Avoids trading mass imbalance - `>> 9` halves and converts q8 to whole
     * mass. */
    int give = drop_q8 >> 9;
    if (__builtin_expect(at == 0 || give <= 0, 1)) {
        return false;
    }
    /* Two clamps the old rule never needed: it only ever moved half of a
     * difference in MASS, which could not overrun either end. A level
     * difference can be far larger than the mass on hand or the room at
     * the far end, so both ends are pinned here. */
    if (__builtin_expect(give > MASS_MAX - lowest, 0)) {
        give = MASS_MAX - lowest;
    }
    if (__builtin_expect(give > mass, 0)) {
        give = mass;
    }
    if (__builtin_expect(give <= 0, 0)) {
        return false;
    }

    const int tx = x + px * at;
    const int ty = y + py * at;
    const int w  = s->w;

    const bool was_empty =
        pour_into(&s->cells[(size_t)ty * (size_t)w + (size_t)tx], id, give);
    row[x] = (mass - give > 0) ? CELL_MAKE(id, mass - give) : CELL_EMPTY;
    if (was_empty) {
        mark_depth_band(s, ty);
    }

    *stayed_in_row = (ty == y);
    if (*stayed_in_row) {
        *touched_x = tx;
    } else {
        mark_move(s, x, y, tx, ty);
    }
    return true;
}

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

/* One cell's cross-flow contribution: tracked liquid, fold same-row transfer.
 * Split for complexity. */
static inline bool equalise_one_row_cell(sand_t *s, uint8_t *row, int x, int y,
                                         const xflow_t *r,
                                         int dx, int dy,
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

    /* DERIVED HERE, NOT CARRIED IN, and derived only once the cell is known to
     * be liquid - which ~30% of examined cells are. Walked incrementally by the
     * caller it cost an add, a mask and a spilled load/store for every cell
     * including the ~70% that never reach this line. The phase is a pure
     * function of position: the caller seeded it as (q_q8 * cx_from) & 255 and
     * stepped by +/-q_q8 per cell, which telescopes to (q_q8 * x) & 255 in
     * either direction. Off the major axis it never advanced at all. */
    const int q_q8 = r->q_q8;
    const int pat  = (r->ax[0] != 0) ? ((q_q8 * x) & 255) : ((q_q8 * y) & 255);
    const bool diagonal = (pat < q_q8);

    const int px = diagonal ? r->dg[0] : r->ax[0];
    const int py = diagonal ? r->dg[1] : r->ax[1];
    const int bias_q8 = diagonal ? r->bias_dg_q8 : r->bias_ax_q8;

    bool stayed_in_row = false;
    int  tx = 0;
    if (equalise_one_cell(s, row, x, y, px, py, dx, dy, sight, id,
                              CELL_VARIANT(c), bias_q8, &stayed_in_row, &tx) &&
            stayed_in_row) {
        /* Marking deferred for gravity-free orientations. mark_rows() impact.
         * Narrow x range for wake. */
        union_touched_x(touched, touched_x0, touched_x1,
                       x < tx ? x : tx, x > tx ? x : tx);
    }
    return true;
}

static inline bool equalise_one_block(sand_t *s, uint8_t *row, int y,
                                      int cx_from, int cx_to, int x_step,
                                      const xflow_t *r, int dx, int dy,
                                      int sight, uint16_t is_liquid,
                                      bool *touched, int *touched_x0,
                                      int *touched_x1)
{
    bool any_liquid = false;

    /* The diagonal-ray phase this loop used to walk per cell now lives in
     * equalise_one_row_cell(), derived from x once a cell is known to be
     * liquid - see its own comment. */
    for (int x = cx_from; x != cx_to; x += x_step) {
        if (equalise_one_row_cell(s, row, x, y, r,
                                  dx, dy, sight,
                                  is_liquid, touched, touched_x0,
                                  touched_x1)) {
            any_liquid = true;
        }
    }
    return any_liquid;
}

/* Whether every cell in [x0, x1) rejects a cross-flow ray landing in it.
 * neighbour_is_lower() reads a full cell, a foreign material and a wall all as
 * MASS_MAX, so only an empty cell or a partly-filled liquid can read as lower.
 * Breaks on the first of either, so a span that is still moving costs a
 * handful of loads rather than its length. */
static inline bool span_blocks_flow(const uint8_t *row, int x0, int x1,
                                    uint16_t is_liquid)
{
    for (int x = x0; x < x1; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            return false;
        }
        if (((is_liquid >> CELL_MATERIAL(c)) & 1u) != 0 &&
            CELL_VARIANT(c) < MASS_MAX) {
            return false;
        }
    }
    return true;
}

/* The answer a skipped span still owes equalise_liquids(): found_any is what
 * clears may_have_liquid. */
static inline bool span_has_liquid(const uint8_t *row, int x0, int x1,
                                   uint16_t is_liquid)
{
    for (int x = x0; x < x1; x++) {
        if (((is_liquid >> CELL_MATERIAL(row[x])) & 1u) != 0) {
            return true;
        }
    }
    return false;
}

/* The columns a span's rays can reach: both rays step x by -1, 0 or +1, so the
 * span's own width plus one cell of margin either side, clipped to the grid.
 * A ray leaving the grid sideways is already a reject. */
static inline bool rays_blocked(const uint8_t *ax_row, const uint8_t *dg_row,
                                int x0, int x1, int w, uint16_t is_liquid)
{
    const int sx0 = (x0 > 0) ? x0 - 1 : 0;
    const int sx1 = (x1 < w) ? x1 + 1 : w;

    if (ax_row != NULL && !span_blocks_flow(ax_row, sx0, sx1, is_liquid)) {
        return false;
    }
    /* Equal covers both "one row, two rays" - which is every landscape and
     * diagonal orientation, where ax and dg differ only in x - and "both off
     * the grid". */
    if (dg_row == ax_row) {
        return true;
    }
    return dg_row == NULL || span_blocks_flow(dg_row, sx0, sx1, is_liquid);
}

static bool equalise_one_row(sand_t *s, int y, int w, int x_step,
                             const xflow_t *r, int dx, int dy, int sight,
                             uint16_t is_liquid)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;

    const uint8_t *const ax_row = dest_row(s, y + r->ax[1]);
    const uint8_t *const dg_row = dest_row(s, y + r->dg[1]);

    bool any_liquid = false;
    bool touched = false;
    int  touched_x0 = 0, touched_x1 = 0;

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

        /* SKIPPED WHOLE when both rays land where nothing can be lower - the
         * shape a SETTLED pool holds, and it pays to rediscover every step
         * forever, because BLOCK_LIQUID_NEAR only says liquid is present,
         * never that it is still moving. Halves a settled basin.
         *
         * RNG-NEUTRAL, which is what makes it safe rather than merely faster:
         * these cells would all have rejected at neighbour_is_lower(), and
         * liquid_may_move()'s viscosity roll sits after that, so no draw is
         * skipped and oil's stream is untouched.
         *
         * PER BLOCK, NOT PER ROW: a row-level form of the same fact only ever
         * fires on a pool spanning the whole screen, and measured nothing on a
         * pool with air beside it - which is every pool the app holds. */
        if (rays_blocked(ax_row, dg_row, lo, hi, w, is_liquid)) {
            if (span_has_liquid(row, lo, hi, is_liquid)) {
                any_liquid = true;
            }
            continue;
        }

        if (equalise_one_block(s, row, y,
                               (x_step > 0) ? lo : hi - 1,
                               (x_step > 0) ? hi : lo - 1,
                               x_step, r, dx, dy, sight, is_liquid,
                               &touched, &touched_x0, &touched_x1)) {
            any_liquid = true;
        }
    }

    if (touched) {
        mark_rows(s, y, y);
        /* Unsigned cast needed for shift instead of signed division
         * correction. */
        const int by = (int)((unsigned)y / SAND_BLOCK_H);
        wake_blocks_range(s, (int)((unsigned)touched_x0 / SAND_BLOCK_W), by,
                   (int)((unsigned)touched_x1 / SAND_BLOCK_W), by);
    }

    return any_liquid;
}

/* BLOCK_LIQUID_NEAR checks blocks/neighbours with liquid; O(blocks). Expanded
 * for liquid movement. See sand_priv.h for BLOCK_HAS_LIQUID. */
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

static void equalise_liquids(sand_t *s, const xflow_t *f, int sight,
                             int dx, int dy)
{
    bool found_any = false;

    if (s->block_state != NULL) {
        mark_liquid_neighbourhoods(s);
    }

    const int px = f->dg[0];
    const int py = f->dg[1];
    const int w  = s->w;
    const int h  = s->h;

    const uint16_t is_liquid = liquid_mask();

    /* Swept so that whatever is being given to has already been visited. */
    const int y_from = (py > 0) ? h - 1 : 0;
    const int y_to   = (py > 0) ? -1    : h;
    const int y_step = (py > 0) ? -1    : 1;

    const int x_step = (px > 0) ? -1 : 1;

    /* See equalise_one_row(). BLOCK_HAS_LIQUID → BLOCK_LIQUID_NEAR. No move
     * cost. Docs/Sand/Performance-Tuning-Attempts.md. */
    for (int y = y_from; y != y_to; y += y_step) {
        if (equalise_one_row(s, y, w, x_step, f, dx, dy,
                             sight, is_liquid)) {
            found_any = true;
        }
    }

    /* Sound despite the block skipping above: a skipped block has
     * BLOCK_LIQUID_NEAR clear, and sand_priv.h's invariant is that every
     * liquid cell sits in a block whose NEAR bit is set - so a skipped
     * block provably held nothing to find. */
    if (!found_any) {
        s->may_have_liquid = false;
    }
}

void sand_step_liquids(sand_t *s, const xflow_t *flow, int dx, int dy)
{
    if (!s->may_have_liquid) {
        return;
    }

    xflow_t run;
    if (s->liquid_flip) {
        run.ax[0] =  flow->ax[0]; run.ax[1] =  flow->ax[1];
        run.dg[0] =  flow->dg[0]; run.dg[1] =  flow->dg[1];
        run.bias_ax_q8 =  flow->bias_ax_q8;
        run.bias_dg_q8 =  flow->bias_dg_q8;
    } else {
        run.ax[0] = -flow->ax[0]; run.ax[1] = -flow->ax[1];
        run.dg[0] = -flow->dg[0]; run.dg[1] = -flow->dg[1];
        run.bias_ax_q8 = -flow->bias_ax_q8;
        run.bias_dg_q8 = -flow->bias_dg_q8;
    }
    run.q_q8 = flow->q_q8;

    /* Cross-flow levels both ways. See equalise_liquids(). */
    equalise_liquids(s, &run, SAND_LIQUID_SIGHT, dx, dy);
    s->liquid_flip = !s->liquid_flip;
}
