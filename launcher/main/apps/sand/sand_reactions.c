/*=============================================================================
 * sand_reactions - fire chemistry: ignites fuel, spreads, is extinguished,
 * burns out.
 *
 * Two materials burn today: fire itself, and the ember a log of wood chars
 * into. Both are dispatched by reaction_t.burns (material.h), not by
 * checking CELL_MATERIAL(c) == MAT_FIRE - that used to be enough when fire
 * was the only heat source, and would now silently ignore ember entirely.
 *
 * WHY WOOD CHARS INTO AN EMBER RATHER THAN IGNITING STRAIGHT TO FIRE
 *
 * This is the one genuinely non-obvious call in this file, worth
 * understanding before touching either material. The obvious design -
 * wood ignites straight to MAT_FIRE, the same way gas does - does not
 * work: fire is KIND_GAS, so a wood cell that became fire would float
 * away on the very next sand_step_gas() pass, leaving a hole where the
 * log was. A log would dissolve into rising flames that drift off, often
 * before they get a turn to ignite the next wood cell along - the burn
 * stalls, or races, depending on nothing the player can see.
 *
 * MAT_EMBER fixes this by splitting the two jobs fire was doing at once.
 * The ember is KIND_STATIC and stays exactly where the log was - it keeps
 * igniting its neighbours, keeps decaying, and eventually burns out, all
 * without moving. The flame licking up off it (reaction_t.flare, below)
 * is ordinary, separate MAT_FIRE, purely for looks and for reaching fuel
 * stacked above - it rises on its own through sand_step_gas() precisely
 * because it is unrelated code from this ember's point of view. "Wood
 * burning below, flame above" falls out of two materials doing their own
 * simple thing, not one material trying to be both a heat source and a
 * moving flame simultaneously.
 *
 * Unlike gas or liquid, nothing here ever relocates a cell - every
 * mutation is IN PLACE: a flammable neighbour becomes fire (or, for
 * wood, an ember) on ignition, a burning cell becomes CELL_EMPTY on
 * extinguish or decay-burnout. That means this pass needs none of
 * sand_step_gas()'s reversed-sweep-order machinery (no move can
 * double-move a cell that never moves at all) - a single fixed, arbitrary
 * scan order is enough.
 *
 * It does, however, mean something related and worth being explicit
 * about: this scan reads row[x] fresh at every index, so a cell ignited
 * earlier in THIS SAME pass (because it was positioned ahead of the scan
 * pointer) is itself read as burning when the scan reaches it, and gets
 * its own turn to ignite further. A cell ignited but positioned BEHIND
 * the scan pointer only becomes fire this step - it does not get to
 * spread further until the NEXT sand_step() call. This is a deliberate,
 * confirmed design choice (explosion-like spread through a connected
 * pocket of fuel, not a slow creep), not an oversight - see
 * docs/Sand/Adding-a-Material.md and the plan this was built from for
 * the full reasoning. It means "a whole pocket ignites in one step" is
 * true for pockets laid out ahead of this pass's own scan direction
 * (fixed row-major, top-to-bottom then left-to-right - arbitrary, but
 * fixed), not a geometry-independent guarantee.
 *
 * Burning out reuses tick_decay() (sand_priv.h) unchanged - the same
 * variant-nibble-as-life-remaining mechanism gas's own decay already
 * proved out, extracted there specifically so this file and sand_gas.c
 * could share it rather than duplicate it. That mechanism is what lets
 * ember decay too, even though it is KIND_STATIC and never touched by
 * the main sweep or the gas pass - this file is the only thing that ever
 * gives an ember cell a turn at all.
 *
 * Fire is kind = KIND_GAS (see material.c), so it ALSO rises and
 * disperses through sand_step_gas() - a genuinely different pass, run
 * earlier in the same sand_step() call. This file stays movement-free
 * regardless: a fire cell that rose this step reacts from its NEW
 * position, which is simply "whatever row[x] holds right now" from this
 * pass's point of view, not something it needs to know or care about.
 * Ember has no such second pass - it is KIND_STATIC and never moves at
 * all, which is exactly the point (see above).
 *
 * Sand/water sinking through fire (density-based, exactly like they
 * already sink through gas) does not, by itself, extinguish it - it
 * only relocates fire to the vacated cell, the same swap gas gets.
 * smothered() below is what makes a sustained, fully-enclosing burial
 * actually put it out, rather than fire always finding somewhere to pop
 * back up to. Ember, at density 150, is essentially never smothered this
 * way - smothered() needs all four neighbours STRICTLY denser, and only
 * stone (200) qualifies, so burying a log in sand will not put it out.
 * That is an accepted limitation, not a bug to chase: only decay, or
 * water, ends an ember today.
 *===========================================================================*/

#include "sand_priv.h"

/* The four cardinal directions a burning cell's reactions look in. Not
 * eight - diagonal spread felt too generous for a first version; widening
 * this table is the whole of what changing that would take. Also the
 * fixed, arbitrary order ember's flare (below) picks its target from. */
static const int reaction_dirs[4][2] = {
    { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 },
};

/* Write `mat` into the cell at (x, y) - at is its precomputed index, so
 * every caller that already looked the cell up once does not have to
 * multiply out y*w+x a second time - with every piece of bookkeeping a
 * new cell needs. Everything in this file that creates a cell goes
 * through here.
 *
 * The may_have_* latching is the reason this exists rather than being
 * three lines at each call site. This pass can create cells of a KIND
 * different from what was there a moment ago - fuel igniting into fire
 * or ember, and (once quenching and heat conduction exist) a liquid
 * boiling into steam - and forgetting to latch the right may_have_* flag
 * for the material just created leaves that cell sitting frozen on the
 * grid forever, because the pass gated on that flag early-returns
 * without ever looking at it again. That failure is invisible to any
 * test that happens to create the same kind of cell some OTHER way in
 * the same scene, which is most of them. Mirrors sand_set()'s own
 * independent-ifs block, for exactly the same reason it is written that
 * way there. */
static inline void place_reacted(sand_t *s, int x, int y, size_t at,
                                 material_id_t mat)
{
    s->cells[at] = CELL_MAKE(mat, MATERIAL_VARIANTS - 1);

    const material_t *m = &materials[mat];
    if (m->kind == KIND_LIQUID) {
        s->may_have_liquid = true;
    }
    if (m->kind == KIND_GAS) {
        s->may_have_gas = true;
    }
    if (reactions[mat].burns) {
        s->may_have_burning = true;
    }

    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
}

/* Whether (nx, ny) is in bounds and holds a liquid - the one thing that
 * extinguishes a burning cell. */
static inline bool neighbor_is_liquid(const sand_t *s, int nx, int ny, int w,
                                      int h)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    return !CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_LIQUID;
}

/* Whether (nx, ny) is in bounds and holds something strictly denser than
 * `density` (the burning cell's own), and is not a liquid - liquid
 * already has its own, more generous single-touch extinguish rule,
 * checked separately and first, so it should not also count here.
 * Strictly denser mirrors can_enter()'s own displacement rule: a
 * neighbour at the burning cell's own density or below (more fire, or
 * plain gas) never counts, or a large, dense pocket of fire/gas would
 * smother itself from the inside out - only genuinely being buried
 * under something heavier (sand, stone) should. */
static inline bool neighbor_smothers(const sand_t *s, int nx, int ny,
                                     int w, int h, uint8_t density)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    const material_t *nm = material_of(n);
    return nm->kind != KIND_LIQUID && nm->density > density;
}

/* Whether every one of the 4 cardinal neighbours smothers this cell -
 * true burial, not a single denser touch. An ALL-of-4 predicate, unlike
 * neighbor_is_liquid()'s/try_ignite()'s own per-neighbour ANY/EACH loops
 * above and below - a gap on even one side means real air still reaches
 * it, so it returns false the moment any direction fails rather than
 * accumulating across all four. */
static inline bool smothered(const sand_t *s, int x, int y, int w, int h,
                             uint8_t density)
{
    for (int d = 0; d < 4; d++) {
        if (!neighbor_smothers(s, x + reaction_dirs[d][0],
                               y + reaction_dirs[d][1], w, h, density)) {
            return false;
        }
    }
    return true;
}

/* Ignites (nx, ny) in place if it is in bounds, holds a non-empty
 * flammable material, and the roll for it succeeds. Returns whether it
 * did - the caller needs this to know whether this burning cell reacted
 * at all. Wake/dirty bookkeeping targets (nx, ny), the cell that
 * actually changed - not whatever burning cell called this, which did
 * not. */
static inline bool try_ignite(sand_t *s, int nx, int ny, int w, int h)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    const reaction_t *r = reaction_of(n);
    if (r->flammability == 0) {
        return false;
    }
    /* s->flammability mirrors s->decay's own override (see
     * sand_set_flammability()): negative (the default) means "each
     * material's own table figure", anything else overrides every
     * material alike.
     *
     * 255 is checked before rolling, not by rolling and always winning:
     * gas is 255, and drawing a random number here would shift the RNG
     * stream for every existing gas/fire scene, changing results that
     * are currently exactly reproducible (the device frame-budget tests
     * depend on that). Checking first keeps them bit-identical. */
    const int f = (s->flammability >= 0) ? s->flammability : r->flammability;
    if (f < 255 && (int)(rng_next(&s->rng) & 0xFF) >= f) {
        return false;
    }
    /* 0 (MAT_EMPTY) reads as MAT_FIRE, so a flammable material that does
     * not care what it turns into (gas) gets the obvious default for
     * free - only a material that needs something else (wood, into an
     * ember) has to say so. */
    const material_id_t becomes = r->ignites_to ? r->ignites_to : MAT_FIRE;
    place_reacted(s, nx, ny, at, becomes);
    return true;
}

/* Ember's flame: rolls reaction_t.flare once per step, and on a hit
 * places ordinary MAT_FIRE in the first empty cell among the four
 * cardinals, in reaction_dirs[] order - arbitrary but fixed, exactly
 * like this pass's own scan order. Direction-agnostic on purpose: the
 * emitted fire rises by itself through sand_step_gas() once it exists,
 * so this code does not need to know, or care, which way is up. Returns
 * whether it placed anything. */
static inline bool try_flare(sand_t *s, int x, int y, int w, int h,
                             uint8_t flare)
{
    if (flare == 0 || (int)(rng_next(&s->rng) & 0xFF) >= flare) {
        return false;
    }
    for (int d = 0; d < 4; d++) {
        const int fx = x + reaction_dirs[d][0];
        const int fy = y + reaction_dirs[d][1];
        if ((unsigned)fx >= (unsigned)w || (unsigned)fy >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)fy * (size_t)w + (size_t)fx;
        if (CELL_IS_EMPTY(s->cells[at])) {
            place_reacted(s, fx, fy, at, MAT_FIRE);
            return true;
        }
    }
    return false;
}

/* One burning cell's turn, in priority order: burn down first (a cell
 * that vanishes this step gets no turn to react further - it cannot
 * both die and spread the same step); extinguish if any neighbour is a
 * liquid (wins outright over everything below, even if a flammable
 * neighbour or three smothering ones also touch this cell); extinguish
 * again if every neighbour smothers it (true burial - see smothered()
 * above, this is the only way sand puts fire out, since sand passes
 * through it uneventfully otherwise, the same way it already passes
 * through gas); otherwise ignite every non-empty, flammable neighbour
 * found - a cell touching fuel on three sides lights all three, not just
 * one - and, if this material flares (ember, not fire), roll for that
 * too. Ignition and flaring are independent of each other rather than
 * either/or: a burning cell can perfectly well do both in the same
 * step. */
static bool step_one_burning_cell(sand_t *s, uint8_t *row, int x, int y,
                                  int w, int h)
{
    cell_t grain = row[x];
    const material_t *mat = material_of(grain);
    const uint8_t mat_id  = CELL_MATERIAL(grain);

    if (!tick_decay(s, row, x, y, &grain, mat, mat_id)) {
        return true;    /* burned out - already woken, nothing left to react */
    }

    for (int d = 0; d < 4; d++) {
        if (neighbor_is_liquid(s, x + reaction_dirs[d][0],
                               y + reaction_dirs[d][1], w, h)) {
            row[x] = CELL_EMPTY;
            mark_rows(s, y, y);
            wake_block_and_neighbors(s, x, y);
            return true;
        }
    }

    if (smothered(s, x, y, w, h, mat->density)) {
        row[x] = CELL_EMPTY;
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return true;
    }

    bool acted = false;
    for (int d = 0; d < 4; d++) {
        if (try_ignite(s, x + reaction_dirs[d][0], y + reaction_dirs[d][1],
                       w, h)) {
            acted = true;
        }
    }

    if (try_flare(s, x, y, w, h, reactions[mat_id].flare)) {
        acted = true;
    }

    return acted;
}

/* One row's share of the scan - only burning cells are dispatched; keyed
 * on reaction_t.burns (material.h), NOT on kind == KIND_STATIC. Stone
 * and ember both share that kind, and every unused material slot shares
 * it too, so a kind-based check here would treat plain stone as a heat
 * source - the exact mistake caught before this shipped, back when fire
 * was the only thing that could burn and MAT_FIRE was tried directly
 * (see the plan this was built from).
 *
 * Presence, not activity: a burning cell that neither reacts nor decays
 * this step (no fuel or liquid touching it, roll not hit) still exists,
 * and may_have_burning must stay set for it - latched the moment the
 * cell is IDENTIFIED as burning, before step_one_burning_cell() runs,
 * mirroring step_one_gas_row's own already-fixed presence-not-movement
 * pattern (sand_gas.c) for the identical reason: getting this backwards
 * would let a quiet, untouched burning cell fall out of
 * may_have_burning's bookkeeping while still physically on the grid,
 * stranding its own eventual burn-out. */
static bool step_one_burning_row(sand_t *s, int y, int w, int h)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;

    bool any = false;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c) || !reaction_of(c)->burns) {
            continue;
        }
        any = true;
        step_one_burning_cell(s, row, x, y, w, h);
    }
    return any;
}

void sand_step_reactions(sand_t *s)
{
    if (!s->may_have_burning) {
        return;
    }

    const int w = s->w;
    const int h = s->h;

    bool found_any = false;
    for (int y = 0; y < h; y++) {
        if (step_one_burning_row(s, y, w, h)) {
            found_any = true;
        }
    }

    if (!found_any) {
        s->may_have_burning = false;
    }
}
