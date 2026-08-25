/*=============================================================================
 * sand_reactions - fire: ignites fuel, spreads, is extinguished, burns out.
 *
 * Unlike gas or liquid, nothing here ever relocates a cell - every mutation
 * is IN PLACE: a flammable neighbour becomes fire on ignition, a fire cell
 * becomes CELL_EMPTY on extinguish or decay-burnout. That means this pass
 * needs none of sand_step_gas()'s reversed-sweep-order machinery (no move
 * can double-move a cell that never moves at all) - a single fixed,
 * arbitrary scan order is enough.
 *
 * It does, however, mean something related and worth being explicit about:
 * this scan reads row[x] fresh at every index, so a cell ignited earlier in
 * THIS SAME pass (because it was positioned ahead of the scan pointer) is
 * itself read as fire when the scan reaches it, and gets its own turn to
 * ignite further. A cell ignited but positioned BEHIND the scan pointer
 * only becomes fire this step - it does not get to spread further until
 * the NEXT sand_step() call. This is a deliberate, confirmed design choice
 * (explosion-like spread through a connected pocket of fuel, not a slow
 * creep), not an oversight - see docs/Sand/Adding-a-Material.md and the
 * plan this was built from for the full reasoning. It means "a whole
 * pocket ignites in one step" is true for pockets laid out ahead of this
 * pass's own scan direction (fixed row-major, top-to-bottom then
 * left-to-right - arbitrary, but fixed), not a geometry-independent
 * guarantee.
 *
 * Burning out reuses tick_decay() (sand_priv.h) unchanged - the same
 * variant-nibble-as-life-remaining mechanism gas's own decay already
 * proved out, extracted there specifically so this file and sand_gas.c
 * could share it rather than duplicate it.
 *
 * Fire is kind = KIND_GAS (see material.c), so it ALSO rises and
 * disperses through sand_step_gas() - a genuinely different pass, run
 * earlier in the same sand_step() call. This file stays movement-free
 * regardless: a fire cell that rose this step reacts from its NEW
 * position, which is simply "whatever row[x] holds right now" from
 * this pass's point of view, not something it needs to know or care
 * about. The two passes are independent; only their ordering (gas
 * before reactions) matters, and that ordering already existed before
 * fire could move at all.
 *
 * Sand/water sinking through fire (density-based, exactly like they
 * already sink through gas) does not, by itself, extinguish it - it
 * only relocates fire to the vacated cell, the same swap gas gets.
 * smothered() below is what makes a sustained, fully-enclosing burial
 * actually put it out, rather than fire always finding somewhere to
 * pop back up to.
 *===========================================================================*/

#include "sand_priv.h"

/* The four cardinal directions a fire cell's reactions look in. Not eight -
 * diagonal spread felt too generous for a first version; widening this
 * table is the whole of what changing that would take. */
static const int reaction_dirs[4][2] = {
    { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 },
};

/* Whether (nx, ny) is in bounds and holds a liquid - the one thing that
 * extinguishes fire. */
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
 * `fire_density`, and is not a liquid - liquid already has its own, more
 * generous single-touch extinguish rule, checked separately and first, so
 * it should not also count here. Strictly denser mirrors can_enter()'s
 * own displacement rule: a neighbour at fire's own density or below
 * (more fire, or plain gas) never counts, or a large, dense pocket of
 * fire/gas would smother itself from the inside out - only genuinely
 * being buried under something heavier (sand, stone) should. */
static inline bool neighbor_smothers(const sand_t *s, int nx, int ny,
                                     int w, int h, uint8_t fire_density)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    const material_t *nm = material_of(n);
    return nm->kind != KIND_LIQUID && nm->density > fire_density;
}

/* Whether every one of the 4 cardinal neighbours smothers this cell -
 * true burial, not a single denser touch. An ALL-of-4 predicate, unlike
 * neighbor_is_liquid()'s/try_ignite()'s own per-neighbour ANY/EACH
 * loops above and below - a gap on even one side means real air still
 * reaches it, so it returns false the moment any direction fails
 * rather than accumulating across all four. */
static inline bool smothered(const sand_t *s, int x, int y, int w, int h,
                             uint8_t fire_density)
{
    for (int d = 0; d < 4; d++) {
        if (!neighbor_smothers(s, x + reaction_dirs[d][0],
                               y + reaction_dirs[d][1], w, h,
                               fire_density)) {
            return false;
        }
    }
    return true;
}

/* Ignites (nx, ny) in place if it is in bounds and holds a non-empty,
 * flammable material. Returns whether it did - the caller needs this to
 * know whether this fire cell reacted at all. Wake/dirty bookkeeping
 * targets (nx, ny), the cell that actually changed - not whatever fire
 * cell called this, which did not. */
static inline bool try_ignite(sand_t *s, int nx, int ny, int w, int h)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    /* flammability == 0 is this table's spelling of "not flammable at
     * all" - material.h's own bool `flammable` field, before the
     * reaction properties moved to their own table. A nonzero-but-below-
     * 255 chance (a material that catches slowly rather than instantly)
     * is not wired up to an actual roll yet - nothing in the table has a
     * value like that today, so this stays a plain presence check for
     * now, and gets its dice roll the moment a material that needs one
     * exists. */
    if (CELL_IS_EMPTY(n) || reaction_of(n)->flammability == 0) {
        return false;
    }
    s->cells[at] = CELL_MAKE(MAT_FIRE, MATERIAL_VARIANTS - 1);
    mark_rows(s, ny, ny);
    wake_block_and_neighbors(s, nx, ny);
    return true;
}

/* One fire cell's turn, in priority order: burn down first (a cell that
 * vanishes this step gets no turn to react further - it cannot both die
 * and spread the same step); extinguish if any neighbour is a liquid
 * (wins outright over everything below, even if a flammable neighbour
 * or three smothering ones also touch this cell); extinguish again if
 * every neighbour smothers it (true burial - see smothered() above,
 * this is the only way sand puts fire out, since sand passes through it
 * uneventfully otherwise, the same way it already passes through gas);
 * otherwise ignite every non-empty, flammable neighbour found - a fire
 * cell touching fuel on three sides lights all three, not just one. */
static bool step_one_fire_cell(sand_t *s, uint8_t *row, int x, int y, int w,
                               int h)
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

    bool ignited_any = false;
    for (int d = 0; d < 4; d++) {
        if (try_ignite(s, x + reaction_dirs[d][0], y + reaction_dirs[d][1],
                       w, h)) {
            ignited_any = true;
        }
    }
    return ignited_any;
}

/* One row's share of the scan - only fire cells are dispatched; keyed on
 * the material ID directly (CELL_MATERIAL(c) == MAT_FIRE), NOT on
 * kind == KIND_STATIC. Stone and every unused material slot also share
 * KIND_STATIC, so a kind-based check here would treat plain stone as
 * fire - the exact mistake caught before this shipped (see the plan).
 *
 * Presence, not activity: a fire cell that neither reacts nor decays this
 * step (no fuel or liquid touching it, roll not hit) still exists, and
 * may_have_fire must stay set for it - latched the moment the cell is
 * IDENTIFIED as fire, before step_one_fire_cell() runs, mirroring
 * step_one_gas_row's own already-fixed presence-not-movement pattern
 * (sand_gas.c) for the identical reason: getting this backwards would let
 * a quiet, untouched fire cell fall out of may_have_fire's bookkeeping
 * while still physically on the grid, stranding its own eventual
 * burn-out. */
static bool step_one_fire_row(sand_t *s, int y, int w, int h)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;

    bool any = false;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c) || CELL_MATERIAL(c) != MAT_FIRE) {
            continue;
        }
        any = true;
        step_one_fire_cell(s, row, x, y, w, h);
    }
    return any;
}

void sand_step_reactions(sand_t *s)
{
    if (!s->may_have_fire) {
        return;
    }

    const int w = s->w;
    const int h = s->h;

    bool found_any = false;
    for (int y = 0; y < h; y++) {
        if (step_one_fire_row(s, y, w, h)) {
            found_any = true;
        }
    }

    if (!found_any) {
        s->may_have_fire = false;
    }
}
