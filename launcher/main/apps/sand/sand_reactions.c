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
/* Write one cell and do the four things every write owes the rest of the
 * simulation: latch the content flags that gate the passes, mark the row
 * dirty for the renderer, and wake this block and its neighbours so a
 * settled region notices. Every placement here goes through it. */
static inline void place_cell(sand_t *s, int x, int y, size_t at, cell_t c)
{
    s->cells[at] = c;
    latch_content_flags(s, c);
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
}

static inline void place_reacted(sand_t *s, int x, int y, size_t at,
                                 uint8_t spec)
{
    if (spec >= (MAT_EXTENDED << 4)) {
        place_cell(s, x, y, at, (cell_t)spec);   /* identity IS low nibble */
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
    /* Note what MATERIAL_VARIANTS - 1 means for wood, whose variant is
     * burn progress: a log placed by a reaction is placed ALIGHT. That is
     * deliberate and load-bearing - it is how a log catches, now that an
     * ember is a state of wood rather than its own material - but it is
     * only right for a product of fire. Anything that makes wood without
     * setting it on fire, such as a plant hardening into a trunk, has to
     * say so, and uses place_cell() with the variant it means. */
    place_cell(s, x, y, at,
               CELL_MAKE(mat, reactions[mat].heat_ramp != 0
                                  ? SAND_AMBIENT_HEAT
                                  : MATERIAL_VARIANTS - 1));
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
/* What a crack leaves behind.
 *
 * Sand gets a shade out of its reserved CULLET band rather than whatever
 * place_reacted() would hand it, so a shattered pane is pale cool ground
 * glass instead of more beach - and a fresh shade per cell, because a
 * broken window is not one flat colour. Anything else a crack might one
 * day produce is placed the ordinary way. */
static inline void place_cracked(sand_t *s, int x, int y, size_t at,
                                 material_id_t into)
{
    if (into == MAT_SAND) {
        place_cell(s, x, y, at,
                   CELL_MAKE(into,
                             (uint8_t)(SAND_CULLET_BASE +
                                       rng_below(&s->rng,
                                                 SAND_CULLET_SHADES))));
        return;
    }
    place_reacted(s, x, y, at, into);
}

static void crack_run(sand_t *s, int x, int y, int w, int h,
                      material_id_t from, material_id_t into)
{
    uint16_t frontier[CRACK_MAX];
    int top = 0, done = 0;

    const size_t first = (size_t)y * (size_t)w + (size_t)x;
    place_cracked(s, x, y, first, into);
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
            place_cracked(s, nx, ny, nat, into);
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
    /* Only the low bits - the top one is soil's carried tone, and reading
     * the whole nibble as wetness would make half of all freshly poured
     * dirt look sodden and feed plants that were never watered. */
    const uint8_t held = CELL_MOISTURE(c);
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
            /* A liquid that WETS things, which is water and nothing
             * else. Taking a unit of any KIND_LIQUID is the obvious rule
             * and it soaked oil, acid and lava into the ground alike. */
            if (CELL_IS_EMPTY(n) ||
                materials[CELL_MATERIAL(n)].kind != KIND_LIQUID ||
                reaction_of(n)->wets == 0) {
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
                 * took - wet sand turning into soil. The tone comes from
                 * the grain's own shade, so a dune that gets rained on
                 * turns to soil without losing the pattern it was poured
                 * with. */
                s->cells[(size_t)y * (size_t)w + (size_t)x] =
                    CELL_SOIL(r->soaks_to,
                              CELL_VARIANT(c) >> SOIL_MOISTURE_BITS, 1);
                latch_content_flags(s, s->cells[(size_t)y * (size_t)w +
                                                (size_t)x]);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
                return true;
            }
            if (held < SOIL_MOISTURE_MAX) {
                row[x] = CELL_WITH_MOISTURE(c, held + 1);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            }
            return true;
        }
    }

    /* MOISTURE SPREADS. A wet cell hands a level to a drier soaker beside
     * it, and hands it to dry SAND by turning that sand into more of
     * itself - which is the difference between a puddle leaving a crust
     * one cell thick and a puddle soaking outward into a patch of soil.
     *
     * Reported as dirt not diffusing what it drinks, and it did not: only
     * a cell touching the LIQUID converted, so the wet dirt that formed
     * was a wall between the water and everything behind it.
     *
     * It runs at the full soaking rate. It was a quarter of it, on the
     * reasoning that water reaching soil should always outpace soil
     * passing it along - true, and it made the front so slow that the two
     * were indistinguishable from "nothing happens". */
    const int spread = soaks;

    /* `dries` is what MARKS a variant as moisture. Sand soaks but does not
     * dry - its variant is a shade - so without this the shade would be
     * read as wetness and a dune would convert itself to soil from the
     * inside out.
     *
     * And `spread` is checked BEFORE the roll, not by rolling and losing.
     * Drawing a random number here would advance the RNG for every soaking
     * cell on the board whether or not anything could come of it, shifting
     * every downstream decision - which is exactly how this first went in,
     * and it broke sand sinking through fire, a scene with no moisture in
     * it at all. try_ignite() has the same note for the same reason. */
    if (r->dries != 0 && held >= 2 && spread != 0 &&
        (int)(rng_next(&s->rng) & 0xFF) < spread) {
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
            if (nr->soaks == 0) {
                continue;               /* not something that drinks */
            }

            /* HALF THE DIFFERENCE, not one level.
             *
             * Handing over a single level and requiring a drop of two to do
             * it makes a front that dies about seven cells out: each hop
             * costs the donor a level and leaves the receiver one step
             * above the floor, so the gradient runs out long before the
             * water does. Worse, a converted grain was born holding 1 -
             * below the >= 2 needed to pass anything on - so the outermost
             * ring of new soil was always a dead end, and drying took it
             * back to nothing within a few hundred steps.
             *
             * Moving half the gap instead is the ordinary way a diffusion
             * settles: it is still exactly conservative, it still cannot
             * oscillate (half of a gap of one is zero, so neighbours that
             * have evened out stop trading), and a saturated cell now
             * reaches as far as its water can rather than as far as the
             * step size allows. */
            int give, cost;
            if (nr->soaks_to != 0) {
                /* Dry sand beside wet soil becomes soil - and is handed
                 * enough to go on wetting ITS neighbours, which is what
                 * turns a puddle into a spreading patch of earth. It keeps
                 * its own shade as the new soil's tone. */
                give = held / 2;
                if (give == 0) {
                    continue;           /* not enough to bind a grain */
                }
                cost = give;
                s->cells[nat] = CELL_SOIL(nr->soaks_to,
                                          CELL_VARIANT(n) >> SOIL_MOISTURE_BITS,
                                          (uint8_t)give);
                latch_content_flags(s, s->cells[nat]);
            } else if (CELL_MATERIAL(n) == CELL_MATERIAL(c)) {
                give = (held - CELL_MOISTURE(n)) / 2;
                if (give == 0) {
                    continue;           /* already even with this one */
                }
                s->cells[nat] = CELL_WITH_MOISTURE(
                    n, (uint8_t)(CELL_MOISTURE(n) + give));
                cost = give;
            } else {
                continue;
            }

            row[x] = CELL_WITH_MOISTURE(c, (uint8_t)(held - cost));
            mark_rows(s, y, y);
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            return true;
        }
    }

    /* PERCOLATION. Water in soil runs DOWNHILL, which diffusion alone
     * cannot express and which is what bounds how deep a soaking gets.
     *
     * Half-the-difference settles at a gradient of one level per cell -
     * and stops there, because half of a gap of one is zero. So the reach
     * of a soaking was capped at the moisture range itself: a pile held
     * under water wet its top seven rows into a perfect 7-6-5-4-3-2-1
     * ramp and then froze, with dry sand underneath it for ever. Reported
     * as dirt not wetting a whole pile "even fully submerged in water".
     *
     * Gravity is the missing term. This hand-off needs NO gradient - only
     * room in the cell it is going to - so it does not stall, and it
     * cannot ping-pong the way an ungated symmetric transfer would,
     * because it only ever goes one way.
     *
     * It goes to ONE of the three cells gravity-ward - straight down or
     * either diagonal - picked at random, and hands over HALF of what it
     * holds. Both halves of that are what make it look like water rather
     * than like a rising tide. A fixed direction and a single level would
     * advance a flat sheet one row at a time, damping everything evenly;
     * a wandering direction carrying a real share instead drives fingers
     * down through the soil, which split when a wet cell sends half one
     * way and half the other on a later step, and merge where two fingers
     * meet. That is what water actually does in sand, and it is much the
     * more interesting thing to watch.
     *
     * Every gate is checked before the roll, INCLUDING whether any of the
     * three can take anything. Drawing a random number for soil with
     * nowhere to send it would advance the RNG for every wet cell on the
     * board and shift every decision downstream - the trap try_ignite()
     * documents, and one this pass has already fallen into once. */
    if (r->dries != 0 && held != 0) {
        /* The three gravity-ward cells: straight down, and down along each
         * perpendicular. Built from the settled direction, so they turn
         * with the board. */
        const int down = ring_of(s->last_load_dx, s->last_load_dy);
        int open[3], n_open = 0;
        for (int i = 0; i < 3; i++) {
            const int *fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t below = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(below)) {
                continue;
            }
            const reaction_t *br = reaction_of(below);
            if (br->soaks == 0) {
                continue;
            }
            if (br->soaks_to != 0 ||
                (br->dries != 0 && CELL_MOISTURE(below) < SOIL_MOISTURE_MAX)) {
                open[n_open++] = i;
            }
        }
        if (n_open != 0 && (int)(rng_next(&s->rng) & 0xFF) < spread) {
            const int pick = open[rng_below(&s->rng, n_open)];
            const int *fd = ring_dir(down + (pick == 0 ? 0
                                             : pick == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t below = s->cells[nat];
            const reaction_t *br = reaction_of(below);

            /* Half, rounded up, so a cell holding 1 still moves it - a
             * finger that rounds down to nothing stops one level short of
             * the bottom every time. */
            int give = (held + 1) / 2;
            int cost = give;
            if (br->soaks_to != 0) {
                /* Turning a grain into soil COSTS a level on top of what
                 * is handed over, because binding sand into earth uses
                 * water up rather than passing it along.
                 *
                 * Rounded DOWN here, unlike the hand-off between two
                 * cells that are already soil - and that one character is
                 * the whole difference between a soaking and a wash.
                 * Rounding up lets a cell holding a single level convert
                 * the grain below it, hand that level over and keep
                 * nothing, whereupon the new cell does the same: one
                 * splash of water turned a forty-by-nineteen bank
                 * entirely to soil, every cell of it bone dry. Rounding
                 * down means a converting cell always keeps at least as
                 * much as it gives, so a chain runs out after a few
                 * branches and what it leaves behind is visibly wet.
                 *
                 * Moving water between two cells of soil creates nothing,
                 * so it keeps the rounding that reaches the bottom row. */
                give = held / 2;
                if (give == 0) {
                    return true;        /* too little to bind a grain */
                }
                cost = give;
                s->cells[nat] = CELL_SOIL(
                    br->soaks_to, CELL_VARIANT(below) >> SOIL_MOISTURE_BITS,
                    (uint8_t)give);
                latch_content_flags(s, s->cells[nat]);
            } else {
                const int room = (int)SOIL_MOISTURE_MAX - CELL_MOISTURE(below);
                if (give > room) {
                    give = room;
                }
                s->cells[nat] = CELL_WITH_MOISTURE(
                    below, (uint8_t)(CELL_MOISTURE(below) + give));
                cost = give;
            }
            row[x] = CELL_WITH_MOISTURE(c, (uint8_t)(held - cost));
            mark_rows(s, y, y);
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            return true;
        }
    }

    if (r->dries != 0 && held != 0 &&
        (int)(rng_next(&s->rng) & 0xFF) < r->dries) {
        row[x] = CELL_WITH_MOISTURE(c, held - 1);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }

    return held != 0 || beside_liquid;
}

/* One cell of hot GAS, warming whatever around it holds a temperature.
 *
 * Gated on may_have_heat_holder by the caller - not may_have_temperature,
 * which cannot do this job: smoke and steam both `warm`, so either one
 * arms may_have_temperature by itself the moment it is placed, and this
 * branch re-arms it every step by being taken, so it never closes on a
 * board holding gas. may_have_heat_holder answers the real question - is
 * there a cell with a heat_ramp anywhere on the grid - so on a board with
 * nothing that can hold one this costs a predicted-false branch per gas
 * cell and no scan at all.
 *
 * Why the gate is safe rather than merely fast: when may_have_heat_holder
 * is false, no cell with a non-zero heat_ramp has ever been written to
 * this grid. Below, every neighbour whose reaction_of(n)->heat_ramp == 0
 * is skipped BEFORE any random number is drawn and before anything is
 * mutated. So on a board where the flag is false, this whole scan
 * provably does nothing - not "usually finds nothing", nothing at all -
 * which means suppressing the call cannot change the simulation, not the
 * grid and not the random-number stream either. */
static void step_one_warming_cell(sand_t *s, int x, int y, int w, int h,
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
        if (CELL_IS_EMPTY(n) || reaction_of(n)->heat_ramp == 0) {
            continue;
        }
        const uint8_t t = CELL_VARIANT(n);
        if (t + 1 >= MATERIAL_VARIANTS) {
            continue;       /* melting is the ramp's job, not convection's */
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->warms) {
            continue;
        }
        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(t + 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
    }
}

/* One cell that FALLS in the cold pass, because it cannot fall in the
 * sweep - see reaction_t.falls for why an extended material has to do it
 * here. Moves one step gravity-ward into empty space and nowhere else, so
 * a seed drops and a stem stands.
 *
 * Returns whether it still has anywhere to go, which is what keeps
 * may_have_faller from latching on for good once everything has landed. */
/* Is (ax, ay) more of `self`, or of what `self` hardens into? */
static inline bool is_kin(cell_t a, cell_t self, const reaction_t *r)
{
    return a == self ||
           (r->clings_to != 0 && CELL_MATERIAL(a) == r->clings_to);
}

/* How much of a connected body this is willing to walk before giving up
 * and letting the cell fall. Comfortably more than a tree between a leaf
 * and its roots; short enough that the search stays cheap on a board
 * covered in growth. A body bigger than this sheds its outermost cells,
 * which is a bounded and quiet way to be wrong. */
#define SUPPORT_MAX 48

/* Whether (x, y) is part of a body that is RESTING ON something.
 *
 * Four attempts, and the first three were each wrong in a way that showed
 * on the board rather than in a test. Worth keeping all of them in view,
 * because every one of them looked sufficient at the time:
 *
 *   four neighbours only  - a branch grows out at an ANGLE, so what it
 *                           grew from sits diagonally below it and
 *                           orthogonally it touches nothing. Every limb
 *                           whose trunk did not happen to continue past
 *                           it snapped off.
 *   the cell above counts - circular. It qualifies as standing on
 *                           something because the something is the very
 *                           cell asking, so two seeds stacked hold each
 *                           other up and a brushful hangs in mid-air
 *                           exactly where it was painted.
 *   ...but not on ME      - the circularity only moves. Exclude the
 *                           asker and a blob finds a chain two links long
 *                           that dodges it: the disc hung by its bottom
 *                           cell from its own upper left, which was
 *                           "held" by a cell standing on the asker. There
 *                           is no local patch for this. Being held up is
 *                           a question about the whole body.
 *
 * So it is asked about the whole body: walk the connected run of kin -
 * plant through its own wood, which is what makes a limb hold on to a
 * trunk - and look for any cell of it with something non-kin gravity-ward.
 * Ground, a wall, a heap of sand: anything that is not more tree.
 *
 * Only the three GRAVITY-WARD directions count as resting on something.
 * Beside does not: a leaf brushing a wall is not held up by it, and
 * counting it would wedge a whole crown against any vertical surface. */
static bool anchored(sand_t *s, int x, int y, int w, int h,
                     cell_t self, const reaction_t *r)
{
    uint16_t body[SUPPORT_MAX];
    int n = 0, head = 0;

    body[n++] = (uint16_t)((size_t)y * (size_t)w + (size_t)x);

    const int down = ring_of(s->last_load_dx, s->last_load_dy);

    while (head < n) {
        const int at = (int)body[head++];
        const int cx = at % w, cy = at / w;

        for (int d = 0; d < 8; d++) {
            const int *nd = ring_dir(down + d);
            const int nx = cx + nd[0], ny = cy + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t c = s->cells[nat];
            if (CELL_IS_EMPTY(c)) {
                continue;
            }
            if (!is_kin(c, self, r)) {
                /* Ring offset 0 from `down` is straight down, and it
                 * is the only one that counts. The two diagonals below
                 * look like support and are not: in a shaft one cell
                 * wide, the wall is diagonally beneath every cell of it,
                 * so a seed dropped in stuck to the side at the height it
                 * was poured instead of falling down the shaft. */
                if (d == 0) {
                    return true;      /* this body is resting on something */
                }
                continue;
            }
            if (n >= SUPPORT_MAX) {
                continue;             /* too big to finish; treat as loose */
            }
            bool known = false;
            for (int i = 0; i < n && !known; i++) {
                known = (body[i] == (uint16_t)nat);
            }
            if (!known) {
                body[n++] = (uint16_t)nat;
            }
        }
    }
    return false;
}

static bool step_one_falling_cell(sand_t *s, int x, int y, int w, int h,
                                  const reaction_t *r)
{
    const int nx = x + s->last_load_dx;
    const int ny = y + s->last_load_dy;
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    if (!CELL_IS_EMPTY(s->cells[nat])) {
        return false;                    /* landed */
    }
    /* ATTACHED things do not fall - see anchored(), which is where the
     * whole of that idea lives and where three wrong versions of it are
     * recorded. A seed painted in mid-air is attached to nothing and
     * drops; a limb is part of a tree and does not. */
    if (anchored(s, x, y, w, h, s->cells[at], r)) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->falls) {
        return true;                     /* still falling, just not now */
    }

    s->cells[nat] = s->cells[at];
    s->cells[at] = SAND_EMPTY;
    mark_rows(s, y, y);
    mark_rows(s, ny, ny);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, ny);
    return true;
}

/* How tall one plant may get, and how far the walk to its tip may run.
 * A bound rather than a rule: growth stops when the soil dries, which is
 * the real limit - this only keeps a cold pass from walking the height of
 * the board looking for a tip. */
#define GROW_REACH 48

/* How much of the top of a hardened run gets foliage hung round it. Three
 * is a crown rather than a tuft, and small enough that the whole shaping
 * pass stays a handful of writes. */
#define CANOPY_SPAN 3


/* How far down through soil a plant's roots reach for water. Deep enough
 * to survive a bed draining under it, short enough that a tree cannot
 * drink from the far side of the board. */
#define ROOT_REACH 6

/* How far a plant can LIFT water, counted in cells of its own stem between
 * the growing cell and the ground.
 *
 * This is what bounds a tree, and it is the only thing that does. Growth
 * has no per-cell state to count against, so there is no age, no size and
 * no budget to spend - but there is a shape on the grid, and how far up it
 * a cell sits is already being measured on the way down to the water. A
 * cap on that is a cap on height, on the length of a branch, and on how
 * far a limb can wander, all at once and for free.
 *
 * Without it the only limit was moisture, which meant a well watered bed
 * grew a solid wall of timber: every cell of every tree rolls to grow
 * every step, so the growth rate rises with the amount already grown, and
 * an unbounded tree is not slow, it is explosive. */
#define TREE_LIFT 10

/* And how wide a trunk may get. Thickening is what turns a sapling into
 * something that reads as a trunk, and left alone it is the one direction
 * with nothing to stop it: the cells that thicken are the ones nearest the
 * ground, so they never run out of lift the way the tip does. */
#define TRUNK_WIDTH 3

/* Where a plant drinks from: down through the plant itself to the ground,
 * then on down into the soil for the first of it holding any water.
 *
 * Two walks, and both have to wander. The first follows the STEM, trying
 * straight down and then either diagonal, because a branch has its own
 * trunk below it at an angle - a walk that only went straight down would
 * find empty air one cell below every limb, and limbs would be the only
 * part of a tree that could never grow. The second follows the SOIL, for
 * a different reason: moisture percolates, so the wettest earth is at the
 * bottom of a bed and the surface a tree stands on is the part that dries
 * first. A plant that drank only from the cell it touched stopped growing
 * with plenty of water two rows down.
 *
 * Both are bounded, and the whole thing runs in the cold pass for cells
 * that grow - which is a handful on any board that has any. */
static int find_water(sand_t *s, int x, int y, int w, int h,
                      const reaction_t *r, cell_t self, int *lift,
                      bool wants_room)
{
    const int dx = s->last_load_dx, dy = s->last_load_dy;
    const int down = ring_of(dx, dy);

    int cx = x, cy = y;
    for (int step = 0; step < GROW_REACH; step++) {
        *lift = step;
        int nx = -1, ny = -1;
        bool on_soil = false;

        for (int i = 0; i < 3; i++) {
            const int *fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int tx = cx + fd[0], ty = cy + fd[1];
            if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) {
                continue;
            }
            const cell_t c = s->cells[(size_t)ty * (size_t)w + (size_t)tx];
            if (CELL_IS_EMPTY(c)) {
                continue;
            }
            if (reaction_of(c)->dries != 0) {
                nx = tx; ny = ty; on_soil = true;
                break;                /* ground: stop looking for stem */
            }
            if (nx < 0 &&
                (c == self ||
                 (r->clings_to != 0 &&
                  CELL_MATERIAL(c) == r->clings_to))) {
                nx = tx; ny = ty;     /* more stem, keep it as a fallback */
            }
        }
        if (nx < 0) {
            return -1;                /* neither stem nor ground below */
        }
        if (!on_soil) {
            cx = nx; cy = ny;         /* carry on down the stem */
            continue;
        }

        /* Into the soil. */
        cx = nx; cy = ny;
        for (int depth = 0; depth < ROOT_REACH; depth++) {
            if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                return -1;
            }
            const size_t at = (size_t)cy * (size_t)w + (size_t)cx;
            const cell_t c = s->cells[at];
            if (CELL_IS_EMPTY(c) || reaction_of(c)->dries == 0) {
                return -1;
            }
            /* Two callers, opposite errands, one walk: growth is
             * looking for soil with something in it to spend, drinking
             * for soil with room to take more. */
            if (wants_room ? CELL_MOISTURE(c) < SOIL_MOISTURE_MAX
                           : CELL_MOISTURE(c) != 0) {
                return (int)at;
            }
            cx += dx;
            cy += dy;
        }
        return -1;
    }
    return -1;
}

/* One cell of something that DRINKS: touching a liquid, and rooted in
 * soil with room in it, it takes a unit of that liquid and puts a level of
 * moisture into the ground.
 *
 * The plant is `KIND_STATIC` at stone's density, because every extended
 * material shares one physics row - so water cannot fall through foliage
 * and nothing about foliage can absorb it. A bowl of leaves held a pond
 * for ever. This gives the water somewhere to go, and the place it goes is
 * the right one: down the stem and into the roots, so watering a canopy
 * waters the tree.
 *
 * Returns whether it is still worth coming back to. */
static bool step_one_drinking_cell(sand_t *s, int x, int y, int w, int h,
                                   const reaction_t *r, cell_t self)
{
    int lx = -1, ly = -1;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) &&
            materials[CELL_MATERIAL(n)].kind == KIND_LIQUID &&
            reaction_of(n)->wets != 0) {
            lx = nx;
            ly = ny;
            break;
        }
    }
    if (lx < 0) {
        return false;                 /* nothing to drink */
    }

    int lift = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, true);
    if (soil_at < 0) {
        return true;                  /* thirsty, but nowhere to put it */
    }
    /* Both ends found before the roll, or every leaf on the board draws a
     * random number every step and shifts everything downstream of it. */
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->drinks) {
        return true;
    }

    pay_quench_cost(s, lx, ly, w);

    const cell_t soil = s->cells[soil_at];
    s->cells[soil_at] = CELL_WITH_MOISTURE(soil,
                                           (uint8_t)(CELL_MOISTURE(soil) + 1));
    mark_rows(s, soil_at / w, soil_at / w);
    wake_block_and_neighbors(s, soil_at % w, soil_at / w);
    return true;
}

/* One cell of something that SPROUTS: standing in wet soil, it buds a
 * cell of `sprouts_to` into an empty space beside it and spends a level of
 * the soil's moisture.
 *
 * Small enough to look like nothing and it closes the loop the whole
 * feature rests on. Growth hardens a plant into wood, which consumes the
 * cells that could grow - so a tree that reached its full height was
 * finished permanently, and one that lost its foliage stayed a bare post.
 * This is how a trunk gets to be alive. */
static bool step_one_sprouting_cell(sand_t *s, int x, int y, int w, int h,
                                    const reaction_t *r)
{
    int soil_at = -1, empty_at = -1, ex = 0, ey = 0;

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            if (empty_at < 0) {
                empty_at = (int)nat;
                ex = nx;
                ey = ny;
            }
            continue;
        }
        if (soil_at < 0 && reaction_of(n)->dries != 0 &&
            CELL_MOISTURE(n) != 0) {
            soil_at = (int)nat;
        }
    }
    /* Both, then the roll - drawing for a trunk with nowhere to bud or
     * nothing to bud with would shift every decision downstream of it. */
    if (soil_at < 0 || empty_at < 0) {
        return soil_at >= 0;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->sprouts) {
        return true;
    }

    place_reacted(s, ex, ey, (size_t)empty_at, r->sprouts_to);

    const cell_t soil = s->cells[soil_at];
    s->cells[soil_at] = CELL_WITH_MOISTURE(soil,
                                           (uint8_t)(CELL_MOISTURE(soil) - 1));
    mark_rows(s, soil_at / w, soil_at / w);
    return true;
}

/* One step along a stem, in the direction (ux, uy) or either diagonal
 * beside it. Returns whether it found one.
 *
 * A stem is not a straight line and cannot be walked as one. Growth points
 * along the DITHERED gravity direction, which spends some steps on each of
 * the two eighths a tilt falls between - that is what stops a tree being a
 * rigid stick at one of eight fixed angles, and it means the trunk wanders
 * by a cell as it climbs. Every walk over a plant has to tolerate that:
 * the walk to the tip, the walk to a branch site, and the run that decides
 * whether it has grown tall enough to be wood. */
static bool stem_next(sand_t *s, int x, int y, int ux, int uy, int w, int h,
                      cell_t self, int *ox, int *oy)
{
    const int up = ring_of(ux, uy);
    for (int i = 0; i < 3; i++) {
        const int *d = ring_dir(up + (i == 0 ? 0 : i == 1 ? 1 : 7));
        const int nx = x + d[0], ny = y + d[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        if (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == self) {
            *ox = nx;
            *oy = ny;
            return true;
        }
    }
    return false;
}

/* How far a shoot will shove to get out from under something. Two or
 * three cells of cover is what burying a seed actually looks like; the
 * bound is here so a plant under half the board does not walk it. */
#define PUSH_REACH 8

/* Make room at (gx, gy) by shoving whatever is there along (dx, dy).
 *
 * A seed covered over with soil could not grow at all: the cell it wanted
 * was occupied, and occupied was the end of it. Which is wrong for the
 * one material on the board whose whole job is to come up through the
 * ground - burying a seed and watering it is how you plant one, and it was
 * the one way to guarantee nothing happened.
 *
 * So the run gets pushed instead. Walk along the growth direction while
 * the cells are things a shoot could displace - powders, liquids, gases,
 * anything not STATIC - until an empty one turns up, then shift the whole
 * run one step into it. Nothing is created or destroyed, the cover ends up
 * one cell higher, and a shoot stops dead at stone or wood or at another
 * tree, which is what those are for.
 *
 * Returns whether (gx, gy) is now free. */
static bool shove_aside(sand_t *s, int gx, int gy, int dx, int dy,
                        int w, int h)
{
    int ex = gx, ey = gy;
    int run = 0;

    while (run < PUSH_REACH) {
        if ((unsigned)ex >= (unsigned)w || (unsigned)ey >= (unsigned)h) {
            return false;             /* shoved into the wall */
        }
        const cell_t c = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        if (CELL_IS_EMPTY(c)) {
            break;                    /* somewhere to put it all */
        }
        if (material_of(c)->kind == KIND_STATIC) {
            return false;             /* will not budge */
        }
        ex += dx;
        ey += dy;
        run++;
    }
    if (run == 0) {
        return true;                  /* was empty to begin with */
    }
    if (run >= PUSH_REACH) {
        return false;                 /* too much of it to lift */
    }

    /* Back to front, so nothing is overwritten before it has moved. */
    for (int i = 0; i < run; i++) {
        const int tx = ex, ty = ey;
        ex -= dx;
        ey -= dy;
        s->cells[(size_t)ty * (size_t)w + (size_t)tx] =
            s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        mark_rows(s, ty, ty);
        wake_block_and_neighbors(s, tx, ty);
    }
    s->cells[(size_t)gy * (size_t)w + (size_t)gx] = SAND_EMPTY;
    mark_rows(s, gy, gy);
    wake_block_and_neighbors(s, gx, gy);
    return true;
}

/* One cell of something that WITHERS: it goes, if it can neither drink
 * nor lean on a trunk.
 *
 * Growth is the only thing here that creates cells, and nothing but fire
 * and acid removed them - so every scrap a tree shed was permanent litter.
 * This is the other end of that.
 *
 * Touching wood is checked first and is by far the commoner answer, which
 * matters: it is eight cell reads, where finding water is a walk. A tree
 * standing in soil that has dried out keeps every leaf. */
static bool step_one_withering_cell(sand_t *s, int x, int y, int w, int h,
                                    const reaction_t *r)
{
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const cell_t self = s->cells[at];

    if (r->clings_to != 0) {
        for (int d = 0; d < 8; d++) {
            const int *nd = ring_dir(d);
            const int nx = x + nd[0], ny = y + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (!CELL_IS_EMPTY(n) && CELL_MATERIAL(n) == r->clings_to) {
                return false;         /* part of a tree; it stays */
            }
        }
    }

    int lift = 0;
    if (find_water(s, x, y, w, h, r, self, &lift, false) >= 0) {
        return false;                 /* it can still drink */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->withers) {
        return false;
    }

    s->cells[at] = SAND_EMPTY;
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
    return true;
}

/* One cell of something that GROWS.
 *
 * It grows from the TIP of whatever column of itself this cell belongs to,
 * against gravity, and pays for it with a level of moisture out of soil
 * touching THIS cell. Which is a strange-sounding arrangement until you
 * see what it is working around: a plant is an extended material, so its
 * low nibble is which material it is and there is no variant left to hold
 * a stem's height, its vigour, or how much water has reached it. It has no
 * per-cell state at all.
 *
 * Walking to the tip is how a stateless material still makes a tree. The
 * cell at the bottom is the one that can reach the soil, so it is the one
 * that rolls and pays; the cell at the top is the one with room, so it is
 * the one that grows. Without the walk a plant could only ever be one cell
 * tall - the moment it grew, the new cell would be out of reach of the
 * ground and nothing further could happen.
 *
 * A run that grows tall enough hardens into `hardens_to`, measured along
 * the gravity axis so a creeper spreading sideways stays soft. */
static bool step_one_growing_cell(sand_t *s, int x, int y, int w, int h,
                                  const reaction_t *r)
{
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    /* Which way is up. last_load_dx/dy is the direction the sweep settled
     * under this step, so inside the simulation it is current - unlike in
     * the renderer, where a frame can pass without a step. */
    /* Up, from the DITHERED direction rather than the nearest one, so a
     * tree leaning under a tilt leans at the angle the board is actually
     * at. The sweep has used the dithered direction all along for exactly
     * this reason; growth using the other one is what made stems rigid. */
    const int ux = -s->last_step_dx;
    const int uy = -s->last_step_dy;
    if (ux == 0 && uy == 0) {
        return true;                  /* free fall: no up to grow towards */
    }

    /* Soil touching THIS cell - and then ROOTS: down through that soil,
     * following gravity, for the first of it holding any water.
     *
     * The reach is what makes a plant survive its own ground draining.
     * Moisture percolates, so the wettest soil is at the bottom of a bed
     * and the surface a tree is standing on is the part that dries first;
     * a plant that could only drink from the cell it touched stopped
     * growing while there was still plenty of water a row or two down. */
    /* A BURIED cell is not a growing point.
     *
     * Every cell of a plant that can reach water rolls to grow, so the
     * growth rate rises with the amount already grown - and hardening was
     * the only thing taking cells back out of that loop. The moment
     * hardening was made to wait for some girth, the loop ran away and
     * a watered bed filled with a solid mass of green.
     *
     * Growth belongs at the surface. A cell with kin on nearly every side
     * has no room to put anything anyway; skipping it early costs one
     * neighbour scan and stops the interior of a thicket from being an
     * engine. */
    int packed = 0;
    for (int d = 0; d < 8; d++) {
        const int *nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            packed++;
            continue;
        }
        if (is_kin(s->cells[(size_t)ny * (size_t)w + (size_t)nx], self, r)) {
            packed++;
        }
    }
    if (packed >= 5) {
        return true;                  /* inside the crowd, not at its edge */
    }

    int lift = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, false);
    if (soil_at < 0) {
        return true;                  /* nothing to drink */
    }
    if (lift >= TREE_LIFT) {
        return true;                  /* too high up to be fed */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->grows) {
        return true;
    }

    /* WHERE it grows. Straight up from the tip, every time, is a stick -
     * which is what this was, and what "grow seems to be mostly one side,
     * the idea is to imitate trees" is about.
     *
     * Two rolls make it a tree instead. One picks the SITE: usually the
     * tip, but one attempt in three starts somewhere further down the
     * stem, which is a branch. The other picks the DIRECTION: a tip mostly
     * carries straight on and sometimes leans, a branch always goes out at
     * an angle. Neither needs anything remembered - the shape so far is on
     * the grid, and re-reading it is what the walk to the tip was already
     * doing. */
    int run = 1, tx = x, ty = y;
    for (int i = 0; i < GROW_REACH; i++) {
        int nx, ny;
        if (!stem_next(s, tx, ty, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        tx = nx;
        ty = ny;
        run++;
    }

    /* Round the RING, not up-plus-a-perpendicular: one step round it is
     * always an adjacent cell, whereas adding a perpendicular to a
     * diagonal up lands two cells away. */
    const int up = ring_of(ux, uy);
    const int side = rng_below(&s->rng, 2) ? 1 : 7;   /* +1 or -1 round */

    int site, dx, dy;
    bool thicken = false;
    const int what = rng_below(&s->rng, 8);
    if (what < 4 || run < 3) {
        site = run - 1;               /* HEIGHT: straight on from the tip */
        dx = 0;
        dy = 0;                       /* along the run - filled in below */
    } else if (what < 6) {
        site = run - 1;               /* LEAN: the tip, one step round */
        dx = side;
        dy = 0;                       /* one step round from the run */
    } else if (what < 7) {
        site = rng_below(&s->rng, run - 1);   /* BRANCH: out and up */
        dx = side;
        dy = 0;
    } else {
        /* WIDTH. Straight out from low down, which is how a trunk
         * thickens - and thickening is most of what turns a sapling into
         * wood, because hardening counts a straight run along gravity and
         * a second column beside the first is a second run of its own. A
         * tree that only ever got taller hardened into a one-cell stick;
         * one that also gets fatter hardens into something that looks
         * like a trunk. */
        site = rng_below(&s->rng, (run + 1) / 2);
        dx = side * 2;                /* square on to the run */
        dy = 0;
        thicken = true;
    }
    /* Back up the stem to the chosen site, the same way. */
    int sx = x, sy = y;
    for (int i = 0; i < site; i++) {
        int nx, ny;
        if (!stem_next(s, sx, sy, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        sx = nx;
        sy = ny;
    }

    /* WHICH WAY THIS RUN IS GOING, re-derived from the shape rather than
     * remembered - the same trick the tip walk uses to answer "where is my
     * top". The step from the cell below the site to the site itself is
     * the direction that run has been travelling.
     *
     * It is what makes a limb a limb. Reckoning every direction from
     * gravity meant a branch went out one cell and then climbed: a
     * one-cell branch is a run of one, which trips the `run < 3` gate on
     * to the straight-up arm, on every attempt, for ever. Nothing could
     * travel outward.
     *
     * Falls back to up when there is no cell below - a seed on soil has no
     * line to hold yet - and when the roll says not to, which is what
     * bends a bough back towards upright instead of firing it off as a
     * straight ray. `holds_line` at zero restores the old behaviour
     * exactly. */
    int head = up;
    if (r->holds_line != 0 &&
        (int)(rng_next(&s->rng) & 0xFF) < r->holds_line) {
        /* The step from the cell below this one to this one IS the way
         * the run has been going. One cell, not an average over several:
         * a three-cell baseline was tried and measured no further -
         * limb reach summed 67, 73 and 70 over eight seeds for baselines
         * of one, two and three, which is one spread - while the longer
         * baseline let a run lock into a lean and wander, taking the
         * trees' horizontal drift from 24 to 38. Shorter is simpler and
         * measured better on the one axis that moved. */
        int px, py;
        if (stem_next(s, sx, sy, -ux, -uy, w, h, self, &px, &py)) {
            head = ring_of(sx - px, sy - py);
        }
    }
    {
        const int *hd = ring_dir(head + dx);
        dx = hd[0];
        dy = hd[1];
    }

    if (thicken) {
        /* TAPERED: the allowance shrinks with height, so a trunk is fat at
         * the foot and a single cell by the time it is up in the branches.
         * A uniform allowance grows a pillar - correct by every rule here
         * and not a tree. */
        const int allowed = TRUNK_WIDTH - (lift + site) / 3;
        if (allowed < 2) {
            return true;              /* too high up to be thickening */
        }
        int wide = 0;
        for (int i = 1; i < allowed; i++) {
            const int wx = sx + dx * i;
            const int wy = sy + dy * i;
            if ((unsigned)wx >= (unsigned)w || (unsigned)wy >= (unsigned)h) {
                break;
            }
            const cell_t c = s->cells[(size_t)wy * (size_t)w + (size_t)wx];
            if (c != self &&
                !(r->clings_to != 0 && CELL_MATERIAL(c) == r->clings_to)) {
                break;
            }
            wide++;
        }
        if (wide >= allowed - 1) {
            return true;              /* thick enough already */
        }
    }

    const int gx = sx + dx, gy = sy + dy;
    if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
        return true;
    }
    /* Only a SHOOT shoves - the leading cell, carrying on the way it was
     * already going. A branch does not tunnel sideways through a bank and
     * a trunk does not widen by pushing the ground apart; what a buried
     * seed does is come up.
     *
     * That distinction is also the whole limit on it. Being able to push
     * through soil means growth is no longer held back by having to find
     * empty space, and empty space was quietly doing a lot of the
     * limiting: with every kind of growth allowed to shove, four seeds in
     * a watered bed grew 220 cells of timber, most of it inside the soil
     * where nothing could have reached before. Restricted to the tip, the
     * same scene grows trees that come up out of the ground. */
    const bool shoot = (site == run - 1) && !thicken;

    const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
    if (!CELL_IS_EMPTY(s->cells[gat]) &&
        !(shoot && shove_aside(s, gx, gy, dx, dy, w, h))) {
        return true;                  /* in the way, and will not move */
    }

    /* Grow, and spend the water. */
    s->cells[gat] = self;
    latch_content_flags(s, self);
    mark_rows(s, gy, gy);
    wake_block_and_neighbors(s, gx, gy);

    const cell_t soil = s->cells[soil_at];
    s->cells[soil_at] = CELL_WITH_MOISTURE(soil,
                                           (uint8_t)(CELL_MOISTURE(soil) - 1));
    mark_rows(s, soil_at / w, soil_at / w);

    /* HARDENING. Counted from the bottom of the column - the cell whose
     * gravity-ward neighbour is not more of the same - so a run is
     * measured once however many of its cells grew this step. */
    if (r->hardens_to == 0 || r->harden_run == 0) {
        return true;
    }
    /* Walk DOWN to the foot of the run first, then measure up from
     * there. It used to require the growing cell to BE the foot, so that
     * a run was counted once however many of its cells grew - which is
     * true and useless: every cell of a plant that can reach water rolls,
     * so the one that happens to grow is almost never the one at the
     * bottom. A six-cell stem stood there for twenty thousand steps
     * growing perfectly well and never once re-measuring itself. */
    int cx = x, cy = y;
    for (int i = 0; i < GROW_REACH; i++) {
        int nx, ny;
        if (!stem_next(s, cx, cy, -ux, -uy, w, h, self, &nx, &ny)) {
            break;
        }
        cx = nx;
        cy = ny;
    }
    const int fx = cx, fy = cy;

    int trunk = 1;
    while (trunk < GROW_REACH) {
        int nx, ny;
        if (!stem_next(s, cx, cy, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        cx = nx;
        cy = ny;
        trunk++;
    }
    if (trunk < r->harden_run) {
        return true;
    }


    /* Not on the first qualifying growth. Measuring the run from its foot
     * means every cell of a stem re-measures it, so a run that is tall
     * enough hardens the instant it gets there - and wood does not grow,
     * so a seedling turned into a post before it ever put out a limb.
     *
     * The roll buys back a delay the old, broken version had by accident:
     * it only counted when the cell that grew happened to be the one at
     * the foot, which for a stem of n cells is about one time in n. One in
     * eight, deliberately, rather than one in however tall it is. */
    /* Hinted TAKEN, and that is a performance note rather than a
     * comment. Everything below is the shaping pass, which is the largest
     * block in this function and runs on one growth in eight of the few
     * that get this far. Unhinted, GCC is free to splice it into the
     * middle of a function that runs once per plant cell per step; the
     * same mistake one branch along in sand_liquid.c cost 26% of a
     * benchmark with the simulation byte-identical either way. See
     * docs/Sand/Tuning-At-a-Glance.md. */
    if (__builtin_expect(rng_below(&s->rng, 8) != 0, 1)) {
        return true;
    }

    /* THE SHAPING PASS.
     *
     * Hardening is the only moment that holds a whole run at once, and
     * after it the tree has no green left low down - every cell that grew
     * is timber, and growth is the only thing that makes cells. So this is
     * the one place a trunk can be given girth or a crown can be given
     * leaves; anything trying to do either through ordinary growth is
     * working on a part of the tree that no longer exists.
     *
     * Three things, in one walk from the foot up:
     *
     *   the run becomes wood      - variant 0, explicitly. place_reacted()
     *                               would hand it MATERIAL_VARIANTS - 1,
     *                               which for wood is burn progress at its
     *                               maximum: every tree that reached this
     *                               line used to burn to nothing over the
     *                               next couple of hundred steps, on a
     *                               board with no fire on it.
     *   it gets THICKER at the
     *   foot than at the top      - tapered by index along the run, not by
     *                               `lift`. Using lift is what killed the
     *                               growth-time version of this: after the
     *                               first hardening every green cell sits
     *                               on a wood column, so its lift is at
     *                               least that column's height and the
     *                               allowance is zero for the rest of the
     *                               tree's life. The index is the honest
     *                               measure of how far up the trunk this
     *                               is.
     *   the top gets a CANOPY     - foliage, which is a material of its
     *                               own precisely so that it cannot grow;
     *                               see MATX_LEAF.
     *
     * The last cell of the run is left green. Hardening the whole thing
     * takes the stem's growing point with it, and wood does not grow, so a
     * seedling became a post the moment it was tall enough. */
    const int up_i = ring_of(ux, uy);
    const int hard = (trunk > 1) ? trunk - 1 : trunk;

    int topx[CANOPY_SPAN], topy[CANOPY_SPAN];
    int ntop = 0;

    cx = fx;
    cy = fy;
    for (int i = 0; i < hard; i++) {
        int nx = 0, ny = 0;
        const bool more = stem_next(s, cx, cy, ux, uy, w, h, self, &nx, &ny);
        place_cell(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx,
                   CELL_MAKE(r->hardens_to, 0));

        /* Girth, tapering linearly to nothing by the top - measured
         * against the LENGTH of this run rather than in fixed steps. A
         * fixed step is only a taper for a run about as long as the step
         * assumed: at one cell per three, a run of twenty stays at full
         * width for fifteen of them and hardens a slab. Measured that
         * way, twelve hardenings laid 90 cells of girth and the trees ran
         * together; proportionally it is 6 or 7 each and they do not. */
        const int span = (hard > 1) ? hard - 1 : 1;
        const int extra = (int)r->trunk_girth * (span - i) / span;
        for (int g = 1; g <= extra; g++) {
            const int sidei = (g & 1) ? 2 : 6;      /* square on, both ways */
            const int *gd = ring_dir(up_i + sidei);
            const int gx = cx + gd[0] * ((g + 1) / 2);
            const int gy = cy + gd[1] * ((g + 1) / 2);
            if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
                continue;
            }
            const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
            if (!CELL_IS_EMPTY(s->cells[gat])) {
                continue;
            }
            place_cell(s, gx, gy, gat, CELL_MAKE(r->hardens_to, 0));
        }

        /* A rolling window of the last few, so the crown can be hung once
         * the cells below it are wood - doing it before would leave
         * foliage in the fan stem_next() is still walking. */
        if (ntop < CANOPY_SPAN) {
            topx[ntop] = cx;
            topy[ntop] = cy;
            ntop++;
        } else {
            for (int k = 1; k < CANOPY_SPAN; k++) {
                topx[k - 1] = topx[k];
                topy[k - 1] = topy[k];
            }
            topx[CANOPY_SPAN - 1] = cx;
            topy[CANOPY_SPAN - 1] = cy;
        }

        if (!more) {
            break;
        }
        cx = nx;
        cy = ny;
    }

    /* The crown. Five upward directions round each of the remembered
     * cells - straight on, both diagonals, and square out either way. */
    if (r->canopy != 0 && r->canopy_to != 0) {
        static const int crown[5] = { 7, 0, 1, 2, 6 };
        for (int t = 0; t < ntop; t++) {
            for (int c = 0; c < 5; c++) {
                const int *cd = ring_dir(up_i + crown[c]);
                const int lx = topx[t] + cd[0], ly = topy[t] + cd[1];
                if ((unsigned)lx >= (unsigned)w ||
                    (unsigned)ly >= (unsigned)h) {
                    continue;
                }
                const size_t lat = (size_t)ly * (size_t)w + (size_t)lx;
                if (!CELL_IS_EMPTY(s->cells[lat])) {
                    continue;
                }
                if ((int)(rng_next(&s->rng) & 0xFF) >= r->canopy) {
                    continue;
                }
                place_reacted(s, lx, ly, lat, r->canopy_to);
            }
        }
    }
    return true;
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
        /* A liquid that BURNS is a heat source and cannot be boiled by
         * heat. A liquid that is FLAMMABLE is fuel, and heat reaching it
         * should light it rather than evaporate it - oil in a hot pan
         * catches fire; it does not turn into steam.
         *
         * That second half was missing, and the symptom was oil vanishing
         * near heat. Measured before the fix: 180 units of oil in a stone
         * pan over a fire went to ZERO in sixty steps, leaving fourteen
         * cells of steam - steam being the tell, since oil has no business
         * producing any. Same shape as lava being boiled by its own
         * conducted heat, one material along: this test was written when
         * water was the only liquid that could be on the far side of a
         * wall, and it has been wrong for every liquid added since. */
        if (bm->kind == KIND_LIQUID && reaction_of(bc)->burns == 0 &&
            reaction_of(bc)->flammability == 0) {
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
            if (br->flammability != 0 &&
                (!br->needs_air || touches_air(s, rx, ry, w, h))) {
                /* needs_air is checked here for the same reason
                 * try_ignite() checks it: a pool of fuel burns at its
                 * surface, not through its volume. Without it a pan would
                 * light the oil against its own bottom - the one cell of
                 * a pool guaranteed to be buried. */
                const uint8_t becomes = br->ignites_to ? br->ignites_to
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

    /* Nothing can put this out while there is no liquid anywhere on the
     * board, so the whole four-neighbour scan goes with one field test.
     * Worth having rather than tidy: neighbor_quenches() reads the
     * material table per neighbour, and measured by deleting, that scan
     * is 9.3% of the full-screen-of-fire benchmark and 10% of the fire
     * cascade - both of which are boards with no liquid on them at all.
     *
     * Sound because may_have_liquid is armed by latch_content_flags(),
     * which every write that can create a liquid goes through, this
     * pass's own included: place_cell() is how snow melting into water
     * gets made, and it latches. The flag is only ever cleared by a full
     * cross-flow sweep that found none, so false really does mean none. */
    if (s->may_have_liquid) {
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
#define FOUND_FALLER          16u
#define FOUND_WITHERING       32u

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
        /* Hot gas warms what it touches, but only where there is something
         * that can hold a temperature. may_have_temperature alone never
         * closes this: smoke and steam both `warm`, so placing either one
         * arms may_have_temperature itself, and taking this branch reports
         * FOUND_TEMPERATURE unconditionally - which re-arms it every step
         * it is taken. Gated on may_have_heat_holder as well, which answers
         * the actual question - is there a heat_ramp cell anywhere on the
         * grid at all - so a board of nothing but gas and fire never pays
         * for the neighbour scan below to find nothing. */
        if (r->warms != 0 && s->may_have_temperature &&
            s->may_have_heat_holder) {
            step_one_warming_cell(s, x, y, w, h, r);
            found |= FOUND_TEMPERATURE;
            continue;
        }
        /* Soaking and drying. Reached by sand and dirt, which are on most
         * boards, so the cheap tests come first: the field check, then
         * may_have_liquid inside, and only then a neighbour scan. */
        if ((r->soaks != 0 || r->dries != 0) &&
            step_one_soaking_cell(s, row, x, y, w, h, r)) {
            found |= FOUND_MOISTURE;
            continue;
        }
        /* Falling. First, because a seed still in the air has nothing to
         * grow from and no soil to look at - and because the cheapest
         * answer for everything else on the board is one zero field. */
        if (r->falls != 0) {
            /* Armed by the cell EXISTING, not by it moving. A landed seed
             * reported nothing, so on a board holding one settled plant
             * the flag cleared and the whole pass stopped running - and
             * then nothing could start it again. Dissolve the ground out
             * from under a plant with acid and it hung in the air, which
             * is the same shape of bug the cold pass documents for snow
             * on dry ground. */
            found |= FOUND_FALLER;
            if (step_one_falling_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
        /* Withering. Not gated on may_have_moisture, deliberately: the
         * cells this is for are the ones with no water anywhere near
         * them, on boards that may have none at all. */
        if (r->withers != 0) {
            /* Armed by being PRESENT, exactly as the faller flag above
             * is, and for the same reason: a leaf that is safe this step
             * because it is touching wood is not a leaf with nothing to
             * do ever, and once the flag clears nothing can set it
             * again. */
            found |= FOUND_WITHERING;
            if (step_one_withering_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
        /* Drinking, before growing: a leaf standing in a puddle should
         * move that water into the ground whether or not the tree has any
         * use for it this step. Gated on may_have_liquid, so a board with
         * no water on it pays one field test. */
        if (r->drinks != 0 && s->may_have_liquid) {
            if (step_one_drinking_cell(s, x, y, w, h, r, c)) {
                found |= FOUND_MOISTURE;
            }
        }
        /* Growing. Reached only where there is soil with water in it,
         * which is what may_have_moisture already tracks - a plant on dry
         * ground costs one field test and nothing else. */
        if (r->grows != 0 && s->may_have_moisture) {
            step_one_growing_cell(s, x, y, w, h, r);
            found |= FOUND_MOISTURE;
            continue;
        }
        /* Budding. Same gate as growing, and reached by unlit wood, which
         * falls through every branch above it. */
        if (r->sprouts != 0 && s->may_have_moisture) {
            if (step_one_sprouting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
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
        !s->may_have_temperature && !s->may_have_moisture &&
        !s->may_have_faller && !s->may_have_withering) {
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
    if (!(found & FOUND_FALLER)) {
        s->may_have_faller = false;
    }
    if (!(found & FOUND_WITHERING)) {
        s->may_have_withering = false;
    }

    /* may_have_heat_holder is deliberately NOT cleared here, unlike the
     * five flags above. "Clear it at the end of the pass when nothing was
     * found, like the other five" is the obvious thing to write next, and
     * it is wrong.
     *
     * The other five are safe to clear this way because the walk above
     * reads every cell that could have set them. may_have_heat_holder is
     * different: a cell with a heat_ramp can be CREATED during this very
     * pass, behind the scan pointer - lava quenching to stone is exactly
     * this, going through place_cell() -> latch_content_flags() mid-row.
     * The walk that already passed that cell's row never sees it, so
     * `found` never gets a bit for it, and clearing here would wipe the
     * flag latch_content_flags() had just armed a moment earlier. Nothing
     * but another write would ever set it again, so convection onto that
     * new stone would be dead for good.
     *
     * Measured, not assumed: a version of this with the end-of-pass clear
     * produced a different simulation from HEAD on two four-liquid scenes.
     * The arm-only version above is byte-identical to HEAD on all eight
     * benchmark scenes tested.
     *
     * Never clearing it costs one case: a board that once held stone or
     * glass and no longer does keeps paying the scan for ever. That is
     * exactly what such a board pays today, without this flag at all, so
     * the flag can never make anything slower than it already is - it can
     * only fail to help. What it does help is the board that has never
     * held a heat-holder, and that is where the whole cost was.
     * This project's convention for a flag like this is to ask who pays to
     * KEEP it true versus who reads it - and the answer here is nobody
     * pays any per-step upkeep at all, ever, once it is armed. */
}
