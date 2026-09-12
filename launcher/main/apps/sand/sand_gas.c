/*
 * sand_gas - everything about a gas: rises, and disperses.
 *
 * Unlike a liquid, no part of a gas's movement can join the main sweep -
 * see step_one_grain()'s own comment in sand.c. Rising is the OPPOSITE of
 * gravity-ward, so it needs its own pass, swept in the reverse row/column
 * order from the main sweep: a move has to land in already-visited
 * territory, or a rising grain gets picked up and moved repeatedly,
 * teleporting to the ceiling in one step.
 *
 * HOW A GRAIN MOVES inside that pass is gas_walk_once(): one draw, one
 * probe, a biased random walk. The exhaustive mover - sand.c's
 * try_fall_or_scatter()/try_slide(), run with the direction negated - is
 * still reachable through sand_set_gas_walk(false) so the two can be
 * compared; it needs driven_by_gravity()'s gas-local table built against
 * the REVERSED gravity vector, and the reversed sweep's x order reuses
 * sand_step()'s own x_step negated rather than call sweep_x_order()
 * again (that function has a side effect - see its own comment in
 * sand.c).
 *
 * Rising alone only piles gas into a heap against whatever it hits, the
 * same shape water's gravity-ward primitive produces alone - why
 * sand_liquid.c's cross-flow pass exists. equalise_gas() below is gas's
 * version: mirrors equalise_liquids()'s structure (sand_liquid.c),
 * swapping mass-splitting for a whole-cell hop to the nearest open cell
 * along the perpendicular.
 *
 * Gas rising through standing LIQUID is its own problem: can_enter()
 * cannot express mobility and is far too hot a predicate to teach it.
 * Both movers carry the case themselves - gas_walk_once() inline,
 * try_bubble() for the exhaustive path.
 *
 * Whole-grain also means gas cannot THIN a saturated pocket the way
 * water levels one - a cell is either a full grain or empty, so a
 * held-down pour saturates its neighbourhood faster than the spread
 * pass can find gaps, and behaves like a pile of sand until it does.
 * tick_decay() (sand_priv.h, shared with fire's burn-out - see
 * sand_reactions.c) keeps that from being permanent: `decay` reuses the
 * variant nibble as LIFE REMAINING, so a grain fades and clears itself.
 * Off by default (sand_set_decay()) - an undecayed test grain is
 * immortal, same as any other material with decay unset.
 */

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

/* Which rows hold gas, carried from the rise sweep's own scan to the spread
 * pass below, which otherwise repeats the same whole-grid scan to find the
 * same cells. A bitmap, not a byte per row: RAM is this board's binding
 * constraint, and this is read once per row against a per-cell loop.
 *
 * GAS_ROW_WORDS caps the grid height it can describe. A taller grid is not
 * wrong, it just leaves the map off and spreads as before. */
#define GAS_ROW_WORDS 16
#define GAS_ROW_MAX   (GAS_ROW_WORDS * 32)

typedef struct {
    uint32_t w[GAS_ROW_WORDS];
} gas_rows_t;

/* File-static rather than threaded through the pass: step_one_gas_grain()
 * already carries sixteen parameters, and this is built and consumed inside
 * one sand_step_gas() call, which never nests. */
static gas_rows_t gas_row_map;
static bool       gas_row_map_live;

/* THE SKIP'S WHOLE SAFETY ARGUMENT: every mover below arms the row it lands
 * in, so the map records what happened rather than inferring it from how far
 * a cell can travel. Widening the map by a fixed number of rows instead does
 * not work - a walk drawing straight down lands in a row the rise sweep has
 * not reached yet, takes a second turn there, and can repeat, so travel in
 * one pass has no bound. A mover that forgets to arm is what
 * sand_gas_row_audit_enable() catches. */
static inline void gas_row_arm(int y)
{
    if (gas_row_map_live && (unsigned)y < (unsigned)GAS_ROW_MAX) {
        gas_row_map.w[(unsigned)y >> 5] |= 1u << ((unsigned)y & 31u);
    }
}

static inline bool gas_row_may_hold(int y)
{
    if (!gas_row_map_live) {
        return true;
    }
    return ((gas_row_map.w[(unsigned)y >> 5] >> ((unsigned)y & 31u)) & 1u) != 0u;
}

/*
 * Sub-pass 1: rise, and the two diagonal slides - try_fall_or_scatter()/
 * try_slide(), reused from sand.c with the direction inverted.
 */

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
    gas_row_arm(y + rdy);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, y + rdy);
    return true;
}


/* THE SPEC FOR THE WALK, not the lookup it reads - gas_walk_offset[] below
 * is derived from this by hand. Offsets are in gravity's frame: ring_dir()
 * is ordered, so with `up` the rise direction, up+-1 are the upper
 * diagonals, up+-2 the sides and up+4 straight down. Weights sum to 256
 * exactly, so one draw decides everything. The lower diagonals are 0:
 * a downward component on five of eight directions read as smoke sinking
 * rather than swirling. */
static const __attribute__((unused)) struct { uint16_t upto; int8_t off; } gas_walk_weights[] = {
    {  72,  0 },   /* straight up          */
    { 144, -1 },   /* up, one side         */
    { 216,  1 },   /* up, the other        */
    { 240,  4 },   /* straight down        */
    { 248, -2 },   /* sideways             */
    { 256,  2 },   /* sideways, the other  */
};

/* DERIVED FROM THE WEIGHTS TABLE ABOVE, the source of truth: every
 * boundary is a multiple of 8, so roll >> 3 selects a bucket exactly,
 * replacing a linear search that cost up to seven iterations. Const, so
 * it costs no RAM - this board has 322 KiB of its ~424 KiB under the
 * framebuffer, and a 256-entry version of this table failed
 * check_static_ram by 224 bytes. Hand-written, so it must agree with the
 * weights: a single wrong entry moves the behaviour fingerprint. */
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

/* BUOYANCY, which move_to() structurally cannot do: can_enter() admits a
 * liquid only to something DENSER, and a gas is lighter by definition, so
 * without this a walking gas cell sits trapped inside a body of liquid.
 *
 * UPWARD PICKS ONLY. Sideways or downward buoyancy is wrong physically, and
 * downward is unsafe for a sweep that guarantees a single move per cell
 * only in the direction it sweeps. */
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

    /* ONE PROBE FOR BOTH OUTCOMES: move_to() and the buoyancy fallback
     * share the same row/target lookup rather than each deriving it
     * separately - on a packed grid the blocked path is the common one.
     * can_enter() has already asked whether the blocker is a liquid and
     * how heavy it is. */
    uint8_t *const trow = dest_row(s, ny);
    if (trow == NULL || (unsigned)nx >= (unsigned)w) {
        return false;
    }
    const cell_t target = trow[nx];

    if (can_enter(density, CELL_MATERIAL(grain), target)) {
        trow[nx] = grain;
        row[x]   = target;
        mark_rows(s, y, ny);
        gas_row_arm(ny);
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
    gas_row_arm(ny);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, ny);
    return true;
}

/* try_fall_or_scatter_impl()/try_slide_impl() report only that they moved,
 * not where to, so the three rows their fall and two slides can reach are
 * armed together. Only the exhaustive mover pays this - the walk and
 * try_bubble() each arm the one row they actually landed in. */
static inline void arm_exhaustive_landing(int y)
{
    gas_row_arm(y - 1);
    gas_row_arm(y);
    gas_row_arm(y + 1);
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

    if (!tick_decay(s, row, x, y, &grain, mat_id,
                    (s->decay >= 0) ? s->decay : mat->decay)) {
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
        if (try_moving) {
            /* gas_walk_once() handles an up-ish draw blocked by a
             * lighter-than-gas liquid itself, so this needs no separate
             * try_bubble() call of its own. */
            moved = gas_walk_once(s, row, x, y, w, rdx, rdy, grain, density);
        }
        if (moved) {
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
        arm_exhaustive_landing(y);
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
        if (!any) {
            any = true;
            gas_row_arm(y);
        }
        step_one_gas_grain(s, row, prow, arow, brow, x, y, w, rdx, rdy,
                           rslide_a, rslide_b, rload_dx, rload_dy, jostle,
                           driven_gas);
    }
    return any;
}

/*
 * Sub-pass 2: perpendicular spread - mirrors sand_liquid.c's
 * equalise_liquids()/equalise_one_row()/equalise_one_cell(), whole-grain
 * instead of mass-based.
 */

/* Mirrors has_room_below() in sand_liquid.c: a grain that can still rise
 * this step was already moved by sub-pass 1, so one comparison keeps this
 * search off the bill while a pocket is mostly rising. */
/* BOTH PROBES TAKE THEIR ROW, not a y to multiply: the target row is fixed
 * for a whole equalise row, so recomputing ny * w + nx per cell paid a
 * multiply and a bounds test 41,216 times for two pointers the row loop can
 * hand down. dest_row() returning NULL is that bounds test, done once. */
static inline bool has_room_above(const uint8_t *arow, int x, int rdx, int w)
{
    const int fx = x + rdx;
    if (arow == NULL || (unsigned)fx >= (unsigned)w) {
        return false;
    }
    return CELL_IS_EMPTY(arow[fx]);
}

/* Whether the immediate neighbour along (px, py) is worth searching past:
 * truly empty, OR the same gas - unlike a wall or denser material. Gas
 * has no level to compare (unlike liquid), so treating another gas cell
 * as a stop would block find_nearest_empty() from reaching real space
 * further out. Matters for a 2D pour under a ceiling: many independent
 * rows, a row mid-pour getting grains faster than one pass resolves. */
static inline bool neighbour_is_open(const uint8_t *nrow, int x, int px,
                                     int w, uint8_t gas_id)
{
    const int nx = x + px;
    if (nrow == NULL || (unsigned)nx >= (unsigned)w) {
        return false;
    }
    const cell_t n = nrow[nx];
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
    /* Loop control was a third of this walk's branches - one load, two
     * rejects per iteration - and straight-line runs pay on a core with no
     * branch predictor. find_shallowest(), the liquid twin, gets nothing:
     * GCC declines there, a liquid's `sight` capping the walk at 8 where
     * smoke's 24 makes four copies worth it. */
#pragma GCC unroll 4
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
static inline bool equalise_gas_one_cell(sand_t *s, uint8_t *row,
                                         const uint8_t *arow,
                                         const uint8_t *nrow, int x,
                                         int y, int px, int py, int rdx,
                                         int rdy, int sight, uint8_t gas_id,
                                         cell_t grain, bool *stayed_in_row,
                                         int *touched_x, bool carry_ok,
                                         gas_run_t *run)
{
    bool moved = false;
    int  tx = 0, ty = 0;

    if (!has_room_above(arow, x, rdx, s->w) &&
        neighbour_is_open(nrow, x, px, s->w, gas_id)) {
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

static inline bool equalise_gas_one_row_cell(sand_t *s, uint8_t *row,
                                             const uint8_t *arow,
                                             const uint8_t *nrow, int x,
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
    if (equalise_gas_one_cell(s, row, arow, nrow, x, y, px, py, rdx, rdy,
                                              sight, id, c, &stayed_in_row, &tx, carry_ok, run)
        && stayed_in_row) {
        gas_union_touched_x(touched, touched_x0, touched_x1,
                            x < tx ? x : tx, x > tx ? x : tx);
    }
    return true;
}

/* A row with no empty cell in it cannot spread - see equalise_gas_one_cell()
 * for why ty == y when py == 0. BREAKS on the first empty cell (a packed row
 * pays w loads instead, to skip two probes per gas cell - 14578us of a fire
 * step). Also reports the widest gas `sight` actually in the row, free from
 * the same scan, so a tilted caller can bound its skip by what this row
 * could reach rather than the worst case over every gas material
 * (smoke's 24). */
static inline bool row_is_packed(const uint8_t *row, int w, uint16_t is_gas,
                                 bool *any_gas, int *max_sight)
{
    bool gas   = false;
    int  sight = 0;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            return false;
        }
        if (((is_gas >> CELL_MATERIAL(c)) & 1u) != 0) {
            gas = true;
            const int cur = material_of(c)->sight;
            if (cur > sight) {
                sight = cur;
            }
        }
    }
    *any_gas   = gas;
    *max_sight = sight;
    return true;
}

/* One row's share of spread. Returns whether it held any gas. No
 * ROW_NO_GAS equivalent yet - deferred until real usage patterns exist to
 * measure against, same as every other tunable in this project; may_have_gas
 * alone is the pass's cheap-skip for now. */
static bool equalise_gas_one_row(sand_t *s, int y, int w, int x_from,
                                 int x_to, int x_step, int px, int py,
                                 int rdx, int rdy, uint16_t is_gas,
                                 int *clean_run)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;
    /* Both fixed for the whole row - see has_room_above()'s comment. */
    const uint8_t *const arow = dest_row(s, y + rdy);
    const uint8_t *const nrow = dest_row(s, y + py);
    bool any_gas = false;
    bool touched = false;
    int  touched_x0 = 0, touched_x1 = 0;

    /* carry_ok gates gas_run_t's cheap re-walk skip off the moment gravity
     * is not axis-aligned - see gas_run_t's own comment for why the sweep
     * geometry it relies on only holds when py == 0. */
    const bool carry_ok = (py == 0);

    /* Reset at the start of every row: a run only ever describes cells
     * within the same row, and the sweep has not looked at any of them
     * yet. */
    gas_run_t run = { .id = -1, .len = 0 };

    /* A tilted ray from THIS row's own cells can only reach rows already
     * behind the sweep - *clean_run counts consecutive packed rows there
     * (see equalise_gas()'s own comment). Once that count covers this row's
     * own widest gas sight, no cell in it has anywhere left to reach, same
     * conclusion as py == 0's own-row check, just aimed further out. */
    int row_sight = 0;
    const bool packed = row_is_packed(row, w, is_gas, &any_gas, &row_sight);
    const bool skip =
        packed && (py == 0 || *clean_run >= row_sight);
    *clean_run = packed ? *clean_run + 1 : 0;
    if (skip) {
        return any_gas;
    }

    /* After row_is_packed(), not before: *clean_run has to stay an exact
     * count of consecutive packed rows for the tilted skip above. That scan
     * breaks on the first empty cell, so on a sparse row it is a handful of
     * loads and the per-cell loop below is what the skip is worth. */
    if (!gas_row_may_hold(y)) {
        return false;
    }

    for (int x = x_from; x != x_to; x += x_step) {
        if (equalise_gas_one_row_cell(s, row, arow, nrow, x, y, px, py, rdx, rdy,
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

static bool gas_row_audit_on;

void sand_gas_row_audit_enable(bool on)
{
    gas_row_audit_on = on;
}

unsigned sand_gas_row_audit_failures;
unsigned sand_gas_row_audit_skippable;

/* Re-derives the map the expensive way and compares, so a mover that moves a
 * gas cell without arming the row it lands in is caught on whatever board the
 * caller is already running rather than on one written to suspect it. Off by
 * default and read once per pass, not per row. */
static void gas_row_audit(const sand_t *s)
{
    for (int y = 0; y < s->h; y++) {
        if (gas_row_may_hold(y)) {
            continue;
        }
        sand_gas_row_audit_skippable++;
        const uint8_t *row = s->cells + (size_t)y * (size_t)s->w;
        for (int x = 0; x < s->w; x++) {
            const cell_t c = row[x];
            if (!CELL_IS_EMPTY(c) &&
                ((gas_kind_mask >> (c >> 3)) & 1u) != 0u) {
                sand_gas_row_audit_failures++;
                return;
            }
        }
    }
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

    /* Consecutive packed rows immediately behind the sweep pointer. py > 0
     * sweeps y descending while a tilted ray reads downward (increasing y);
     * py < 0 sweeps ascending while the ray reads upward - either way the
     * ray's targets are exactly the rows this count has already crossed. */
    int clean_run = 0;

    for (int y = y_from; y != y_to; y += y_step) {
        if (equalise_gas_one_row(s, y, w, x_from, x_to, x_step, px, py, rdx,
                                 rdy, is_gas, &clean_run)) {
            found_any = true;
        }
    }
    return found_any;
}

/* The whole step. */

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
    gas_row_map_live = (s->h <= GAS_ROW_MAX);
    memset(gas_row_map.w, 0, sizeof gas_row_map.w);

    for (int y = y_from; y != y_to; y += y_step) {
        if (step_one_gas_row(s, y, w, rdx, rdy, rslide_a, rslide_b,
                             rx_step, rload_dx, rload_dy, jostle,
                             driven_gas)) {
            found_any = true;
        }
    }

    if (gas_row_audit_on) {
        gas_row_audit(s);
    }

    /* Then spread, alternating which way it looks each step - same reason
     * liquid's cross-flow does (see sand_step_liquids() in sand_liquid.c).
     * Kept on its own flip flag rather than sharing liquid_flip, so gas's
     * alternation is not coupled to whether water also moved this step. */
    if (equalise_gas(s, s->gas_flip ? perp_a : perp_b,
                                     rdx, rdy)) {
        found_any = true;
    }
    s->gas_flip = !s->gas_flip;

    if (!found_any) {
        s->may_have_gas = false;
    }
}
