/*=============================================================================
 * sand_liquid_move - the liquid-movement helpers called FROM sand_step()'s
 * main sweep, `static inline` here for the same reason as dest_row() and
 * mark_rows() in sand_priv.h: move_liquid_grain() sits on the hottest path
 * in the simulation (once per liquid cell) and a cross-translation-unit call
 * is not guaranteed to inline the way a call within the same file is. Shared
 * between sand.c (the sweep that calls move_liquid_grain()) and
 * sand_liquid.c (whose cross-flow half still uses pour_into() and
 * liquid_may_move()) - each .c file gets its own inlinable copy.
 *===========================================================================*/
#pragma once

#include "sand_priv.h"

/* WATER ONLY - see acid_bubble() (sand_reactions.c) for what replaced
 * this trigger there. MASKED TO `mat_id`, not a plain sand_displace():
 * an unmasked throw scatters whatever else is nearby too, not just
 * water splashing itself. WATER DECAYS TWO WAYS, INDEPENDENTLY - see
 * SAND_SPLASH_RADIUS_WATER's own comment in sand.h for the full
 * account. */
/* NOT INLINED, though it lives in a header for the one caller that is.
 * Inlining move_liquid_grain() into the sweep won 18.4% on liquid scenes
 * but grew sand_step() 975 -> 1425 instructions, because the whole helper
 * chain came with it - and that growth is paid by every cell, which cost
 * the liquid-free rows 5.5%. This body is big and its success path is
 * uncommon, so it is the wrong thing to duplicate at the call site. */
static __attribute__((noinline, unused)) void splash_displace(sand_t *s, int x, int y, uint8_t mat_id)
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
/* NOT INLINED, though it lives in a header for the one caller that is.
 * Inlining move_liquid_grain() into the sweep won 18.4% on liquid scenes
 * but grew sand_step() 975 -> 1425 instructions, because the whole helper
 * chain came with it - and that growth is paid by every cell, which cost
 * the liquid-free rows 5.5%. This body is big and its success path is
 * uncommon, so it is the wrong thing to duplicate at the call site. */
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

static inline bool move_liquid_grain(sand_t *s, uint8_t *row, uint8_t *prow,
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
