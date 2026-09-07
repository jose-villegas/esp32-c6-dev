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

#include "util/fixed.h"

/* See liquid_mask() in sand_priv.h */

/* WATER ONLY - see acid_bubble() (sand_reactions.c) for what replaced
 * this trigger there. MASKED TO `mat_id`, not a plain sand_displace():
 * an unmasked throw scatters whatever else is nearby too, not just
 * water splashing itself. WATER DECAYS TWO WAYS, INDEPENDENTLY - see
 * SAND_SPLASH_RADIUS_WATER's own comment in sand.h for the full
 * account. */
static inline void splash_displace(sand_t *s, int x, int y, uint8_t mat_id)
{
    if (mat_id != MAT_WATER) {
        return;
    }
    if ((rng_next(&s->rng) & 0xFF) > s->splash_chance) {
        return;   /* this echo lost the roll - let the bounce die here */
    }
    sand_displace_material(s, x, y, s->splash_radius_water, mat_id);
    /* Directed toward EMPTY neighbours: a radial spray mostly throws
     * water at other water, invisible on screen. CHECKED, not a fixed
     * away-from-gravity guess: during a sustained pour that direction
     * is usually more incoming water, so a guess kept swapping
     * identical cells with no visible gap. Each push sources from the
     * NEIGHBOUR outward, not the contact point - firing from the
     * contact point makes every push compete for one grain, so only
     * one resolves. See test_a_water_splash_actually_opens_a_gap. */
    for (int dir = 0; dir < 8; dir++) {
        const int *d = ring_dir(dir);
        const int nx = x + d[0], ny = y + d[1];
        const int fx = x + 2 * d[0], fy = y + 2 * d[1];
        if ((unsigned)nx >= (unsigned)s->w || (unsigned)ny >= (unsigned)s->h ||
            (unsigned)fx >= (unsigned)s->w || (unsigned)fy >= (unsigned)s->h) {
            continue;
        }
        const cell_t neighbour = s->cells[(size_t)ny * (size_t)s->w + (size_t)nx];
        if (CELL_IS_EMPTY(neighbour)) {
            continue;   /* nothing there to push outward */
        }
        if (CELL_MATERIAL(neighbour) != mat_id) {
            continue;
        }
        if (!CELL_IS_EMPTY(s->cells[(size_t)fy * (size_t)s->w + (size_t)fx])) {
            continue;   /* no room one further out to push it into */
        }
        sand_impulse(s, nx, ny, dir, SAND_EXPLODE_INITIAL_SPEED);
    }
    s->splash_chance =
        s->splash_chance > SAND_SPLASH_CHANCE_FLOOR + SAND_SPLASH_CHANCE_STEP
            ? (uint8_t)(s->splash_chance - SAND_SPLASH_CHANCE_STEP)
            : SAND_SPLASH_CHANCE_FLOOR;
    s->splash_radius_water =
        s->splash_radius_water
                > SAND_SPLASH_RADIUS_WATER_FLOOR + SAND_SPLASH_RADIUS_WATER_STEP
            ? (uint8_t)(s->splash_radius_water - SAND_SPLASH_RADIUS_WATER_STEP)
            : SAND_SPLASH_RADIUS_WATER_FLOOR;
}

/* Mass is only ever moved, never made - every caller subtracts the same
 * figure from somewhere else in the same breath. Returns whether `dst`
 * was empty beforehand: the only case that can move where a puddle's
 * surface sits, which callers that care feed to mark_depth_band()
 * (sand_priv.h). */
static inline bool pour_into(cell_t *dst, uint8_t id, int amount)
{
    const bool was_empty = CELL_IS_EMPTY(*dst);
    const int had = was_empty ? 0 : CELL_VARIANT(*dst);

    *dst = CELL_MAKE(id, had + amount);
    return was_empty;
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

/* Row-shaped bookkeeping. Calls mark_rows() up to thrice per grain. Cache
 * removal makes it cheap. */
static inline int give_mass(sand_t *s, uint8_t *to_row, int tx, int w,
                            int mass, uint8_t mat_id, int y, int ty)
{
    if (to_row == NULL || (unsigned)tx >= (unsigned)w) {
        return 0;
    }
    const int room = room_in(to_row[tx], mat_id);
    const int give = mass < room ? mass : room;
    if (give > 0) {
        /* Gating on was_empty here would reintroduce per-transfer cost. See
         * equalise_one_cell() comment. */
        pour_into(&to_row[tx], mat_id, give);
        mark_rows(s, y, ty);
    }
    return give;
}

/* Carrying an AMOUNT (not full/empty) lets a wide pool level instead of
 * freezing into a staircase. Fill below, then share what is left beside
 * it, from sand_step()'s own sweep: only its order guarantees a
 * destination has not been visited. `mobility` (material.h), inverted
 * viscosity, gates whether a liquid cell acts: water (255) always
 * moves, oil (90) crawls, defaulting to 255 so code that never asks for
 * viscosity keeps old behaviour. No jostle bypass: shaking a viscous
 * liquid does not thin it. */
static inline bool liquid_may_move(sand_t *s, uint8_t id)
{
    const int m = (s->mobility >= 0) ? s->mobility
                                     : material_by_id((material_id_t)id)->mobility;

    /* Default is NO VISCOSITY, not "never moves". Unset lava behaves like
     * pre-field liquids. Trap if read otherwise. "Twelve times too slow"
     * means "extremely viscous". Default fails with water. Test:
     * test_every_liquid_declares_a_mobility. */
    if (m == 0) {
        return true;
    }
    return m >= 255 || (int)(rng_next(&s->rng) & 0xFF) < m;
}

static inline int foreign_liquid_neighbours(const sand_t *s, int x, int y,
                                            uint8_t id)
{
    static const int dirs[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
    const int w = s->w;
    const int h = s->h;

    int n = 0;
    for (int k = 0; k < 4; k++) {
        const int nx = x + dirs[k][0];
        const int ny = y + dirs[k][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t c = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const material_t *m = material_of(c);
        if (m->kind == KIND_LIQUID && CELL_MATERIAL(c) != id) {
            n++;
        }
    }
    return n;
}

/* Drag stops FINGER: first cell moves freely, subsequent cells progressively
 * less willing. */
static inline bool drag_allows_swap(sand_t *s, int x, int y, uint8_t id)
{
    const int surrounded = foreign_liquid_neighbours(s, x, y, id);
    if (surrounded <= 1) {
        return true;
    }
    /* 2 -> mask 1 (1 in 2), 3 -> mask 3 (1 in 4), 4 -> mask 7 (1 in 8). */
    const unsigned mask = (1u << (unsigned)(surrounded - 1)) - 1u;
    return (rng_next(&s->rng) & mask) == 0u;
}

/* Whole cells swap by density (denser sinks) - not mass transfer, since
 * a cell cannot be part oil and part water. Denser moves DOWN, not
 * lighter rising, so it inherits the sweep's no-double-move guarantee
 * for free (a rise needs its own reversed pass, like try_bubble() in
 * sand_gas.c). NOT paced by viscosity: gating on `mobility` looks
 * obvious but PREVENTS separation rather than slowing it - throttling
 * swap and levelling together settles a tilted pair into a permanent
 * shear instead of converging. */
static inline bool sink_through_lighter_liquid(sand_t *s, uint8_t *row,
                                               uint8_t *prow, int x, int y,
                                               int tx, int ty, int w,
                                               cell_t grain,
                                               const material_t *mat)
{
    if (prow == NULL || (unsigned)tx >= (unsigned)w) {
        return false;
    }
    const cell_t below = prow[tx];
    if (CELL_IS_EMPTY(below)) {
        return false;   /* open space - the ordinary fall handles it */
    }
    if (CELL_MATERIAL(below) == CELL_MATERIAL(grain)) {
        return false;   /* more of the same liquid - give_mass() handles
                         * it, and far better: it splits the amount */
    }
    const material_t *bm = material_of(below);
    if (bm->kind != KIND_LIQUID || mat->density <= bm->density) {
        return false;
    }
    if (!drag_allows_swap(s, x, y, CELL_MATERIAL(grain))) {
        return false;
    }

    prow[tx] = grain;
    row[x]   = below;

    mark_rows(s, y, ty);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, tx, ty);
    return true;
}

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

    /* Viscosity checked ONCE for this grain's whole move (fall + both
     * slides), not per attempt, to avoid a sluggish liquid jittering
     * between options. Deliberately before the density swap below,
     * which is exempt - see sink_through_lighter_liquid(). Marked
     * unlikely: verified on device that the hint moves cold
     * too-viscous-to-move code out of the hot path, worth ~26% on a
     * water benchmark. Wrong for oil (refuses ~2 in 3 steps), but water
     * is what a screen of liquid usually is. */
    if (__builtin_expect(!liquid_may_move(s, mat_id), 0)) {
        return false;
    }

    if (sink_through_lighter_liquid(s, row, prow, x, y, tx0, ty0, w, grain,
                                    material_by_id((material_id_t)mat_id))) {
        return true;
    }

    /* Checked BEFORE give_mass() writes into the target - afterward it
     * never reads as empty again. */
    const bool target_occupied = (unsigned)tx0 < (unsigned)w && prow != NULL
        && !CELL_IS_EMPTY(prow[tx0]);

    const int down = give_mass(s, prow, tx0, w, mass, mat_id, y, ty0);
    mass -= down;
    if (down > 0) {
        moved = true;

        if (target_occupied) {
            splash_displace(s, tx0, ty0, mat_id);
        }
    }

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

    /* CELL_EMPTY avoids material nibble misuse */
    row[x] = (mass > 0) ? CELL_MAKE(mat_id, mass) : CELL_EMPTY;

    return moved;
}

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
    if (SAND_STEP_GATED(xflow_body,
            equalise_one_cell(s, row, x, y, px, py, dx, dy, sight, id,
                              CELL_VARIANT(c), bias_q8, &stayed_in_row, &tx) &&
            stayed_in_row)) {
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

static bool equalise_one_row(sand_t *s, int y, int w, int x_step,
                             const xflow_t *r, int dx, int dy, int sight,
                             uint16_t is_liquid)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;
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
