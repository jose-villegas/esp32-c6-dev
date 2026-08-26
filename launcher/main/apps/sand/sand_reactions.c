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
 *
 * QUENCHING PRODUCES STEAM, AT A COST - AND BURNING OUT PRODUCES SMOKE
 *
 * A burning cell touched by water used to simply vanish. It still gets
 * put out in one touch - that generosity is unchanged - but now becomes
 * MAT_STEAM instead of CELL_EMPTY (reaction_t.quench_to), and the liquid
 * neighbour that did the quenching pays a unit of its own mass for the
 * privilege (pay_quench_cost(), below).
 *
 * A burning cell that simply runs out of life leaves MAT_SMOKE instead,
 * on a roll of reaction_t.smoke, with no water involved anywhere. The
 * split between the two byproducts is the whole point and is worth
 * stating plainly, because they were ONE material at first and it was
 * wrong:
 *
 *     steam  water that got hot   - boiled through a conductor, or
 *                                   flashed off a fire a liquid put out
 *     smoke  fuel that burned out - a fire or an ember reaching the end
 *                                   of its life
 *
 * Physically they behave almost identically - both are a light gas that
 * rises, spreads and fades - and that is exactly why sharing one row
 * looked right on paper. It was wrong on the SCREEN. A lone fire
 * burning out in mid-air, nowhere near water, puffing bright white
 * kettle-steam reads as a bug to anyone watching, because the player
 * can see for themselves there is nothing there to have boiled. The two
 * rows differ mostly in their palettes (cool and bright for steam, warm
 * and dim for smoke - see material.c), which is the actual payload of
 * the split; the small differences in decay, mobility and sight are
 * flavour on top.
 *
 * THE BOILER: HEAT CONDUCTS, FIRE DOES NOT PASS THROUGH STONE
 *
 * A fire under a one-cell-thick stone basin can boil the water sitting
 * in that basin without fire ever physically crossing the stone -
 * conduct_heat(), below. The obvious alternative - give fire a chance to
 * pass through stone directly - was rejected: it would need a special
 * case inside can_enter(), the single hottest predicate in the project,
 * and it would make every sealed stone container leak fire. Conduction
 * gets the same boiler for zero cost in the main sweep, and it reads
 * better besides: the stone gets hot, the stone does not become porous.
 * Conduction only ever does two things to whatever it reaches on the far
 * side - boil a liquid, or ignite fuel - and NEVER creates fire in empty
 * space, which is what keeps a sealed box sealed.
 *
 * Two problems specific to conduction are solved separately, in the two
 * functions below:
 *
 * Boiling happens AT THE HEAT SOURCE - conduct_heat() converts the very
 * cell it reaches, the one touching the hot conductor, and the steam
 * then climbs out of the pool by itself through try_bubble()
 * (sand_gas.c), roughly a cell a step. A pot on a hot stone therefore
 * reads as a column of bubbles rising off its base, which is what a real
 * one does.
 *
 * That is the second design here, and the first is worth recording
 * because the reasoning was sound and the conclusion still had to be
 * thrown away. Steam is lighter than gas and fire (see material.c), but
 * can_enter()'s displacement rule is one-directional - a cell can only
 * be entered by something DENSER - so steam could not rise into the
 * water above it, and room_in() (sand_liquid.c) would not let that water
 * fall into the steam either. Steam made at the bottom of a pool was
 * therefore stuck there permanently, and the first version of this code
 * worked around it with a boil_surface() walk: climb against gravity
 * through the liquid run and convert the LAST cell instead, so the steam
 * appeared at the surface where it was free to leave. It worked, and it
 * cost this pass a gravity vector it otherwise had no use for.
 *
 * try_bubble() then made gas able to swap places with liquid above it,
 * which dissolved the constraint the walk existed to dodge. The walk,
 * its BOIL_REACH cap and the whole (gx, gy) plumbing came back out
 * again. The lesson worth keeping: a workaround built on a limitation
 * should be re-examined the moment that limitation is lifted, or it
 * quietly outlives its reason and starts looking like a design choice.
 *
 * conduct_heat() solves a reach problem, and this is worth reading
 * carefully because an earlier version got it wrong in a way that was
 * invisible from inside the simulation. Heat crossing exactly one
 * conductor cell reads as a clean rule, and was the first version of
 * this feature - but app_sand.c's pour brush (POUR_RADIUS 5, no size
 * control anywhere in the UI) cannot draw anything one cell thick. The
 * thinnest stone floor a finger can drag out is on the order of eleven
 * cells, so a reach-of-one boiler was unbuildable on the actual device,
 * a fact no amount of testing the simulation in isolation would ever
 * surface. The fix - and what shipped - is a walk that attenuates with
 * thickness instead of stopping cold at one cell: crossing d cells of
 * conductor succeeds with probability (conducts/256)^d, rolled fresh
 * per cell, so a thin wall conducts briskly and a thick one conducts
 * slowly, which is thermal resistance for free and needs no second
 * constant. CONDUCT_REACH bounds the walk (32 cells) so this cold pass
 * still cannot become an unbounded scan; it is a bound on the cost, not
 * a claim about how heat behaves at that depth.
 *
 * That bound was 16, which was itself still too tight for the same
 * reason the reach-of-one was: it assumed a basin floor is about as
 * thick as ONE drag of the pour brush. Nothing stops a player scribbling
 * back and forth, and a floor built that way runs well past sixteen
 * cells - at which point the walk gave up and the boiler was silently,
 * completely dead rather than merely slow. A cost bound should never be
 * the thing that decides whether a feature works, so it now sits far
 * enough out that attenuation, not the cap, is what limits depth in
 * every scene the brush can realistically draw.
 *
 * The general lesson, worth keeping past this one feature: a rule that
 * is clean in the abstract can be unreachable through the very UI that
 * has to produce the scene it depends on, and the material and reaction
 * tables are not where anyone finds that out - only asking "can a
 * player actually draw this" does.
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
/* `spec` is either an ordinary material id, or a whole extended cell byte
 * (MATX(k), whose high nibble is MAT_EXTENDED). The two cannot be confused
 * because an ordinary id is at most MAT_COUNT - 1, well under 0xF0.
 *
 * That is what lets a reaction PRODUCE an extended material: every target
 * field - heats_to, ignites_to, shatters_to, quench_to - is a uint8_t, so
 * MATX(k) fits in one with nothing to change. Without this an extended
 * material could only ever be painted, never made. */
static inline void place_reacted(sand_t *s, int x, int y, size_t at,
                                 uint8_t spec)
{
    if (spec >= (MAT_EXTENDED << 4)) {
        s->cells[at] = (cell_t)spec;     /* identity IS the low nibble */
        latch_content_flags(s, (cell_t)spec);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return;
    }
    const material_id_t mat = (material_id_t)spec;
    /* A heat-ramping material's variant is HEAT, and a cell that has just
     * come into existence has none of it yet. MATERIAL_VARIANTS - 1 would
     * mean sand fusing into glass produced a pane already at the top of its
     * melt ramp, which the very next step would turn into lava - so sand
     * under a steady flame would run to lava in two ticks and the whole
     * duration mechanism would be dead on arrival.
     *
     * Starting cold also makes the ramp measure exposure since THIS cell
     * existed, which is what "long exposure" has to mean for a material
     * that can be created mid-fire. The reading is nice too: sand turns to
     * cool teal glass, glows as the flame keeps working on it, and only
     * then runs. */
    s->cells[at] = CELL_MAKE(mat, reactions[mat].heat_ramp != 0
                                      ? SAND_AMBIENT_HEAT
                                      : MATERIAL_VARIANTS - 1);

    latch_content_flags(s, s->cells[at]);

    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
}

/* Whether (nx, ny) is in bounds and holds a liquid that actually puts
 * fires out - which is not the same as "holds a liquid" any more, and
 * this is the check that has to change first when a second liquid
 * arrives.
 *
 * A liquid quenches only if it is NEITHER fuel (`flammability`) NOR a
 * heat source itself (`burns`). Water is both zero and quenches on one
 * touch exactly as it always has. Oil would otherwise put out the fire
 * that is supposed to be lighting it - and it would win, because this
 * check runs before ignition and returns outright. Lava would put out
 * fires by existing, which is worse.
 *
 * Inferred from the two fields rather than opted into with a `quenches`
 * flag of its own, deliberately: a flag would have to be set on every
 * existing liquid or they silently stop extinguishing anything, and
 * "does not burn and is not on fire" is the honest definition of the
 * thing anyway. */
static inline bool neighbor_quenches(const sand_t *s, int nx, int ny, int w,
                                     int h)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n) || material_of(n)->kind != KIND_LIQUID) {
        return false;
    }
    const reaction_t *r = reaction_of(n);
    return r->flammability == 0 && r->burns == 0;
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
 * neighbor_quenches()'s/try_ignite()'s own per-neighbour ANY/EACH loops
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

/* Whether any of the four cardinal neighbours is open to the air - the
 * test behind reaction_t.needs_air.
 *
 * "Air" is empty space OR any KIND_GAS cell, and that second half is not
 * a nicety, it is the whole thing working. Fire is a gas. The moment a
 * flame settles onto the surface of a pool of fuel, that surface stops
 * having an EMPTY neighbour - it has a burning one - so a version of this
 * that tested only for emptiness declared the slick unexposed exactly
 * when it was most obviously on fire, and the pool could never light at
 * all. Measured, not reasoned about: a flame blob sat on an oil surface
 * for thirty steps doing nothing before this was fixed.
 *
 * Off the grid does NOT count as air: the walls are solid (sand_at()
 * reads out-of-bounds as stone), so a pool lying against one is no more
 * exposed there than against a stone block.
 *
 * An ANY-of-4 predicate, unlike smothered()'s ALL-of-4 just above - one
 * open face is enough to feed a flame. */
static inline bool touches_air(const sand_t *s, int x, int y, int w, int h)
{
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (CELL_IS_EMPTY(n) || material_of(n)->kind == KIND_GAS) {
            return true;
        }
    }
    return false;
}

/* Defined below, beside the burning cell it was written for - the soaking
 * cell takes a unit of liquid the same way, and for the same reason. */
static inline void pay_quench_cost(sand_t *s, int nx, int ny, int w);

/* Defined below, beside the cold cell that is its other caller - a crack
 * can start from either direction of shock, and this is the earlier one. */
static void crack_run(sand_t *s, int x, int y, int w, int h,
                      material_id_t from, material_id_t into);

/* Turns (nx, ny) into whatever reaction_t.heats_to names, if it is in
 * bounds and the roll succeeds - heat WITHOUT burning.
 *
 * Sand into glass is the only use today. Kept apart from try_ignite()
 * rather than folded into it because the two are different events that
 * happen to share a trigger: one is combustion and consumes fuel, the
 * other is a phase change and consumes nothing. A material can sensibly
 * have both, neither, or one.
 *
 * Returns whether it changed anything. */
static inline bool try_heat_transform(sand_t *s, int nx, int ny, int w, int h)
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

    /* A material that BANKS heat climbs one level instead of transforming,
     * and only becomes `heats_to` on reaching the top. Same trigger as the
     * memoryless form below and reached from the same two places - contact
     * with a burning cell, and through a conductor - so a fire behind a
     * stone wall heats a pane on the far side exactly as it fuses sand
     * there. What differs is only that this one remembers. */
    if (r->heat_ramp != 0) {
        /* SHOCK, the hot-onto-cold direction. A cell that has been chilled
         * well below room temperature cracks when heat arrives rather than
         * warming up through it - the mirror of step_one_cold_cell()'s
         * test, and immediate for the same reason: the roll that would gate
         * it is the roll that would also warm the cell out of danger.
         *
         * Checked before the ramp roll so that a frosted pane meeting lava
         * breaks on contact, which is what makes "chill it, then heat it" a
         * thing the player can actually aim. */
        if (r->shatters_to != 0 && CELL_VARIANT(n) <= SAND_SHOCK_COLD) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n),
                      (material_id_t)r->shatters_to);
            return true;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_ramp) {
            return false;
        }
        const uint8_t heat = CELL_VARIANT(n);
        if (heat + 1 >= MATERIAL_VARIANTS) {
            if (r->heats_to == 0) {
                return false;   /* banks heat but melts into nothing */
            }
            place_reacted(s, nx, ny, at, (material_id_t)r->heats_to);
            return true;
        }
        s->cells[at] = CELL_MAKE(CELL_MATERIAL(n), heat + 1);
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
        return true;
    }

    if (r->heats_to == 0 || r->heat_chance == 0) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_chance) {
        return false;
    }
    place_reacted(s, nx, ny, at, (material_id_t)r->heats_to);
    return true;
}

/* How far a single crack runs. A drawn vessel is a few dozen cells and a
 * hand-drawn wall rarely more than a couple of hundred, so this shatters
 * everything anyone actually builds while keeping the worst case bounded:
 * a screen filled edge to edge with glass would otherwise turn the whole
 * board over in one step.
 *
 * Also the size of the frontier, as uint16 indices - 512 bytes of stack,
 * which is why it is not simply "the whole grid". */
#define CRACK_MAX 256

/* A crack, once started, RUNS. Converts the connected run of `from` cells
 * reachable from (x, y) into `into`, up to CRACK_MAX of them.
 *
 * Shattering used to convert one cell, which meant breaking a pane took as
 * many separate successful shocks as it had cells - and each one needs a
 * cold thing touching glass that is still hot, at the moment it touches.
 * Getting that to happen once is the interesting part; getting it to
 * happen sixty times in the same place is just attrition, and on the board
 * it read as thermal shock barely working at all.
 *
 * It is also what shattering looks like. Glass does not crumble cell by
 * cell as each part of it independently decides to; a crack starts
 * somewhere and travels, and the pane goes at once.
 *
 * Deliberately does NOT check temperature as it spreads. The stress that
 * releases is the whole pane's, not each cell's - a crack does not stop
 * because the far end of the sheet happened to be cooler. The temperature
 * test belongs at the point the crack STARTS, which is where it is. */
static void crack_run(sand_t *s, int x, int y, int w, int h,
                      material_id_t from, material_id_t into)
{
    uint16_t frontier[CRACK_MAX];
    int top = 0, done = 0;

    const size_t first = (size_t)y * (size_t)w + (size_t)x;
    place_reacted(s, x, y, first, into);
    frontier[top++] = (uint16_t)first;

    while (top > 0 && done < CRACK_MAX) {
        const uint16_t at = frontier[--top];
        const int cx = (int)(at % (unsigned)w);
        const int cy = (int)(at / (unsigned)w);
        done++;

        for (int d = 0; d < 4; d++) {
            const int nx = cx + reaction_dirs[d][0];
            const int ny = cy + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            if (CELL_MATERIAL(s->cells[nat]) != from) {
                continue;
            }
            place_reacted(s, nx, ny, nat, into);
            if (top < CRACK_MAX) {
                frontier[top++] = (uint16_t)nat;
            }
        }
    }
}

/* One cell that soaks up liquid, or holds what it soaked.
 *
 * Two halves that belong together because they are the same quantity going
 * in and coming back out. Soaking takes a UNIT of an adjacent liquid - the
 * liquid is consumed, which is what separates this from `thaws` - and
 * either turns the cell into `soaks_to` or raises its own variant. Drying
 * lowers that variant again.
 *
 * Returns whether this cell still gives the pass a reason to run: it is
 * wet, or it is a soaker with liquid beside it. Reporting that honestly is
 * what keeps may_have_moisture from latching on for good on any board with
 * sand on it, which is almost all of them.
 *
 * Driven from the SOAKING side rather than the liquid's, for the reason
 * step_one_cold_cell() gives: a scan per liquid cell would be a scan on
 * the commonest material there is. */
static bool step_one_soaking_cell(sand_t *s, uint8_t *row, int x, int y,
                                  int w, int h, const reaction_t *r)
{
    const cell_t c = row[x];
    const uint8_t held = CELL_VARIANT(c);
    bool beside_liquid = false;

    const int soaks = (s->soak >= 0) ? s->soak : r->soaks;

    if (soaks != 0 && r->soaks != 0 && s->may_have_liquid) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t n = s->cells[nat];
            if (CELL_IS_EMPTY(n) ||
                materials[CELL_MATERIAL(n)].kind != KIND_LIQUID) {
                continue;
            }
            beside_liquid = true;

            if ((int)(rng_next(&s->rng) & 0xFF) >= soaks) {
                continue;
            }
            /* The liquid pays for what was taken out of it. */
            pay_quench_cost(s, nx, ny, w);

            if (r->soaks_to != 0) {
                /* Becomes something else, holding the one unit it just
                 * took - wet sand turning into soil. */
                s->cells[(size_t)y * (size_t)w + (size_t)x] =
                    CELL_MAKE(r->soaks_to, 1);
                latch_content_flags(s, s->cells[(size_t)y * (size_t)w +
                                                (size_t)x]);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
                return true;
            }
            if (held + 1 < MATERIAL_VARIANTS) {
                row[x] = CELL_MAKE(CELL_MATERIAL(c), held + 1);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            }
            return true;
        }
    }

    if (r->dries != 0 && held != 0 &&
        (int)(rng_next(&s->rng) & 0xFF) < r->dries) {
        row[x] = CELL_MAKE(CELL_MATERIAL(c), held - 1);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }

    return held != 0 || beside_liquid;
}

/* One cell that is COLD. It does two things to its four neighbours: melts
 * against a liquid, and pulls the temperature out of anything that holds
 * one - cracking it instead if it is hot enough.
 *
 * BOTH are driven from the cold side, and the chilling half only moved
 * here after snow beside a resting pane turned out to do nothing at all.
 * Driven from the warm cell, chilling could only reach panes that were
 * ALREADY off ambient, because that is the only time a warm cell gets a
 * turn - so a pane at room temperature never looked at the snow on top of
 * it and could never be pulled below room temperature. Fire reaches out to
 * its neighbours for exactly the same reason.
 *
 * Melting is here rather than on the liquid for a different reason: a scan
 * per liquid cell would be a scan on the commonest material on the board
 * and the hottest loop in the program, where a scan per snow cell is a
 * scan on something that arrives in drifts.
 *
 * Nothing here consults the liquid's temperature: liquid is warm BY
 * DEFINITION, because nothing except glass has one to consult. That is
 * also why any liquid counts rather than water alone - oil or acid leaving
 * snow untouched would need explaining in a way that melting does not.
 *
 * Returns whether the cell survived, which keeps may_have_temperature
 * honest. */
static bool step_one_cold_cell(sand_t *s, int x, int y, int w, int h,
                               const reaction_t *r)
{
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const reaction_t *nr = reaction_of(n);

        /* MELTING, from any liquid - see reaction_t.thaws. */
        if (r->thaws != 0 && r->heats_to != 0 &&
            materials[CELL_MATERIAL(n)].kind == KIND_LIQUID &&
            (int)(rng_next(&s->rng) & 0xFF) < r->thaws) {
            place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x,
                          (material_id_t)r->heats_to);
            return false;
        }

        if (r->chills == 0 || nr->heat_ramp == 0) {
            continue;
        }
        const uint8_t temp = CELL_VARIANT(n);

        /* SHOCK, which does not wait for the chilling roll. Contact is
         * enough when the neighbour is hot enough, and the roll that would
         * gate it is the same one that cools it - so rolling first usually
         * talked a pane down below the threshold instead of breaking it. */
        if (temp >= SAND_SHOCK_HEAT && nr->shatters_to != 0) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n),
                      (material_id_t)nr->shatters_to);
            if (try_heat_transform(s, x, y, w, h)) {
                return false;   /* and this cell melted paying for it */
            }
            continue;
        }

        if (temp == 0) {
            continue;           /* already as cold as this scale goes */
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);

        /* The exchange runs both ways, but ONLY when there was actually
         * heat to take. A material that cooled a glowing pane for nothing
         * would be an unlimited heat sink arriving in a light drift, so
         * taking heat out of something above room temperature costs it its
         * own heats_to.
         *
         * Pushing cold INTO something at or below room temperature is not
         * absorbing heat and does not cost anything. Without that
         * distinction snow melted on contact with ordinary cold glass -
         * at the rate tuned for standing next to a fire - which made a
         * snowbank impossible to keep on a glass shelf and read, fairly,
         * as a bug. It appeared the moment chilling started reaching
         * resting panes; before that, snow only ever paid when it had
         * genuinely cooled something hot. */
        if (temp > SAND_AMBIENT_HEAT && try_heat_transform(s, x, y, w, h)) {
            return false;
        }
    }
    return true;
}



/* How much slower temperature spreads ALONG a material than a fire's heat
 * crosses it: `conducts` shifted right by this much.
 *
 * `conducts` is 220 for glass and stone, tuned for the question "does a
 * flame on one side of this wall reach what is on the other side" - which
 * wants to be nearly certain. Spreading a cell's own temperature to its
 * neighbour at that rate makes the material ISOTHERMAL within a step or
 * two, and a wall that is all one temperature cannot be hot on the inside
 * and cold on the outside, which is the entire mechanic. Measured: at the
 * full rate a pane under lava never reached melting at all, because the
 * heat was shared out faster than any one cell could bank it.
 *
 * Derived from `conducts` rather than being its own field, because it IS
 * the same physical property - a material that carries a fire's heat well
 * carries its own temperature well - and two independent numbers could
 * disagree about a material for no reason anyone could explain. */
#define SPREAD_SHIFT 1

/* One cell whose temperature is not room temperature: it relaxes back
 * towards it.
 *
 * BOTH directions, at the same `cools` rate. A hot pane cools and a
 * frosted one warms up, because ambient is a resting point rather than a
 * floor - which is what putting cold BELOW ambient buys, and what makes
 * frost something that fades rather than something permanent.
 *
 * Chilling and shattering used to live here and now belong to the cold
 * cell, because this only runs for cells ALREADY off ambient: a resting
 * pane never got a turn, so it could never be pulled below room
 * temperature by a neighbour. That is exactly why snow beside a glass
 * basin appeared to do nothing at all.
 *
 * Returns whether the cell still differs from ambient, which is what keeps
 * s->may_have_temperature honest. */
static bool step_one_tempered_cell(sand_t *s, uint8_t *row, int x, int y,
                                   int w, int h, const reaction_t *r)
{
    const cell_t c = row[x];
    const uint8_t temp = CELL_VARIANT(c);

    /* SPREAD ALONG THE MATERIAL first. A pane is a sheet of the same
     * stuff, so a cell that has been chilled drags its neighbours down and
     * a cell that has been heated pulls them up - which is what `conducts`
     * has always meant, applied within the material rather than only to
     * what is on the far side of it.
     *
     * Without this, only the single cell a flake is touching ever changes,
     * and it barely changes: snow melts after a chill or two, so the cell
     * sits one level off ambient and the colour shift is almost invisible.
     * Spreading turns that into a patch of frost creeping outward from
     * where the snow landed, which is both what it should look like and
     * what makes the state readable at a glance.
     *
     * PUSHED onto neighbours rather than pulled from them, because pulling
     * would mean a cell at ambient needing a turn to notice a frosted
     * neighbour - and a cell at ambient never gets one. The same shape of
     * bug as chilling being driven from the warm side.
     *
     * Only across a gap of 2 or more. A difference of one is left alone,
     * so a smooth gradient across a wall survives instead of collapsing to
     * a single flat temperature - which would erase the hot-inside,
     * cold-outside difference the whole mechanic runs on. */
    if (r->conducts != 0) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t n = s->cells[nat];
            if (CELL_IS_EMPTY(n) || reaction_of(n)->heat_ramp == 0) {
                continue;
            }
            const uint8_t nt = CELL_VARIANT(n);
            const int gap = (int)temp - (int)nt;
            if (gap > -2 && gap < 2) {
                continue;
            }
            if ((int)(rng_next(&s->rng) & 0xFF) >=
                (r->conducts >> SPREAD_SHIFT)) {
                continue;
            }
            s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n),
                                      (uint8_t)(gap > 0 ? nt + 1 : nt - 1));
            s->may_have_temperature = true;
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
        }
    }

    /* COOLING GETS HARDER TO OUTRUN THE HOTTER IT IS. The drain scales with
     * how far above room temperature the cell already is, so `cools` is the
     * rate one level above ambient and every level above that costs more to
     * hold.
     *
     * This is what lets one constant serve two jobs that pull opposite
     * ways. Getting a pane WARM should be easy - a single brush of fire
     * ought to make it fragile, and with a flat drain it could not: a burst
     * of fire peaked at 5 against a threshold of 9 and simply never got
     * there. Getting a pane MOLTEN should stay hard, and with a flat drain
     * the only way to fix the first was to raise the ramp, which dropped
     * time-to-melt from ~450 steps to ~30 and threw away the whole point of
     * a long exposure.
     *
     * Scaled, the same ramp does both: near ambient the drain is small and
     * a single source climbs quickly, while near the top it grows until
     * only several adjacent sources can push through it. One flame makes
     * glass shatterable; a lava bath melts it. */
    unsigned drain = r->cools;
    if (temp > SAND_AMBIENT_HEAT) {
        drain *= (unsigned)(temp - SAND_AMBIENT_HEAT);
        if (drain > 255u) {
            drain = 255u;
        }
    }
    if (drain == 0 || (unsigned)(rng_next(&s->rng) & 0xFF) >= drain) {
        return temp != SAND_AMBIENT_HEAT;
    }

    const uint8_t next = (uint8_t)(temp > SAND_AMBIENT_HEAT ? temp - 1
                                                            : temp + 1);
    row[x] = CELL_MAKE(CELL_MATERIAL(c), next);
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
    return next != SAND_AMBIENT_HEAT;
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
    /* Already alight. Without this a flame beside a burning log would keep
     * re-igniting it - place_reacted() writes a FULL variant, so every hit
     * would reset how much was left to burn and the log would never go
     * out. */
    if (r->burn_decay != 0 && CELL_VARIANT(n) != 0) {
        return false;
    }
    if (r->needs_air && !touches_air(s, nx, ny, w, h)) {
        return false;   /* buried in more of itself - a pool of fuel burns
                         * at its surface, not through its volume */
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

/* The liquid neighbour that just quenched a burning cell pays for it:
 * one unit of its own mass, gone. Steam is a byproduct, not a free
 * lunch - without this, a fire could boil an entire pool dry one cell at
 * a time for nothing, which reads as the water simply vanishing rather
 * than a pot boiling. One unit (of the 15 a full cell holds) keeps that
 * slow: a puddle quenching a whole line of fire noticeably shrinks, but
 * does not evaporate outright on contact. Mirrors give_mass()'s own
 * "written as CELL_EMPTY rather than a zero variant" rule (sand_liquid.c)
 * for the same reason: a zero variant would leave the material nibble
 * claiming an occupied cell holding nothing. */
static inline void pay_quench_cost(sand_t *s, int nx, int ny, int w)
{
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    const int mass = CELL_VARIANT(n) - 1;
    s->cells[at] = (mass > 0) ? CELL_MAKE(CELL_MATERIAL(n), mass) : CELL_EMPTY;
    mark_rows(s, ny, ny);
    wake_block_and_neighbors(s, nx, ny);
}

/* Bounds the along-a-conductor walk conduct_heat() does below - see this
 * file's own top comment ("THE BOILER") for why it exists and why the
 * number is sized the way it is. It caps a cold pass; it does not claim
 * anything about how far heat can really travel. */
#define CONDUCT_REACH 32

/* Heat crossing a run of conductor cells - the boiler. See this file's
 * own top comment ("THE BOILER") for the two problems this and
 * solves, and why an exactly-one-cell reach was tried first and did
 * not work.
 *
 * For each cardinal neighbour that is a conductor (reaction_t.conducts,
 * checked cheaply before ever entering the walk below), walks along
 * that same direction one conductor cell at a time, rolling `conducts`
 * again for every cell crossed - so crossing depth d succeeds with
 * probability (conducts/256)^d, capped at CONDUCT_REACH cells so this
 * cold pass cannot become an unbounded scan. The moment the roll fails,
 * or the walk runs off the grid, or the next cell is empty, heat simply
 * stops there - nothing happens, and in particular no fire is ever
 * created in that empty cell, which is what keeps a sealed stone box
 * sealed. The moment the walk reaches a cell that is NOT itself a
 * conductor, that is the far side: a liquid there boils in place and
 * bubbles out on its own afterwards, fuel there ignites (deterministically - the conducts roll
 * already gated this, so ignition here does not also draw from
 * reaction_t.flammability), and anything else is simply warmed with no
 * visible effect. Returns whether it did anything. */
static inline bool conduct_heat(sand_t *s, int x, int y, int w, int h)
{
    bool acted = false;

    for (int d = 0; d < 4; d++) {
        const int dx = reaction_dirs[d][0];
        const int dy = reaction_dirs[d][1];
        int rx = x + dx;
        int ry = y + dy;

        if ((unsigned)rx >= (unsigned)w || (unsigned)ry >= (unsigned)h) {
            continue;
        }
        if (CELL_IS_EMPTY(s->cells[(size_t)ry * (size_t)w + (size_t)rx])) {
            continue;
        }
        if (reaction_of(s->cells[(size_t)ry * (size_t)w + (size_t)rx])
                ->conducts == 0) {
            continue;   /* cheap early-out - most neighbours are not a
                         * conductor at all, and the walk below is not
                         * worth entering for them */
        }

        bool got_through = false;
        for (int depth = 0; depth < CONDUCT_REACH; depth++) {
            const cell_t here = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
            const int c = (s->conduction >= 0) ? s->conduction
                                               : reaction_of(here)->conducts;
            if ((int)(rng_next(&s->rng) & 0xFF) >= c) {
                break;      /* heat stops inside this cell of the run */
            }

            const int nx = rx + dx;
            const int ny = ry + dy;
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                break;
            }
            const cell_t next = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(next)) {
                break;      /* never creates fire in empty space */
            }
            rx = nx;
            ry = ny;
            if (reaction_of(next)->conducts == 0) {
                got_through = true;    /* the far side - not a conductor */
                break;
            }
            /* still inside the conductor run - loop again and roll for
             * THIS cell's own conducts figure */
        }

        if (!got_through) {
            continue;
        }

        const size_t bat = (size_t)ry * (size_t)w + (size_t)rx;
        const cell_t bc = s->cells[bat];
        const material_t *bm = material_of(bc);
        /* A liquid that BURNS is a heat source, and a heat source cannot
         * be boiled by heat. This tested `kind == KIND_LIQUID` alone, and
         * lava is a liquid, so lava on the far side of a conductor was
         * boiled into steam by the heat of other lava - through a stone
         * pillar, a glass wall, anything that conducts.
         *
         * It never showed up in a plain pool because a pool has no
         * conductor running through it. It showed up the moment anyone
         * drew a vessel by hand: measured, a vessel with one-cell stone
         * pillars in it lost 83% of its lava in 200 steps while a
         * flat-floored one lost none, with steam coming off the top the
         * whole time. Water and oil in the same vessel were untouched,
         * which is what proved it was not a liquid-movement bug.
         *
         * Exactly the rule neighbor_quenches() already applies at the
         * other end: a liquid that burns is not a coolant. It is not a
         * kettle either. */
        if (bm->kind == KIND_LIQUID && reaction_of(bc)->burns == 0) {
            /* Boils the cell the heat actually reached, which is the one
             * touching the conductor - the bottom of a pot sitting on a
             * hot stone, not its surface. That steam then climbs out on
             * its own through try_bubble() (sand_gas.c), one cell a step,
             * which is what makes a boiler read as a rising column of
             * bubbles rather than a puff appearing at the top from
             * nowhere.
             *
             * It used to boil the SURFACE instead, walking against
             * gravity through the liquid run to find it, and that walk is
             * now deleted along with the (updx, updy) plumbing and the
             * BOIL_REACH cap that bounded it. Worth knowing why, because
             * the walk was not arbitrary: before bubbling existed, steam
             * made at the bottom of a pool was stuck there permanently
             * (see try_bubble()'s own comment for the two rules that
             * deadlocked), so boiling anywhere but the surface produced
             * nothing anyone could see. Bubbling removed that constraint
             * entirely, and with it the only reason this code needed to
             * know which way was up. */
            place_reacted(s, rx, ry, bat, MAT_STEAM);
            acted = true;
        } else {
            const reaction_t *br = reaction_of(bc);
            if (br->heat_ramp != 0 ||
                (br->heats_to != 0 && br->heat_chance != 0)) {
                /* Sand behind a hot wall becomes glass, the same way water
                 * behind one boils - heat that has crossed a conductor
                 * does everything heat in contact does. */
                if (try_heat_transform(s, rx, ry, w, h)) {
                    acted = true;
                }
            }
            if (br->flammability != 0) {
                const material_id_t becomes = br->ignites_to ? br->ignites_to
                                                              : MAT_FIRE;
                place_reacted(s, rx, ry, bat, becomes);
                acted = true;
            }
        }
    }

    return acted;
}

/* One dissolver cell's turn: eat one cardinal neighbour, and pay for it.
 *
 * Two rolls, on two different materials, and both have to pass: this
 * cell's `dissolves` (how hard the acid tries) and the neighbour's
 * `dissolvable` (how easily it gives way). Splitting it that way is what
 * lets one acid figure produce different rates against sand, wood and
 * stone without acid knowing any of their names - and `dissolvable`
 * defaulting to zero is what keeps acid from eating the container it is
 * standing in, the floor it is standing on, or the air above it.
 *
 * The bite costs the acid one unit of its own mass, through the same
 * pay_quench_cost() a liquid pays to put out a fire, because it is the
 * same kind of transaction: the liquid is CONSUMED rather than merely
 * consulted. Without that a single cell of acid dissolves an unbounded
 * amount of anything and remains a single cell - the exact mistake
 * oil-soaked ash made before soaking became a real transfer, and worth
 * naming twice because it is the easy one to make.
 *
 * At most one neighbour per step, so a cell of acid surrounded by sand
 * eats into it rather than opening a hole on all four sides at once.
 * Returns whether it dissolved anything. */
static bool step_one_dissolver_cell(sand_t *s, uint8_t *row, int x, int y,
                                    int w, int h, const reaction_t *r)
{
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->dissolves) {
        return false;
    }

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[at];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const uint8_t give = reaction_of(n)->dissolvable;
        if (give == 0 || (int)(rng_next(&s->rng) & 0xFF) >= give) {
            continue;
        }

        /* The fizz. Placed in the cell that was just eaten, which is
         * about to be empty anyway, so it costs nothing extra and appears
         * exactly where the reaction happened.
         *
         * It reads properly without any help from this code: smoke is
         * lighter than every liquid, so try_bubble() (sand_gas.c) walks it
         * up and out of the acid rather than leaving it stranded at the
         * bottom of the pool. */
        if (r->fizz != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->fizz) {
            place_reacted(s, nx, ny, at, MAT_SMOKE);
        } else {
            s->cells[at] = CELL_EMPTY;
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
        }

        /* The acid pays, and may spend itself doing it - pay_quench_cost()
         * clears the cell when its last unit goes. Done after the target
         * is dealt with so the two can never both survive a bite. */
        pay_quench_cost(s, x, y, w);
        return true;
    }
    return false;
}

/* One burning cell's turn, in priority order: burn down first (a cell
 * that vanishes this step gets no turn to react further - it cannot
 * both die and spread the same step, but it can leave smoke behind, see
 * reaction_t.smoke and place_reacted() above); extinguish if any
 * neighbour is a liquid (wins outright over everything below, even if a
 * flammable neighbour or three smothering ones also touch this cell) -
 * the burning cell becomes reaction_t.quench_to (steam, today) rather
 * than simply vanishing, and the liquid that did it pays a unit of mass
 * for the privilege (see pay_quench_cost() above); extinguish again if
 * every neighbour smothers it (true burial - see smothered() above,
 * this is the only way sand puts fire out, since sand passes through it
 * uneventfully otherwise, the same way it already passes through gas);
 * otherwise ignite every non-empty, flammable neighbour found - a cell
 * touching fuel on three sides lights all three, not just one - conduct
 * heat into any conductor neighbour (see conduct_heat() above), and, if
 * this material flares (ember, not fire), roll for that too. Ignition,
 * conduction and flaring are independent of each other rather than
 * either/or: a burning cell can perfectly well do all three in the same
 * step - conduction sits alongside ignition deliberately, not after an
 * early return, since there is no reason a fire cell could not both
 * light a neighbour AND warm a stone wall on its other side at once. */
static bool step_one_burning_cell(sand_t *s, uint8_t *row, int x, int y,
                                  int w, int h)
{
    cell_t grain = row[x];
    const material_t *mat = material_of(grain);
    const uint8_t mat_id  = CELL_MATERIAL(grain);
    const size_t at = (size_t)y * (size_t)w + (size_t)x;

    /* A material that burns only while lit counts its VARIANT down at its
     * own rate, rather than the movement table's `decay` - which stays 0
     * for it, because wood is not a transient. It does not disappear on
     * its own; it disappears because it burned. */
    const reaction_t *rx = reaction_of(grain);
    const bool lit_state = rx->burn_decay != 0;
    const int burn_rate = (s->decay >= 0) ? s->decay : rx->burn_decay;

    if (lit_state
        ? !tick_decay_at(s, row, x, y, &grain, mat_id, burn_rate)
        : !tick_decay(s, row, x, y, &grain, mat, mat_id)) {
        /* Burned out. tick_decay() already cleared the cell and woke it -
         * this only adds smoke on top, via place_reacted(), which
         * overwrites the CELL_EMPTY tick_decay() just wrote and repeats
         * the same wake/dirty bookkeeping. That double wake is harmless
         * (mark_rows()/wake_block_and_neighbors() are both idempotent
         * within a step) and far simpler than threading a "did it
         * already wake this cell" flag back out of a shared, hot-header
         * helper for a cold pass's cosmetic byproduct. */
        /* MAT_SMOKE, not MAT_STEAM: nothing here got wet, and a fire
         * puffing kettle-steam as it dies is the exact confusion the
         * two-material split exists to avoid - see this file's own top
         * comment. Hardcoded rather than a `smokes_to` field mirroring
         * quench_to, because every material that burns wants the same
         * residue; if one ever does not, that field is the change. */
        const uint8_t residue = reaction_of(grain)->residue;
        if (residue != 0 && (int)(rng_next(&s->rng) & 0xFF) < residue) {
            place_reacted(s, x, y, at, MAT_SMOKE);
        }
        return true;
    }

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if (neighbor_quenches(s, nx, ny, w, h)) {
            const uint8_t quench_to = rx->quench_to;
            if (lit_state) {
                /* Water on a burning log puts it OUT. The log is still
                 * there, just no longer alight - which is only expressible
                 * now that being alight is a state of the wood rather than
                 * a different material. Ember had to name something to
                 * become, because the ember WAS the fire. */
                row[x] = CELL_MAKE(mat_id, 0);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            } else if (quench_to != 0) {
                place_reacted(s, x, y, at, quench_to);
            } else {
                row[x] = CELL_EMPTY;
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            }
            pay_quench_cost(s, nx, ny, w);
            return true;
        }
    }

    /* A burning LIQUID is never smothered. Being buried puts a flame out
     * because it starves it of air, and lava is not a flame - it is not
     * burning anything, it is simply hot, so burying it should bury
     * something hot rather than delete it.
     *
     * Measured before this guard existed: a single lava cell walled in by
     * stone vanished on the very next step, and a vessel whose floor had
     * one-cell dimples - which is every vessel anyone draws by hand - lost
     * 83% of its lava within 200 steps, silently, with the flare still
     * bubbling off the top as it went. A flat-floored rectangle conserved
     * it perfectly, which is why the first probe found nothing and the
     * report was right anyway.
     *
     * neighbor_smothers() already refuses to count a liquid NEIGHBOUR, for
     * the mirror of this reason. This is the same rule applied to the cell
     * doing the burning. */
    if (mat->kind != KIND_LIQUID &&
        smothered(s, x, y, w, h, mat->density)) {
        /* Burying a burning log smothers the BURN, not the log. Same
         * reasoning as quenching one. */
        row[x] = lit_state ? CELL_MAKE(mat_id, 0) : CELL_EMPTY;
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return true;
    }

    bool acted = false;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if (try_ignite(s, nx, ny, w, h)) {
            acted = true;
        }
        /* Separate from ignition, and reached whether or not that fired:
         * a neighbour is either fuel or something heat merely changes, and
         * nothing is both today, but there is no reason one could not be. */
        if (try_heat_transform(s, nx, ny, w, h)) {
            acted = true;
        }
    }

    if (conduct_heat(s, x, y, w, h)) {
        acted = true;
    }

    if (try_flare(s, x, y, w, h, reaction_of(grain)->flare)) {
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
/* Returns a bitmask rather than a bool: a row may hold burning cells,
 * dissolving ones, or both, and sand_step_reactions() clears the two
 * may_have_* flags independently. Collapsing them into one answer would
 * let a board of nothing but acid keep may_have_burning armed for ever,
 * and a board of quiet fire keep may_have_dissolver armed. */
#define FOUND_BURNING   1u
#define FOUND_DISSOLVER 2u
#define FOUND_TEMPERATURE      4u
#define FOUND_MOISTURE         8u

static unsigned step_one_reacting_row(sand_t *s, int y, int w, int h)
{
    uint8_t *row = s->cells + (size_t)y * (size_t)w;

    unsigned found = 0;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const reaction_t *r = reaction_of(c);
        if (cell_is_burning(c)) {
            found |= FOUND_BURNING;
            step_one_burning_cell(s, row, x, y, w, h);
            continue;
        }
        if (r->dissolves) {
            found |= FOUND_DISSOLVER;
            step_one_dissolver_cell(s, row, x, y, w, h, r);
            continue;
        }
        /* Only cells ALREADY holding heat need a turn. A cold pane is
         * heated from the fire's side by try_heat_transform(), the same as
         * any other neighbour of a flame, so the common case - a board full
         * of glass and one candle - walks past nearly all of it on a
         * variant test. */
        if (r->heat_ramp != 0) {
            if (CELL_VARIANT(c) != SAND_AMBIENT_HEAT &&
                step_one_tempered_cell(s, row, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* A cold cell keeps the flag set whether or not it melts this
         * step: a drift on dry ground has nothing to do now and still has
         * to be found later, when a liquid reaches it. */
        if (r->chills != 0) {
            if (step_one_cold_cell(s, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* Soaking and drying. Reached by sand and dirt, which are on most
         * boards, so the cheap tests come first: the field check, then
         * may_have_liquid inside, and only then a neighbour scan. */
        if ((r->soaks != 0 || r->dries != 0) &&
            step_one_soaking_cell(s, row, x, y, w, h, r)) {
            found |= FOUND_MOISTURE;
        }
    }
    return found;
}

/* Takes only `s` again. It briefly took (gx, gy) as well, for
 * a since-deleted surface walk to climb against gravity;
 * boiling now happens at the heat source instead and the steam finds its
 * own way up by bubbling, so nothing in this pass has any interest in
 * which way gravity points. */
void sand_step_reactions(sand_t *s)
{
    /* Dissolving is not a fire reaction and must not be gated behind one:
     * acid has to work on a board with no flame anywhere. */
    /* Heat is a third independent reason to run, not a rider on fire: glass
     * goes on cooling long after the flame that heated it is out, and gated
     * behind may_have_burning it would freeze mid-ramp instead. */
    if (!s->may_have_burning && !s->may_have_dissolver &&
        !s->may_have_temperature && !s->may_have_moisture) {
        return;
    }

    const int w = s->w;
    const int h = s->h;

    unsigned found = 0;
    for (int y = 0; y < h; y++) {
        found |= step_one_reacting_row(s, y, w, h);
    }

    if (!(found & FOUND_BURNING)) {
        s->may_have_burning = false;
    }
    if (!(found & FOUND_DISSOLVER)) {
        s->may_have_dissolver = false;
    }
    if (!(found & FOUND_TEMPERATURE)) {
        s->may_have_temperature = false;
    }
    if (!(found & FOUND_MOISTURE)) {
        s->may_have_moisture = false;
    }
}
