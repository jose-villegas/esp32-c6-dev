/*=============================================================================
 * sand_impulse - grains, chunks and splashes in flight: the OUTWARD half of
 * the simulation, not gravity-ward.
 *
 * sand_step()'s gravity sweep (sand.c) settles every ordinary grain toward
 * gravity, one cell at a time. An impulse - a thrown chunk, an explosion's
 * blast, a splash's pushback - moves the opposite way: OUTWARD, against or
 * across gravity, for as many cells as its queued speed still allows. That
 * is a different enough rule that it cannot share the sweep's own pass, so
 * it lives here instead.
 *
 * step_impulses() is still called from sand_step(), exactly once, right
 * before finalize_settling() - see that call site's own comment for why it
 * has to run LAST, after every other pass that can move a cell: running
 * last is what turns a plain outward push into a ballistic arc for free,
 * since gravity has already pulled by the time this pass gets a turn. That
 * single call is this file's one seam back into sand.c, the same shape
 * sand_step_liquids() (sand_liquid.c) and sand_step_gas() (sand_gas.c) each
 * already have.
 *===========================================================================*/

#include "sand_priv.h"

/* Integer floor(sqrt(v)) for exact_disc_count() - Newton's method, converges
 * quickly (v ≤ grid dimension squared, a few hundred thousand). Not the same
 * as tilt.c's isqrt64(), which is static and for int64_t accelerometer
 * readings. A smaller, local version is more efficient. */
static int isqrt_floor(int v)
{
    if (v <= 0) {
        return 0;
    }
    int x = v;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

/* Exact lattice cell count inside a disc radius r; sum of 2*floor(sqrt(r*r -
 * dy*dy)) + 1 for each row dy from -r to r, in O(r) integer square roots. */
static int exact_disc_count(int radius)
{
    if (radius < 0) {
        return 0;
    }
    const int r2 = radius * radius;
    int count = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        const int rem = r2 - dy * dy;
        if (rem < 0) {
            continue;
        }
        count += 2 * isqrt_floor(rem) + 1;
    }
    return count;
}

/* Forward-declared: the shared implementation behind both sand_impulse()
 * and this file's own annulus seeding lives right next to sand_impulse()
 * itself, further down, not up here next to its other caller - see that
 * function's own comment for why one body serves both. */
static void queue_flying_grain(sand_t *s, int x, int y, int dir, int speed,
                               bool allow_dislodge_static, int mat_filter,
                               bool guaranteed_dislodge, int ramp);

/* `r2` enforces the true circular disc though the caller loop walks square
 * Chebyshev shells (shell order picks the SEQUENCE, this decides
 * MEMBERSHIP). `disc_count`/`keep`/`*accum` self-limit via a DDA, not a
 * modulo stride (aliases with the ring's own edge lengths), degrading
 * DENSITY evenly instead of truncating the SHAPE. */
static void queue_outward_impulse(sand_t *s, int cx, int cy, int dx, int dy,
                                  int r2, int disc_count, int keep, int *accum,
                                  int mat_filter)
{
    if (dx * dx + dy * dy > r2) {
        return;
    }

    *accum += keep;
    if (*accum < disc_count) {
        return;
    }
    *accum -= disc_count;

    /* (dx, dy) is the vector from the centre to this cell. Using the same
     * quantiser as gravity gives "away from the centre" in one of eight
     * directions. The centre cell (dx, dy) = (0, 0) is reported as no
     * direction, leaving it unchanged. It spends one unit of `keep` due to a
     * fixed, one-time rounding cost. */
    int qdx, qdy;
    sand_gravity_direction(dx, dy, &qdx, &qdy);
    if (qdx == 0 && qdy == 0) {
        return;
    }

    /* DELIBERATELY - a distance-scaled falloff worsened the blast when
     * tried: cells that escape the footprint are disproportionately near
     * the outer radius, with least distance left to travel, and falloff
     * there guts that evidence. Flat speed stays until a falloff is found
     * that doesn't trade the outer annulus for a hotter core. */

    /* true HERE, AND ONLY HERE - the one caller that turns a wall's
     * KIND_STATIC refusal from a certainty into a density-scaled chance
     * (see queue_flying_grain()). Everything else about the dislodged
     * cell's flight is identical to any other entry. */
    queue_flying_grain(s, cx + dx, cy + dy, ring_of(qdx, qdy),
                       SAND_EXPLODE_INITIAL_SPEED, true, mat_filter, false,
                       SAND_IMPULSE_SPEED_RAMP);
}

/* SHARED IMPLEMENTATION behind sand_impulse() and sand_explode()'s own
 * annulus seeding, so the bounds/empty/buffer-full checks stay in one
 * place. `allow_dislodge_static` and `ramp` (speed decay) differ per
 * caller. */
static void queue_flying_grain(sand_t *s, int x, int y, int dir, int speed,
                               bool allow_dislodge_static, int mat_filter,
                               bool guaranteed_dislodge, int ramp)
{
    /* Disabled, or already full - see sand_impulse()'s own comment in
     * sand_impulse.h on why both are silent no-ops rather than something a
     * caller has to check for itself first. */
    if (s->impulse_buf == NULL || s->impulse_count >= s->impulse_max) {
        return;
    }
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return;
    }

    const size_t at = (size_t)y * (size_t)s->w + (size_t)x;
    const cell_t cell = s->cells[at];
    if (CELL_IS_EMPTY(cell)) {
        return;   /* nothing there to throw */
    }

    /* mat_filter < 0 means "any material", typical for sand_impulse() or
     * unmasked sand_displace(). sand_displace_material() is an exception,
     * preventing a liquid's splash from flinging nearby objects. */
    if (mat_filter >= 0 && CELL_MATERIAL(cell) != (uint8_t)mat_filter) {
        return;
    }

    /* WALL CANNOT BE THROWN BY DEFAULT, any more than one can be entered -
     * can_impulse_enter() gates the DESTINATION, this gates the SOURCE.
     * `allow_dislodge_static` uses `255 - density` for chance: LOWER
     * density means HIGHER chance. */
    if (material_of(cell)->kind == KIND_STATIC) {
        if (!allow_dislodge_static) {
            return;
        }
        /* `guaranteed_dislodge` bypasses the roll above, for
         * sand_impulse_dislodge()'s caller, which wants a KIND_STATIC
         * target moved unconditionally rather than toughness-scaled. */
        if (!guaranteed_dislodge) {
            const int chance = 255 - (int)material_of(cell)->density;
            if ((int)(rng_next(&s->rng) & 0xFF) >= chance) {
                return;   /* the roll failed - the wall holds, same as always */
            }
        }
    }

    /* REFUSE A SECOND ENTRY FOR A CELL ALREADY TRACKED - two entries both
     * claiming to be the one grain at `at` is a bookkeeping error. Matched
     * on `cell` too, not index alone: queuing happens during reactions,
     * before step_impulses()'s own re-acquisition runs, so a stored index
     * can be stale here. */
    for (int existing = 0; existing < s->impulse_count; existing++) {
        if (s->impulse_buf[existing].index == (uint16_t)at &&
            s->impulse_buf[existing].cell == cell) {
            return;
        }
    }

    /* A pane knocked loose is a pane broken: it flies on as cullet,
     * carrying the push that dislodged it rather than sailing off as an
     * intact sheet. Written to the grid as well as the entry - flight
     * matches `cell` against what is actually there before moving it. */
    cell_t flying = cell;
    if (CELL_MATERIAL(cell) == MAT_GLASS) {
        flying = cullet_cell(s);
        s->cells[at] = flying;
        latch_content_flags(s, flying);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
    }

    impulse_t *entry = &s->impulse_buf[s->impulse_count++];
    entry->index = (uint16_t)at;
    entry->cell  = flying;
    entry->dir   = (uint8_t)dir;
    entry->speed = (uint8_t)speed;
    entry->ramp  = (uint8_t)ramp;
}

void sand_enable_impulses(sand_t *s, impulse_t *buf, int max)
{
    s->impulse_buf   = buf;
    s->impulse_max   = (buf != NULL) ? max : 0;
    /* Nothing can already be in flight against a buffer that was just handed
     * over - the same reasoning sand_enable_sleeping() gives for zeroing
     * `blocks`, applied to a count rather than a memset since there is no
     * grid content of `buf`'s own to know anything about yet. */
    s->impulse_count = 0;
}

void sand_impulse(sand_t *s, int x, int y, int dir, int speed)
{
    queue_flying_grain(s, x, y, dir, speed, false, -1, false,
                       SAND_IMPULSE_SPEED_RAMP);
}

/* Static-wall version of sand_impulse() with `allow_dislodge_static` true and
 * `guaranteed_dislodge` set. Skips density-scaled toughness roll. `ramp`
 * passed to impulse_t for custom decay. */
void sand_impulse_dislodge(sand_t *s, int x, int y, int dir, int speed,
                           int ramp)
{
    queue_flying_grain(s, x, y, dir, speed, true, -1, true, ramp);
}

/* Shared logic in `sand_displace()` and `sand_displace_material()`. Annulus
 * math, buffer-sharing, and ring-order scan are independent of the material
 * filter. `mat_filter` and `guaranteed_dislodge` are passed to
 * `queue_outward_impulse()` and `queue_flying_grain()`. See comments for
 * details. */
static void displace_disc(sand_t *s, int cx, int cy, int radius,
                          int mat_filter, bool guaranteed_dislodge)
{
    if (s->impulse_buf == NULL) {
        return;   /* sand_enable_impulses() was never called - see its comment */
    }

    /* RING ORDER (Chebyshev distance), NOT ROW ORDER: an undersized buffer's
     * cap would otherwise land entirely in the disc's top rows before the
     * scan reaches the core or lower half. The r2 check still enforces the
     * true circular disc regardless of ring - ring order only changes the
     * SEQUENCE cells are offered in, never which cells qualify. */
    const int r2 = radius * radius;

    /* `disc_count` is exact_disc_count()'s own EXACT count, not a safe
     * over-estimate - fine for sizing the buffer itself, but an overshoot
     * would compute `keep` too low for small discs. `keep` is sized
     * against `room`, not `s->impulse_max` - DO NOT simplify: a second
     * displacement mid-arc would otherwise size its density as if the
     * whole buffer were free - see
     * test_two_overlapping_blasts_share_the_buffer_evenly. */
    const int disc_count = exact_disc_count(radius);
    const int room = s->impulse_max - s->impulse_count;
    const int keep = (disc_count < room) ? disc_count : room;
    int accum = 0;

    queue_outward_impulse(s, cx, cy, 0, 0, r2, disc_count, keep, &accum,
                          mat_filter);
    for (int ring = 1; ring <= radius; ring++) {
        for (int dx = -ring; dx <= ring; dx++) {
            queue_outward_impulse(s, cx, cy, dx, -ring, r2, disc_count, keep,
                                  &accum, mat_filter);
            queue_outward_impulse(s, cx, cy, dx,  ring, r2, disc_count, keep,
                                  &accum, mat_filter);
        }
        for (int dy = -ring + 1; dy <= ring - 1; dy++) {
            queue_outward_impulse(s, cx, cy, -ring, dy, r2, disc_count, keep,
                                  &accum, mat_filter);
            queue_outward_impulse(s, cx, cy,  ring, dy, r2, disc_count, keep,
                                  &accum, mat_filter);
        }
    }
}

void sand_displace(sand_t *s, int cx, int cy, int radius)
{
    displace_disc(s, cx, cy, radius, -1, false);
}

/* Like sand_displace(), queues only cells with material `mat_id`; ignores
 * others. For a liquid's splash: water lands hard, throws water, not
 * underlying dirt. */
void sand_displace_material(sand_t *s, int cx, int cy, int radius,
                            uint8_t mat_id)
{
    displace_disc(s, cx, cy, radius, (int)mat_id, false);
}

void sand_explode(sand_t *s, int cx, int cy, int radius)
{
    if (s->impulse_buf == NULL) {
        return;   /* sand_enable_impulses() was never called - see its comment */
    }

    /* FILLS a cavity with fire before queuing any flight entries - see
     * SAND_EXPLODE_CORE_DIVISOR's own comment in sand_impulse.h. Every cell
     * in the core is written unconditionally, occupied or empty alike.
     * Written by hand rather than through place_cell() (sand_reactions.c),
     * which is static to that file. */
    const int core_radius_raw = radius / SAND_EXPLODE_CORE_DIVISOR;
    /* Clamped to a minimum core_radius of 1 for radius >= 2 - see
     * SAND_EXPLODE_CORE_DIVISOR's own comment in sand_impulse.h. radius 1 is
     * excluded: it stays at core_radius 0, the single centre cell every
     * test already assumes. */
    const int core_radius = (core_radius_raw == 0 && radius >= 2)
                                 ? 1 : core_radius_raw;
    const int core_r2 = core_radius * core_radius;
    for (int fdy = -core_radius; fdy <= core_radius; fdy++) {
        for (int fdx = -core_radius; fdx <= core_radius; fdx++) {
            if (fdx * fdx + fdy * fdy > core_r2) {
                continue;
            }
            const int fx = cx + fdx;
            const int fy = cy + fdy;
            if (fx < 0 || fx >= s->w || fy < 0 || fy >= s->h) {
                continue;
            }
            const size_t fat = (size_t)fy * (size_t)s->w + (size_t)fx;
            /* Life is floored at 1, variant 0 indicates an expired fire. */
            int life = MATERIAL_VARIANTS - 1;
            if (core_r2 > 0) {
                life -= (fdx * fdx + fdy * fdy) * SAND_EXPLODE_CORE_FADE / core_r2;
                if (life < 1) {
                    life = 1;
                }
            }
            const cell_t fire = CELL_MAKE(MAT_FIRE, (uint8_t)life);
            s->cells[fat] = fire;
            latch_content_flags(s, fire);
            mark_rows(s, fy, fy);
            wake_block_and_neighbors(s, fx, fy);
        }
    }

    sand_displace(s, cx, cy, radius);
}

/* Whether a FLYING grain may swap into `target` - unlike can_enter(), which
 * requires the mover be DENSER (useless here, a buried grain sits in
 * identical-density material). Density does not gate this. KIND_STATIC IS
 * STILL A WALL, keeping a blast inside a sealed vessel. A FLYING LIQUID
 * mover is refused against anything but another liquid, so a splash cannot
 * tunnel through a powder bed. */
static inline bool can_impulse_enter(cell_t target, cell_t mover)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }
    const material_t *t = material_of(target);
    if (t->kind == KIND_STATIC) {
        return false;
    }
    return material_of(mover)->kind != KIND_LIQUID || t->kind == KIND_LIQUID;
}

/* SHARED PREDICATE in `step_impulses()` uses speed gating in
 * `can_impulse_enter()`. Fixes gravity-drift and settled check.
 * `impulse_gravity_candidates()` applies. At or above
 * `SAND_IMPULSE_SINK_MIN_SPEED`, drag stops sideways but not downward. Below,
 * SPENT only enters empty cells. */
static inline bool can_impulse_enter_gravity_ward(cell_t target, cell_t mover,
                                                  uint8_t speed)
{
    if (speed < SAND_IMPULSE_SINK_MIN_SPEED) {
        /* Packed grain holds chunks near the rim; fluids part around solids.
         * Exhausted chunks check the medium; only powder and walls stop them. */
        return CELL_IS_EMPTY(target) ||
               material_of(target)->kind == KIND_LIQUID;
    }
    return can_impulse_enter(target, mover);
}

/* Fills `cand` with three cells gravity-ward of (x, y) in order: straight
 * down, then diagonals. Uses same geometry and order as `step_one_grain()`
 * (sand.c). Shared by gravity-drift and settled check in `step_impulses()`
 * to avoid divergent candidate lists. */
static void impulse_gravity_candidates(int x, int y, int dx, int dy,
                                       int cand[3][2])
{
    const int i_dir = ring_of(dx, dy);
    const int *slide_a = ring_dir(i_dir + 7);
    const int *slide_b = ring_dir(i_dir + 1);
    cand[0][0] = x + dx;          cand[0][1] = y + dy;
    cand[1][0] = x + slide_a[0];  cand[1][1] = y + slide_a[1];
    cand[2][0] = x + slide_b[0];  cand[2][1] = y + slide_b[1];
}

/* `index` occupied by TRACKED impulse entry during step_impulses() loop; rare
 * case in three-way gate check. O(entries) against buffer max 2048
 * (APP_IMPULSE_MAX). Skipped range [kept, self_i) is scratch from emptied
 * entries. */
static bool impulse_index_still_tracked(const sand_t *s, int kept, int self_i,
                                        uint16_t index)
{
    for (int j = 0; j < kept; j++) {
        if (s->impulse_buf[j].index == index) {
            return true;
        }
    }
    for (int j = self_i + 1; j < s->impulse_count; j++) {
        if (s->impulse_buf[j].index == index) {
            return true;
        }
    }
    return false;
}

/* Both step_impulses() call sites (KIND_STATIC, KIND_POWDER) used to write
 * out the same three-candidate scan by hand, which let them diverge -
 * shared here instead. `cand_out` is always filled; KIND_STATIC reads
 * `cand_out[0]` post-false to tell wall from support, KIND_POWDER ignores
 * it. */
static bool impulse_has_opening(const sand_t *s, int x, int y, int dx, int dy,
                                cell_t mover, uint8_t speed, int cand_out[3][2])
{
    impulse_gravity_candidates(x, y, dx, dy, cand_out);
    for (int c = 0; c < 3; c++) {
        if (can_impulse_enter_gravity_ward(
                sand_at(s, cand_out[c][0], cand_out[c][1]), mover, speed)) {
            return true;
        }
    }
    return false;
}

/* ONE RAMP CHARGE, for `cells` cells of it - shared by step_impulses()'s
 * two charge sites instead of each re-stating the water/acid-vs-rest
 * branch. MAT_WATER/MAT_ACID decay geometrically; everything else,
 * linearly via entry->ramp. Every entry pays the same total either way a
 * step goes. */
static void impulse_decay(impulse_t *entry, uint8_t mat_id, int cells)
{
    if (mat_id == MAT_WATER || mat_id == MAT_ACID) {
        for (int i = 0; i < cells; i++) {
            entry->speed = (uint8_t)(entry->speed -
                                     (entry->speed >> SAND_SPLASH_SPEED_DECAY_SHIFT));
        }
        return;
    }
    const unsigned total = (unsigned)entry->ramp * (unsigned)cells;
    entry->speed = (entry->speed > total) ? (uint8_t)(entry->speed - total) : 0;
}

/* Shared by push and gravity-drift, charged identically: a free drift
 * swap once let a mostly-spent chunk tunnel through with no drag.
 * `impact_speed` is captured before drag touches `entry->speed` -
 * transfer derives from what was LOST, not what is left. `dir_for_transfer`
 * is not always `entry->dir`: the drift can displace along a heading it
 * never moved through. */
static void impulse_charge_displacement(sand_t *s, impulse_t *entry,
                                        size_t new_index, int dir_for_transfer,
                                        impulse_t *deferred,
                                        int *deferred_transfer_count)
{
    const int w = s->w;
    const size_t old_index = entry->index;
    const cell_t displaced = s->cells[new_index];
    const uint8_t impact_speed = entry->speed;

    if (!CELL_IS_EMPTY(displaced) &&
        (material_of(entry->cell)->kind == KIND_STATIC ||
         material_of(entry->cell)->kind == KIND_POWDER)) {
        const uint8_t drag = impulse_drag_of(displaced);
        entry->speed = (entry->speed > drag) ? (uint8_t)(entry->speed - drag)
                                              : 0;

        if (impact_speed >= SAND_IMPULSE_TRANSFER_MIN_SPEED &&
            *deferred_transfer_count < SAND_CASCADE_TRANSFER_MAX_PER_STEP) {
            /* Throw only cells with open air; surface cells dominate the
             * budget. Random scan avoids sheet-like ejection. sand_at()
             * ensures no out-of-world cells. */
            const int ex = (int)((unsigned)old_index % (unsigned)w);
            const int ey = (int)((unsigned)old_index / (unsigned)w);
            const int base = dir_for_transfer + 3;
            const unsigned first = rng_below(&s->rng, 3);
            int chosen = -1;

            for (unsigned k = 0; k < 3u && chosen < 0; k++) {
                const int cand = (base + (int)((first + k) % 3u)) & 7;
                const int *cd = ring_dir(cand);
                if (CELL_IS_EMPTY(sand_at(s, ex + cd[0], ey + cd[1]))) {
                    chosen = cand;
                }
            }
            if (chosen >= 0) {
                impulse_t *t = &deferred[(*deferred_transfer_count)++];
                t->index = (uint16_t)old_index;
                t->cell  = displaced;
                t->dir   = (uint8_t)chosen;
                t->speed = (uint8_t)(((unsigned)impact_speed *
                                      SAND_IMPULSE_TRANSFER_KEEP) >> 8);
            }
            /* chosen < 0 means buried - no surface to leave by - so nothing
             * is queued, but the displacement below still happens: burial
             * is a fact about the EJECTA, not about whether the mover
             * itself still moves into the cell it just paid drag for. */
        }
    }

    s->cells[new_index] = entry->cell;
    s->cells[old_index] = displaced;
    latch_content_flags(s, entry->cell);
    /* Displaced occupant, if any, existed at `new_index`. Bookkeeping costs
     * one more latch regardless of occupancy, skipped only when nothing to
     * displace. */
    if (!CELL_IS_EMPTY(displaced)) {
        latch_content_flags(s, displaced);
    }
    mark_move(s, (int)((unsigned)old_index % (unsigned)w),
             (int)((unsigned)old_index / (unsigned)w),
             (int)((unsigned)new_index % (unsigned)w),
             (int)((unsigned)new_index / (unsigned)w));
    entry->index = (uint16_t)new_index;
}

/* The flight pass: every entry in s->impulse_buf either moves one cell
 * along its queued direction, waits another turn, or is finally dropped.
 * Called from sand_step(), immediately before finalize_settling() - see
 * docs/Sand/Explosion-Plan.md's "Where the pass runs, and why it must be
 * LAST": running after every pass that can move a cell keeps an entry's
 * position honest, and turns a plain outward push into a ballistic arc
 * for free, since gravity has already pulled by the time this runs. */

/* BLOCKED MEANS WAIT, NOT STOP. A cell in the way used to drop the entry
 * on the spot - fine for open air, wrong for anything packed: an
 * explosion into a bed of sand or water starts with every queued cell
 * surrounded by more of the same material, so that rule dropped nearly
 * everything on its first turn. */

/* Only the annulus already touching open space (or the fire-filled core
 * sand_explode() now writes - see SAND_EXPLODE_CORE_DIVISOR in
 * sand_impulse.h - which a denser neighbour can swap straight through)
 * ever went anywhere. */

/* Keeping a blocked entry instead lets it try again next step, once
 * whatever was ahead of it has had a chance to move out of the way -
 * which is what lets the disturbance the core's fire starts unpack
 * outward over several steps instead of being a single frozen ring. */

/* WAIT STILL HAPPENS, BUT ONLY AGAINST A TRUE WALL NOW - see
 * can_impulse_enter()'s own comment just above this function for why a
 * flying grain shoulders aside any non-static occupant it meets instead of
 * only ever moving into a genuinely empty cell. This narrows "blocked" to
 * KIND_STATIC and the grid edge, but the branch is still needed:
 * wait-then-retry keeps an entry pinned rather than dropped the instant
 * it arrives. */

/* `dx`/`dy` is this step's own dithered gravity direction, the same one
 * sand_step()'s main sweep just used - needed for RE-ACQUISITION, below,
 * which is what makes a thrown grain arc at all rather than flying dead
 * straight for exactly one cell. Without it, a stale stored index fails
 * the identity check and the entry is dropped after one hop - lateral
 * scatter out of a crater, not an arc. */

/* The sweep runs BEFORE this pass, every step, on every ordinary cell
 * including ones this list still has an eye on: an airborne grain
 * sitting in open air is not special to the sweep, so gravity moves it
 * down one cell before this pass ever gets a turn on it that step. */

/* A single `if` with nothing queued, which is every step on a board with
 * nothing in flight - the same shape sand_step_gas()'s own may_have_gas
 * gate gives a board with no gas on it. */
void step_impulses(sand_t *s, int dx, int dy)
{
    if (s->impulse_count == 0) {
        return;
    }

    const int w = s->w;
    const int h = s->h;
    int kept = 0;

    /* DEFERRED follow-up entries (CASCADE relays and TRANSFER hits alike),
     * appended only after the main loop finishes: queuing mid-loop would
     * grow s->impulse_count while the loop's own bound still reads it,
     * letting a freshly-queued entry be revisited the same pass. TWO
     * COUNTERS, ONE ARRAY: transfer writes from the front (capped at
     * SAND_CASCADE_TRANSFER_MAX_PER_STEP), cascade from the back (capped
     * at whatever transfer left unused) - neither can starve the other. */
    impulse_t deferred[SAND_CASCADE_MAX_PER_STEP];
    int deferred_transfer_count = 0;
    int deferred_cascade_count = 0;

    for (int i = 0; i < s->impulse_count; i++) {
        impulse_t entry = s->impulse_buf[i];

        /* Verify before moving: nothing marks a cell "spoken for", so
         * gravity or a reaction may have touched it since. RE-ACQUIRE by
         * checking the three cells step_one_grain() could have moved this
         * grain to, for a byte-for-byte match - same material and variant
         * is the same GRAIN anywhere else in this file. Never adopt a
         * DIFFERENT byte. */
        if (s->cells[entry.index] != entry.cell) {
            bool reacquired = false;

            /* HEAT-RAMPING MATERIALS FIRST, CHECK POSITION ONLY. VARIANT
             * NIBBLE DRIFTS NEAR HEAT. BLOCKED ENTRIES DON'T MOVE, CAUSING
             * MISMATCH. TRYING MOVEMENT FIRST MAY CAUSE FALSE POSITIVES,
             * ESPECIALLY WITH COVERED LAVA (bd esp32c6-mqt,
             * sand_reactions.c). */
            const uint8_t lost_mat = CELL_MATERIAL(entry.cell);
            /* reaction_of(entry.cell), not reactions[lost_mat] - lost_mat is
             * high nibble shared by statics and gunpowder in MAT_EXTENDED
             * range; reaction_of() handles this, while reactions[lost_mat]
             * incorrectly reads reactions[MAT_EXTENDED]. */
            if (reaction_of(entry.cell)->heat_ramp != 0) {
                const cell_t here = s->cells[entry.index];
                if (!CELL_IS_EMPTY(here) && CELL_MATERIAL(here) == lost_mat) {
                    entry.cell = here;
                    reacquired = true;
                }
            }

            const int ox = (int)((unsigned)entry.index % (unsigned)w);
            const int oy = (int)((unsigned)entry.index / (unsigned)w);
            /* THE SAME THREE CANDIDATES impulse_gravity_candidates() (above)
             * already builds for the gravity-drift move and the settled
             * check further down this loop - hand-rolled here before an
             * adversarial review (bd esp32c6-w2h) pointed out this was the
             * exact duplication that helper exists to prevent. */
            int cand[3][2];
            impulse_gravity_candidates(ox, oy, dx, dy, cand);

            for (int c = 0; c < 3 && !reacquired; c++) {
                const int cx = cand[c][0];
                const int cy = cand[c][1];
                if ((unsigned)cx >= (unsigned)w ||
                    (unsigned)cy >= (unsigned)h) {
                    continue;
                }
                const size_t cat = (size_t)cy * (size_t)w + (size_t)cx;
                if (s->cells[cat] == entry.cell) {
                    entry.index = (uint16_t)cat;
                    reacquired = true;
                }
            }

            /* LIQUIDS GET A SECOND CHANCE, MATCHED ON MATERIAL ONLY:
             * cross-flow moves MASS, not grains, so a cell's variant can
             * change without moving it to one of the three candidates
             * above - measured 44.5% of queued water entries lost to this
             * gap otherwise. Scoped to water/acid; checked at the original
             * cell, then its 8 neighbours. */
            if (!reacquired) {
                if (lost_mat == MAT_WATER || lost_mat == MAT_ACID) {
                    const cell_t here = s->cells[entry.index];
                    if (!CELL_IS_EMPTY(here) &&
                        CELL_MATERIAL(here) == lost_mat) {
                        entry.cell = here;
                        reacquired = true;
                    }
                    for (int c = 0; c < 8 && !reacquired; c++) {
                        const int *rd = ring_dir(c);
                        const int cx = ox + rd[0];
                        const int cy = oy + rd[1];
                        if ((unsigned)cx >= (unsigned)w ||
                            (unsigned)cy >= (unsigned)h) {
                            continue;
                        }
                        const size_t cat = (size_t)cy * (size_t)w + (size_t)cx;
                        const cell_t found = s->cells[cat];
                        if (!CELL_IS_EMPTY(found) &&
                            CELL_MATERIAL(found) == lost_mat) {
                            entry.index = (uint16_t)cat;
                            entry.cell  = found;
                            reacquired = true;
                        }
                    }
                }
            }

            if (!reacquired) {
                continue;
            }
        }

        /* Read once, after re-acquisition (which can rewrite entry.cell -
         * see its own comment above) has had its say, and reused for every
         * material check below instead of re-deriving it three times. */
        const uint8_t mat_id = CELL_MATERIAL(entry.cell);

        /* AIRBORNE SOLIDS FALL TOO - KIND_STATIC never moves in the
         * ordinary sweep, so a thrown chunk never arced like sand or
         * water, which get gravity for free every step besides this
         * loop's own push. While tracked, it also gets one UNCONDITIONAL
         * gravity-ward attempt every step - not rolled, since gating it
         * on `speed` would tie "still falling" to "still has push left",
         * which is backwards. */
        if (material_of(entry.cell)->kind == KIND_STATIC) {
            const int gx = (int)((unsigned)entry.index % (unsigned)w);
            const int gy = (int)((unsigned)entry.index / (unsigned)w);
            int gcand[3][2];
            impulse_gravity_candidates(gx, gy, dx, dy, gcand);
            /* Direction each candidate is: straight down, then two diagonals.
             * Matches order in impulse_gravity_candidates(). gcand_dir[c] is
             * needed for impulse_charge_displacement()'s transfer cone, not
             * just entry.dir. */
            const int i_dir = ring_of(dx, dy);
            const int gcand_dir[3] = { i_dir, (i_dir + 7) & 7, (i_dir + 1) & 7 };
            for (int c = 0; c < 3; c++) {
                const int cx = gcand[c][0];
                const int cy = gcand[c][1];
                if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                    continue;
                }
                const cell_t gtarget = sand_at(s, cx, cy);
                /* can_impulse_enter_gravity_ward() is the same predicate
                 * the settled check below uses. No separate "but not
                 * liquid" exclusion: this move is a SWAP, not an
                 * overwrite, so a lava cell a thrown chunk enters just
                 * relocates - conservation and "never smothered" both
                 * hold. An ENERGETIC chunk sinks into a liquid like a
                 * dense powder already does; below
                 * SAND_IMPULSE_SINK_MIN_SPEED a SPENT one gets none of
                 * this. */
                if (!can_impulse_enter_gravity_ward(gtarget, entry.cell,
                                                    entry.speed)) {
                    continue;
                }
                /* A DISPLACEMENT IS A DISPLACEMENT -
                 * impulse_charge_displacement() handles silent tunnelling:
                 * slow chunks paid nothing, threw nothing. It manages
                 * swap/latch/mark/index-update, not just charge. */
                const size_t gnat = (size_t)cy * (size_t)w + (size_t)cx;
                impulse_charge_displacement(s, &entry, gnat, gcand_dir[c],
                                            deferred, &deferred_transfer_count);
                break;
            }
        }

        /* The roll happens before the move attempt, EVERY turn, blocked or
         * not: rolling only on a move would let a wedged entry wait
         * FOREVER if its target never opens (a sealed vessel, an
         * undisturbed pile), breaking the one guarantee this design rests
         * on - every entry's lifetime is bounded. The roll's own chance IS
         * entry.speed - see SAND_IMPULSE_SPEED_RAMP for why one byte
         * carries both "chance this turn's move happens" and "how much
         * flight is left". */
        const bool rolled_move = rng_chance(&s->rng, entry.speed);

        /* First cell's ramp charges every turn. `speed` ages with roll.
         * `push_count` from distance before `impulse_decay()`. Further cells
         * charge once inside hop loop. Saturating `speed` prevents
         * `rng_chance()` success, dropping entry next turn. No separate "done
         * flying" check needed. */
        impulse_decay(&entry, mat_id, 1);

        /* WATER AND ACID GET THEIR OWN GEOMETRIC DECAY
         * (SAND_SPLASH_SPEED_DECAY_SHIFT) instead of the linear ramp
         * everything else uses - see that constant's own comment in
         * sand.h. Interacts with the cascade's own gate; see
         * SAND_CASCADE_MIN_SPEED's comment for why that constant is 1. */

        /* Uses entry.ramp, not a hardcoded SAND_IMPULSE_SPEED_RAMP - see
         * impulse_t's own comment for why the decay rate is per-entry;
         * sand_impulse_dislodge() is the one caller that queues a
         * different figure. */

        /* Dropping from tracking (not re-added to `kept` below) the instant
         * a roll fails is harmless for every kind except KIND_STATIC: a
         * liquid or powder entry keeps falling under the ordinary sweep
         * whether or not this loop still tracks it. */

        if (!rolled_move) {
            /* KIND_STATIC has no other fallback, so SUPPORTED, NOT MERELY
             * ROLLED, decides settled for it - the same predicate the
             * drift above uses, so the two can never disagree about an
             * opening (a cheaper CELL_IS_EMPTY()-only check once
             * regressed that way). A SUPPORT ITSELF IN FLIGHT MEANS WAIT,
             * NOT SETTLE: the blocker may be another tracked entry that
             * hasn't drifted yet this step - settling on it would freeze
             * two flying chunks forever. impulse_index_still_tracked()
             * catches that rare case. */
            if (material_of(entry.cell)->kind == KIND_STATIC) {
                const int rx = (int)((unsigned)entry.index % (unsigned)w);
                const int ry = (int)((unsigned)entry.index / (unsigned)w);
                int rcand[3][2];
                if (impulse_has_opening(s, rx, ry, dx, dy, entry.cell,
                                        entry.speed, rcand)) {
                    s->impulse_buf[kept++] = entry;
                    continue;   /* still airborne - keep falling */
                }

                /* Off-grid is excluded first to prevent synthetic edge cells. */
                const int bx = rcand[0][0];
                const int by = rcand[0][1];
                if ((unsigned)bx < (unsigned)w && (unsigned)by < (unsigned)h) {
                    const cell_t blocker =
                        s->cells[(size_t)by * (size_t)w + (size_t)bx];
                    if (!CELL_IS_EMPTY(blocker) &&
                        material_of(blocker)->kind == KIND_STATIC) {
                        const uint16_t block_index =
                            (uint16_t)((size_t)by * (size_t)w + (size_t)bx);
                        if (impulse_index_still_tracked(s, kept, i,
                                                        block_index)) {
                            s->impulse_buf[kept++] = entry;
                            continue;   /* support is itself still in
                                         * flight - wait, don't settle */
                        }
                    }
                }
            /* A THROWN GRAIN GETS THE SAME "STILL AIRBORNE" TREATMENT A
             * THROWN CHUNK GETS ABOVE - without it, a powder entry dropped
             * the instant one push-roll failed, so TRANSFER never fired on
             * impact. No "support in flight" wait here: the sweep already
             * falls a powder grain regardless. FLOORED AT
             * SAND_IMPULSE_BOUNCE_MIN_SPEED: below it a grain can neither
             * clear the transfer floor nor bounce, so tracking further is
             * pure bookkeeping cost. */
            } else if (material_of(entry.cell)->kind == KIND_POWDER &&
                       entry.speed >= SAND_IMPULSE_BOUNCE_MIN_SPEED) {
                const int rx = (int)((unsigned)entry.index % (unsigned)w);
                const int ry = (int)((unsigned)entry.index / (unsigned)w);
                int rcand[3][2];
                if (impulse_has_opening(s, rx, ry, dx, dy, entry.cell,
                                        entry.speed, rcand)) {
                    s->impulse_buf[kept++] = entry;
                    continue;   /* still airborne - keep tracked; the
                                 * ordinary sweep does the actual falling */
                }
            }
            continue;   /* settled - out of flight for good */
        }

        /* DISTANCE BUDGET - MAX CELLS STEP PUSH CAN COVER. See
         * SAND_IMPULSE_CELLS_PER_STEP_DIVISOR in sand_impulse.h. Computed
         * from post-ramp speed. Under divisor, exactly 1 cell. NOT ONLY
         * BUDGET - hop loop also has ENERGY exit. This is hard upper
         * bound, preventing mover from exceeding divisor. */
        const int push_count =
            1 + (int)entry.speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;

        /* Position and direction before this step's cells move - CASCADE
         * block measures against "one step behind where this entry started
         * ITS OWN MOVE THIS STEP", not where multi-cell push ends up. See
         * block's comment for why backward, not forward. */
        const int x0 = (int)((unsigned)entry.index % (unsigned)w);
        const int y0 = (int)((unsigned)entry.index / (unsigned)w);
        const int *d0 = ring_dir(entry.dir);

        /* Counts `push_count` cells moved before budget exhaustion or
         * obstruction, gating CASCADE check like single-cell moves: triggered
         * only if entry moved at least once, not for those stuck immediately. */
        int moved = 0;

        for (int hop = 0; hop < push_count; hop++) {
            const int x = (int)((unsigned)entry.index % (unsigned)w);
            const int y = (int)((unsigned)entry.index / (unsigned)w);
            const int *d = ring_dir(entry.dir);
            const int nx = x + d[0];
            const int ny = y + d[1];

            /* can_impulse_enter() checks if a flying grain can enter a cell,
             * displacing non-static occupants except liquids against
             * non-liquids. STATIC blocks unconditionally. sand_at() handles
             * grid edges as STATIC. */
            const cell_t target = sand_at(s, nx, ny);
            if (!can_impulse_enter(target, entry.cell)) {
                /* Blocked means WAIT: keeps position, direction and speed
                 * for another try next step, rather than dropping.
                 * WATER/ACID BOUNCE INSTEAD: reversing `dir` turns speed
                 * into rebound, scoped to water/acid, matching
                 * splash_displace(). THROWN CHUNK OR GRAIN REFLECTS TOO,
                 * off surface's normal, floored at
                 * SAND_IMPULSE_BOUNCE_MIN_SPEED and charges restitution. */
                if (mat_id == MAT_WATER || mat_id == MAT_ACID) {
                    entry.dir = (entry.dir + 4) & 7;
                } else if ((material_of(entry.cell)->kind == KIND_STATIC ||
                            material_of(entry.cell)->kind == KIND_POWDER) &&
                           entry.speed >= SAND_IMPULSE_BOUNCE_MIN_SPEED) {
                    const int normal = blocker_normal(s, x, y, entry.dir);
                    const int reflected = (normal < 0)
                                              ? -1
                                              : reflect_off_normal(entry.dir, normal);
                    if (reflected >= 0) {
                        const bool head_on =
                            (reflected == ((entry.dir + 4) & 7));
                        entry.dir = (uint8_t)reflected;
                        entry.speed = head_on
                            ? (uint8_t)(entry.speed >> 1)
                            : (uint8_t)(entry.speed - (entry.speed >> 2));
                    }
                }
                break;
            }

            /* A SWAP, not an overwrite - move_to()'s trick, so
             * conservation needs nothing extra. Drag and transfer are
             * charged by impulse_charge_displacement(), the same body
             * gravity-drift uses, at the move site so open air costs
             * nothing. `impact_speed` is captured fresh per hop, so a
             * multi-cell move pays as if it took one step per cell. */

            /* ONLY AN *EXTRA* CELL IS CHARGED HERE - hop 0's ramp was
             * already paid before push_count was computed, so charging it
             * again here would double it. */
            if (hop > 0) {
                impulse_decay(&entry, mat_id, 1);
            }

            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            impulse_charge_displacement(s, &entry, nat, entry.dir,
                                        deferred, &deferred_transfer_count);
            moved++;

            /* THE ENERGY EXIT - push_count is a DISTANCE budget only,
             * fixed from speed before this loop's drag charged, so a
             * chunk that paid nearly all its speed to drag on hop 0 would
             * otherwise still take every hop the budget allowed, since
             * packed medium is not a wall. DISTANCE alone still stays a
             * hard cap too: an energy-only exit would let open air tunnel
             * a full-speed entry far past push_count's cells. Both are
             * required. */
            if (entry.speed < SAND_IMPULSE_CELLS_PER_STEP_DIVISOR) {
                break;
            }
        }

        /* RELAYS BACKWARD, NOT FORWARD - the cell ahead is open, so relay
         * material feeding this move from behind. */
        if (moved > 0 &&
            (mat_id == MAT_WATER || mat_id == MAT_ACID) &&
            entry.speed >= SAND_CASCADE_MIN_SPEED * SAND_CASCADE_SPEED_DIVISOR &&
            deferred_cascade_count <
                SAND_CASCADE_MAX_PER_STEP - deferred_transfer_count) {
            const int rx = x0 - d0[0];
            const int ry = y0 - d0[1];
            if ((unsigned)rx < (unsigned)w && (unsigned)ry < (unsigned)h) {
                const cell_t relay_target =
                    s->cells[(size_t)ry * (size_t)w + (size_t)rx];
                if (!CELL_IS_EMPTY(relay_target) &&
                    CELL_MATERIAL(relay_target) == mat_id) {
                    /* Writes from the BACK of `deferred` - see the array's
                     * own top comment for why this and TRANSFER (which
                     * writes from the front, inside
                     * impulse_charge_displacement()) never collide: each
                     * partition is sized to its own cap, and the two caps
                     * sum to exactly this array's capacity. */
                    const int relay_slot = SAND_CASCADE_MAX_PER_STEP - 1 -
                                           deferred_cascade_count;
                    deferred_cascade_count++;
                    impulse_t *c = &deferred[relay_slot];
                    c->index = (uint16_t)((size_t)ry * (size_t)w + (size_t)rx);
                    c->cell  = relay_target;
                    c->dir   = entry.dir;
                    c->speed = (uint8_t)(entry.speed / SAND_CASCADE_SPEED_DIVISOR);
                }
            }
        }

        s->impulse_buf[kept].index = entry.index;
        s->impulse_buf[kept].cell  = entry.cell;
        s->impulse_buf[kept].dir   = entry.dir;
        s->impulse_buf[kept].speed = entry.speed;
        s->impulse_buf[kept].ramp  = entry.ramp;
        kept++;
    }

    s->impulse_count = kept;

    /* Queued `kept` impulses safely; TRANSFERs in [0,
     * deferred_transfer_count); CASCADEs in last deferred_transfer_count
     * cells. */
    for (int i = 0; i < deferred_transfer_count; i++) {
        sand_impulse(s, (int)((unsigned)deferred[i].index % (unsigned)w),
                    (int)((unsigned)deferred[i].index / (unsigned)w),
                    deferred[i].dir, deferred[i].speed);
    }
    for (int i = 0; i < deferred_cascade_count; i++) {
        const impulse_t *c = &deferred[SAND_CASCADE_MAX_PER_STEP - 1 - i];
        sand_impulse(s, (int)((unsigned)c->index % (unsigned)w),
                    (int)((unsigned)c->index / (unsigned)w),
                    c->dir, c->speed);
    }
}
