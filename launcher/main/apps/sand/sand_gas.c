/*=============================================================================
 * sand_gas - everything about a gas: rises, and disperses.
 *
 * Unlike a liquid, no part of a gas's movement can join the main sweep -
 * see step_one_grain()'s own comment in sand.c. Rising is the OPPOSITE of
 * gravity-ward, so it needs its own pass, swept in the reverse row/column
 * order from the main sweep, for exactly the same reason the main sweep's
 * own order has to be right: a move has to land in already-visited
 * territory, or a rising grain gets picked up and moved repeatedly,
 * teleporting to the ceiling in one step.
 *
 * HOW A GRAIN MOVES inside that pass is gas_walk_once(): one draw, one
 * probe, a biased random walk. The exhaustive mover it replaced - sand.c's
 * try_fall_or_scatter()/try_slide(), run with the direction negated - is
 * still reachable through sand_set_gas_walk(false) so the two can be
 * compared, and is the only thing that still needs the care those
 * primitives take here: driven_by_gravity() wants a gas-local table built
 * against the REVERSED gravity vector, not the main sweep's own, and the
 * reversed sweep's x order has to reuse sand_step()'s own x_step negated
 * rather than call sweep_x_order() again (that function has a side effect -
 * see its own comment in sand.c).
 *
 * Rising alone only piles gas into a heap against whatever it hits - the
 * same shape water's own gravity-ward primitive produces alone, which is
 * exactly why sand_liquid.c's cross-flow pass exists. equalise_gas() below
 * is gas's version of that: mirrors equalise_liquids()'s structure closely
 * (see sand_liquid.c), swapping mass-splitting for a plain whole-cell hop
 * to the nearest open cell along the perpendicular.
 *
 * Gas rising through standing LIQUID is its own problem: can_enter() cannot
 * express mobility and is far too hot a predicate to teach it. Both movers
 * carry the case themselves - gas_walk_once() inline, try_bubble() for the
 * exhaustive path.
 *
 * Whole-grain also means gas cannot THIN a saturated pocket the way water
 * levels one - a cell is either a full grain or empty, nothing between, so
 * a held-down pour saturates its own neighbourhood faster than the spread
 * pass can find real gaps to move into, and looks and behaves like a pile
 * of sand until it does. tick_decay() (sand_priv.h, shared with fire's
 * own burn-out - see sand_reactions.c) is what keeps that from being
 * permanent: material.h's `decay` field reuses the variant nibble as LIFE
 * REMAINING (per that file's own top comment), so a grain fades and clears
 * itself rather than accumulating forever. Off by default (see
 * sand_set_decay()) - a test that places gas and does not ask for decay
 * gets an immortal grain, same as every material before this one.
 *===========================================================================*/

#include "sand_priv.h"

/* Which materials are gas, as a bitmask over the nibble - see
 * liquid_mask()'s own comment in sand_liquid.c for why this exists at all
 * rather than reading materials[id].kind directly per cell. */
static uint16_t gas_mask(void)
{
    uint16_t mask = 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (material_by_id((material_id_t)m)->kind == KIND_GAS) {
            mask |= (uint16_t)(1u << m);
        }
    }
    return mask;
}

/*---------------------------------------------------------------------------
 * Sub-pass 1: rise, and the two diagonal slides - try_fall_or_scatter()/
 * try_slide(), reused from sand.c with the direction inverted.
 *-------------------------------------------------------------------------*/

/* One gas grain's turn - the same dispatch step_one_grain() runs for a
 * powder (fall/rise, then the two slides), but with an explicit wake,
 * since this pass gets none of step_one_block()'s own moved_here ->
 * BLOCK_ACTIVE bookkeeping. Unlike a powder, the whole attempt is gated
 * behind material.h's `mobility` roll first (jostle == 0 only): a grain
 * that misses the roll just sits this step, which is what makes gas rise
 * at a lazy drift instead of sand's instant one-cell-per-step. */

/* A BUBBLE: a gas cell trading places with the LIQUID above it - the one
 * move that runs against can_enter()'s rule rather than through it.
 * can_enter() only lets a DENSER mover displace a LIGHTER target
 * (backwards for steam rising through water); room_in() refuses a liquid
 * falling into a different material. A gas cell under standing liquid had
 * NO legal move either way - frozen mid-pour. Not fixed in can_enter():
 * read per cell per step by the sweep, costing every falling grain
 * forever. */
static bool try_bubble(sand_t *s, uint8_t *row, uint8_t *prow, int x, int y,
                       int w, int rdx, int rdy, cell_t grain, uint8_t density)
{
    if (prow == NULL) {
        return false;
    }
    const int nx = x + rdx;
    if ((unsigned)nx >= (unsigned)w) {
        return false;
    }

    const cell_t target = prow[nx];
    if (CELL_IS_EMPTY(target)) {
        return false;   /* an ordinary rise, and try_fall_or_scatter() has
                         * already had its turn at it */
    }
    const material_t *tm = material_of(target);
    if (tm->kind != KIND_LIQUID) {
        return false;   /* only liquids get pushed aside this way - a gas
                         * still cannot bubble through sand or stone */
    }
    if (density >= tm->density) {
        return false;   /* mobility, and the inverse of can_enter()'s own
                         * test: only something LIGHTER than the liquid
                         * rises through it. A gas as heavy as the liquid
                         * would just sit, which is the correct answer */
    }

    /* Safe with respect to sweep order: this pass sweeps so the rise
     * destination is territory already visited, and the liquid's own
     * passes (sand_step_liquids(), move_liquid_grain()) both ran earlier
     * this same sand_step(), so the displaced liquid gets exactly one
     * move. Mass is conserved by construction - a swap of two whole
     * cells, the liquid keeping its own variant nibble untouched. */
    prow[nx] = grain;
    row[x]   = target;

    mark_rows(s, y, y + rdy);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, y + rdy);
    return true;
}


/* THE SPEC FOR THE WALK, not the lookup it reads. Offsets are in gravity's
 * frame: ring_dir() is ordered, so with `up` the ring index of the rise
 * direction, up-1 and up+1 are the upper diagonals, up+-2 the sides and up+4
 * straight down - no per-material rotation table needed.
 *
 * Weights are out of 256 and sum to it exactly, so one draw decides
 * everything. The lower diagonals are deliberately 0: a particle that drifts
 * down does so bluntly, and giving five of eight directions a downward
 * component read as smoke sinking rather than swirling.
 *
 * gas_walk_offset[] below is derived from this by hand and is what the walk
 * actually reads; to change the distribution, edit these and rewrite that
 * table to match. */
static const __attribute__((unused)) struct { uint16_t upto; int8_t off; } gas_walk_weights[] = {
    {  72,  0 },   /* straight up          */
    { 144, -1 },   /* up, one side         */
    { 216,  1 },   /* up, the other        */
    { 240,  4 },   /* straight down        */
    { 248, -2 },   /* sideways             */
    { 256,  2 },   /* sideways, the other  */
};

/* DERIVED FROM THE WEIGHTS TABLE ABOVE, WHICH STAYS THE SOURCE OF TRUTH.
 * Every boundary in it is a multiple of 8, so roll >> 3 selects a bucket
 * exactly and thirty-two entries cover all 256 rolls - the linear search this
 * replaces cost up to seven iterations, each with its own load and branch.
 *
 * Const, so it costs no RAM: the loop was the expense, not the memory, and
 * this board has 322 KiB of its ~424 KiB under the framebuffer (a 256-entry
 * version of this table failed check_static_ram by 224 bytes). One cached
 * load replaces the walk.
 *
 * Hand-written, so it must agree with the weights: a single wrong entry moves
 * the behaviour fingerprint, which was confirmed by mutating one. */
static const int8_t gas_walk_offset[32] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,     /* rolls   0.. 71 - stay on course */
    -1, -1, -1, -1, -1, -1, -1, -1, -1,    /* rolls  72..143 - one notch left */
    1,  1,  1,  1,  1,  1,  1,  1,  1,     /* rolls 144..215 - one notch right */
    4,  4,  4,                             /* rolls 216..239 - straight down */
    -2,                                    /* rolls 240..247 - side */
    2,                                     /* rolls 248..255 - side */
};

/* One bit per materials[] row, so the row filter reads a shift instead of
 * dereferencing a 12-byte struct in flash for every cell on the grid, gas or
 * not. Four bytes rather than a 32-byte table, for the same reason. */
static uint32_t gas_kind_mask;
static bool gas_tables_ready;

static void build_gas_tables(void)
{
    if (gas_tables_ready) {
        return;
    }
    for (int r = 0; r < MATERIAL_ROWS; r++) {
        if (materials[r].kind == KIND_GAS) {
            gas_kind_mask |= 1u << r;
        }
    }
    gas_tables_ready = true;
}

/* BUOYANCY, which move_to() structurally cannot do: can_enter() admits a liquid
 * only to something DENSER, and a gas is lighter by definition - so without this
 * a walking gas cell cannot enter liquid at all and sits trapped inside a body
 * of it. try_bubble() exists for exactly this in the exhaustive mover; the walk
 * needs its own, because it reaches move_to() directly.
 *
 * UPWARD PICKS ONLY. A bubble rises: sideways or downward buoyancy is wrong
 * physically, and downward is also unsafe for the sweep, which guarantees a
 * single move per cell only in the direction it sweeps. */
static inline bool gas_walk_once(sand_t *s, uint8_t *row, int x, int y, int w,
                                 int rdx, int rdy, cell_t grain,
                                 uint8_t density)
{
    const int up   = ring_of(rdx, rdy);
    const int roll = (int)(rng_next(&s->rng) & 0xFF);

    const int off = gas_walk_offset[roll >> 3];

    const int *d  = ring_dir(up + off);
    const int ny  = y + d[1];
    const int nx  = x + d[0];

    /* ONE PROBE FOR BOTH OUTCOMES. move_to() and the buoyancy fallback each
     * used to derive the row (a multiply), load the target and dereference
     * materials[] for themselves - and on a packed grid the blocked path,
     * which paid both, is the common one. can_enter() has already asked
     * whether the blocker is a liquid and how heavy it is. */
    uint8_t *const trow = dest_row(s, ny);
    if (trow == NULL || (unsigned)nx >= (unsigned)w) {
        return false;
    }
    const cell_t target = trow[nx];

    if (can_enter(density, CELL_MATERIAL(grain), target)) {
        trow[nx] = grain;
        row[x]   = target;
        mark_rows(s, y, ny);
        return true;
    }

    /* Blocked. UPWARD PICKS ONLY: a bubble rises, and sideways or downward
     * buoyancy is wrong physically and unsafe for a sweep that guarantees one
     * move per cell in the direction it sweeps. Without this a walking gas
     * cell cannot enter liquid at all and sits trapped inside a body of it;
     * try_bubble() covers the same case for the exhaustive mover, which
     * reaches move_to() by a different route. */
    if (off < -1 || off > 1) {
        return false;
    }
    const material_t *tm = material_of(target);
    if (tm->kind != KIND_LIQUID || density >= tm->density) {
        return false;   /* only through a liquid, and only if lighter than it -
                         * a gas as heavy as the liquid correctly just sits */
    }

    trow[nx] = grain;
    row[x]   = target;
    mark_rows(s, y, ny);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, ny);
    return true;
}

static bool step_one_gas_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                               uint8_t *arow, uint8_t *brow, int x, int y,
                               int w, int rdx, int rdy, const int *rslide_a,
                               const int *rslide_b, int rload_dx,
                               int rload_dy, int jostle,
                               bool driven_gas[MATERIAL_MAX][2])
{
    cell_t grain = row[x];
    const material_t *mat = material_of(grain);
    const uint8_t mat_id  = CELL_MATERIAL(grain);
    const uint8_t density = mat->density;

    bool vanished = false;
    SAND_STEP_GATE(gas_decay) {
        vanished = !tick_decay(s, row, x, y, &grain, mat, mat_id);
    }
    if (vanished) {
        return true;    /* vanished - already woken, nothing left to move */
    }

    /* s->mobility mirrors s->scatter's own override (see
     * sand_set_mobility()), but defaults to 255 (always) rather than 0 -
     * "off" for a rise-gate means "never rises", which would break every
     * test that places gas and expects a deterministic one-cell move. */
    const int mobility = (s->mobility >= 0) ? s->mobility : mat->mobility;
    const bool try_moving = jostle != 0 ||
                            (int)(rng_next(&s->rng) & 0xFF) < mobility;

    bool moved = false;

    /* The walk replaces only the MOVEMENT half - tick_decay() above still runs,
     * so fire still burns down at the same rate. It draws its own direction, so
     * it does not consume the mobility roll differently than the branch below;
     * both paths have already drawn it. */
    if (s->gas_walk) {
        if (SAND_STEP_GATED(gas_move, try_moving)) {
            /* gas_walk_once() handles an up-ish draw blocked by a
             * lighter-than-gas liquid itself, so this needs no separate
             * try_bubble() call of its own. */
            moved = gas_walk_once(s, row, x, y, w, rdx, rdy, grain, density);
        }
        if (SAND_STEP_GATED(gas_wake, moved)) {
            wake_block_and_neighbors(s, x, y);
        }
        return moved;
    }

    if (try_moving && jostle == 0) {
        const int scatter = (s->scatter >= 0) ? s->scatter : mat->scatter;
        /* The _impl, not the public wrapper. Both live in sand_priv.h as
         * static inline; the wrappers are extern in sand.c and exist for the
         * suite, which cannot reach a static. Calling them from here made every
         * gas grain pay a cross-translation-unit call with thirteen arguments -
         * and the gas rise sweep is 49% of the app's most expensive scene
         * (bd esp32c6-dp8). The main sweep already calls the _impl directly. */
        if (try_fall_or_scatter_impl(s, row, prow, arow, brow, x, y, w, rdx,
                                     rdy, rslide_a, rslide_b, grain, density,
                                     scatter)) {
            moved = true;
        }
    }
    if (try_moving && !moved) {
        /* Same as above, and worse: try_slide() is a 22-byte thunk, so this
         * marshalled sixteen arguments only to forward them to try_slide_impl
         * on the other side of the call. */
        moved = try_slide_impl(s, row, prow, arow, brow, x, y, w, rdx, rdy,
                               rslide_a, rslide_b, rload_dx, rload_dy, jostle,
                               grain, mat_id, density, mat, driven_gas);
    }
    /* Last, so an ordinary rise into open space always wins over shoving a
     * liquid aside - a gas with somewhere free to go takes it, and only a
     * gas that is genuinely capped by liquid pays for the extra check. */
    if (try_moving && !moved) {
        moved = try_bubble(s, row, prow, x, y, w, rdx, rdy, grain, density);
    }
    if (moved) {
        wake_block_and_neighbors(s, x, y);
    }
    return moved;
}

/* One row of the reversed sweep - only gas cells are dispatched; everything
 * else was either already handled by the main sweep or is a static wall
 * gas has to work around, not through. */
static bool step_one_gas_row(sand_t *s, int y, int w, int rdx, int rdy,
                             const int *rslide_a, const int *rslide_b,
                             int rx_step, int rload_dx, int rload_dy,
                             int jostle, bool driven_gas[MATERIAL_MAX][2])
{
    uint8_t *row  = s->cells + (size_t)y * (size_t)w;
    uint8_t *prow = dest_row(s, y + rdy);
    uint8_t *arow = dest_row(s, y + rslide_a[1]);
    uint8_t *brow = dest_row(s, y + rslide_b[1]);

    const int x_from = (rx_step > 0) ? 0 : w - 1;
    const int x_to   = (rx_step > 0) ? w : -1;

    bool any = false;
    for (int x = x_from; x != x_to; x += rx_step) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c) || ((gas_kind_mask >> (c >> 3)) & 1u) == 0u) {
            continue;
        }
        /* Presence, not movement: a gas cell that neither moves nor decays
         * this step (jammed solid, roll not hit) still exists, and
         * may_have_gas must stay set for it. Getting this wrong meant a
         * trapped, motionless pocket of gas could clear may_have_gas
         * while still physically on the grid - harmless before decay
         * existed, but with decay it would strand that gas immortal,
         * since sand_step_gas() early-returns on !may_have_gas and decay
         * only ever rolls from inside this loop. */
        any = true;
        step_one_gas_grain(s, row, prow, arow, brow, x, y, w, rdx, rdy,
                           rslide_a, rslide_b, rload_dx, rload_dy, jostle,
                           driven_gas);
    }
    return any;
}

/*---------------------------------------------------------------------------
 * Sub-pass 2: perpendicular spread - mirrors sand_liquid.c's
 * equalise_liquids()/equalise_one_row()/equalise_one_cell(), whole-grain
 * instead of mass-based.
 *-------------------------------------------------------------------------*/

/* Mirrors has_room_below() in sand_liquid.c: if this grain still has
 * somewhere to rise THIS step, sub-pass 1 above already moved it (or
 * will, being swept before this runs) - one comparison, and it keeps
 * this search off the bill for the common case of a gas pocket still
 * mostly rising rather than pooled under something. */
static inline bool has_room_above(const sand_t *s, int x, int y, int rdx,
                                  int rdy)
{
    const int fx = x + rdx;
    const int fy = y + rdy;
    if ((unsigned)fx >= (unsigned)s->w || (unsigned)fy >= (unsigned)s->h) {
        return false;
    }
    return CELL_IS_EMPTY(s->cells[(size_t)fy * (size_t)s->w + (size_t)fx]);
}

/* Whether the immediate neighbour along (px, py) is worth searching past:
 * truly empty, OR the same gas - unlike a wall or denser material. The
 * earlier version returned true only for CELL_IS_EMPTY, mirroring the
 * liquid fast path too closely: gas has no level to compare, so it
 * treated another gas cell like a wall, never calling find_nearest_empty()
 * for real space further out. Matters for a 2D pour under a ceiling: many
 * INDEPENDENT rows, a row mid-pour getting grains faster than one pass
 * resolves. */
static inline bool neighbour_is_open(const sand_t *s, int x, int y, int px,
                                     int py, uint8_t gas_id)
{
    const int nx = x + px;
    const int ny = y + py;
    if ((unsigned)nx >= (unsigned)s->w || (unsigned)ny >= (unsigned)s->h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)s->w + (size_t)nx];
    return CELL_IS_EMPTY(n) || CELL_MATERIAL(n) == gas_id;
}

/* Mirrors find_shallowest(): nearest open cell along (px, py) within
 * `sight`, or 0 if none. Passes through gas cells the way find_shallowest()
 * passes through the same liquid; flow stops at anything else. On a
 * saturated screen this walk is the single biggest cost in the gas step.
 * `*run_len_out` is written - always to `sight` - only when the loop runs
 * every `sight` cell without finding an empty or blocker; only then can
 * equalise_gas_one_cell() skip re-walking next cell (see gas_run_t). */
static inline int find_nearest_empty(const sand_t *s, int x, int y, int px,
                                     int py, int sight, uint8_t gas_id,
                                     int *run_len_out)
{
    for (int k = 1; k <= sight; k++) {
        const int sx = x + px * k;
        const int sy = y + py * k;
        if ((unsigned)sx >= (unsigned)s->w || (unsigned)sy >= (unsigned)s->h) {
            return 0;
        }
        const cell_t o = s->cells[(size_t)sy * (size_t)s->w + (size_t)sx];
        if (CELL_IS_EMPTY(o)) {
            return k;
        }
        if (CELL_MATERIAL(o) != gas_id) {
            return 0;      /* blocked by a wall or something denser */
        }
    }
    *run_len_out = sight;
    return 0;
}

/* equalise_gas_one_row() sweeps with x_step = -px, so the cell after x
 * sits at x - px and casts a ray revisiting x's ray shifted by one. What
 * find_nearest_empty() found about x's ray is also true of x - px's,
 * once x is counted at the near end - this lets the sweep skip a full
 * sight-length re-walk of a packed pocket on almost every cell. `id`/`len`
 * hold the verified run's material and length. Valid only when py == 0;
 * equalise_gas_one_cell() writes them only when carry_ok, so id stays -1
 * otherwise. */
typedef struct {
    int id;
    int len;
} gas_run_t;

/* One cell's share of spread: hops the whole grain to the nearest open
 * cell along (px, py), if sub-pass 1 could not already move it and a real
 * gap exists. No mass to split - a grain either moves the whole way, or
 * not at all. `run` carries gas_run_t's verified-run state between cells
 * of the same row sweep (see that struct's own comment for the geometry)
 * and `carry_ok` is that carry's on/off switch, true only when py == 0. */
static inline bool equalise_gas_one_cell(sand_t *s, uint8_t *row, int x,
                                         int y, int px, int py, int rdx,
                                         int rdy, int sight, uint8_t gas_id,
                                         cell_t grain, bool *stayed_in_row,
                                         int *touched_x, bool carry_ok,
                                         gas_run_t *run)
{
    bool moved = false;
    int  tx = 0, ty = 0;

    if (!has_room_above(s, x, y, rdx, rdy) &&
        neighbour_is_open(s, x, y, px, py, gas_id)) {
        int at;

        /* Known, without looking, to return 0: `run` already covers every
         * cell this scan would walk, courtesy of the previous cell in the
         * sweep having walked - or itself skipped - the very same ray.
         * Re-walking it would only confirm what carrying the run forward
         * already guarantees. */
        if (carry_ok && run->id == (int)gas_id && run->len >= sight) {
            at = 0;
        } else {
            int scan_len = 0;

            at = find_nearest_empty(s, x, y, px, py, sight, gas_id,
                                    &scan_len);
            /* Only a scan that paid the full `sight` walk is worth
             * remembering - see find_nearest_empty's own comment for why
             * the two early-break cases (the edge of the grid, a wall or
             * a different material) are left alone instead: they are
             * already cheap, so caching them would cost more than just
             * repeating them next time. */
            if (carry_ok && at == 0 && scan_len == sight) {
                run->id  = (int)gas_id;
                run->len = scan_len;
            }
        }

        if (at != 0) {
            tx = x + px * at;
            ty = y + py * at;
            moved = true;
        }
    }

    /* If moved, `run->id` resets to -1. If not moved and `run` already
     * named this material, it folds forward by one. If it named
     * something else, it is left AS-IS, NOT restarted: restarting was
     * tried and cost +4.4% on a smoke/steam screen alternating gases
     * cell by cell, every cell paying to write a length-one run the next
     * threw away unread. Leaving `run` alone is safe: a
     * different-material gas cell is never empty, so it blocks a
     * same-ray scan regardless of stale material. Do not re-add that
     * branch. */
    if (moved) {
        run->id = -1;
    } else if (run->id == (int)gas_id) {
        run->len += 1;
    }

    if (!moved) {
        return false;
    }

    const int w = s->w;

    s->cells[(size_t)ty * (size_t)w + (size_t)tx] = grain;
    row[x] = CELL_EMPTY;

    *stayed_in_row = (py == 0);
    if (*stayed_in_row) {
        *touched_x = tx;
    } else {
        mark_move(s, x, y, tx, ty);
    }
    return true;
}

/* Widens [*x0,*x1] to also cover [lo,hi] - see union_touched_x() in
 * sand_liquid.c, duplicated here rather than shared: six lines, one call
 * site each, not worth widening sand_priv.h's surface for. */
static inline void gas_union_touched_x(bool *touched, int *x0, int *x1,
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

static inline bool equalise_gas_one_row_cell(sand_t *s, uint8_t *row, int x,
                                             int y, int px, int py, int rdx,
                                             int rdy, uint16_t is_gas,
                                             bool *touched, int *touched_x0,
                                             int *touched_x1, bool carry_ok,
                                             gas_run_t *run)
{
    const cell_t c = row[x];
    if (CELL_IS_EMPTY(c)) {
        /* Nothing here for a future ray to pass through as "the same gas"
         * - see gas_run_t's own comment. Invalidating is always safe, it
         * just costs a real scan next time instead of a free skip. */
        run->id = -1;
        return false;
    }
    const uint8_t id = CELL_MATERIAL(c);
    if (((is_gas >> id) & 1u) == 0) {
        /* Same reasoning as the empty case just above: whatever this cell
         * holds, it is not gas, so it cannot extend a gas run either. */
        run->id = -1;
        return false;
    }

    /* Per-material now, not a pass-wide constant - see material.h's own
     * comment on `sight` for why: two materials can share this pass (gas,
     * fire) and disperse by different amounts. material_of(c), not
     * material_by_id(id): `c` is the cell this `id` was just extracted
     * from, and only material_of() finds the right row for every cell
     * byte, gunpowder included, now that nibble 15 is two rows. */
    const int sight = material_of(c)->sight;

    bool stayed_in_row = false;
    int  tx = 0;
    if (SAND_STEP_GATED(gas_eq_body,
                        equalise_gas_one_cell(s, row, x, y, px, py, rdx, rdy, sight, id, c,
                                              &stayed_in_row, &tx, carry_ok, run))
        && stayed_in_row) {
        gas_union_touched_x(touched, touched_x0, touched_x1,
                            x < tx ? x : tx, x > tx ? x : tx);
    }
    return true;
}

/* A row with no empty cell in it cannot spread, and knowing that skips its
 * whole body. Every target equalise_gas_one_cell() can pick lies in this same
 * row when py == 0 - ty is y, and stayed_in_row is exactly that test - so with
 * nowhere in the row to go, none of its gas moves however long it looks.
 *
 * Pays for itself in both directions: it BREAKS on the first empty cell, so a
 * sparse row costs a handful of loads and then runs the body as before, while
 * a packed one costs w loads to skip two probes on every gas cell it holds.
 * That is the trade the device asked for - the body is 14578 us of a fire step
 * and its cost is per-cell probes, not the sight scan, which gas_run_t already
 * carries.
 *
 * Reports whether the row held gas on the way past, since the caller needs
 * that for may_have_gas whether or not the body runs. */
static inline bool row_is_packed(const uint8_t *row, int w, uint16_t is_gas,
                                 bool *any_gas)
{
    bool gas = false;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            return false;
        }
        gas |= (((is_gas >> CELL_MATERIAL(c)) & 1u) != 0);
    }
    *any_gas = gas;
    return true;
}

/* One row's share of spread. Returns whether it held any gas. No
 * ROW_NO_GAS equivalent yet - deferred until real usage patterns exist to
 * measure against, same as every other tunable in this project; may_have_gas
 * alone is the pass's cheap-skip for now. */
static bool equalise_gas_one_row(sand_t *s, int y, int w, int x_from,
                                 int x_to, int x_step, int px, int py,
                                 int rdx, int rdy, uint16_t is_gas)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;
    bool any_gas = false;
    bool touched = false;
    int  touched_x0 = 0, touched_x1 = 0;

    /* carry_ok gates the whole run-skipping scheme off the moment gravity
     * is not axis-aligned - see gas_run_t's own comment for why the sweep
     * geometry it relies on only holds when py == 0. Computed once per
     * row (constant for the whole equalise_gas() call, since px and py do
     * not change mid-pass) rather than re-checked per cell. */
    const bool carry_ok = (py == 0);

    /* Reset at the start of every row: a run only ever describes cells
     * within the same row, and the sweep has not looked at any of them
     * yet. */
    gas_run_t run = { .id = -1, .len = 0 };

    /* Same py == 0 precondition carry_ok rests on, for the same reason: a
     * tilted sweep's targets leave the row, so this row's contents no longer
     * decide the answer. */
    if (carry_ok && row_is_packed(row, w, is_gas, &any_gas)) {
        return any_gas;
    }

    for (int x = x_from; x != x_to; x += x_step) {
        if (equalise_gas_one_row_cell(s, row, x, y, px, py, rdx, rdy,
                                      is_gas, &touched, &touched_x0,
                                      &touched_x1, carry_ok, &run)) {
            any_gas = true;
        }
    }

    if (touched) {
        mark_rows(s, y, y);
        const int by = (int)((unsigned)y / SAND_BLOCK_H);
        wake_blocks_range(s, (int)((unsigned)touched_x0 / SAND_BLOCK_W), by,
                          (int)((unsigned)touched_x1 / SAND_BLOCK_W), by);
    }
    return any_gas;
}

static bool equalise_gas(sand_t *s, const int *perp, int rdx, int rdy)
{
    bool found_any = false;

    const int px = perp[0];
    const int py = perp[1];
    const int w  = s->w;
    const int h  = s->h;

    const uint16_t is_gas = gas_mask();

    const int y_from = (py > 0) ? h - 1 : 0;
    const int y_to   = (py > 0) ? -1    : h;
    const int y_step = (py > 0) ? -1    : 1;

    const int x_from = (px > 0) ? w - 1 : 0;
    const int x_to   = (px > 0) ? -1    : w;
    const int x_step = (px > 0) ? -1    : 1;

    for (int y = y_from; y != y_to; y += y_step) {
        if (equalise_gas_one_row(s, y, w, x_from, x_to, x_step, px, py, rdx,
                                 rdy, is_gas)) {
            found_any = true;
        }
    }
    return found_any;
}

/*---------------------------------------------------------------------------
 * The whole step.
 *-------------------------------------------------------------------------*/

void sand_step_gas(sand_t *s, int gx, int gy, int dx, int dy,
                   const int *slide_a, const int *slide_b,
                   const int *perp_a, const int *perp_b, int load_dx,
                   int load_dy, int x_step, int jostle)
{
    if (!s->may_have_gas) {
        return;
    }
    build_gas_tables();

    const int rdx = -dx, rdy = -dy;
    const int rslide_a[2] = { -slide_a[0], -slide_a[1] };
    const int rslide_b[2] = { -slide_b[0], -slide_b[1] };
    const int rload_dx = -load_dx, rload_dy = -load_dy;
    const int rx_step  = -x_step;

    /* driven_by_gravity()'s descent = m . g dot product is against real
     * gravity - feeding it gas's reversed slide vectors together with
     * the main sweep's forward (gx, gy) would make descent negative for
     * every gas slide, unconditionally, so they'd never fire. Built
     * fresh here against the reversed vector instead. MATERIAL_MAX (16),
     * not MATERIAL_ROWS (32): this is only ever built/read for KIND_GAS
     * cells, and no gas material lives in MAT_EXTENDED's twin-row
     * range. */
    bool driven_gas[MATERIAL_MAX][2];
    for (int m = 0; m < MATERIAL_MAX; m++) {
        const int repose = material_by_id((material_id_t)m)->repose;
        driven_gas[m][0] = driven_by_gravity(rslide_a[0], rslide_a[1], -gx,
                                             -gy, repose);
        driven_gas[m][1] = driven_by_gravity(rslide_b[0], rslide_b[1], -gx,
                                             -gy, repose);
    }

    /* Swept in reverse from the main sweep - see this file's own top
     * comment for why. */
    const int y_from = (rdy > 0) ? s->h - 1 : 0;
    const int y_to   = (rdy > 0) ? -1       : s->h;
    const int y_step = (rdy > 0) ? -1       : 1;

    bool found_any = false;
    const int w = s->w;
    SAND_STEP_GATE(gas_rise) {
        for (int y = y_from; y != y_to; y += y_step) {
            if (step_one_gas_row(s, y, w, rdx, rdy, rslide_a, rslide_b,
                                 rx_step, rload_dx, rload_dy, jostle,
                                 driven_gas)) {
                found_any = true;
            }
        }
    }

    /* Then spread, alternating which way it looks each step - same reason
     * liquid's cross-flow does (see sand_step_liquids() in sand_liquid.c).
     * Kept on its own flip flag rather than sharing liquid_flip, so gas's
     * alternation is not coupled to whether water also moved this step. */
    if (SAND_STEP_GATED(gas_equalise,
                        equalise_gas(s, s->gas_flip ? perp_a : perp_b,
                                     rdx, rdy))) {
        found_any = true;
    }
    s->gas_flip = !s->gas_flip;

    if (!found_any) {
        s->may_have_gas = false;
    }
}
