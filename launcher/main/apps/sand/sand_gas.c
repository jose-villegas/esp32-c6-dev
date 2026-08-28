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
 * That reversed pass reuses sand.c's own try_fall_or_scatter()/try_slide()
 * directly - gas is a whole grain, not a mass amount, and those two
 * functions are already written generically against a direction vector,
 * not hardcoded to "down". Two things need care doing that, both explained
 * where they happen below: driven_by_gravity() needs a gas-local table
 * built against the REVERSED gravity vector, not the main sweep's own; and
 * the reversed sweep's x order has to reuse sand_step()'s own x_step
 * negated, not call sweep_x_order() a second time (that function has a
 * side effect - see its own comment in sand.c).
 *
 * Rising alone only piles gas into a heap against whatever it hits - the
 * same shape water's own gravity-ward primitive produces alone, which is
 * exactly why sand_liquid.c's cross-flow pass exists. equalise_gas() below
 * is gas's version of that: mirrors equalise_liquids()'s structure closely
 * (see sand_liquid.c), swapping mass-splitting for a plain whole-cell hop
 * to the nearest open cell along the perpendicular.
 *
 * Gas rising through standing LIQUID is its own problem, solved by
 * try_bubble() below rather than by the shared movement primitives -
 * can_enter() cannot express mobility, and it is far too hot a predicate
 * to teach it. See that function's own comment.
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
        if (materials[m].kind == KIND_GAS) {
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
 * powder (try to fall/rise, then try the two slides), but with an explicit
 * wake, since this pass gets none of step_one_block()'s own moved_here ->
 * BLOCK_ACTIVE bookkeeping (it is not walked by step_one_block() at all).
 *
 * Unlike a powder, the whole attempt is gated behind material.h's
 * `mobility` roll first (jostle == 0 only - shaking bypasses it, same as
 * every other resistance here): a grain that misses the roll just sits
 * this step, decay tick aside, which is what makes gas rise at a lazy
 * drift instead of sand's instant one-cell-per-step. */
/* A BUBBLE: a gas cell trading places with the LIQUID directly above it.
 *
 * This is the one movement in the whole simulation that runs against
 * can_enter()'s rule rather than through it, and it needs to, because that
 * rule cannot express mobility. can_enter() only ever lets a DENSER mover
 * displace a LIGHTER target - which is right for sand sinking through
 * water, and exactly backwards for steam rising through it. Worse, a
 * liquid never consults can_enter() at all: room_in() (sand_liquid.c)
 * refuses any cell holding a different material outright, so water will
 * not fall into a steam cell either. Between the two rules, a gas cell
 * underneath standing liquid had NO legal move in either direction and
 * simply sat there forever - a bubble frozen mid-pour, which is what this
 * fixes.
 *
 * Deliberately NOT solved by touching can_enter(). That predicate is the
 * hottest thing in the project, read several times per cell per step from
 * the main sweep, and a mobility special case there would be paid for by
 * every falling grain of sand on the board forever. Here it costs one
 * comparison, only for gas cells, only in a pass already gated behind
 * may_have_gas, and only on the cells whose ordinary rise was already
 * blocked.
 *
 * Why the swap is safe with respect to sweep order, which is the usual
 * hazard for anything that moves two cells at once: this pass sweeps so
 * that the rise destination is territory it has ALREADY visited, so the
 * liquid that lands at (x, y) cannot be picked up again by this pass. The
 * liquid passes (sand_step_liquids(), and move_liquid_grain() inside the
 * main sweep) both ran EARLIER in this same sand_step(), so they are done
 * for the step too. The displaced liquid therefore gets exactly one move,
 * the same guarantee every other move in this file has.
 *
 * Mass is conserved by construction - this is a swap of two whole cells,
 * and the liquid keeps its own variant nibble (its amount) untouched as it
 * moves. Nothing is split, so nothing can round away. */
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

    prow[nx] = grain;
    row[x]   = target;

    mark_rows(s, y, y + rdy);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, y + rdy);
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

    if (!tick_decay(s, row, x, y, &grain, mat, mat_id)) {
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
    if (try_moving && jostle == 0) {
        const int scatter = (s->scatter >= 0) ? s->scatter : mat->scatter;
        if (try_fall_or_scatter(s, row, prow, arow, brow, x, y, w, rdx, rdy,
                                rslide_a, rslide_b, grain, density,
                                scatter)) {
            moved = true;
        }
    }
    if (try_moving && !moved) {
        moved = try_slide(s, row, prow, arow, brow, x, y, w, rdx, rdy,
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

    /* Presence, not movement: a gas cell that neither moves nor decays this
     * step (jammed solid, roll not hit) still exists, and may_have_gas must
     * stay set for it - the same reason equalise_gas_one_row_cell() below
     * counts a cell as found the moment it is gas, before it even attempts
     * to move it. Getting this wrong meant a trapped, motionless pocket of
     * gas could clear may_have_gas while still physically on the grid -
     * harmless before decay existed (it just sat inert), but with decay it
     * would strand that gas immortal, since sand_step_gas() early-returns
     * on !may_have_gas and decay only ever rolls from inside this loop. */
    bool any = false;
    for (int x = x_from; x != x_to; x += rx_step) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c) || material_of(c)->kind != KIND_GAS) {
            continue;
        }
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
 * somewhere to rise THIS step, sub-pass 1 above already moved it (or will,
 * being swept before this runs) - one comparison, and it is what keeps
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
 * truly empty, OR the same gas - unlike a wall or a denser material, which
 * really does mean "nothing to do here, don't bother searching further".
 *
 * The earlier version only returned true for CELL_IS_EMPTY, mirroring
 * neighbour_is_lower()'s own fast path too closely: for a liquid that
 * fast path is correct, because "immediate neighbour holds the same
 * liquid" can still be a real level imbalance worth transferring mass
 * over. For a whole-grain gas there is no level to compare, so the old
 * check treated "touching another gas cell" exactly like "touching a
 * wall" - never even calling find_nearest_empty() (which already happily
 * passes through same-gas cells looking for room) to find out whether
 * real space existed two cells further out.
 *
 * Note this pass still resolves most single-row runs the SAME way either
 * version does, because equalise_gas_one_row() sweeps in the direction it
 * is giving to, so a cell that moves clears the way for the very next
 * cell processed in the same pass - the same cascading equalise_liquids()
 * relies on. Where this actually matters is what that cascade cannot
 * reach: a dense 2D pour under a ceiling is many INDEPENDENT rows (this
 * pass never crosses rows for straight-down gravity), and a row still
 * mid-pour gets new grains added faster than one pass resolves - this
 * check is what lets each of those rows use real nearby space the moment
 * it is asked, rather than waiting on a lucky sweep direction or another
 * grain moving first. */
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

/* Mirrors find_shallowest(): the nearest open cell along (px, py) within
 * `sight` cells, or 0 if none. Passes through other gas cells the same way
 * find_shallowest() passes through the same liquid - flow stops at
 * anything else (a wall, a different material), same as water's own
 * search does.
 *
 * On a screen already saturated with one gas this walk is the single
 * biggest cost in the whole gas step: every cell drags its material's
 * whole `sight` through identical neighbours before giving up.
 * equalise_gas_one_cell() avoids re-walking the same cells on the very
 * next cell processed - see gas_run_t's own comment for the geometry that
 * makes that safe - but only when THIS scan was the expensive kind:
 * `*run_len_out` is written - always to `sight` - only when the loop runs
 * every one of `sight` cells without finding an empty or a blocker. It is
 * left untouched otherwise: when an empty was found (the caller resets its
 * run to nothing regardless, so the value is moot), and when the loop
 * instead breaks off the edge of the grid or on a wall/different material.
 * Those two breaks are cheap by construction - they stop within a cell or
 * two - so they are not worth reporting; see equalise_gas_one_cell's own
 * comment for what it cost the one time this file tried remembering them
 * anyway. */
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

/* equalise_gas_one_row() sweeps each row with x_step = -px (see
 * equalise_gas() below: x_step is -1 when px > 0, +1 when px < 0) - the
 * sweep always advances by exactly one cell OPPOSITE the ray direction.
 * That means the cell processed right after x, which sits at x - px,
 * casts a ray along (px, py) that starts by revisiting x, then x + px,
 * x + 2*px, ... - exactly the same cells x's own ray just walked, shifted
 * out by one. So whatever find_nearest_empty() just established about x's
 * ray - "the next N cells are all this same gas, with no empty among
 * them" - is ALSO true of x - px's ray, once cell x itself (known, right
 * here, to hold that gas and not have moved) is counted at the near end.
 * Reusing that fact is what lets the row sweep skip a full `sight`-length
 * re-walk of a packed gas pocket on almost every cell: only the newest
 * cell needs to be looked at, not the whole tail again.
 *
 * `id` is the material the verified run is made of, or -1 if nothing is
 * currently verified - the reset value whenever an empty/non-gas cell was
 * just crossed, a grain just moved and changed what lies ahead, or the
 * last real scan found an empty and so proved nothing about a longer run.
 * `len` is how many cells starting one ray-step from the CURRENT cell are
 * already confirmed to hold `id` with no empty among them - always at
 * least `sight` once `id` is valid, since a run is only ever armed from a
 * scan that walked the whole of `sight` (see find_nearest_empty's own
 * comment) and only ever grows from there.
 *
 * The whole argument rests on the sweep advancing by exactly -px, which is
 * only guaranteed when py == 0: with a tilted perpendicular (py != 0,
 * diagonal gravity) the next cell's ray is a diagonal step away, not a
 * copy of this cell's ray shifted by one, and none of the above holds.
 * equalise_gas_one_cell() only ever writes a real id/len when carry_ok is
 * true, so `id` simply stays -1 for the whole pass and the skip check
 * below never fires when it is not - cheaper than adding a branch to strip
 * valid state back out on every cell. */
typedef struct {
    int id;
    int len;
} gas_run_t;

/* One cell's share of spread: hops the whole grain to the nearest open
 * cell along (px, py), if sub-pass 1 could not already move it and a real
 * gap exists. No mass to split - a grain either moves the whole way, or
 * not at all.
 *
 * `run` carries gas_run_t's verified-run state between cells of the same
 * row sweep (see that struct's own comment for the geometry) and
 * `carry_ok` is that carry's on/off switch, true only when py == 0. Two
 * things can happen to `run` here:
 *   - the grain moved: the cells ahead just changed, so nothing about them
 *     can be trusted any more - `run->id` resets to -1;
 *   - the grain did not move: if `run` already described this same
 *     material, it is folded forward by one to also cover this cell, seen
 *     from the next one down the sweep. If it described something else (or
 *     nothing), it is left exactly as it stood - NOT restarted against
 *     this cell.
 *
 * That last case used to restart the run here too: any cell that did not
 * move and did not already match got armed at length one against whatever
 * material it held - even a cell that never scanned at all, blocked before
 * it got the chance. Measured against a smoke/steam screen that alternates
 * the two gases cell by cell - a scene that never holds a long run of one
 * gas, so the memo above never once pays off - that arm-from-nothing cost
 * +4.4% all by itself: every cell paid to write a length-one run that the
 * very next cell, almost always the other gas, would throw away unread
 * without ever reaching the `len >= sight` the skip check needs. Leaving
 * `run` alone here instead is still safe with no fresh arm to replace it: a
 * gas cell of a different material is never empty, so it always blocks a
 * same-ray scan cast from any earlier cell in the sweep, regardless of
 * which stale material `run` still names - which is exactly the answer
 * the skip check below gives when it fires on stale data. Do not re-add
 * that branch - it was tried, it cost real time, and it bought nothing. */
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
            if (carry_ok && at == 0 && scan_len == sight) {
                /* Only a scan that paid the full `sight` walk is worth
                 * remembering - see find_nearest_empty's own comment for
                 * why the two early-break cases (the edge of the grid, a
                 * wall or a different material) are left alone instead:
                 * they are already cheap, so caching them would cost more
                 * than just repeating them next time. */
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
     * comment on `sight` for why: two materials can share this pass
     * (gas, fire) and disperse by different amounts. */
    const int sight = materials[id].sight;

    bool stayed_in_row = false;
    int  tx = 0;
    if (equalise_gas_one_cell(s, row, x, y, px, py, rdx, rdy, sight, id, c,
                              &stayed_in_row, &tx, carry_ok, run) &&
        stayed_in_row) {
        gas_union_touched_x(touched, touched_x0, touched_x1,
                            x < tx ? x : tx, x > tx ? x : tx);
    }
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
     * geometry it relies on only holds when py == 0. Computed once per row
     * (it is really constant for the whole equalise_gas() call, since px
     * and py do not change mid-pass) rather than re-checked per cell. */
    const bool carry_ok = (py == 0);

    /* Reset at the start of every row: a run only ever describes cells
     * within the same row, and the sweep has not looked at any of them
     * yet. */
    gas_run_t run = { .id = -1, .len = 0 };

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

    const int rdx = -dx, rdy = -dy;
    const int rslide_a[2] = { -slide_a[0], -slide_a[1] };
    const int rslide_b[2] = { -slide_b[0], -slide_b[1] };
    const int rload_dx = -load_dx, rload_dy = -load_dy;
    const int rx_step  = -x_step;

    /* driven_by_gravity()'s descent = m . g dot product is against real
     * gravity - feeding it gas's reversed slide vectors together with the
     * main sweep's own forward (gx, gy) would make descent negative for
     * every gas slide, unconditionally, so they would never fire. Built
     * fresh here against the reversed vector instead - cheap enough
     * (MATERIAL_MAX is 16) that duplicating compute_driven()'s own loop
     * shape inline is simpler than sharing it. */
    bool driven_gas[MATERIAL_MAX][2];
    for (int m = 0; m < MATERIAL_MAX; m++) {
        const int repose = materials[m].repose;
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
    for (int y = y_from; y != y_to; y += y_step) {
        if (step_one_gas_row(s, y, w, rdx, rdy, rslide_a, rslide_b, rx_step,
                             rload_dx, rload_dy, jostle, driven_gas)) {
            found_any = true;
        }
    }

    /* Then spread, alternating which way it looks each step - same reason
     * liquid's cross-flow does (see sand_step_liquids() in sand_liquid.c).
     * Kept on its own flip flag rather than sharing liquid_flip, so gas's
     * alternation is not coupled to whether water also moved this step. */
    if (equalise_gas(s, s->gas_flip ? perp_a : perp_b, rdx, rdy)) {
        found_any = true;
    }
    s->gas_flip = !s->gas_flip;

    if (!found_any) {
        s->may_have_gas = false;
    }
}
