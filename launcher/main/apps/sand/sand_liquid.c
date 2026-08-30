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

/* liquid_mask() - which materials are liquid, as a bitmask over the nibble -
 * moved to sand_priv.h (still static inline) now that sand.c's own sweep needs
 * it too, to maintain BLOCK_HAS_LIQUID. See its comment there. */

/* A liquid splash: sand_displace_material() at a per-material radius,
 * called whenever a WATER or ACID grain lands hard - falling onto an
 * already-occupied surface, or its ordinary gravity-ward fall being
 * blocked by a wall or the grid edge (both call sites in
 * move_liquid_grain() below). Water and acid only, for now - oil and lava
 * are not gated in here, see SAND_SPLASH_RADIUS_WATER's own comment in
 * sand.h.
 *
 * MASKED TO `mat_id`, not a plain sand_displace() - an unmasked
 * displacement throws whatever it finds within the radius, which meant
 * pouring water over water sitting on dirt flung the dirt around too.
 * This is meant to be water/acid splashing itself, nothing else.
 *
 * WATER is gated by a chance-in-256 roll against this sand_t's own
 * decaying budget (sand_t::splash_chance, sand.h), so a displaced grain
 * landing again does not just re-splash itself indefinitely - a real
 * splash does not keep re-triggering off its own spray.
 *
 * ACID DOES NOT DECAY OVER TIME - it always splashes at its own full
 * radius, at full intensity, and never touches `splash_chance` at all (so
 * it cannot spend down water's own budget either). A balance choice, not
 * a bounce-suppression gap: acid's splash is part of its dissolve-vs-metal
 * balance (see docs/Sand/Metal-Smelting-Plan.md), and a decaying trigger
 * would make that balance depend on how many OTHER acid splashes already
 * happened this simulation, not on the material stats alone.
 *
 * ACID IS CAPPED PER STEP INSTEAD (sand_t::acid_splashes_this_step,
 * sand.h) - a bulk pour lands many columns hard on the same step or two,
 * and each one is a genuinely fresh trigger by the rule above, so without
 * this a wide pour queued a full SAND_SPLASH_RADIUS_ACID displacement per
 * column - dozens at once, reading as one chaotic explosion instead of a
 * splash. The cap forgets itself every step (sand_step(), sand.c), so it
 * throttles simultaneous triggers without making acid's splash depend on
 * history the way a decaying chance would. */
static inline void splash_displace(sand_t *s, int x, int y, uint8_t mat_id)
{
    if (mat_id == MAT_ACID) {
        if (s->acid_splashes_this_step >= SAND_SPLASH_ACID_PER_STEP_CAP) {
            return;
        }
        s->acid_splashes_this_step++;
        sand_displace_material(s, x, y, SAND_SPLASH_RADIUS_ACID, mat_id);
        return;
    }
    if (mat_id != MAT_WATER) {
        return;
    }
    if ((rng_next(&s->rng) & 0xFF) > s->splash_chance) {
        return;   /* this echo lost the roll - let the bounce die here */
    }
    sand_displace_material(s, x, y, SAND_SPLASH_RADIUS_WATER, mat_id);
    s->splash_chance =
        s->splash_chance > SAND_SPLASH_CHANCE_FLOOR + SAND_SPLASH_CHANCE_STEP
            ? (uint8_t)(s->splash_chance - SAND_SPLASH_CHANCE_STEP)
            : SAND_SPLASH_CHANCE_FLOOR;
}

/* Add `amount` of `id` to a cell that is either empty or already that same
 * material. Mass is only ever moved, never made: every caller subtracts the
 * same figure from somewhere else in the same breath.
 *
 * Returns whether `dst` was empty beforehand - true exactly when this pour
 * just turned a non-liquid cell into a liquid one, which is the only case
 * that can move where a puddle's surface sits. Every caller already computed
 * this internally before the change that added the return value; callers
 * that care feed it straight to mark_depth_band() (sand_priv.h) to catch
 * app_sand.c's LOCAL DEPTH render up on the newly-claimed cell without
 * re-marking anything for the far more common case of mass moving between
 * two cells that were already liquid. */
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
        /* pour_into()'s was_empty is deliberately IGNORED here, unlike at
         * equalise_one_cell()'s call site - this is move_liquid_grain()'s
         * per-grain fall, the hottest path in the simulation (11,130 of
         * 11,142 mark_rows() calls on a screen of water, per this
         * function's own comment above), and a grain falling into empty
         * space below it is the ORDINARY case, not the rare one equalise's
         * mark_depth_band() call exists to catch. Gating on was_empty here
         * would fire on nearly every one of those calls, reintroducing the
         * same shape of per-transfer cost
         * this function's own history (mark_rows() cut to two byte writes)
         * exists to avoid. A settled reservoir's surface only actually
         * moves once mass reaches it and levels out sideways, which is
         * equalise_one_cell()'s job, not this one's - see that call site's
         * own comment for where the real invalidation happens. */
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
/* Whether a liquid cell gets to act at all this step - material.h's
 * `mobility` field, which a liquid reads as viscosity inverted. Water is
 * 255 and therefore always moves; oil is 90 and moves on about a third
 * of its steps, so it crawls.
 *
 * s->mobility mirrors the other per-material overrides and DEFAULTS TO
 * 255, not to "per material" - which matters more here than it looks. It
 * means a test that does not ask for viscosity gets a liquid that moves
 * every step, exactly as every liquid did before this field had a second
 * reader, so nothing that was written against the old behaviour has to
 * know this exists. sand_set_mobility(SAND_MOBILITY_PER_MATERIAL) is what
 * the real app calls to opt in.
 *
 * No jostle bypass, unlike the gas pass: move_liquid_grain() is called
 * from the main sweep and never sees the jostle value. Shaking a viscous
 * liquid therefore does not thin it, which is a limitation rather than a
 * decision - it just needs the argument threading through if it ever
 * matters. */
static inline bool liquid_may_move(sand_t *s, uint8_t id)
{
    const int m = (s->mobility >= 0) ? s->mobility : materials[id].mobility;

    /* Zero reads as NO VISCOSITY here, not as "never moves", and the
     * direction of that default is the whole point. `mobility` began as a
     * gas-only field, so a liquid that does not set it is a liquid whose
     * row predates liquids reading it at all - and the honest thing for
     * such a row to do is behave the way every liquid did before the
     * field had a second reader, which is to flow freely.
     *
     * Read the other way it is a trap, and this is not hypothetical:
     * lava's row was written before liquids read this and left it unset,
     * so lava spent a while at an effective mobility of zero. Measured,
     * that was not quite frozen - a column still crept sideways, reaching
     * a point in 249 steps that takes 20 at its intended figure, because
     * the wall-rebound splash moves liquid without consulting this gate.
     * Twelve times too slow reads as "extremely viscous" rather than as
     * broken, which is exactly why it survived being looked at on a
     * device.
     *
     * That is the argument for the default failing towards water: a
     * material that quietly flows too fast is obvious, and a material
     * that quietly flows too slow is a plausible-looking design choice.
     * See test_every_liquid_declares_a_mobility, which catches the
     * omission in the table rather than trying to catch it in motion. */
    if (m == 0) {
        return true;
    }
    return m >= 255 || (int)(rng_next(&s->rng) & 0xFF) < m;
}

/* How many of the four cardinal neighbours hold a DIFFERENT liquid.
 *
 * The measure of how deep inside another fluid a cell has got: 0 or 1 for a
 * cell sitting on the boundary, 3 or 4 for one that has pushed a finger into
 * the middle of the other liquid. */
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

/* INTERFACIAL DRAG: how readily a cell keeps pushing into another liquid,
 * given how far into it it already is.
 *
 * Free at the boundary - nought or one foreign neighbour - and then halving
 * for each one beyond that, so 2 neighbours is one step in two, 3 is one in
 * four, 4 is one in eight. The halving shape is `slip`'s, from material.h:
 * "chance in 256 that a grain carrying one unit of load may still slide,
 * halving for each further unit". Same idea, different load.
 *
 * The problem it solves: two liquids exchange by swapping whole cells along
 * the gravity-ward direction, and under tilt that direction is DITHERED
 * between two octants step by step. Unchecked, every water cell with oil
 * below it swaps every step, so water drills into the oil along alternating
 * diagonals and the boundary becomes a mess of grid-aligned stripes -
 * measured at 7 water cells sitting inside the oil body on a thin slick,
 * which reads on screen as straight lines through what should be a smooth
 * surface.
 *
 * Drag does not stop the exchange, it stops the FINGER: the first cell into
 * the oil moves freely, the one behind it is already half as willing, and by
 * three deep the intrusion has effectively stalled while the broad interface
 * keeps sorting itself out normally. */
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

/* Two DIFFERENT liquids meeting, resolved by density: the denser one
 * sinks and the lighter one is pushed up into the cell it vacated. Whole
 * cells, swapped - not the mass transfer everything else in this file
 * does, because there is no sense in which a cell can be part oil and
 * part water.
 *
 * Without this, two liquids simply block each other forever. room_in()
 * refuses a cell holding any other material, so neither can give mass to
 * the other; and a liquid never consults can_enter() at all, so the
 * density rule that sorts out sand and water never runs for them. Oil
 * poured ONTO water already floats without any of this - it lands on the
 * surface and cannot get in - but oil that ends up underneath water (a
 * wave breaking over it, a pour from below, a basin filling around it)
 * would stay there permanently, which looks broken in the specific way
 * only a fluid can.
 *
 * Expressed as "the denser liquid moves DOWN" rather than "the lighter
 * one rises", and that phrasing is doing real work. Down is gravity-ward,
 * so it inherits the main sweep's own no-double-move guarantee for free -
 * the sweep runs against gravity, so the cell being swapped into has
 * already been visited and the liquid landing there cannot move again
 * this step. The mirror-image rule, written as a rise, would have been
 * anti-gravity and needed its own reversed pass, exactly as gas's does.
 * Same physics, and one of the two is nearly free. (Compare try_bubble()
 * in sand_gas.c, which could NOT be phrased this way: nothing else was
 * going to move that liquid down out of a gas's path, so it had to be a
 * rise, and it lives in the reversed gas pass because of it.)
 *
 * Strictly denser, mirroring can_enter()'s own rule: two liquids at equal
 * density block each other, which is the correct answer and not a case
 * worth special-casing.
 *
 * NOT paced by viscosity, unlike everything else a liquid does, and that
 * exception is the whole reason this comment is long.
 *
 * Gating this on `mobility` is the obvious thing - a sluggish liquid
 * ought to separate sluggishly - and it was tried, and it was measured,
 * and it was wrong. Slowing the swap does not slow separation so much as
 * PREVENT it: with the exchange and the levelling both throttled, a
 * tilted pair settles into a diagonal shear running with the tilt and
 * stays there. Not slow convergence - a fixed point. Two thousand steps
 * later the interface was still a staircase, where the ungated version
 * had a flat surface inside forty.
 *
 * The reading that makes sense of it: separation is not motion of one
 * liquid, it is the two of them being in the wrong ORDER. Sorting is a
 * correction, not a journey, and there is nothing for viscosity to
 * resist. What viscosity properly slows is a liquid flowing under its
 * own weight - falling, running downhill, finding its level - and those
 * are gated, in move_liquid_grain() and equalise_one_cell() below. Let
 * the layers form promptly and then let each of them level at its own
 * pace, and both effects come out right at once. */
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

    /* Viscosity, once, for everything this grain does under its own
     * weight this step - the fall and both slides. Rolling per attempt
     * instead would make a sluggish liquid jitter between its three
     * options rather than simply moving less often.
     *
     * Deliberately AFTER nothing and BEFORE the density swap below, which
     * is exempt - see sink_through_lighter_liquid()'s own comment for why
     * sorting two liquids into the right order is not the kind of motion
     * viscosity should resist.
     *
     * Marked unlikely, and that is a performance fix rather than
     * documentation. Without the hint this single branch cost ~26% of the
     * screen-of-water benchmark on the host, with the simulation
     * byte-identical either way - the cost is not the test, it is where
     * GCC puts the code it guards. Disassembled on the real device build:
     * unhinted, the cold "too viscous to move" blocks land at offsets 0xb6
     * and 0xce, spliced into the middle of a 1102-byte function that runs
     * about 11,130 times a step, so every fetched line of the hot path
     * carries dead bytes. Hinted, they move to the tail and the hot path is
     * contiguous again. Splitting the cold half into its own function was
     * tried and only half worked (it depends on GCC's size heuristic, which
     * a later edit can flip); marking that half noinline was worse than
     * doing nothing, because then the call itself sits in the hot path. The
     * hint is wrong for a genuinely viscous liquid like oil, which refuses
     * about two steps in three - that costs oil the far branch and is worth
     * it, because water is what a screen of liquid is usually made of. */
    if (__builtin_expect(!liquid_may_move(s, mat_id), 0)) {
        return false;
    }

    /* Before any of the mass bookkeeping: if what is directly
     * gravity-ward is a DIFFERENT, lighter liquid, the two trade places
     * whole and this grain's turn is over. Returning here rather than
     * falling through matters - row[x] now holds the OTHER liquid, and
     * the mass accounting below would happily overwrite it with this one.
     *
     * Only the straight gravity-ward direction, and it was worth checking
     * that. Offering the swap the two diagonal slides as well - the same
     * three directions the mass flow below uses - looks like it should
     * help a tilted pair sort itself out faster. Measured, it does the
     * opposite: the boundary between the two liquids came out roughly
     * 60% MORE ragged (its spread along the gravity axis went from 6,600
     * to 10,800 in one box size and 9,000 to 13,600 in another), because
     * three chances a step to swap churns the interface faster than the
     * levelling passes can flatten it. One direction, and let
     * equalise_liquids() do the smoothing. */
    if (sink_through_lighter_liquid(s, row, prow, x, y, tx0, ty0, w, grain,
                                    &materials[mat_id])) {
        return true;
    }

    /* Checked BEFORE give_mass() writes into the target - afterward it
     * never reads as empty again. */
    const bool target_occupied = (unsigned)tx0 < (unsigned)w && prow != NULL
        && !CELL_IS_EMPTY(prow[tx0]);

    /* Set below, in the wall/edge branch, and consulted only after row[x]
     * has its final byte written - see that branch's own comment for why
     * firing splash_displace() immediately, at decision time, queued an
     * impulse this grain's own later diagonal slides could still
     * invalidate before it ever got a turn to move. */
    bool wall_splash = false;

    const int down = give_mass(s, prow, tx0, w, mass, mat_id, y, ty0);
    mass -= down;
    if (down > 0) {
        moved = true;

        /* A drop that was exposed to open space one step away from
         * gravity (not buried inside the body) landing on an already-
         * occupied liquid surface throws a small splash - see
         * splash_displace()'s own comment above. */
        if (target_occupied) {
            const uint8_t *arow = dest_row(s, y - dy);
            const int ax = x - dx;
            const bool exposed = arow == NULL || (unsigned)ax >= (unsigned)w
                                  || CELL_IS_EMPTY(arow[ax]);
            if (exposed) {
                splash_displace(s, tx0, ty0, mat_id);
            }
        }
    } else if ((unsigned)tx0 >= (unsigned)w || prow == NULL ||
               (target_occupied &&
                material_of(prow[tx0])->kind == KIND_STATIC)) {
        /* down == 0 with an occupied target is ambiguous by itself - a
         * DIFFERENT liquid blocks the same way a wall does (room_in()
         * returns 0 for a material mismatch either way) - so KIND_STATIC
         * narrows the IN-BOUNDS case to an actual wall or floor, which
         * give_mass()'s own room-based check can never distinguish from
         * "blocked by more liquid" on its own.
         *
         * THE GRID EDGE HAS NO CELL TO CHECK THE MATERIAL OF - `tx0`
         * out of bounds, or `prow` NULL (dest_row() off the grid), is the
         * ordinary way this simulation's own outer boundary blocks a
         * move, with no explicit wall cell there at all (see sand_at()'s
         * own comment on reading out-of-bounds as solid). Without this
         * half of the check, a real device board - which has no drawn
         * border, only the grid's own edge - never satisfied
         * `target_occupied` at all (prow == NULL short-circuits it),
         * so nothing here ever fired outside of a test fixture that
         * happened to paint an explicit STONE floor.
         *
         * QUEUED HERE, FIRED AFTER row[x] IS WRITTEN, below - not called
         * immediately the way the target_occupied branch's splash is.
         * This grain's own diagonal slides (the "DOWN THE SLOPE" loop
         * right after this whole if/else) can still shrink `mass` at
         * THIS exact cell before the function returns; row[x] does not
         * get its final byte until the very end. Queuing immediately
         * here captured the PRE-slide byte, which step_impulses() later
         * found did not match what was actually still sitting at (x, y)
         * once the slide had run - the entry failed re-acquisition (see
         * that check's own comment in step_impulses(), sand.c) and was
         * silently dropped every time, never once surviving to move.
         * Splashes at (x, y), the grain's OWN position - it did not
         * move, unlike the target_occupied branch above where the splash
         * originates from where mass actually landed. Same exposed-to-
         * open-space gate as the liquid-landing case, for the same
         * reason: without it, a settled pool resting on the floor would
         * splash every single step forever. */
        const uint8_t *arow = dest_row(s, y - dy);
        const int ax = x - dx;
        wall_splash = arow == NULL || (unsigned)ax >= (unsigned)w
                      || CELL_IS_EMPTY(arow[ax]);
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

    /* Fired here, not at the wall/edge branch above that set the flag -
     * see wall_splash's own comment for why: row[x] just got its final
     * byte, so the impulse this queues will still match what step_
     * impulses() finds there later, instead of a byte this grain's own
     * diagonal slides have since changed underneath it. */
    if (wall_splash) {
        splash_displace(s, x, y, mat_id);
    }

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
 * with anywhere to send anything.
 *
 * That early-out depends on `there < MASS_MAX` staying part of the test,
 * not just the level comparison beside it. Once a bias is in play an
 * EQUAL-mass neighbour can legitimately read as lower - the bias is what
 * makes a settled surface tilt at all - so the plain "is it lower" half of
 * the test alone would send every interior cell of a full pool on a full
 * sight walk, the exact cost this function exists to avoid. A full
 * neighbour has no room regardless of any bias, so `there < MASS_MAX`
 * rejects it in one comparison and keeps the early-out intact. */
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
    /* A cell of some OTHER material reads as full, which rejects it on the
     * same comparison that rejects a full one of our own - one branch for
     * both, rather than the two the obvious spelling costs. */
    const int there = CELL_IS_EMPTY(n) ? 0
                    : (CELL_MATERIAL(n) == id ? CELL_VARIANT(n) : MASS_MAX);
    return there < MASS_MAX && (there << 8) - bias_q8 < (mass << 8);
}

/* The shallowest place this liquid can reach along (px, py), within `sight`
 * cells, how many steps away it is, and the level drop to it, in 1/256 mass
 * units - which is what the caller halves to decide the transfer. "Shallowest"
 * now means lowest in LEVEL, not lowest in mass: a farther cell that is
 * physically fuller can still be lower once the bias accumulated by the
 * steps to reach it is taken into account, and it is that cell, not the
 * merely-lightest one, that this walk returns. Flow stops at anything that
 * is not the same liquid, so it cannot reach through a wall. */
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

    /* Written once, after the walk, rather than through the pointers inside
     * it: the walk tracks its best find in locals as it goes, and this
     * function now hands back three results rather than two, one of them
     * the return value itself - keeping all three together at the exit
     * reads better than three stores scattered through the loop. */
    *lowest = low;
    *at = k_at;
    return mine - best;
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
        return false;   /* viscosity applies to levelling too - without it
                         * a syrupy liquid would still find its own level
                         * instantly sideways, which is most of what
                         * "runny" looks like */
    }

    int lowest, at;
    const int drop_q8 = find_shallowest(s, x, y, px, py, sight, id, mass,
                                        bias_q8, &lowest, &at);

    /* Half the difference in LEVEL, not in mass - `drop_q8` is a level drop
     * in 1/256 mass units (see find_shallowest()), and handing over all of
     * it would only move the imbalance rather than settle it, the two would
     * trade places for ever. `>> 9` is a halving and a q8-to-whole-mass
     * conversion done in one shift: when bias_q8 is zero (an axis-aligned or
     * exactly-45-degree ray - see build_xflow()) drop_q8 is exactly
     * (mass - lowest) << 8, and >> 9 on that is bit-for-bit the old
     * `(mass - lowest) / 2`, because (2k*256) >> 9 == k and
     * ((2k+1)*256) >> 9 == k. */
    int give = drop_q8 >> 9;
    if (__builtin_expect(at == 0 || give <= 0, 1)) {
        return false;
    }
    /* Two clamps the old rule never needed, because it only ever moved half
     * of a difference in MASS and so could not overrun either end. A level
     * difference can be far larger than the mass on hand or the room at the
     * far end - a steep tilt puts several cells of head between two cells
     * eight apart - so both ends are pinned here. Cold: on a body of water
     * that is finding its level, the ordinary transfer is a unit or two. */
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

    /* was_empty gates mark_depth_band() below: this transfer just moved
     * mass ONTO a cell that had none, which is the only case that can shift
     * a puddle's surface - see mark_depth_band()'s own comment
     * (sand_priv.h). The far commoner case here, topping up a neighbour
     * that already held some of the same liquid, leaves the depth topology
     * untouched, so it stays exactly as cheap as it always was: only
     * mark_rows()'s ordinary two-row mark. */
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
                                         bool diagonal, const xflow_t *r,
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

    /* Which of the two rays this cell levels along - resolved HERE, past the
     * two checks above, and not in the block loop that calls this: most of
     * the cells a scanned block holds are empty or not liquid, and picking a
     * ray for them cost about 10% of the whole host water benchmark step
     * versus choosing it after those checks. */
    const int px = diagonal ? r->dg[0] : r->ax[0];
    const int py = diagonal ? r->dg[1] : r->ax[1];
    const int bias_q8 = diagonal ? r->bias_dg_q8 : r->bias_ax_q8;

    bool stayed_in_row = false;
    int  tx = 0;
    if (equalise_one_cell(s, row, x, y, px, py, dx, dy, sight, id,
                          CELL_VARIANT(c), bias_q8, &stayed_in_row, &tx) &&
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
                                      const xflow_t *r, int dx, int dy,
                                      int sight, uint16_t is_liquid,
                                      bool *touched, int *touched_x0,
                                      int *touched_x1)
{
    bool any_liquid = false;

    const int q_q8 = r->q_q8;

    /* Which columns take the diagonal ray: a fixed pattern in space, of
     * density q_q8/256 - see xflow_t. Along the axis the ray does not move
     * in, the answer is the same for the whole span, so it is settled once
     * here rather than per cell; along the other, it walks by q_q8 a cell,
     * which is the same running remainder a line-drawer keeps. */
    const bool x_major = (r->ax[0] != 0);
    int pat = x_major ? ((q_q8 * cx_from) & 255) : ((q_q8 * y) & 255);
    const int pat_step = x_major ? ((x_step > 0) ? q_q8 : -q_q8) : 0;

    for (int x = cx_from; x != cx_to; x += x_step) {
        const bool diagonal = (pat < q_q8);
        pat = (pat + pat_step) & 255;
        if (equalise_one_row_cell(s, row, x, y, diagonal, r,
                                  dx, dy, sight,
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
static bool equalise_one_row(sand_t *s, int y, int w, int x_step,
                             const xflow_t *r, int dx, int dy, int sight,
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
                               x_step, r, dx, dy, sight, is_liquid,
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

static void equalise_liquids(sand_t *s, const xflow_t *f, int sight,
                             int dx, int dy)
{
    bool found_any = false;

    if (s->block_state != NULL) {
        mark_liquid_neighbourhoods(s);
    }

    /* Sweep order is pinned by the ray that leaves the row/column, which is
     * the diagonal one - the axis ray never leaves its own row, so it is
     * safe under either order. */
    const int px = f->dg[0];
    const int py = f->dg[1];
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
        if (equalise_one_row(s, y, w, x_step, f, dx, dy,
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

void sand_step_liquids(sand_t *s, const xflow_t *flow, int dx, int dy)
{
    if (!s->may_have_liquid) {
        return;
    }

    /* The two senses across the flow are exact opposites of each other, so
     * the reverse one is the forward one negated - directions and the
     * potential each step costs alike. */
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

    /* Cross-flow, in its own pass and alternating which way each step - so a
     * tilted pool levels both ways instead of walking into one corner. See
     * equalise_liquids(). */
    equalise_liquids(s, &run, SAND_LIQUID_SIGHT, dx, dy);
    s->liquid_flip = !s->liquid_flip;
}
