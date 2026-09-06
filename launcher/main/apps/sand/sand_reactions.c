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
#include "reaction_doc.h"

/* The four cardinal directions a burning cell's reactions look in. Not
 * eight - diagonal spread felt too generous for a first version; widening
 * this table is the whole of what changing that would take. Also the
 * fixed, arbitrary order ember's flare (below) picks its target from. */
static const int reaction_dirs[4][2] = {
    {0, -1},
    {0, 1},
    {-1, 0},
    {1, 0},
};

/* PAIR_BITS - one byte per (mine, theirs) material-id ordered pair,
 * classifying a neighbour probe before it ever loads reaction_of() or
 * draws from the RNG. Replaces s->heat_mask/s->wet_mask (both folded in
 * here) and adds three more of the same shape: sand_step_reactions()'s own
 * top comment explains why an O(16x16) rebuild every PASS, never cached
 * across steps, cannot go stale against a sand_set_* table override or a
 * cell created mid-pass - everything said there applies here unchanged,
 * this is that same mechanism widened to five probes instead of two.
 *
 * HONESTY NOTE, because it shapes what this table actually is: every bit
 * below except PAIR_DENSER depends only on `theirs` - the neighbour being
 * probed - never on `mine`. That is not an oversight; it is what this
 * file's chemistry mostly IS (see this file's own top comment and
 * docs/Sand/Reaction-Table.md): a probe's outcome today is almost always a
 * fact about the neighbour's own reaction row, independent of what is
 * doing the probing, which is exactly why the two 1-D masks this table
 * absorbs already worked. Storing five theirs-only bits in a 16x16 shape
 * costs nothing beyond the four-theirs-only-bits' own broadcast loop below
 * (still O(16), same as heat_mask/wet_mask always cost) and buys one
 * consistent lookup shape for every consumer in this file, including the
 * one bit (PAIR_DENSER) that is genuinely pairwise. See pair_theirs_bits()
 * just below for how a caller with no real `mine` (try_heat_transform has
 * none - see its own call sites) reads the theirs-only bits without
 * inventing one. */
#define PAIR_HEAT_RESPONSIVE (1u << 0) /* theirs could pass try_heat_transform()'s first two gates - was heat_mask */
#define PAIR_WETS            (1u << 1) /* theirs is a liquid whose reaction row wets - was wet_mask */
#define PAIR_IGNITABLE       (1u << 2) /* theirs has a nonzero flammability - try_ignite()'s own first reject */
#define PAIR_QUENCHES        (1u << 3) /* theirs is a liquid that is neither fuel nor a heat source - neighbor_quenches() */
#define PAIR_DISSOLVABLE     (1u << 4) /* theirs has a nonzero dissolvable - step_one_dissolver_cell()'s own reject */
/* PAIR_DENSER not packed; no data cache. neighbor_smothers() avoids
 * reaction_of() and RNG, already in registers. Routing through table adds
 * load. Threading `mine` could simplify but unmeasured. */
static uint8_t pair_bits[MATERIAL_MAX][MATERIAL_MAX];

/* Reads theirs-only bits for callers without `mine`. Used by
 * try_heat_transform(), step_one_cold_cell(), and conduct_heat(). No prober
 * material ID needed. Row MAT_EMPTY (0) stores all theirs-only bits. Never
 * test PAIR_DENSER; every row reads as denser than empty space. */
static inline uint8_t
pair_theirs_bits(uint8_t theirs) {
    return pair_bits[MAT_EMPTY][theirs];
}

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
static inline void
place_cell(sand_t* s, int x, int y, size_t at, cell_t c) {
    s->cells[at] = c;
    latch_content_flags(s, c);
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
}

static inline void
place_reacted(sand_t* s, int x, int y, size_t at, uint8_t spec) {
    if (spec >= (MAT_EXTENDED << 4)) {
        place_cell(s, x, y, at, (cell_t)spec); /* identity IS the low nibble,
                                                 * but only for the static
                                                 * half (0xF0-F7) - gunpowder
                                                 * spends its low nibble's
                                                 * bit 3 on identity and the
                                                 * bottom three bits on a
                                                 * code, same as any other
                                                 * `spec` reaching here as a
                                                 * whole byte */
        return;
    }
    const material_id_t mat = (material_id_t)spec;
    /* HEAT starts at zero. MATERIAL_VARIANTS - 1 turns sand to lava
     * instantly. Cold measures exposure since creation. For wood,
     * MATERIAL_VARIANTS - 1 means alight. Use place_cell() with specific
     * variant for non-fire. */
    place_cell(s, x, y, at, CELL_MAKE(mat, reactions[mat].heat_ramp != 0 ? SAND_AMBIENT_HEAT : MATERIAL_VARIANTS - 1));
}

/* Checks if (nx, ny) is in bounds, holds a liquid, and neither `flammability`
 * nor `burns`. Water quenches. Oil and lava do not. Quenches determined by
 * `flammability` and `burns`, not `quenches` flag. */
static inline bool
neighbor_quenches(const sand_t* s, int nx, int ny, int w, int h) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    /* PAIR_QUENCHES (this file's own top comment) packs exactly the three
     * checks this used to make one at a time - liquid, not fuel, not a
     * heat source - so a non-quenching neighbour never reaches reaction_of
     * (n) at all now, the same win try_heat_transform()'s heat_mask
     * already banked. */
    return (pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_QUENCHES) != 0;
}


/* Checks if all 4 cardinal neighbours smother a cell, ensuring true burial.
 * Unlike neighbor_quenches()/try_ignite(), it returns false at first failure.
 * Not cover_mask()/covered_at(). Burial differs from gravity-relative
 * coverage, avoiding rotation or side ignoring. */
static inline bool
smothered(const sand_t* s, int x, int y, int w, int h, uint8_t density) {
    for (int d = 0; d < 4; d++) {
        if (!neighbor_smothers(s, x + reaction_dirs[d][0], y + reaction_dirs[d][1], w, h, density)) {
            return false;
        }
    }
    return true;
}

/* Checks if any cardinal neighbor is air for reaction_t.needs_air. Fire needs
 * this; empty alone fails. Flame stops being air-bound once it touches fuel.
 * Flame sat on oil for 30 steps before fix. Off-grid and walls are not air.
 * One open neighbor suffices. */
static inline bool
touches_air(const sand_t* s, int x, int y, int w, int h) {
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
static inline void pay_quench_cost(sand_t* s, int nx, int ny, int w);

/* Defined below, beside the cold cell that is its other caller - a crack
 * can start from either direction of shock, and this is the earlier one. */
static void crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into);

/* Defined below, beside try_flare() - the wet-dirt stage of
 * try_heat_transform() needs the exact same "put a cell of `spec` into
 * the first empty cardinal" step ember's flame does, and this is the
 * earlier of its two callers. */
static inline bool emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec);

/* Defined just below try_heat_transform() itself - the wrapper needs to
 * call forward into the core it hands off to (stage 2 of bd esp32c6-iu5's
 * pair-matrix restructure - see try_heat_transform()'s own comment). */
static inline __attribute__((always_inline)) bool try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h,
                                                                           size_t at, cell_t n);

/* Defined below, beside step_one_soaking_cell() and the rest of the soil
 * moisture machinery - try_heat_transform_given()'s wet-earth branch drives
 * moisture to zero exactly the way that machinery does, and needs the same
 * dry-tone handling, not a copy of it. */
static inline cell_t soil_set_moisture(cell_t c, uint8_t new_moisture, uint8_t nearby_moisture);

/* How many consecutive successful dry smelts share one flaw/no-flaw decision?
 * See reaction_t.flaw_to in material.h and try_heat_transform() SMELT FLAW
 * comment for details. Tune on device once ore is available. */
#define HEAT_FLAW_CLUMP 5

/* FORCED INLINE, and that is a performance fix rather than a preference.
 *
 * The wet-earth branch below (723fac6) pushed this function past GCC's
 * size heuristic. At the commit before it the symbol does not exist at
 * all - inlined at all four call sites - and at 723fac6 it is emitted
 * out of line with a cold .part.0 split beside it, so every scene that
 * carries heat pays for a call it did not used to make.
 *
 * The flaw/spoils branches added after this measurement grow the function
 * further still, in the same direction the wet-earth branch already did -
 * this has NOT been re-measured on device. If a future capture shows the
 * cost climbing again, look here first rather than assuming the ratio
 * above still holds.
 *
 * Host, best of seven with all five candidates interleaved, simulation
 * byte-identical either way: full screen of fire -12.1%, lava stress
 * -6.5%, four liquids -5.1%, thermal shock -4.8%, and both liquid-free
 * controls flat. Measure-by-deleting the wet-earth branch outright is
 * WORSE than this (-9.5% on fire against -12.1%), which is what says the
 * cost is the shape of the code and not the work that branch does.
 *
 * Both obvious alternatives lost, measured rather than reasoned: hinting
 * the branch unlikely with __builtin_expect - the idiom
 * move_liquid_grain() uses in sand_liquid.c - moved nothing, and
 * splitting the branch into its own noinline function bought about 1%,
 * because it shrank this function without getting it back under the
 * threshold.
 *
 * Forcing an inline is not free on this chip, and this campaign has
 * twice measured it costing more than the call once the loop outgrew the
 * 32 KB i-cache (attempts 07 and 08). This forces four call sites at
 * once, so it is a bet only a device capture can settle - that capture
 * is pending as this lands. If it disagrees with the host, take this
 * attribute back out: the revert is the finding, not a failure.
 */
/* Turns (nx, ny) into whatever reaction_t.heats_to names, if it is in
 * bounds and the roll succeeds - heat WITHOUT burning.
 *
 * Sand into glass is the only use today. Kept apart from try_ignite_given()
 * rather than folded into it because the two are different events that
 * happen to share a trigger: one is combustion and consumes fuel, the
 * other is a phase change and consumes nothing. A material can sensibly
 * have both, neither, or one.
 *
 * SPLIT IN TWO for stage 2 of bd esp32c6-iu5's pair-matrix restructure -
 * this self-contained wrapper (bounds check, cell load, PAIR_HEAT_
 * RESPONSIVE gate) for the three call sites that have no neighbour of
 * their own already loaded (step_one_cold_cell()'s two, conduct_heat()'s
 * one - all completely unchanged by this split, same signature, same
 * bytes at the source level), and try_heat_transform_given() below for the
 * fourth: the shared ignite+heat walk in step_one_burning_cell(), which
 * loads the neighbour and its pair_bits[][] byte once for BOTH probes and
 * has no reason to pay this wrapper's redundant second load and second
 * gate test. Every call this wrapper makes to the core below happens
 * after this wrapper has done its own three prologue steps, so nothing
 * about what a neighbour must pass to reach the core changed - only who
 * does the checking, and how many times.
 *
 * Returns whether it changed anything. */
static inline __attribute__((always_inline)) bool
try_heat_transform(sand_t* s, int nx, int ny, int w, int h) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    /* PAIR_HEAT_RESPONSIVE: Rejects neighbours with clear bits before
     * accessing reaction table or RNG, bit-identical to subsequent checks for
     * false return. */
    if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_HEAT_RESPONSIVE) == 0) {
        return false;
    }
    return try_heat_transform_given(s, nx, ny, w, h, at, n);
}

/* try_heat_transform() hands off if neighbour is non-empty and
 * PAIR_HEAT_RESPONSIVE, not re-deriving. FORCED INLINE for two call sites,
 * maintaining wrapper's performance bet. Wrapper's four call sites unchanged,
 * shared walk's direct call confirms step_one_burning_cell() shrink. */
static inline __attribute__((always_inline)) bool
try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h, size_t at, cell_t n) {
    const reaction_t* r = reaction_of(n);

    /* Material that BANKS heat climbs one level, transforming only at the
     * top. Triggered by contact with a burning cell or through a conductor,
     * like memoryless form but remembers. */
    if (r->heat_ramp != 0) {
        /* SHOCK, hot-to-cold direction. Cell cracks when heated after
         * chilling below room temperature. Mirror of step_one_cold_cell().
         * Checked before ramp roll to break frosted pane on contact with
         * lava. */
        REACTION_DOC(shatters_to, "if warmed while badly chilled");
        if (r->shatters_to != 0 && CELL_VARIANT(n) <= SAND_SHOCK_COLD) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n), (material_id_t)r->shatters_to);
            return true;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_ramp) {
            return false;
        }
        const uint8_t heat = CELL_VARIANT(n);
        if (heat + 1 >= MATERIAL_VARIANTS) {
            if (r->heats_to == 0) {
                return false; /* banks heat but melts into nothing */
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
    /* A LIT CELL is `heats_to` GUNPOWDER_LIT_CELL, causing neighbours to roll
     * and react identically. Rejected pre-roll, `explodes != 0` singles out
     * this material, avoiding extra compare. */
    if (r->explodes != 0 && cell_code(n) >= r->lit_from) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_chance) {
        return false;
    }

    /* WET EARTH FIRST. Dirt variant shows moisture. Roll converts moisture to
     * steam. `dries != 0` marks moisture; CELL_MOISTURE() checks. Sand, glass
     * unaffected. Two extra reads on success. Saturated dirt needs
     * SOIL_MOISTURE_MAX + 1 for metal, with steam. */
    if (r->dries != 0 && moisture_of(n, r) != 0) {
        /* RUINED BY HASTE, off this SAME roll - see reaction_t.spoils_to's
         * own comment (material.h). Checked first, so a spoil pre-empts
         * the moisture-driving step below rather than competing with it:
         * a cell either cracks to `spoils_to` this roll, or it dries by
         * one level as it always did - never both from one success.
         *
         * UNCONDITIONAL - no "exempt the first roll" gate, on purpose. An
         * earlier version of this gated on `CELL_MOISTURE(n) <
         * SOIL_MOISTURE_MAX`, trying to guarantee a cell always puffs one
         * free level of steam before it is ever at risk. That reasoning
         * had a hole: dirt also dries AMBIENTLY (this same `dries` field,
         * ticking every step regardless of any heat source - see the
         * MOISTURE SPREADS block above), so a cell can already be below
         * SOIL_MOISTURE_MAX by the time HEAT ever touches it for the first
         * time, at which point the gate reads as "already used its
         * exemption" when it never actually got one. At dirt's own rates
         * (heat_chance 10, dries 5) that race is won by ambient drying
         * roughly a third of the time - rare enough to hide behind a low
         * spoils_chance, and exactly what broke
         * test_watered_dirt_steams_before_it_smelts once spoils_chance was
         * rebalanced high enough to matter. No amount of moisture-based
         * gating can fix it without a spare bit dirt's variant does not
         * have (material.c's own comment: "Dirt's variant is fully
         * spent"). So: no gate. A cell CAN now spoil on the very first
         * heat contact it ever gets, with no warning puff first - which is
         * the correct reading of "even stronger" besides: wet ore cracking
         * on first contact, not after a polite warning, is the point. */
        REACTION_DOC(spoils_to, "if wet when heat reaches it");
        if (r->spoils_to != 0 &&
            (int)(rng_next(&s->rng) & 0xFF) < r->spoils_chance) {
            place_reacted(s, nx, ny, at, (material_id_t)r->spoils_to);
            return true;
        }
        /* No neighbour to bias from - the heat came from whatever is
         * burning, not from a wetter cell of soil, so a cell driven bone
         * dry by fire has nothing nearby to leave an imprint of. See
         * soil_set_moisture()'s own comment for what 0 means here. */
        s->cells[at] = soil_set_moisture(n, (uint8_t)(moisture_of(n, r) - 1), 0);
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
        emit_into_empty_neighbor(s, nx, ny, w, h, MAT_STEAM);
        return true;
    }

    /* REVISION 2: `explodes` is no longer read here at all - gunpowder's
     * heat path just falls straight through to the ordinary `yield =
     * heats_to` below, exactly like any other material, because heats_to
     * is GUNPOWDER_LIT_CELL (material.c) rather than MAT_FIRE. Heat lights
     * the fuse; whether that fuse ends in a blast is now entirely
     * step_one_burning_cell()'s burn-out question, not this function's -
     * see reaction_t.explodes's own comment (material.h). */
    material_id_t yield = (material_id_t)r->heats_to;

    /* HEAT_FLAW_CLUMP triggers roll heat_flaw_seq. Each HEAT_FLAW_CLUMP,
     * heat_flaw_is_flawed rerolls, reused for next HEAT_FLAW_CLUMP - 1
     * triggers. Flaws streak along scan, deemed acceptable. Mod check before
     * increment, first trigger rolls fresh. */
    if (r->flaw_to != 0) {
        if (s->heat_flaw_seq % HEAT_FLAW_CLUMP == 0) {
            s->heat_flaw_is_flawed =
                (int)(rng_next(&s->rng) & 0xFF) < r->flaw_chance;
        }
        s->heat_flaw_seq++;
        if (s->heat_flaw_is_flawed) {
            yield = (material_id_t)r->flaw_to;
        }
    }

    place_reacted(s, nx, ny, at, yield);
    return true;
}

/* How far a crack runs. A drawn vessel is a few dozen cells, a hand-drawn
 * wall a couple of hundred, shattering most builds. Limits worst case to a
 * screen filled with glass, turning the board over. Frontier size as uint16
 * indices, 512 bytes of stack. */
#define CRACK_MAX 256

/* A crack starts at (x, y), converting up to CRACK_MAX cells to sand.
 * Previously, shattering converted one cell, making thermal shock seem weak.
 * Cracks spread regardless of cell temperatures, leaving behind CULLET sand. */
static inline void
place_cracked(sand_t* s, int x, int y, size_t at, material_id_t into) {
    if (into == MAT_SAND) {
        place_cell(s, x, y, at, CELL_MAKE(into, (uint8_t)(SAND_CULLET_BASE + rng_below(&s->rng, SAND_CULLET_SHADES))));
        return;
    }
    place_reacted(s, x, y, at, into);
}

static void
crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into) {
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

/* Walks a chain of lava-cooling events outward from (x, y), where the
 * burning liquid that stood there has already become `product` (this
 * chain's own caller placed it, immediately before calling this). Each
 * link rolls `chance`; on success it picks ONE cardinal neighbour still
 * holding the SAME burning liquid, freezes it to `product` too, and
 * repeats from there. A failed roll, no eligible neighbour, or
 * SAND_LAVA_COOLOFF_MAX_CHAIN links stop it - see that constant's own
 * comment (sand.h) for why it lives there rather than here.
 *
 * "Still the same burning liquid" is answered by comparing the
 * neighbour's OWN quench_to against `product`, rather than by carrying a
 * `from` material id through every link - reaction_of(n)->quench_to ==
 * product is true for exactly the same cells CELL_MATERIAL(n) == (the
 * original lava's id) would have picked out, since only a burning LIQUID
 * whose own quench product is `product` can be the thing this chain
 * started from (see the KIND_LIQUID gate at both call sites, which is
 * what keeps fire - quench_to MAT_STEAM, KIND_GAS - from ever reaching
 * here at all). Lava is the only material satisfying that today, but the
 * test itself makes no assumption of being the only one - it would keep
 * meaning the right thing if a second burning liquid arrived with its own
 * distinct quench_to.
 *
 * ITERATIVE, NOT RECURSIVE. A chain through a real pool can legitimately
 * want to run several cells deep, and recursion would grow the call stack
 * by one frame per link - on a chip with 368 KB of usable RAM and no
 * MMU-backed guard page (docs/Notes/README.md), a chain long enough to
 * matter is also long enough to be dangerous. This only ever has one
 * live cursor (the current end of the chain), unlike crack_run()'s
 * frontier array above, which explores several directions in parallel -
 * so a plain loop is enough; there is nothing here for a stack to help
 * with. */
static void
cool_off_chain(sand_t* s, int x, int y, int w, int h, uint8_t product, int chance) {
    int cx = x, cy = y;
    for (int link = 0; link < SAND_LAVA_COOLOFF_MAX_CHAIN; link++) {
        if (chance == 0 || (int)(rng_next(&s->rng) & 0xFF) >= chance) {
            return;
        }
        /* Count-then-index: collect eligible neighbours first for uniform
         * pick among up to four cardinal directions. */
        int cand_x[4], cand_y[4], n_cand = 0;
        for (int d = 0; d < 4; d++) {
            const int nx = cx + reaction_dirs[d][0];
            const int ny = cy + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(n)) {
                continue;
            }
            if (material_of(n)->kind != KIND_LIQUID || reaction_of(n)->quench_to != product) {
                continue;
            }
            cand_x[n_cand] = nx;
            cand_y[n_cand] = ny;
            n_cand++;
        }
        if (n_cand == 0) {
            return;
        }
        const int pick = rng_below(&s->rng, n_cand);
        cx = cand_x[pick];
        cy = cand_y[pick];
        place_reacted(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx, product);
    }
}

/* Fast wet soil downhill rate; separate from `spread` (drinking, diffusing).
 * `#define` used instead of new `reaction_t` field for materials with `dries
 * != 0` (dirt, gunpowder). 15 is a quarter of dirt's old `spread` value (60). */
#define SOIL_PERCOLATE_CHANCE 15

/* THE DRYING-FRONT IMPRINT. A cell whose moisture just reached zero picks
 * its new dry tone here rather than through CELL_WITH_MOISTURE() directly
 * - see that macro's own comment (material.h) for why zero cannot go
 * through it like every other level does: there is no old tone left to
 * fall back on, so a naive write would land every drying cell on the same
 * fixed value, which is the flat, historyless dry soil this whole
 * re-encoding exists to fix.
 *
 * `nearby_moisture` is the one thing worth spending a read on: the
 * moisture level, 0 to SOIL_MOISTURE_MAX, of whatever cell this drying was
 * busy WATERING at the exact moment it ran out - the neighbour a hand-off
 * had just given a level to, or a root's own sink. Pass 0 where no such
 * neighbour exists (heat driving off a last puff of steam, ambient decay
 * with nothing beside it to receive anything, a plant drinking the last
 * level for itself) and the cell dries bone pale, which is correct: nothing
 * nearby stayed wet to leave an impression. Every site that can drive
 * moisture to zero funnels through here - see this function's callers -
 * so the SAME cell dries to the SAME look whichever of them did it.
 *
 * SCALED THROUGH dry_tone_from_moisture() (material.h), not read straight
 * in as a tone - `nearby_moisture` is a moisture value, 0..moist_max, and
 * only byte-identical to a tone for a material whose tone and moisture
 * ranges happen to be the same width. Dirt's are, by construction
 * (SOIL_DRY_TONES - 1 == SOIL_MOISTURE_MAX, both 7, see the _Static_assert
 * right below) so this is a no-op for dirt and the direct read this used
 * to be was never wrong there. Gunpowder's are not (three tones against
 * four moisture levels): passing its own moisture straight through would
 * have clamped every neighbour above tone 2 to the same darkest shade
 * instead of spreading across the three it actually has to work with. */
static inline cell_t soil_dry_out(cell_t c, uint8_t nearby_moisture)
{
    /* soil_cell() uses reaction_of(c) instead of hardcoded MAT_DIRT to handle
     * any material. Avoids direct index read error with gunpowder's
     * MAT_EXTENDED. Byte-identical to CELL_SOIL() for dirt. */
    const reaction_t* r = reaction_of(c);
    return soil_cell(c, dry_tone_from_moisture(nearby_moisture, r), 0, r);
}

_Static_assert(SOIL_DRY_TONES - 1 == SOIL_MOISTURE_MAX,
               "dry_tone_from_moisture() is identity for dirt only because "
               "these two ranges are the same width - the fingerprint gate "
               "is what actually proves soil_dry_out() stayed byte-"
               "identical for dirt after this stopped being a direct read");

/* All sites changing soil cell moisture call this instead of
 * CELL_WITH_MOISTURE() directly. At zero moisture, it calls soil_dry_out(),
 * biased by `nearby_moisture`. */
static inline cell_t soil_set_moisture(cell_t c, uint8_t new_moisture, uint8_t nearby_moisture)
{
    return new_moisture != 0
               ? with_moisture(c, new_moisture, reaction_of(c))
               : soil_dry_out(c, nearby_moisture);
}

/* One cell holding liquid, split for input and output. Soaks a UNIT,
 * transforming into `soaks_to` or increasing its variant. Drying decreases
 * the variant. Returns true if wet or near liquid. Prevents
 * `may_have_moisture` in most boards. Activated by SOAKING side, not liquid. */
static bool
step_one_soaking_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t c = row[x];
    /* moisture_of(), not the raw code - a dry cell's code is a TONE
     * (material.h's own comment on the moisture codec), and reading the
     * whole code as wetness would make all but the very palest of freshly
     * poured dirt look sodden and feed plants that were never watered. */
    const uint8_t held = moisture_of(c, r);

    /* SATURATION CAN MINT A NEW LIQUID - gunpowder's own reaction, checked
     * before soak loop. `>=` used, not `==`. Short-circuits on `soaked_to !=
     * 0` for dirt and other materials. */
    REACTION_DOC(soaked_to, "once fully saturated, at a per-step chance");
    if (r->soaked_to != 0 && held >= r->moist_max && (int)(rng_next(&s->rng) & 0xFF) < r->soaked_chance) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, r->soaked_to);
        return true;
    }

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
            /* A liquid that wets things, specifically water. Taking a unit of
             * any KIND_LIQUID soaks oil, acid, and lava into the ground.
             * PAIR_WETS combines the old three-part test (empty, not
             * KIND_LIQUID, wets == 0) into one shift-and-test, covering
             * CELL_IS_EMPTY() without loading materials[] or reaction_of(n). */
            if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_WETS) == 0) {
                continue;
            }
            beside_liquid = true;

            if ((int)(rng_next(&s->rng) & 0xFF) >= soaks) {
                continue;
            }
            /* The liquid pays for what was taken out of it. */
            pay_quench_cost(s, nx, ny, w);

            if (r->soaks_to != 0) {
                /* Becomes wet, holding no tone; soil's dry tones restored by
                 * sacrificing wet tones, moisture gradient dictates wet
                 * soil's variation; tone argument ignored, 0 used. */
                s->cells[(size_t)y * (size_t)w + (size_t)x] =
                    soil_cell(CELL_MAKE(r->soaks_to, 0), 0, 1,
                             &reactions[r->soaks_to]);
                latch_content_flags(s, s->cells[(size_t)y * (size_t)w + (size_t)x]);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
                return true;
            }
            if (held < r->moist_max) {
                row[x] = with_moisture(c, (uint8_t)(held + 1), r);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            }
            return true;
        }
    }

    /* MOISTURE SPREADS. A wet cell hands a level to a drier soaker beside it,
     * and hands it to dry SAND by turning that sand into more of itself -
     * which is the difference between a puddle leaving a crust one cell thick
     * and a puddle soaking outward into a patch of soil. Reported as dirt not
     * diffusing what it drinks, and it did not: only a cell touching the
     * LIQUID converted, so the wet dirt that formed was a wall between the
     * water and everything behind it. It runs at the full soaking rate. It
     * was a quarter of it, on the reasoning that water reaching soil should
     * always outpace soil passing it along - true, and it made the front so
     * slow that the two were indistinguishable from "nothing happens". */
    const int spread = soaks;

    /* `dries` marks a variant as moisture. Without it, sand would be read as
     * wet, converting dunes to soil. `spread` checked before the roll to
     * avoid RNG shift, fixing sand sinking through fire. try_ignite() has the
     * same note. */
    if (r->dries != 0 && held >= 2 && spread != 0 && (int)(rng_next(&s->rng) & 0xFF) < spread) {
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
            const reaction_t* nr = reaction_of(n);
            if (nr->soaks == 0) {
                continue; /* not something that drinks */
            }

            /* HALF THE DIFFERENCE, not one level. A single level drop fails
             * after seven cells; a converted grain starts with 1, below the
             * needed 2. Half gap movement is conservative, non-oscillating,
             * and reaches full water range. */
            int give, cost, recv_m;
            if (nr->soaks_to != 0) {
                /* Dry sand beside wet soil becomes soil - and is handed
                 * enough to go on wetting ITS neighbours, which is what
                 * turns a puddle into a spreading patch of earth. It
                 * arrives WET, so it keeps no tone of its own - see the
                 * soaking branch above's own comment on the same trade. */
                give = held / 2;
                if (give == 0) {
                    continue; /* not enough to bind a grain */
                }
                cost = give;
                recv_m = give;
                s->cells[nat] = soil_cell(CELL_MAKE(nr->soaks_to, 0), 0,
                                         (uint8_t)give, &reactions[nr->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else if (same_species(n, c) && !cell_is_burning(n)) {
                /* A LIT NEIGHBOUR IS NOT A DRIER ONE - moisture_of() reads a
                 * lit fuse as 0, like bone-dry soil, causing the gap
                 * calculation to overwrite its lit byte with an arbitrary
                 * moisture code. `n` is already loaded, so the check is free. */
                give = (held - moisture_of(n, nr)) / 2;
                if (give == 0) {
                    continue; /* already even with this one */
                }
                recv_m = moisture_of(n, nr) + give;
                s->cells[nat] = with_moisture(n, (uint8_t)recv_m, nr);
                cost = give;
            } else {
                continue;
            }

            /* If this hand-off empties the donor, its dry tone is biased
             * by the neighbour it just watered - see soil_dry_out()'s own
             * comment for why that neighbour, of everything on the board,
             * is the one worth reading. */
            row[x] = soil_set_moisture(c, (uint8_t)(held - cost), (uint8_t)recv_m);
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
            const int* fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t below = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(below)) {
                continue;
            }
            const reaction_t* br = reaction_of(below);
            if (br->soaks == 0) {
                continue;
            }
            /* !cell_is_burning(below): a lit fuse reads moisture 0 (its
             * own lit-code carve-out) and would otherwise look like open
             * room to percolate into, dousing it - `below` is already
             * loaded, so this costs nothing a failing room check would
             * not have. */
            if (!cell_is_burning(below) &&
                (br->soaks_to != 0 || (br->dries != 0 && moisture_of(below, br) < br->moist_max))) {
                open[n_open++] = i;
            }
        }
        if (n_open != 0 && (int)(rng_next(&s->rng) & 0xFF) < SOIL_PERCOLATE_CHANCE) {
            const int pick = open[rng_below(&s->rng, n_open)];
            const int* fd = ring_dir(down + (pick == 0 ? 0 : pick == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t below = s->cells[nat];
            const reaction_t* br = reaction_of(below);

            /* Half, rounded up, so a cell holding 1 still moves it - a
             * finger that rounds down to nothing stops one level short of
             * the bottom every time. */
            int give = (held + 1) / 2;
            int cost = give;
            int recv_m;
            if (br->soaks_to != 0) {
                /* Converting a grain into soil costs a level due to water
                 * usage. Rounding down prevents dry cells, keeping soil wet.
                 * Soil-to-soil transfers retain rounding. */
                give = held / 2;
                if (give == 0) {
                    return true; /* too little to bind a grain */
                }
                cost = give;
                recv_m = give;
                /* Arrives WET, so no tone of its own - the soaking
                 * branch above's own comment covers why. */
                s->cells[nat] = soil_cell(CELL_MAKE(br->soaks_to, 0), 0,
                                          (uint8_t)give, &reactions[br->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else {
                /* `below` already passed the open[] gate above, which
                 * rejects a lit cell before it is ever a candidate - so the
                 * write here can never land on one; see that gate's own
                 * comment for why it matters at all. */
                const int room = (int)br->moist_max - moisture_of(below, br);
                if (give > room) {
                    give = room;
                }
                recv_m = moisture_of(below, br) + give;
                s->cells[nat] = with_moisture(below, (uint8_t)recv_m, br);
                cost = give;
            }
            /* Same imprint rule as the diffusion hand-off above: a donor
             * this empties dries biased by the cell it just fed. */
            row[x] = soil_set_moisture(c, (uint8_t)(held - cost), (uint8_t)recv_m);
            mark_rows(s, y, y);
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            return true;
        }
    }

    if (r->dries != 0 && held != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->dries) {
        /* AMBIENT DRYING: No neighbours to bias from, so cells with no wet
         * neighbours go bone pale, which is correct. */
        row[x] = soil_set_moisture(c, (uint8_t)(held - 1), 0);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }

    /* r->dries gates the wetness half, preventing false "still wet" reports
     * for grains with sand, which reads as moisture that was never there.
     * Without this, may_have_moisture could latch on indefinitely. */
    return (r->dries != 0 && held != 0) || beside_liquid;
}

/* One cell of hot GAS warms nearby. Gated by may_have_heat_holder, not
 * may_have_temperature (smoke and steam may have_temperature).
 * may_have_heat_holder checks for heat_ramp on grid. If false, skips cells
 * with heat_ramp == 0 before drawing random numbers or mutating. */
static void
step_one_warming_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
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
        const reaction_t* nr = reaction_of(n);

        /* Something that BANKS heat climbs one level. */
        if (nr->heat_ramp != 0) {
            const uint8_t t = CELL_VARIANT(n);
            if (t + 1 >= MATERIAL_VARIANTS) {
                continue; /* melting is the ramp's job, not convection's */
            }
            if ((int)(rng_next(&s->rng) & 0xFF) >= r->warms) {
                continue;
            }
            s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(t + 1));
            s->may_have_temperature = true;
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
            continue;
        }

        /* And something that CANNOT bank it melts outright.
         *
         * This branch was missing, and the gap had a shape worth naming:
         * convection only ever knew how to warm a material whose variant
         * is a temperature - glass and stone. Ice and snow have no such
         * variant to climb. Ice structurally cannot: it is an extended
         * material, so its low nibble is which material it IS, with no
         * room left to hold a temperature at all.
         *
         * So steam walked straight past ice, and a boiler under a sheet of
         * it did nothing - which is what it looked like on the device, and
         * is wrong for the only reason that matters: steam is water at a
         * hundred degrees and ice is water at zero.
         *
         * try_heat_transform() has had this second, memoryless branch all
         * along for heat arriving by contact or through a wall. This is
         * the same transform, reached by the third route.
         *
         * Only for something COLD, though - `chills`, which in this
         * table is what being cold means. Not for anything with a
         * heats_to at all, which was the first shape of this and is far
         * too wide: sand's heats_to is glass, so a smoke cloud drifting
         * over a dune would slowly vitrify it. Warm air thaws; it does
         * not fire a kiln. Ice and snow are the whole of what this is
         * for, and both of them chill.
         *
         * BOTH gates, deliberately. The gas has to be willing (`warms`)
         * and the target has to be meltable at its own rate
         * (`heat_chance`), so convection melts more slowly than a flame
         * touching the same cell would. That is not only physical, it is
         * a debt to the thermal-shock tuning: warmer air already costs
         * snow its life, and snow is the scarce half of "chill it, then
         * heat it". One gate would have made every boiler on the board a
         * snow-eater. */
        if (nr->chills == 0 || nr->heats_to == 0 || nr->heat_chance == 0) {
            continue;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->warms) {
            continue;
        }
        /* At a quarter rate, convection is weakest, weaker than flame or
         * conduction. Snow loses half in 98 steps at quarter rate (46 at
         * full) and 78 steps under steam (56). Snow crucial for thermal
         * shock: a 1.5-second smoke cloud clearing snow indicates an
         * accidental boiler. A sheet of ice over a boiler halves in 120 steps
         * at full rate, 130 at quarter, limited by steam reach. Thus, quarter
         * rate minimally affects the case but doubles snow life. */
        if ((int)(rng_next(&s->rng) & 0xFF) >= (nr->heat_chance >> 2)) {
            continue;
        }
        place_reacted(s, nx, ny, nat, (material_id_t)nr->heats_to);
    }
}

/* FALLS in cold pass, cannot fall in sweep. Moves gravity-ward into empty
 * space. Returns whether it still has space to move. Determines if (ax, ay)
 * is more `self` or hardened form. */
static inline bool
is_kin(cell_t a, cell_t self, const reaction_t* r) {
    return a == self || (r->clings_to != 0 && CELL_MATERIAL(a) == r->clings_to);
}

/* How much of a connected body it walks before giving up, more than a tree
 * from leaf to roots, short enough for cheap search on a board covered in
 * growth. Larger bodies shed outer cells, a bounded way to be wrong. */
#define SUPPORT_MAX 48

/* Checks if (x, y) is part of a resting body. Four attempts: neighbours only
 * fails. Cell above included causes circular references. Exclude asker lets
 * chains dodge. Being held up concerns whole body. Walk connected parts, seek
 * gravity-ward non-kin. Only gravity-ward directions count. */
static bool
anchored(sand_t* s, int x, int y, int w, int h, cell_t self, const reaction_t* r) {
    uint16_t body[SUPPORT_MAX];
    int n = 0, head = 0;

    body[n++] = (uint16_t)((size_t)y * (size_t)w + (size_t)x);

    const int down = ring_of(s->last_load_dx, s->last_load_dy);

    while (head < n) {
        const int at = (int)body[head++];
        const int cx = at % w, cy = at / w;

        for (int d = 0; d < 8; d++) {
            const int* nd = ring_dir(down + d);
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
                /* Ring offset 0 from `down` is straight down and counts.
                 * Diagonals below appear supportive but are not; in a
                 * one-cell-wide shaft, the wall is diagonally beneath every
                 * cell, causing a seed dropped in to stick at the pour height
                 * instead of falling down. */
                if (d == 0) {
                    return true; /* this body is resting on something */
                }
                continue;
            }
            if (n >= SUPPORT_MAX) {
                continue; /* too big to finish; treat as loose */
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

static bool
step_one_falling_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const int nx = x + s->last_load_dx;
    const int ny = y + s->last_load_dy;
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    if (!CELL_IS_EMPTY(s->cells[nat])) {
        return false; /* landed */
    }
    /* ATTACHED things do not fall - see anchored(), which is where the
     * whole of that idea lives and where three wrong versions of it are
     * recorded. A seed painted in mid-air is attached to nothing and
     * drops; a limb is part of a tree and does not. */
    if (anchored(s, x, y, w, h, s->cells[at], r)) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->falls) {
        return true; /* still falling, just not now */
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
#define GROW_REACH  48

/* How much of the top of a hardened run gets foliage hung round it. Three
 * is a crown rather than a tuft, and small enough that the whole shaping
 * pass stays a handful of writes. */
#define CANOPY_SPAN 3

/* What a bud costs the soil, in moisture levels. See
 * step_one_budding_cell() - a bud compounds, so water is what bounds it. */
#define BUD_COST    3

/* How far down through soil a plant's roots reach for water. Deep enough
 * to survive a bed draining under it, short enough that a tree cannot
 * drink from the far side of the board. */
#define ROOT_REACH  6

/* Plant stems limit water lift, bounding tree height, branch length, and limb
 * spread. Cells grow based on water flow, not age or budget. Shape and cell
 * position follow water paths. Without this limit, growth is dictated solely
 * by moisture, potentially leading to uncontrolled expansion. */
#define TREE_LIFT   10


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
/* `contact_at` is the grid index of the FIRST soil cell the stem walk
 * reaches - the collar, where the tree actually stands - or -1 if the
 * walk never gets there. `root_depth` is how many cells of root (see
 * reaction_t.roots) the stem walk passed through on the way down.
 *
 * Both exist for PART 1 of the roots feature (spend_soil_moisture()
 * below): the collar is where the FIRST root has to form, and
 * `root_depth == 0` is how spend_soil_moisture() tells "this collar is
 * still bare" from "a root system already grows here, PART 2's job now"
 * - see that function's own comment. Neither is `lift` - a root cell is
 * below the water line and must not cost the tree any of TREE_LIFT, which
 * is why it gets its own counter rather than folding into the existing
 * one. */
static int
find_water(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self, int* lift, int* contact_at,
           int* root_depth, bool wants_room) {
    const int dx = s->last_load_dx, dy = s->last_load_dy;
    const int down = ring_of(dx, dy);

    *contact_at = -1;

    int cx = x, cy = y;
    int lift_count = 0;
    int roots_passed = 0;
    for (int step = 0; step < GROW_REACH; step++) {
        /* `step` counts all cell types; `lift_count` tracks STEM transitions
         * only. Reusing `step` for `*lift` fails as `step` advances per
         * iteration, counting roots incorrectly. */
        *lift = lift_count;
        *root_depth = roots_passed;
        int nx = -1, ny = -1;
        bool on_soil = false;
        bool via_root = false;
        /* Whether pre-check COMMITTED to root. Not same as `nx >= 0`: scan
         * sets nx early as FALLBACK, continues searching for soil. Testing nx
         * stops scan on fallback, missing ground beside root. Mistake cost
         * feature's regression test. See lookahead note for details. */
        bool took_root = false;

        /* OUR OWN ROOT, STRAIGHT DOWN, BEFORE THE SCAN BELOW RUNS AT ALL.
         *
         * The ordering IS the mechanism, and getting it wrong is what made
         * roots a single row rather than a system. The scan takes the
         * first SOIL it finds and breaks, keeping stem only as a fallback
         * - so the moment a collar cell rooted, the walk found the loose
         * dirt sitting DIAGONALLY beside that root before it ever
         * considered going THROUGH it. The contact therefore stayed on the
         * surface row for ever, each conversion landing beside the last
         * instead of beneath it. Measured on a watered bed at 20,000
         * steps: three roots, every one of them at depth 0, spreading
         * sideways. Reported from the device as roots never growing past
         * the attachment point.
         *
         * The scan DOES stumble down eventually without this - once a
         * whole surface row has rooted there is no dirt left beside it to
         * find, so the fallback crosses a root and the contact drops a
         * row. What it does not do is get there RELIABLY. Measured over
         * six seeds at 20,000 steps, deepest root reached: 3/3/3/3/3/3
         * with this check against 3/2/3/0/2/3 without, the zero being a
         * tree that put out two roots and never left the surface. The
         * trees come out bigger too (wood 86-159 against 18-102), which
         * is the same cause seen from the other end: a walk that goes
         * through its roots reaches the wetter soil underneath them.
         *
         * A tree stands ON its roots, and the ground that feeds it is
         * what lies BELOW them, so the root has to be crossed before the
         * soil beside it is considered. Straight down only: a root grows
         * into the contact cell, which is always the cell gravity-ward,
         * so that is the one direction a root column can occupy. A root
         * found diagonally is still crossable through the fallback in the
         * scan, for a stem that has wandered off the column. */
        /* ONLY IF THERE IS GROUND UNDER IT, which is the other half and was
         * missing at first. Preferring the root unconditionally commits the
         * walk to it, and a root with nothing below it is a DEAD END: the
         * walk arrives, finds no soil and no stem, and gives up - even
         * though the soil it wanted was sitting diagonally beside the root
         * all along. A bed one row deep on stone is exactly that shape, and
         * it is the shape the whole feature's own regression test uses, so
         * this turned "the tree survives its bed shifting" straight back
         * into a failure. One cell of lookahead is the entire fix: cross a
         * root when soil or more root lies under it, and otherwise leave
         * the scan below to find the ground beside it. */
        if (r->roots_to != 0) {
            const int* fd = ring_dir(down);
            const int tx = cx + fd[0], ty = cy + fd[1];
            if ((unsigned)tx < (unsigned)w && (unsigned)ty < (unsigned)h
                && s->cells[(size_t)ty * (size_t)w + (size_t)tx] == (cell_t)r->roots_to) {
                const int ax = tx + dx, ay = ty + dy;
                if ((unsigned)ax < (unsigned)w && (unsigned)ay < (unsigned)h) {
                    const cell_t under = s->cells[(size_t)ay * (size_t)w + (size_t)ax];
                    if (!CELL_IS_EMPTY(under)
                        && (reaction_of(under)->soil != 0 || under == (cell_t)r->roots_to)) {
                        nx = tx;
                        ny = ty;
                        via_root = true;
                        took_root = true;
                    }
                }
            }
        }

        for (int i = 0; !took_root && i < 3; i++) {
            const int* fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int tx = cx + fd[0], ty = cy + fd[1];
            if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) {
                continue;
            }
            const cell_t c = s->cells[(size_t)ty * (size_t)w + (size_t)tx];
            if (CELL_IS_EMPTY(c)) {
                continue;
            }
            if (reaction_of(c)->soil != 0) {
                nx = tx;
                ny = ty;
                on_soil = true;
                break; /* ground: stop looking for stem */
            }
            if (nx < 0 && (c == self || (r->clings_to != 0 && CELL_MATERIAL(c) == r->clings_to))) {
                nx = tx;
                ny = ty; /* more stem, keep it as a fallback */
            } else if (nx < 0 && r->roots_to != 0 && c == (cell_t)r->roots_to) {
                /* A ROOT counts as stem too. Without this, the very bug
                 * roots exist to fix - a stem finding neither stem nor
                 * ground below it once the ground it stood on has moved -
                 * would come straight back the moment a root actually
                 * grew there. */
                nx = tx;
                ny = ty;
                via_root = true;
            }
        }
        if (nx < 0) {
            return -1; /* neither stem nor ground below */
        }
        if (!on_soil) {
            if (via_root) {
                roots_passed++; /* below the water line - see this
                                  * function's own top comment */
            } else {
                lift_count++;
            }
            cx = nx;
            cy = ny; /* carry on down the stem */
            continue;
        }

        /* Into the soil. This is the collar. */
        cx = nx;
        cy = ny;
        *contact_at = (int)((size_t)cy * (size_t)w + (size_t)cx);
        for (int depth = 0; depth < ROOT_REACH; depth++) {
            if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                return -1;
            }
            const size_t at = (size_t)cy * (size_t)w + (size_t)cx;
            const cell_t c = s->cells[at];
            /* A root is TRANSPARENT to the soil walk. Dirt shifts, so a
             * root can end up with soil piled back on top of it - and
             * without this, a root would cut the tree off from the water
             * below its own root, which is the exact bug this feature
             * exists to fix, reintroduced from the other side. */
            if (r->roots_to != 0 && c == (cell_t)r->roots_to) {
                cx += dx;
                cy += dy;
                continue;
            }
            if (CELL_IS_EMPTY(c) || reaction_of(c)->soil == 0) {
                return -1;
            }
            /* Two callers, opposite errands, one walk: growth is
             * looking for soil with something in it to spend, drinking
             * for soil with room to take more. */
            {
                const reaction_t* cr = reaction_of(c);
                /* !cell_is_burning(c): The `soil == 0` check ensures a lit
                 * fuse is excluded. This rule states a lit cell never has
                 * room or water. */
                if (!cell_is_burning(c) &&
                    (wants_room ? moisture_of(c, cr) < cr->moist_max
                                : moisture_of(c, cr) != 0)) {
                    return (int)at;
                }
            }
            cx += dx;
            cy += dy;
        }
        return -1;
    }
    return -1;
}

/* Every grower that spends a cell of soil moisture pays through here -
 * growing, budding and sprouting alike - see PART 1 of the roots
 * feature. It spends the moisture exactly as each site used to do
 * inline, then - separately, and only once the spend itself has already
 * happened - rolls whether the CONTACT cell (the collar, where the stem
 * actually touches ground) welds into the FIRST root of a new system.
 *
 * ONLY THE FIRST. `root_depth == 0` means find_water()'s stem walk
 * crossed no root at all on the way down - a bare collar - which is
 * exactly the case this exists for: a tree resting on top of its bed
 * rather than embedded in it, one shift of the soil away from being
 * stranded (see reaction_t.roots's own comment in material.h for the
 * bug this is the fix for). The moment that first root exists, PART 2 -
 * step_one_rooting_cell(), below, gated on the cell ITSELF being a root
 * rather than on anything spending moisture nearby - takes over growing
 * the system outward and downward on its own, cell eating cell, the way
 * an actual root network spreads. Gating this path shut once root_depth
 * is nonzero is what keeps the two from fighting over the same collar:
 * without it, every grow/bud/sprout event for the rest of the tree's
 * life keeps re-rolling here too, seeding fresh disconnected root cells
 * at whatever the CURRENT deepest contact happens to be, on top of a
 * system PART 2 is already growing outward from the first one.
 *
 * `soil_at` is never the cell that gets converted - it can be several
 * rows down the ROOT_REACH walk in find_water(), and a root planted
 * there would be a disconnected woody speck in the middle of the bed,
 * anchoring nothing. `contact_at` is -1 for a caller that never reaches
 * soil at all (step_one_drinking_cell()'s find_water() call, which
 * passes a scratch int instead of caring); this function is simply not
 * called in that case, since drinking never spends soil moisture. */
static void
spend_soil_moisture(sand_t* s, int w, const reaction_t* r, int soil_at, uint8_t amount, int contact_at,
                     int root_depth) {
    const cell_t soil = s->cells[soil_at];
    /* Drunk by growth, no neighbour to bias tone. Collar goes pale like
     * ambient drying. Uses soil's codec for moisture. */
    s->cells[soil_at] = soil_set_moisture(
        soil, (uint8_t)(moisture_of(soil, reaction_of(soil)) - amount), 0);
    mark_rows(s, soil_at / w, soil_at / w);

    if (r->roots == 0 || contact_at < 0 || root_depth != 0) {
        return;
    }
    /* Roll AFTER every other gate, the same discipline this whole file
     * uses everywhere else - drawing a random number for a root that
     * cannot happen shifts every decision downstream of it. */
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return;
    }
    place_reacted(s, contact_at % w, contact_at / w, (size_t)contact_at, r->roots_to);
}

/* Max neighbours a root can read before stopping growth; 2 ensures
 * filamentary shape, not slab. Measured, not guessed
 * (docs/Sand/Sand-Simulation.md). At 1, system starved to 4 cells; at 3,
 * never stopped growing. At 2, reached 40 cells by step 2000, stabilised
 * thereafter. */
#define ROOT_SURFACE_MAX 2

/* Two skews in step_one_rooting_cell() adjust for moisture. DOWN and AWAY
 * weights set to 2. Trunk counts as parent. MEASURED: mean deepest root 7.4
 * rows, mean half-width 7.9. Roots stop at dry soil, not depth. Weights guide
 * moist cell selection. Six-seed test showed RNG scatter; 30 seeds averaged. */
#define ROOT_WEIGHT_AWAY 2
#define ROOT_WEIGHT_DOWN 2

/* How readily a root CONDUCTS water downward - step_one_conducting_cell()
 * below. Chance in 256 per root cell per step of moving one level of moisture
 * from the wettest soil beside or above it into the driest soil beneath it.
 * See the function for why this exists at all. Roots are few enough that the
 * per-cell cost of a generous figure is nothing. MEASURED on the dry bed
 * watered at the collar only (the device case), ten seeds, 20,000 steps, mean
 * deepest root of 19 rows, against the percolation rate - because the obvious
 * alternative was to undo the SOIL_PERCOLATE_CHANCE slowdown instead:
 * SOIL_PERCOLATE_CHANCE conduit 0 conduit 64 15 (current) 4.6 15.0 30 7.0
 * 18.0 60 (old value) 5.3 15.1 Conduction is worth about three times what
 * percolation is: even the old, fast percolation reached 5-7 rows without it.
 * And the runaway scene - collar re-saturated every step - still pins, 93
 * roots at step 2000 and 93 at 20,000: ROOT_SURFACE_MAX bounds the SHAPE
 * whatever amount of water is carried into it, so carrying more does not undo
 * the bound. 64 is the first value tried and it was decisive; it wants eyes
 * on the panel for feel, not more measurement for depth. */
#define ROOT_CONDUCT_CHANCE 64

/* One cell of ROOT, carrying water DOWN through itself - a conduit.
 *
 * Why a root system needs this at all: depth is bounded by water, not by
 * heading. A root can only eat moist soil, a bed watered from the top
 * dries from the top, and measured over thirty seeds the moist front the
 * fingers chase was gone before they reached the floor - every saturated
 * bed 98-99% dry after 20,000 steps, not one live tip beside a moist cell,
 * and on a dry bed watered at the collar alone a mean deepest root of 5.7
 * rows of 19. Steeper direction weights narrowed the system and did not
 * deepen it (ROOT_WEIGHT_DOWN's own comment). So the water has to be
 * brought to where the roots want to go, and that is what real roots do:
 * they carry it.
 *
 * The rule: take one level from the WETTEST soil among the five
 * neighbours beside or above this root, put it into the DRIEST soil among
 * the three beneath it. Moves only, never makes - the same conservation
 * step_one_soaking_cell()'s percolation keeps - and only ever one way,
 * gravity-ward, so it cannot ping-pong against diffusion. The sink is the
 * next cell a tip wants to eat, and a fresh tip is itself a conduit, so
 * the moisture front and the root front move down together: depth is
 * EARNED, a level of water at a time, rather than handed out by a weight.
 *
 * Sides count as sources, not only "above". A column of root has more
 * root above each cell, not soil - only its top cell would ever conduct
 * if the source had to be overhead. Drawing from the wet soil flanking
 * each cell is what lets a whole column drain the surface layer downward.
 *
 * Every gate before the roll, the file's rule: with no source or no sink
 * this touches neither the grid nor the RNG. */
static bool
step_one_conducting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const int down = ring_of(s->last_load_dx, s->last_load_dy);
    (void)r;

    int src_at = -1, src_x = 0, src_y = 0, src_m = 0;
    int dst_at = -1, dst_x = 0, dst_y = 0, dst_m = 0;

    for (int k = 0; k < 8; k++) {
        const int* nd = ring_dir(down + k);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t c = s->cells[nat];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const reaction_t* cr = reaction_of(c);
        if (cr->soil == 0) {
            continue; /* not soil: root, wood, stone, air - and, since D1,
                        * gunpowder: a fuse is not ground a root conducts
                        * water through. */
        }
        const int m = moisture_of(c, cr);
        if (k == 0 || k == 1 || k == 7) {
            /* Gravity-ward: a sink, if it has room - its own
             * reaction_of(c)->moist_max, not dirt's SOIL_MOISTURE_MAX.
             * Prevents narrower-codec soil (like gunpowder) from exceeding
             * moist_max. !cell_is_burning(c) to avoid lit cells reading
             * moisture 0 and appearing as sinks. */
            if (m < cr->moist_max && !cell_is_burning(c) && (dst_at < 0 || m < dst_m)) {
                dst_m = m;
                dst_at = (int)nat;
                dst_x = nx;
                dst_y = ny;
            }
        } else if (m > src_m) {
            /* Beside or above: a source, if it holds anything. */
            src_m = m;
            src_at = (int)nat;
            src_x = nx;
            src_y = ny;
        }
    }
    if (src_at < 0 || dst_at < 0) {
        return false; /* nothing to carry, or nowhere to carry it */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= ROOT_CONDUCT_CHANCE) {
        return true;
    }
    const cell_t src = s->cells[src_at], dst = s->cells[dst_at];
    /* Same imprint rule as every other hand-off in this file: a source
     * this drains dry is biased by the sink it just carried water into,
     * post-transfer - see soil_dry_out()'s own comment. */
    s->cells[src_at] = soil_set_moisture(src, (uint8_t)(src_m - 1), (uint8_t)(dst_m + 1));
    s->cells[dst_at] = with_moisture(dst, (uint8_t)(dst_m + 1), reaction_of(dst));
    mark_rows(s, src_y, src_y);
    mark_rows(s, dst_y, dst_y);
    wake_block_and_neighbors(s, src_x, src_y);
    wake_block_and_neighbors(s, dst_x, dst_y);
    return true;
}

/* One cell of ROOT, eating into the moist soil it touches - PART 2 of the
 * roots feature (docs/Sand/Sand-Simulation.md), and the whole growth
 * rule: find a neighbour that is soil (reaction_of(c)->soil != 0) and
 * still holds moisture, roll a small chance, and convert it - which
 * consumes the moisture as the price of the conversion, the same "spend
 * the scarce thing" discipline growth, budding and sprouting already
 * rest on (reaction_t.roots's own comment, material.h).
 *
 * Moisture itself already has most of the shape - it percolates down
 * through a bed (step_one_soaking_cell()) and diffuses out from anything
 * drinking or pouring nearby - so a root reaching for whichever neighbour
 * still has water in it spreads wide near a wet surface and fingers
 * downward through a bed drying from the top. The first cut had no
 * direction weights at all on exactly that reasoning, and it was mostly
 * right: it was replaced by a SMALL skew, not a walk (see the pick below
 * for what the skew is and the measurement behind it), because near a wet
 * surface the sideways spread won so completely that depth only happened
 * at the angles the geometry favoured. An earlier draft before that walked
 * a stem-shaped run outward from the tree's collar instead, reusing
 * step_one_growing_cell()'s own site-roll-plus-fan-walk machinery; it was
 * dropped because a root does not need a stem's machinery to look like a
 * root, and because this rests on the same scarce-resource bound the rest
 * of the feature already uses rather than adding a second kind of bound
 * beside it.
 *
 * Bounded three ways, in the order that was actually measured to matter
 * - see the Roots section of docs/Sand/Sand-Simulation.md for the
 * numbers behind each:
 *   1. THE MOISTURE ITSELF. A cell with nothing to spend has nothing to
 *      grow into, and the total on a board is finite unless something
 *      keeps pouring more in - the same bound budding rests on.
 *   2. ROOT_SURFACE_MAX, above - a root already thick with root
 *      neighbours does not roll at all, which is what keeps the system
 *      a filigree rather than a block.
 *   3. `r->roots` itself, on MATX_ROOT's own row (material.c) - a small
 *      chance, the same discipline every other roll in this file uses. */
static bool
step_one_rooting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    /* Cheapest question first, and it rejects most already-thick columns
     * without the neighbour scan below ever touching the RNG. */
    int root_neighbors = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        if (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == (cell_t)r->roots_to) {
            root_neighbors++;
        }
    }
    if (root_neighbors > ROOT_SURFACE_MAX) {
        return false; /* buried inside its own kind; nothing to do here */
    }

    /* ALL EIGHT, not the four cardinals - compared directly, both ways,
     * against the same six seeds (docs/Sand/Sand-Simulation.md's Roots
     * section): four gave a near-straight taproot, one or two cells wide,
     * that only fanned out where moisture happened to pool against the
     * stone floor; eight let a root step diagonally as it reaches for
     * water, which is what actually produces the wandering, forking
     * shape a root system is supposed to have - the difference is
     * qualitative, not a rounding error, and it is the reason this rule
     * bothers with ring_dir() at all instead of the plainer reaction_dirs
     * four-neighbour scan sprouting and drinking already use.
     *
     * A WEIGHTED PICK, not the first match. The first cut took whichever
     * moist neighbour the fixed ring order reached first, which is a
     * uniform pick in disguise - and uniform is isotropic, so the shape
     * of the moisture decided everything. Near a wet surface moisture
     * spreads sideways, so the system spread sideways, and reported from
     * the device as roots preferring the sides so strongly that deeper
     * roots only happened at the few angles the geometry favoured.
     *
     * Two biases, both read off the grid with no state:
     *   AWAY FROM ITS PARENT - the sum of the directions from this cell's
     *     own root neighbours to it, `away`. A tip has one parent, so the
     *     vector is strong and the tip keeps going the way it was going,
     *     the same thing holds_line does for a stem without ever
     *     remembering anything. A junction has several, they partly
     *     cancel, and it is free to go any way - which is right for a
     *     junction. A lone seed has none and is left to gravity.
     *   GRAVITY-WARD - what actually buys depth: a root reaching for the
     *     water percolating down beneath it rather than the water
     *     diffusing beside it.
     * THE TRUNK COUNTS AS A PARENT TOO. The first root a tree puts down
     * has no root neighbours at all, so its away-vector was zero and it
     * was left to the gravity term alone - and it is the one root whose
     * heading matters most, since everything else grows from it. The
     * wood standing directly on top of it is what it grew from, so it
     * enters the away-vector exactly as a parent root does (`clings_to`,
     * the material the row says a root is part of), and the collar's
     * first move is down and out from under the trunk rather than a
     * coin-toss along the wet surface. Reported as roots still not going
     * deep enough with the parent skew alone.
     *
     * Two draws, both after every gate: the `roots` roll, then the pick -
     * the second is only ever drawn once the first has said a conversion
     * happens, so a root with nothing moist beside it still touches the
     * RNG exactly as often as before, which is not at all. */
    int away_x = 0, away_y = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t c = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (c == (cell_t)r->roots_to || (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == r->clings_to)) {
            away_x -= nd[0];
            away_y -= nd[1];
        }
    }
    const int gx = s->last_load_dx, gy = s->last_load_dy;

    int cand_at[8], cand_x[8], cand_y[8], cand_w[8];
    int n_cand = 0, total_w = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n) || reaction_of(n)->soil == 0 ||
            moisture_of(n, reaction_of(n)) == 0) {
            continue;
        }
        int wgt = 1;
        if (nd[0] * away_x + nd[1] * away_y > 0) {
            wgt += ROOT_WEIGHT_AWAY; /* carries on away from its parent */
        }
        if (nd[0] * gx + nd[1] * gy > 0) {
            wgt += ROOT_WEIGHT_DOWN; /* reaches down */
        }
        cand_at[n_cand] = (int)nat;
        cand_x[n_cand] = nx;
        cand_y[n_cand] = ny;
        cand_w[n_cand] = wgt;
        total_w += wgt;
        n_cand++;
    }
    if (n_cand == 0) {
        return false; /* nothing moist beside it right now */
    }
    /* Everything found before the roll - the same discipline every site
     * in this file uses, drawing a random number only once everything
     * else has already said this cell has somewhere to spend it. */
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return true; /* a candidate exists; just not this roll */
    }
    int pick = rng_below(&s->rng, total_w);
    int k = 0;
    while (pick >= cand_w[k]) {
        pick -= cand_w[k];
        k++;
    }
    const int eat_at = cand_at[k], ex = cand_x[k], ey = cand_y[k];

    /* Conversion replaces cell with new root, losing moisture nibble;
     * spend_soil_moisture debits moisture in-place on existing dirt cell. */
    place_reacted(s, ex, ey, (size_t)eat_at, r->roots_to);
    return true;
}

/* DRINKS cell: touches liquid, rooted in soil, adds moisture. `KIND_STATIC`
 * at stone's density; water cannot fall through foliage. A bowl of leaves
 * held a pond. Water goes down stem and into roots. Returns if worth
 * revisiting. */
static bool
step_one_drinking_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self) {
    int lx = -1, ly = -1;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_LIQUID && reaction_of(n)->wets != 0) {
            lx = nx;
            ly = ny;
            break;
        }
    }
    if (lx < 0) {
        return false; /* nothing to drink */
    }

    int lift = 0, contact_at = -1, root_depth = 0; /* drinking never spends
                                                     * soil moisture, so
                                                     * nothing here roots -
                                                     * scratch values */
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, true);
    if (soil_at < 0) {
        return true; /* thirsty, but nowhere to put it */
    }
    /* Both ends found before the roll, or every leaf on the board draws a
     * random number every step and shifts everything downstream of it. */
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->drinks) {
        return true;
    }

    pay_quench_cost(s, lx, ly, w);

    /* `soil_at` already passed find_water()'s own soil/cell_is_burning
     * gates, so this can never land on a lit fuse - see that function's
     * own comment on the wants_room branch for why it matters at all. */
    const cell_t soil = s->cells[soil_at];
    const reaction_t* sr = reaction_of(soil);
    s->cells[soil_at] = with_moisture(soil, (uint8_t)(moisture_of(soil, sr) + 1), sr);
    mark_rows(s, soil_at / w, soil_at / w);
    wake_block_and_neighbors(s, soil_at % w, soil_at / w);
    return true;
}

/* SPROUTS: In wet soil, buds a cell of `sprouts_to` into an empty space,
 * consuming soil moisture. Tiny, closing the feature loop. Growth hardens
 * plant into wood, consuming growth cells, ending full-height trees and
 * leaving bare posts. Trunks remain alive. */
static bool
step_one_sprouting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
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
        if (soil_at < 0 && reaction_of(n)->soil != 0 &&
            moisture_of(n, reaction_of(n)) != 0) {
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

    /* SPENDS, BUT NEVER SEEDS A ROOT - contact_at deliberately -1. Direct
     * four-neighbour scan, no stem walk, no depth. Passing 0 would
     * incorrectly report bare collar on sprouts, allowing disconnected root
     * cells. Rooting handled by growth and budding. Sprouting pays for leaf. */
    spend_soil_moisture(s, w, r, soil_at, 1, -1, 0);
    return true;
}

/* A tree cell BUDS, rooted near water, producing a `buds_to` - new growth.
 * Unlike tips that create a POPULATION, buds cause EVENTS. This means a
 * settled tree has no plant cells and costs nothing. Buds grow away from
 * gravity, increasing height and spread, not into the trunk. */
static bool
step_one_budding_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    /* In leaf? Cheapest question, and much the commonest answer, so it
     * goes first: bare wood is most of a trunk and pays only this. */
    bool crowned = false;
    for (int d = 0; d < 8 && !crowned; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        crowned = (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == (cell_t)r->sprouts_to);
    }
    if (!crowned) {
        return false;
    }
    /* And at the HEAD of its trunk - nothing more of itself directly
     * against gravity. A canopy touches a dozen cells of wood; without
     * this every one of them is a bud site and the rate scales with the
     * tree all over again. */
    {
        const int ax = x - s->last_load_dx, ay = y - s->last_load_dy;
        if ((unsigned)ax < (unsigned)w && (unsigned)ay < (unsigned)h
            && s->cells[(size_t)ay * (size_t)w + (size_t)ax] == self) {
            return false;
        }
    }

    /* Somewhere to put it, up and away from gravity. */
    const int up_i = ring_of(-s->last_load_dx, -s->last_load_dy);
    static const int out[5] = {7, 0, 1, 2, 6};
    int at = -1, bx = 0, by = 0;
    for (int d = 0; d < 5; d++) {
        const int* nd = ring_dir(up_i + out[d]);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        if (CELL_IS_EMPTY(s->cells[nat])) {
            at = (int)nat;
            bx = nx;
            by = ny;
            break;
        }
    }
    if (at < 0) {
        return true; /* crowned, but boxed in */
    }

    int lift = 0, contact_at = -1, root_depth = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false);
    if (soil_at < 0) {
        return true; /* nothing to drink */
    }
    /* Everything settled before roll, or cells draw random numbers, shifting
     * what follows. A limb's worth of water. Buds compound, growing,
     * hardening, crowning, making new bud sites. Bounds are scarce, not
     * probability. Buds drank pour, forest ran away. */
    const cell_t soil = s->cells[soil_at];
    if (moisture_of(soil, reaction_of(soil)) < BUD_COST) {
        return true;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->buds) {
        return true;
    }

    place_reacted(s, bx, by, (size_t)at, r->buds_to);

    spend_soil_moisture(s, w, r, soil_at, BUD_COST, contact_at, root_depth);
    return true;
}

/* One step along a stem in direction (ux, uy) or a nearby diagonal. Returns
 * if found. Stems grow along DITHERED gravity, alternating steps between two
 * eighths of a tilt, preventing rigid angles and making the trunk wander.
 * Every walk over a plant must tolerate this. */
static bool
stem_next(sand_t* s, int x, int y, int ux, int uy, int w, int h, cell_t self, int* ox, int* oy) {
    const int up = ring_of(ux, uy);
    for (int i = 0; i < 3; i++) {
        const int* d = ring_dir(up + (i == 0 ? 0 : i == 1 ? 1 : 7));
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

/* Make room at (gx, gy) by shoving occupied cells along (dx, dy). Seeds need
 * space to grow. Shift shoot until empty cell found. Stops at STATIC cells.
 * Returns if (gx, gy) is free. */
static bool
shove_aside(sand_t* s, int gx, int gy, int dx, int dy, int w, int h) {
    int ex = gx, ey = gy;
    int run = 0;

    while (run < PUSH_REACH) {
        if ((unsigned)ex >= (unsigned)w || (unsigned)ey >= (unsigned)h) {
            return false; /* shoved into the wall */
        }
        const cell_t c = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        if (CELL_IS_EMPTY(c)) {
            break; /* somewhere to put it all */
        }
        if (material_of(c)->kind == KIND_STATIC) {
            return false; /* will not budge */
        }
        ex += dx;
        ey += dy;
        run++;
    }
    if (run == 0) {
        return true; /* was empty to begin with */
    }
    if (run >= PUSH_REACH) {
        return false; /* too much of it to lift */
    }

    /* Back to front, so nothing is overwritten before it has moved. */
    for (int i = 0; i < run; i++) {
        const int tx = ex, ty = ey;
        ex -= dx;
        ey -= dy;
        s->cells[(size_t)ty * (size_t)w + (size_t)tx] = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        mark_rows(s, ty, ty);
        wake_block_and_neighbors(s, tx, ty);
    }
    s->cells[(size_t)gy * (size_t)w + (size_t)gx] = SAND_EMPTY;
    mark_rows(s, gy, gy);
    wake_block_and_neighbors(s, gx, gy);
    return true;
}

/* Cells WITHER if they cannot drink or lean on a trunk. Growth creates cells;
 * fire and acid remove them. Touching wood is checked first (8 cell reads) as
 * it's more common than finding water. Dried soil keeps all leaves. */
static bool
step_one_withering_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const cell_t self = s->cells[at];

    if (r->sheltered_by != 0) {
        for (int d = 0; d < 8; d++) {
            const int* nd = ring_dir(d);
            const int nx = x + nd[0], ny = y + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (!CELL_IS_EMPTY(n) && CELL_MATERIAL(n) == r->sheltered_by) {
                return false; /* under its tree; it stays */
            }
            /* More ROOT counts as shelter; `roots_to` matches `find_water()`.
             * Roots touch wood only at the top cell; otherwise, the column
             * rots from the bottom up when soil dries. */
            if (r->roots_to != 0 && n == (cell_t)r->roots_to) {
                return false;
            }
        }
    }

    /* Is it ON a tree? Only asked where the answer can change what
     * withering does - see the lignifying branch at the bottom - so a
     * leaf, which has no hardens_to, never pays for these eight reads. */
    bool attached = false;
    if (r->hardens_to != 0 && r->clings_to != 0) {
        for (int d = 0; d < 8; d++) {
            const int* nd = ring_dir(d);
            const int nx = x + nd[0], ny = y + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (!CELL_IS_EMPTY(n) && CELL_MATERIAL(n) == r->clings_to) {
                attached = true;
                break;
            }
        }
    }

    int lift = 0, contact_at = -1, root_depth = 0; /* just a reachability
                                                     * check - nothing here
                                                     * spends, so nothing
                                                     * roots */
    if (find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false) >= 0) {
        return false; /* it can still drink */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->withers) {
        return false;
    }

    /* A shoot on a tree that stops being fed lignifies, turning green to
     * woody. Without touching the trunk, it becomes a permanent woody speck.
     * Withering exists to prevent this. CELL_MAKE(..., 0) is used instead of
     * place_reacted() for wood, where nibble represents burn progress. */
    REACTION_DOC(hardens_to, "if withering but still touching its own hardened trunk");
    if (attached) {
        place_cell(s, x, y, at, CELL_MAKE(r->hardens_to, 0));
        return true;
    }

    s->cells[at] = SAND_EMPTY;
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
    return true;
}

/* A stateless cell that grows from the column's tip against gravity, using
 * moisture from touching soil. The cell at the bottom pays, while the top
 * grows. Without this, growth would stop at one cell. Growth hardens into
 * `hardens_to` along the gravity axis. */
static bool
step_one_growing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    /* Determines up direction using last_load_dx/dy from dithered sweep, not
     * nearest, to prevent rigid stems. */
    const int ux = -s->last_step_dx;
    const int uy = -s->last_step_dy;
    if (ux == 0 && uy == 0) {
        return true; /* free fall: no up to grow towards */
    }

    /* Soil touching THIS cell; ROOTS follow gravity for water. Reach aids
     * survival by accessing deeper moisture. BURIED cells do not grow. Growth
     * increases with size until hardening. Growth belongs at surface; dense
     * areas skip to save scans. */
    int packed = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
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
        return true; /* inside the crowd, not at its edge */
    }

    int lift = 0, contact_at = -1, root_depth = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false);
    if (soil_at < 0) {
        return true; /* nothing to drink */
    }
    if (lift >= TREE_LIFT) {
        return true; /* too high up to be fed */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->grows) {
        return true;
    }

    /* WHERE it grows. Stick straight up from tip. Two rolls make tree. One
     * roll picks SITE: tip or branch. Other picks DIRECTION: tip straight or
     * lean, branch angled. Shape on grid, re-read by walk. */
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
    const int side = rng_below(&s->rng, 2) ? 1 : 7; /* +1 or -1 round */

    int site, dx, dy;
    bool thicken = false;
    const int what = rng_below(&s->rng, 8);
    if (what < 4 || run < 3) {
        site = run - 1; /* HEIGHT: straight on from the tip */
        dx = 0;
        dy = 0; /* along the run - filled in below */
    } else if (what < 6) {
        site = run - 1; /* LEAN: the tip, one step round */
        dx = side;
        dy = 0; /* one step round from the run */
    } else if (what < 7) {
        site = rng_below(&s->rng, run - 1); /* BRANCH: out and up */
        dx = side;
        dy = 0;
    } else {
        /* WIDTH represents trunk thickening by adding columns, simulating how
         * a tree grows wider alongside its height. */
        site = rng_below(&s->rng, (run + 1) / 2);
        dx = side * 2; /* square on to the run */
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

    /* Run direction re-derived from shape, similar to tip walk. Step from
     * below defines limbs. Gravity causes branches to grow one cell then
     * climb, triggering `run < 3` gate, halting growth. Falls back or roll
     * bends branches. `holds_line` at zero restores old behavior. */
    int head = up;
    if (r->holds_line != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->holds_line) {
        /* Three-cell baseline tried, measured no further (67, 73, 70 over
         * eight seeds). Longer baseline caused run to lean and drift,
         * increasing horizontal drift from 24 to 38. Shorter baseline simpler
         * and measured better. */
        int px, py;
        if (stem_next(s, sx, sy, -ux, -uy, w, h, self, &px, &py)) {
            head = ring_of(sx - px, sy - py);
        }
    }
    {
        const int* hd = ring_dir(head + dx);
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
            return true; /* too high up to be thickening */
        }
        int wide = 0;
        for (int i = 1; i < allowed; i++) {
            const int wx = sx + dx * i;
            const int wy = sy + dy * i;
            if ((unsigned)wx >= (unsigned)w || (unsigned)wy >= (unsigned)h) {
                break;
            }
            const cell_t c = s->cells[(size_t)wy * (size_t)w + (size_t)wx];
            if (c != self && !(r->clings_to != 0 && CELL_MATERIAL(c) == r->clings_to)) {
                break;
            }
            wide++;
        }
        if (wide >= allowed - 1) {
            return true; /* thick enough already */
        }
    }

    const int gx = sx + dx, gy = sy + dy;
    if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
        return true;
    }
    /* Only a SHOOT shoves; a branch does not tunnel sideways, nor does a
     * trunk widen. Growth is no longer limited by empty space, allowing seeds
     * to grow 220 cells of timber, mostly underground. Restricted to the tip,
     * growth produces trees that emerge from the ground. */
    const bool shoot = (site == run - 1) && !thicken;

    const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
    if (!CELL_IS_EMPTY(s->cells[gat]) && !(shoot && shove_aside(s, gx, gy, dx, dy, w, h))) {
        return true; /* in the way, and will not move */
    }

    /* Grow, and spend the water. */
    s->cells[gat] = self;
    latch_content_flags(s, self);
    mark_rows(s, gy, gy);
    wake_block_and_neighbors(s, gx, gy);

    spend_soil_moisture(s, w, r, soil_at, 1, contact_at, root_depth);

    /* HARDENING. Counted from the bottom of the column - the cell whose
     * gravity-ward neighbour is not more of the same - so a run is
     * measured once however many of its cells grew this step. */
    if (r->hardens_to == 0 || r->harden_run == 0) {
        return true;
    }
    /* Measure from foot of run up. Previously, growing cell had to be foot,
     * counted once per run. This was true but useless, as any cell that can
     * reach water rolls. A six-cell stem grew for 20,000 steps without
     * re-measuring. */
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
     * docs/Sand/Tuning-At-a-Glance.md.
     *
     * ONE IN FOUR, measured over eight trees. It was one in eight, chosen
     * before the shaping pass existed - back when hardening produced a
     * bare stick and delaying it was all that kept trees from being
     * posts. Now that hardening also lays girth and hangs a crown, a
     * shorter delay is better on every count: green stem halves (33 cells
     * to 23), columns of stem hugging a trunk halve (8 to 4), and there
     * is MORE wood rather than less (195 to 213), because a leader that
     * turns to timber promptly goes on growing from its new tip. One in
     * two is too far - wood falls to 166, since runs harden before they
     * are long and wood does not grow. */
    if (__builtin_expect((int)(rng_next(&s->rng) & 0xFF) >= r->harden_chance, 1)) {
        return true;
    }

    /* SHAPING PASS: Hardening converts run to wood, thickens trunk by index,
     * adds canopy (MATX_LEAF) at top. Last cell remains green. Hardening ends
     * growth. */
    const int up_i = ring_of(ux, uy);

    /* The WHOLE run, tip included. Growth now comes from crowned wood
     * (reaction_t.buds), so the tree can grow entirely into timber while
     * maintaining a future. */
    const int hard = trunk;

    int topx[CANOPY_SPAN], topy[CANOPY_SPAN];
    int ntop = 0;

    cx = fx;
    cy = fy;
    for (int i = 0; i < hard; i++) {
        int nx = 0, ny = 0;
        const bool more = stem_next(s, cx, cy, ux, uy, w, h, self, &nx, &ny);
        place_cell(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx, CELL_MAKE(r->hardens_to, 0));

        /* Girth tapers linearly to nothing by the top, measured against the
         * run's LENGTH. A fixed step is a taper only for runs about as long
         * as the step. E.g., at one cell per three, a run of twenty stays
         * full width for fifteen cells. Twelve hardenings make trees run
         * together; 6 or 7 are better. */
        const int span = (hard > 1) ? hard - 1 : 1;
        const int extra = (int)r->trunk_girth * (span - i) / span;
        for (int g = 1; g <= extra; g++) {
            const int sidei = (g & 1) ? 2 : 6; /* square on, both ways */
            const int* gd = ring_dir(up_i + sidei);
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
        /* Four directions: up and out, not straight. Straight is leader's
         * next move, capping trunk. Stem grows around leaf, run splits,
         * stopping wood hardening. Tagged placements revealed HEIGHT growth,
         * not branches. */
        static const int crown[4] = {7, 1, 2, 6};
        for (int t = 0; t < ntop; t++) {
            for (int c = 0; c < 4; c++) {
                const int* cd = ring_dir(up_i + crown[c]);
                const int lx = topx[t] + cd[0], ly = topy[t] + cd[1];
                if ((unsigned)lx >= (unsigned)w || (unsigned)ly >= (unsigned)h) {
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

/* One COLD cell melts against liquids and pulls temperature from neighbours,
 * cracking if hot. Chilling driven from cold side, melting from snow cells
 * for efficiency. Liquid assumed warm by definition. Returns cell survival
 * for accuracy. */
static bool
step_one_cold_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
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
        const reaction_t* nr = reaction_of(n);

        /* MELTING, from any liquid - see reaction_t.thaws. */
        if (r->thaws != 0 && r->heats_to != 0 && material_of(n)->kind == KIND_LIQUID
            && (int)(rng_next(&s->rng) & 0xFF) < r->thaws) {
            place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
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
        REACTION_DOC(shatters_to, "if chilled while hot");
        if (temp >= SAND_SHOCK_HEAT && nr->shatters_to != 0) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n), (material_id_t)nr->shatters_to);
            if (try_heat_transform(s, x, y, w, h)) {
                return false; /* and this cell melted paying for it */
            }
            continue;
        }

        if (temp == 0) {
            continue; /* already as cold as this scale goes */
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);

        /* Exchange runs both ways only when heat is taken. Cooling a glowing
         * pane without heat costs its heats_to. Pushing cold to something ≤
         * room temperature doesn't absorb heat. Snow melted on cold glass,
         * making a snowbank unreadable, due to heat absorption rate. */
        if (temp > SAND_AMBIENT_HEAT && try_heat_transform(s, x, y, w, h)) {
            return false;
        }
    }
    return true;
}

/* `conducts` shifts right for slower heat spread than fire, 220 for glass and
 * stone, preventing flame spread. Slower heat sharing makes material
 * isothermal, hot inside equals cold outside. Lava never melted pane due to
 * fast heat sharing. Derived from `conducts` as same physical property. */
#define SPREAD_SHIFT 1

/* One cell cools or warms towards ambient at the same rate. A hot pane cools,
 * a cold one warms, as ambient is the resting point. Chilling and shattering
 * occur in cold cells. The process repeats until the cell matches ambient,
 * ensuring s->may_have_temperature is accurate. */
static bool
step_one_tempered_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
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
    /* THE WET TEST RIDES THIS SAME WALK. Hoisted out from under `if
     * (r->conducts != 0)`, which now guards only the spread body below -
     * every heat_ramp material also sets conducts today, so this changes
     * no behaviour and draws no different RNG, it only stops the wet
     * probe silently depending on a coupling that happened to be true
     * rather than one that is actually guaranteed.
     *
     * Not on water's own row, on purpose: water is the most numerous
     * material on almost any board and would pay a four-neighbour scan
     * per cell per step for a feature that only ever matters while
     * something nearby is hot. Hot cells are scarce, and this pass
     * already scans them - "push the question to where it is already
     * cheap" (see this file's own performance notes elsewhere). */
    bool wet = false;
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
        /* PAIR_QUENCHES means "a liquid that is neither fuel nor a heat
         * source" (water and acid). Reusing it saves a table and avoids
         * reaction_of(n) load. No random number is drawn. */
        if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_QUENCHES) != 0) {
            wet = true;
        }
        if (r->conducts == 0 || reaction_of(n)->heat_ramp == 0) {
            continue;
        }
        const uint8_t nt = CELL_VARIANT(n);
        const int gap = (int)temp - (int)nt;
        if (gap > -2 && gap < 2) {
            continue;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= (r->conducts >> SPREAD_SHIFT)) {
            continue;
        }
        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(gap > 0 ? nt + 1 : nt - 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
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
     * glass shatterable; a lava bath melts it.
     *
     * WET MAKES THE ABOVE-AMBIENT HALF STEEPER, ONLY. A quenching liquid
     * sitting on a hot cell (see `wet`, above) multiplies this same drain
     * by SAND_WET_COOLING_FACTOR (sand.h) - pouring water on hot stone or
     * hot glass is what lets its banked heat actually come back down in a
     * reasonable number of steps instead of the many dozens plain ambient
     * cooling alone would take.
     *
     * GATED TO temp > SAND_AMBIENT_HEAT ON PURPOSE - the branch below this
     * one, which warms a frosted cell back UP, is untouched by `wet`. That
     * is what floors water at ambient rather than letting it act like
     * snow: nothing here can ever push a cell below room temperature, so
     * water can never reach SAND_SHOCK_COLD and thermally shock glass the
     * way an actual chill (snow) can. Going below ambient stays snow/ice's
     * job alone. */
    unsigned drain = r->cools;
    if (temp > SAND_AMBIENT_HEAT) {
        drain *= (unsigned)(temp - SAND_AMBIENT_HEAT);
        if (wet) {
            drain *= SAND_WET_COOLING_FACTOR;
        }
        if (drain > 255u) {
            drain = 255u;
        }
    }
    if (drain == 0 || (unsigned)(rng_next(&s->rng) & 0xFF) >= drain) {
        return temp != SAND_AMBIENT_HEAT;
    }

    const uint8_t next = (uint8_t)(temp > SAND_AMBIENT_HEAT ? temp - 1 : temp + 1);
    row[x] = CELL_MAKE(CELL_MATERIAL(c), next);
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
    return next != SAND_AMBIENT_HEAT;
}

/* Blast radius for confined gas ignition in gas_ignite_confined(). Fixed, not
 * scaled to gas pocket size. Cascade ignition along rim simulates lid giving
 * way. Radius 8, raised from 3, for better visibility and threat. Tune on
 * device. */
#define SAND_GAS_IGNITE_BLAST_RADIUS 8

/* How moisture affects flammability in try_ignite_given(): right-shift per
 * moisture LEVEL, not flat penalty. Gunpowder's 200 becomes 200 -> 50 -> 12
 * -> 3 -> 0 with moist_max 4. Damp powder often misfires, wet powder is
 * inert, needing heat or time to reignite. Shift for simplicity and
 * reliability. */
#define SAND_DAMP_IGNITION_SHIFT     2

/* Checks if an igniting GAS cell at (x, y) is confined by touching at least
 * one KIND_STATIC neighbour. Uses four neighbour reads; off-grid is not
 * considered a wall, mirroring touches_air(). Avoids flood fill as it can be
 * expensive. */
static inline bool
gas_ignite_confined(const sand_t* s, int x, int y, int w, int h) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_STATIC) {
            return true;
        }
    }
    return false;
}

/* Ignites (nx, ny) if flammable and roll succeeds. Returns if ignited.
 * Updates wake/dirty. Part of bd esp32c6-iu5's restructure, using pre-loaded,
 * classified neighbor. Cascades from single bounds-check, load, and
 * pair_bits[][] byte in step_one_burning_cell() walk. Safe for
 * -finline-functions-called-once. */
static inline bool
try_ignite_given(sand_t* s, int nx, int ny, int w, int h, size_t at, cell_t n) {
    const reaction_t* r = reaction_of(n);
    if (r->flammability == 0) {
        return false;
    }
    /* Without this, a flame beside a burning log would keep re-igniting it.
     * place_reacted() writes a FULL variant, resetting burn amount and
     * preventing the log from going out. cell_is_burning() checks for
     * specific GUNPOWDER_LIT code (7). */
    if (cell_is_burning(n)) {
        return false;
    }
    if (r->needs_air && !touches_air(s, nx, ny, w, h)) {
        return false; /* buried in more of itself - a pool of fuel burns
                         * at its surface, not through its volume */
    }
    /* s->flammability mirrors s->decay's override (see
     * sand_set_flammability()): negative (default) means each material's own
     * table figure; anything else overrides all materials. 255 is checked
     * before rolling to maintain reproducibility of gas/fire scenes. */
    int f = (s->flammability >= 0) ? s->flammability : r->flammability;
    /* Moisture damps the roll rather than blocking it outright. Only
     * materials with a moisture codec (`dries != 0`) pay for the
     * moisture_of() lookup; others skip this block. Gunpowder is the only
     * material that reaches here, dampening its re-ignition odds with
     * moisture. */
    REACTION_DOC(flammability, "damped further per moisture level, on any material with a moisture codec");
    const uint8_t m = (r->dries != 0) ? moisture_of(n, r) : 0;
    if (m) {
        f >>= SAND_DAMP_IGNITION_SHIFT * m;
    }
    if (f <= 0) {
        return false; /* before any RNG draw - a fully damped roll must not
                          shift the RNG stream for scenes that never reach
                          this material's moisture range */
    }
    if (f < 255 && (int)(rng_next(&s->rng) & 0xFF) >= f) {
        return false;
    }
    /* Constrained gas pocket bursts, using gas_ignite_confined() for
     * neighbour scan. Replaces sand_explode() call. Checks s->impulse_buf !=
     * NULL to prevent silent failure in scenes without impulse enable. */
    if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS &&
        gas_ignite_confined(s, nx, ny, w, h)) {
        sand_explode(s, nx, ny, SAND_GAS_IGNITE_BLAST_RADIUS);
        return true;
    }
    /* 0 (MAT_EMPTY) reads as MAT_FIRE, so a flammable material that does
     * not care what it turns into (gas) gets the obvious default for
     * free - only a material that needs something else (wood, into an
     * ember) has to say so. */
    const material_id_t becomes = r->ignites_to ? r->ignites_to : MAT_FIRE;
    place_reacted(s, nx, ny, at, becomes);
    return true;
}

/* Places `spec` in the first empty cardinal direction of (x, y) following
 * reaction_dirs[] order. Used by ember's flame and try_heat_transform()'s
 * wet-dirt stage. Direction-agnostic. Returns if placement occurred.
 * Extraction unverified; duplicate if regression appears. */
static inline bool
emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec) {
    for (int d = 0; d < 4; d++) {
        const int fx = x + reaction_dirs[d][0];
        const int fy = y + reaction_dirs[d][1];
        if ((unsigned)fx >= (unsigned)w || (unsigned)fy >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)fy * (size_t)w + (size_t)fx;
        if (CELL_IS_EMPTY(s->cells[at])) {
            place_reacted(s, fx, fy, at, spec);
            return true;
        }
    }
    return false;
}

/* Ember's flame: rolls reaction_t.flare once per step, and on a hit
 * places ordinary MAT_FIRE in the first empty cardinal neighbour - see
 * emit_into_empty_neighbor() just above, which this now shares with the
 * wet-dirt stage of try_heat_transform(). Returns whether it placed
 * anything.
 *
 * SKIPPED OUTRIGHT WHILE THIS CELL IS STILL FALLING - reaction_t.flare's
 * own comment (material.h) is explicit that the mechanic is "meaningless
 * for anything that is not a static heat source with nothing above it -
 * left at zero for everything but the one material that needs to look
 * like it is licking a flame upward while STAYING PUT itself." Lava
 * later got a non-zero flare too, and lava is KIND_LIQUID - it moves,
 * including free-falling one cell per step for the whole length of a
 * pour, which the mechanic was never designed to run against: a poured
 * stream lands as many separate single-cell grains that each spend
 * several steps in open air before settling, and emit_into_empty_
 * neighbor()'s FIXED "up" first (reaction_dirs[0], not gravity-relative)
 * finds empty air on nearly every one of those falling steps, so a tall
 * pour rolled - and on a hit, placed - flare once per falling cell per
 * step of fall, not once per cell that actually settled. Each successful
 * roll is a fresh MAT_FIRE cell that then latches may_have_burning and
 * keeps the whole reactions pass alive until it burns out and rolls its
 * own residue chance for smoke - not yet measured against a real pour on
 * device, but the clear suspect for lava reading as the most expensive
 * material to pour.
 * SUPPORTED, gravity-relative (s->last_step_dx/dy - the same per-step
 * dithered direction the movement sweep just used, so "is there
 * anything to fall onto" matches what the sweep itself would ask), and
 * off-grid reads as STONE (sand_at()'s own convention) so falling off
 * the bottom edge still counts as supported rather than perpetually
 * airborne. A settled
 * pool's cells all have something beneath them (the floor, or more of
 * the same pool) and flare exactly as before; a falling grain does not,
 * and now skips the roll entirely rather than spending it on a cell that
 * is about to move again next step regardless.
 *
 * KIND_STATIC IS EXEMPT FROM THIS CHECK - ember, the mechanic's original
 * case, is defined by never moving on its own (material_kind_t's own
 * comment: "never moves"), so "nothing beneath it" says nothing about
 * whether it is about to fall - it never falls, supported or not, the
 * ordinary gravity sweep does not touch KIND_STATIC at all. Applying the
 * falling check to ember too made an unsupported (but perfectly settled,
 * by ember's own definition of settled) ember cell refuse to flare,
 * which is exactly backwards for the mechanic's own original case. */
static inline bool
try_flare(sand_t* s, int x, int y, int w, int h, const material_t* mat,
         uint8_t flare) {
    if (flare == 0) {
        return false;
    }
    if (mat->kind != KIND_STATIC) {
        const cell_t below = sand_at(s, x + s->last_step_dx,
                                     y + s->last_step_dy);
        if (CELL_IS_EMPTY(below)) {
            return false;
        }
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= flare) {
        return false;
    }
    return emit_into_empty_neighbor(s, x, y, w, h, MAT_FIRE);
}

/* The quenched cell pays one unit of mass, producing steam. Without this, a
 * fire could drain a pool, seeming to vanish. One unit preserves slow
 * quenching: a puddle noticeably shrinks but doesn't vanish on contact.
 * Mirrors follow give_mass()'s rule to avoid zero variant issues. */
static inline void
pay_quench_cost(sand_t* s, int nx, int ny, int w) {
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


/* Heat traverses conductor cells, solving two issues. Attempts direct cell
 * connection, fails. Moves to adjacent cells, rolling `conducts`, up to
 * CONDUCT_REACH. Stops on failure, off-grid, or empty cell. Liquid boils,
 * fuel ignites, neighbors warm. Returns action status. */
static inline bool
conduct_heat(sand_t* s, int x, int y, int w, int h) {
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
        if (reaction_of(s->cells[(size_t)ry * (size_t)w + (size_t)rx])->conducts == 0) {
            continue; /* cheap early-out - most neighbours are not a
                         * conductor at all, and the walk below is not
                         * worth entering for them */
        }

        bool got_through = false;
        for (int depth = 0; depth < CONDUCT_REACH; depth++) {
            const cell_t here = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
            const int c = (s->conduction >= 0) ? s->conduction : reaction_of(here)->conducts;
            if ((int)(rng_next(&s->rng) & 0xFF) >= c) {
                break; /* heat stops inside this cell of the run */
            }

            const int nx = rx + dx;
            const int ny = ry + dy;
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                break;
            }
            const cell_t next = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(next)) {
                break; /* never creates fire in empty space */
            }
            rx = nx;
            ry = ny;
            if (reaction_of(next)->conducts == 0) {
                got_through = true; /* the far side - not a conductor */
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
        const material_t* bm = material_of(bc);
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
        if (bm->kind == KIND_LIQUID && reaction_of(bc)->burns == 0 && reaction_of(bc)->flammability == 0) {
            /* Boils cells near the conductor. Steam rises cell-by-cell via
             * try_bubble() (sand_gas.c). Replaces old surface-boiling method.
             * Eliminates deadlock, showing bottom boiling. Gated by `boils`
             * in reaction_t; failure means retry. */
            const int boils = (s->boils >= 0) ? s->boils : reaction_of(bc)->boils;
            if (boils != 0 && (int)(rng_next(&s->rng) & 0xFF) < boils) {
                /* boils_to, not hardcoded MAT_STEAM; water remains MAT_STEAM.
                 * place_reacted() used instead of place_cell() to avoid
                 * shortened starting life for steam. */
                const uint8_t boils_to = reaction_of(bc)->boils_to ? reaction_of(bc)->boils_to : MAT_STEAM;
                place_reacted(s, rx, ry, bat, boils_to);
                acted = true;
            }
        } else {
            const reaction_t* br = reaction_of(bc);
            if (br->heat_ramp != 0 || (br->heats_to != 0 && br->heat_chance != 0)) {
                /* Sand behind a hot wall becomes glass, the same way water
                 * behind one boils - heat that has crossed a conductor
                 * does everything heat in contact does. */
                if (try_heat_transform(s, rx, ry, w, h)) {
                    acted = true;
                }
            }
            if (br->flammability != 0 && (!br->needs_air || touches_air(s, rx, ry, w, h))) {
                /* needs_air is checked here for the same reason
                 * try_ignite() checks it: a pool of fuel burns at its
                 * surface, not through its volume. Without it a pan would
                 * light the oil against its own bottom - the one cell of
                 * a pool guaranteed to be buried. */
                const uint8_t becomes = br->ignites_to ? br->ignites_to : MAT_FIRE;
                place_reacted(s, rx, ry, bat, becomes);
                acted = true;
            }
        }
    }

    return acted;
}

/* One dissolver cell's turn: evaporate on a low roll, or else eat one
 * cardinal neighbour and pay for it.
 *
 * Evaporation is checked first and returns immediately - unconditional,
 * with no dissolve roll or neighbour involved, so a cell that evaporates
 * this step does not also get a free bite the same step.
 *
 * Two rolls, on two different materials, and both have to pass: this
 * cell's `dissolves` (how hard the acid tries) and the neighbour's
 * `dissolvable` (how easily it gives way). Splitting it that way is what
 * lets one acid figure produce different rates against sand, wood and
 * stone without acid knowing any of their names - and `dissolvable`
 * defaulting to zero is what keeps acid from eating the container it is
 * standing in, the floor it is standing on, or the air above it.
 *
 * The bite always costs the acid something - never a free eat - but how
 * much varies by what it ate. Against oil and the generic sand/wood/stone
 * targets, the ordinary cost is one unit of mass via the same
 * pay_quench_cost() a liquid pays to put out a fire (the liquid is
 * CONSUMED rather than merely consulted), but a separate, much higher
 * death-roll can spend the whole cell in one bite instead - deliberately,
 * so the acid itself has a real chance to run out rather than dissolving
 * an unbounded amount of anything while remaining a single cell forever.
 * That "acid never pays" mistake is the one oil-soaked ash made before
 * soaking became a real transfer, and worth naming twice because it is
 * the easy one to make.
 *
 * At most one neighbour per step, so a cell of acid surrounded by sand
 * eats into it rather than opening a hole on all four sides at once.
 * Returns whether it dissolved anything. */
/* ACID BUBBLES - a flat, non-decaying chance-in-256 per exposed acid cell,
 * per step, that pops a single grain up off the surface. "Flying pixels
 * above it, almost like it's bubbling, or carbonated" was the ask - a
 * CONTINUOUS, AMBIENT look, not a reaction to any specific event, which is
 * also why the chance itself never decays (see SAND_ACID_BUBBLE_CHANCE's
 * own comment in sand.h): a bubble that gets rarer the longer a pool has
 * existed would read as the fizz running out, not as carbonation.
 *
 * REPLACED THE OLD "LANDED HARD" TRIGGER splash_displace() used to share
 * with water, 2026-09-01. That trigger fired from wherever a landing event
 * happened to occur, and a real-scene reproduction (a symmetric pool,
 * poured continuously into its own centre) found those events
 * concentrating hard against whichever wall ordinary cross-flow levelling
 * happened to reach first - not a bug in any one line (ruled out one at a
 * time: the disc-seeding math, the diagonal-slide try-order, block
 * alignment, the liquid_flip/sweep_flip alternation, exact pool/pour
 * centring all still reproduced it), but an emergent, self-reinforcing
 * consequence of "where a splash triggers" being entirely determined by
 * wherever ordinary physics happens to pile material up first.
 *
 * LIVES HERE, NOT IN move_liquid_grain() (sand_liquid.c) WHERE IT FIRST
 * LANDED - moved the same day, once a REAL calm puddle on device never
 * bubbled at all. move_liquid_grain() only runs for cells the MAIN SWEEP
 * visits, and the main sweep skips any block marked settled under block-
 * sleeping (sand_enable_sleeping(), see step_one_row()'s own skip in
 * sand.c) - exactly what a calm, undisturbed puddle becomes within a few
 * quiet steps. A host test never caught this because it never enabled
 * sleeping, so it always visited every cell regardless of settled state -
 * a blind spot in the test, not evidence the mechanism worked on device.
 * This pass (sand_step_reactions(), called from step_one_reacting_row()
 * below) is NOT gated by block-sleeping at all - see this file's own
 * comment on why dissolving and cooling already needed that, for the
 * identical reason: acid dissolving a neighbour, wood cooling after its
 * fire went out, and now acid bubbling all have to keep happening on a
 * board with nothing else moving. Reached from the SAME `r->dissolves`
 * branch step_one_dissolver_cell() already uses, since acid is the only
 * material with dissolves set at all - no separate flag needed, that
 * branch already runs on every acid cell every step this pass runs.
 *
 * NO dx,dy PARAMETER - unlike move_liquid_grain(), this pass carries no
 * gravity vector of its own (see sand_step_reactions()'s own signature).
 * s->last_step_dx/last_step_dy (sand.h) is the same "which way is down"
 * accessor growth already reads here for an identical reason - the
 * dithered direction of the step just taken, written once in sand.c
 * before this pass runs, not stale from an earlier step.
 *
 * "RIM" MEANS EXPOSED, NOT ANY PARTICULAR SHAPE - the one cell directly
 * AGAINST gravity from this one is empty.
 *
 * DIRECTION IS UP, WITH A SMALL SPREAD - one of the three ring directions
 * centred on straight against gravity (that direction and its two
 * immediate diagonals), not splash_displace()'s full outward ring: a
 * bubble breaking the surface has no "point of impact" to spray outward
 * from, it just pops. */
static void
acid_bubble(sand_t* s, int x, int y) {
    const int dx = s->last_step_dx, dy = s->last_step_dy;
    const int ux = x - dx, uy = y - dy; /* one step AGAINST gravity */
    if (!CELL_IS_EMPTY(sand_at(s, ux, uy))) {
        return; /* not exposed - nothing above to pop into */
    }
    if ((rng_next(&s->rng) & 0xFF) >= SAND_ACID_BUBBLE_CHANCE) {
        return;
    }
    const int i_up = (ring_of(dx, dy) + 4) & 7;
    const int spread = (int)(rng_next(&s->rng) % 3) - 1; /* -1, 0 or 1 */
    sand_impulse(s, x, y, (i_up + spread + 8) & 7, SAND_ACID_BUBBLE_SPEED);
}

static bool
step_one_dissolver_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    /* r evaporates at 1/256. Puddles boost this. A second roll, starting at
     * 1/1024 (1/4), tightens to 1/5120 (1/20), then 1/15360 (1/60), adjusts
     * material's figure. Test_acid_evaporates_into_gas_when_forced;
     * sand_set_evaporates() unchanged. Modulo for non-powers of 2. */
    const bool per_material = s->evaporates < 0;
    const int evaporates = per_material ? r->evaporates : s->evaporates;
    if (evaporates != 0 && (int)(rng_next(&s->rng) & 0xFF) < evaporates
        && (!per_material || (rng_next(&s->rng) % 60) == 0)) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, MAT_GAS);
        return true;
    }

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
        /* PAIR_DISSOLVABLE: rejects a neighbour with a dissolvable figure of
         * 0 in every ordinary material's row before reaction_of(n) loads it -
         * this only runs for genuine acid cells. */
        if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_DISSOLVABLE) == 0) {
            continue;
        }
        const uint8_t give = reaction_of(n)->dissolvable;
        if (give == 0 || (int)(rng_next(&s->rng) & 0xFF) >= give) {
            continue;
        }

        /* DILUTION occurs, not eat-water. The dissolvable roll determines the
         * winner via SAND_ACID_DILUTE_TO_WATER_CHANCE. Both cells change
         * symmetrically. The winner vaporizes (MAT_STEAM for water, MAT_GAS
         * for acid); the loser converts to the winner's TARGET. CELL_VARIANT
         * persists. Transformation incurs cost. Earlier versions left the
         * winning side unchanged, causing unchecked growth. */
        if (CELL_MATERIAL(n) == MAT_WATER) {
            /* SAND_ACID_DILUTE_MASS_BIAS (sand.h): Count ACID and WATER
             * cardinal neighbours (up to 3 each, excluding direct link).
             * Difference determines split. Measuring acid alone was tried but
             * asymmetrical: deep acid always looks backed vs. adjacent water.
             * Eight bounds-checked reads, cheap, symmetric. */
            int acid_backing = 0, water_backing = 0;
            for (int bd = 0; bd < 4; bd++) {
                const int abx = x + reaction_dirs[bd][0];
                const int aby = y + reaction_dirs[bd][1];
                if ((unsigned)abx < (unsigned)w && (unsigned)aby < (unsigned)h
                    && CELL_MATERIAL(s->cells[(size_t)aby * (size_t)w + (size_t)abx]) == MAT_ACID) {
                    acid_backing++;
                }

                const int wbx = nx + reaction_dirs[bd][0];
                const int wby = ny + reaction_dirs[bd][1];
                if ((unsigned)wbx < (unsigned)w && (unsigned)wby < (unsigned)h
                    && CELL_MATERIAL(s->cells[(size_t)wby * (size_t)w + (size_t)wbx]) == MAT_WATER) {
                    water_backing++;
                }
            }
            const int mass_bias = (s->acid_dilute_mass_bias >= 0)
                                       ? s->acid_dilute_mass_bias : SAND_ACID_DILUTE_MASS_BIAS;
            const int water_wins_chance = SAND_ACID_DILUTE_TO_WATER_CHANCE
                                           + (water_backing - acid_backing) * mass_bias;

            /* ONE roll, a three-way ladder - see SAND_ACID_DILUTE_EVAPORATE_CHANCE's
             * own comment (sand.h) for why this replaced two independent
             * rolls. */
            const int roll = (int)(rng_next(&s->rng) & 0xFF);
            const size_t self_at = (size_t)y * (size_t)w + (size_t)x;
            if (roll < SAND_ACID_DILUTE_EVAPORATE_CHANCE) {
                /* Acid's own ambient-style boil-off, unrelated to who
                 * wins the water/acid split below - see this constant's
                 * own comment (sand.h). Only the acid cell is touched. */
                place_reacted(s, x, y, self_at, MAT_GAS);
            } else if (roll < SAND_ACID_DILUTE_EVAPORATE_CHANCE + water_wins_chance) {
                /* WATER wins, creates MAT_STEAM via place_reacted(), unlike
                 * earlier mass-carrying bug. Acid converts to water, mass
                 * carries over. Both use place_cell()/place_reacted() for
                 * content flags. */
                place_cell(s, x, y, self_at, CELL_MAKE(MAT_WATER, CELL_VARIANT(row[x])));
                place_reacted(s, nx, ny, at, MAT_STEAM);
            } else {
                /* ACID WINS - acid is the source, spent boiling off into
                 * its own MAT_GAS at a fresh full life, same reasoning
                 * as above. Water is the target, converted into acid,
                 * mass carried over. */
                place_reacted(s, x, y, self_at, MAT_GAS);
                place_cell(s, nx, ny, at, CELL_MAKE(MAT_ACID, CELL_VARIANT(n)));
            }
            return true;
        }

        /* OIL BOILS OFF, IT DOES NOT BREED MORE ACID. See
         * SAND_ACID_OIL_TO_GAS_CHANCE and SAND_ACID_OIL_DEATH_CHANCE
         * (sand.h). Oil mostly turns to gas; acid has a high chance to die
         * outright instead of paying quench cost. Rolls are independent. */
        if (CELL_MATERIAL(n) == MAT_OIL) {
            const uint8_t oil_residue = ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_TO_GAS_CHANCE)
                                         ? MAT_GAS : MAT_ACID;
            place_reacted(s, nx, ny, at, oil_residue);

            if ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_DEATH_CHANCE) {
                const size_t self_at = (size_t)y * (size_t)w + (size_t)x;
                s->cells[self_at] = CELL_EMPTY;
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            } else {
                pay_quench_cost(s, x, y, w);
            }
            return true;
        }

        /* Fizz placed in cell just eaten, costless, reaction spot. Smoke
         * lighter, try_bubble() in sand_gas.c moves it. Smoke or gas, 50%
         * chance, "acid breathes gas" logic. */
        if (r->fizz != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->fizz) {
            const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_GAS : MAT_SMOKE;
            place_reacted(s, nx, ny, at, residue);
        } else {
            s->cells[at] = CELL_EMPTY;
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
        }

        /* The acid may die outright or spend one unit if
         * SAND_ACID_EAT_DEATH_CHANCE roll fails. Done after target is dealt
         * with. */
        if ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_EAT_DEATH_CHANCE) {
            row[x] = CELL_EMPTY;
            mark_rows(s, y, y);
            wake_block_and_neighbors(s, x, y);
        } else {
            pay_quench_cost(s, x, y, w);
        }
        return true;
    }
    return false;
}

/* Whether (x, y) is one corner of a 2x2 block whose other three cells are
 * lit cells of the SAME species as `grain` - any of the four 2x2 squares
 * that contain it will do. The gate step_one_burning_cell()'s burn-out
 * branch reads to decide whether a fuse ends in a blast or in plain fire
 * (see reaction_t.explodes's own comment, material.h).
 *
 * 2x2, NOT 3x3. The first version asked for all eight neighbours lit, and
 * on the device blasts became rare to the point of looking broken: burn-out
 * rolls are independent per cell, so by the time any one cell burns out,
 * the neighbours that were lit before it usually already are fire - a
 * fully-lit 3x3 exists only in the brief window between the fuse front
 * passing and the first burn-out behind it. Three lit neighbours in one
 * quadrant is the smallest shape that still says "a body of powder, not a
 * trail", and it is what a lit pile actually presents at burn-out time.
 * Cheaper too: at most four cells looked at per quadrant, with an early out.
 *
 * OFF-BOARD COUNTS AS NOT LIT - lit_here() bounds-checks first, so a
 * neighbour past the edge never counts as one of the three corners this
 * looks for. That is narrower than this comment once claimed: an edge cell
 * CAN still be the corner of a 2x2 that folds entirely inward (all three
 * other corners on-board) and detonates like any interior one - the old
 * "board edge never detonates" claim was retired for exactly that reason.
 * What off-board-as-not-lit actually buys: a lit cell whose only lit
 * neighbours are off-board or strung out in a single line never completes
 * a square, wherever it sits - see test_a_one_wide_lit_trail_never_
 * detonates (suite_sand_gunpowder.c).
 *
 * same_species(), not a raw material compare - gunpowder shares its high
 * nibble with the extended statics (GUNPOWDER_BASE, material.h), and a
 * neighbour of static ice must never count as "more of this fuse" just
 * because both happen to decode through MAT_EXTENDED. cell_code(n) >=
 * r->lit_from, not cell_is_burning(n): reaction_of(n) would be this exact
 * row anyway once same_species() has already agreed, so there is nothing
 * left to re-derive - just the one threshold compare. */
static inline bool
lit_here(const sand_t* s, int nx, int ny, int w, int h, cell_t grain, const reaction_t* r) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    return same_species(n, grain) && cell_code(n) >= r->lit_from;
}

/* `r`, from the caller - step_one_burning_cell() already has reaction_of(
 * grain) in hand as `rx`, and it is the same row this function would
 * otherwise re-derive from `grain` on every call. */
static inline bool
find_lit_two_by_two(const sand_t* s, int x, int y, int w, int h, cell_t grain, const reaction_t* r,
                     int* out_dx, int* out_dy) {
    for (int dy = -1; dy <= 1; dy += 2) {
        if (!lit_here(s, x, y + dy, w, h, grain, r)) {
            continue;
        }
        for (int dx = -1; dx <= 1; dx += 2) {
            if (lit_here(s, x + dx, y, w, h, grain, r) && lit_here(s, x + dx, y + dy, w, h, grain, r)) {
                *out_dx = dx;
                *out_dy = dy;
                return true;
            }
        }
    }
    return false;
}

/* SPEND THE 2x2 THAT QUALIFIED. The three lit cells that made this a blast
 * become fire before sand_explode() runs, rather than being left for its core
 * fill to catch on its own. NOT NEEDED AT TODAY'S RADIUS, and that is the
 * point of keeping it rather than the reason for it. At
 * SAND_GUNPOWDER_BLAST_RADIUS 16 and SAND_EXPLODE_CORE_DIVISOR 5 the core is
 * radius 3, which already reaches every cell of the 2x2 - the two cardinals
 * at distance 1 and the diagonal at distance root-two, both comfortably
 * inside 3 - so sand_explode()'s own core fill turns all three to fire
 * whether this runs or not. What this buys is the rule holding at ANY radius:
 * tune SAND_GUNPOWDER_BLAST_RADIUS (or the divisor) down far enough and the
 * core shrinks below root-two, at which point the diagonal corner would be
 * left lit for the annulus to throw as a still-lit grain instead - free to
 * land as the corner of some other 2x2 and blast again. Three cheap writes,
 * once per blast, is what keeps "one 2x2, one blast, nothing lit leaves it"
 * true independent of whatever the radius is tuned to next. */
static inline void
spend_lit_two_by_two(sand_t* s, int x, int y, int w, int dx, int dy) {
    const int px[3] = {x + dx, x, x + dx};
    const int py[3] = {y, y + dy, y + dy};
    for (int i = 0; i < 3; i++) {
        const size_t at = (size_t)py[i] * (size_t)w + (size_t)px[i];
        place_reacted(s, px[i], py[i], at, MAT_FIRE);
    }
}

/* STEPS BETWEEN FUSE BLASTS, board-wide: one blast, then this many steps
 * before another may fire. 1 is one blast a step, 2 one every other step, and
 * 0 lifts the limit entirely. A burn-out inside the wait becomes plain fire,
 * exactly as one that found no lit 2x2 does. This is what bounds a big pile's
 * burst cost per frame outright, instead of leaving the stagger to the luck
 * of independent burn-out rolls - and it IS the cadence: a lit pile goes off
 * at this rate for as long as it keeps presenting lit 2x2s, so raising it
 * spaces a pile's detonations out in time without touching how big any one of
 * them is (that is SAND_GUNPOWDER_BLAST_RADIUS) or how long a fuse burns
 * before it reaches one (reaction_t.burn_decay). Board-wide rather than per
 * pile because "per pile" would need a region walk, and one blast a frame is
 * already more than the eye separates. */
#define SAND_GUNPOWDER_BLAST_COOLDOWN 8

/* One burning cell's turn: burn down first, extinguish if liquid neighbour
 * (quench_to, pay mass), extinguish if smothered, ignite flammable
 * neighbours, conduct heat, flare if applicable. */
static bool
step_one_burning_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h) {
    cell_t grain = row[x];
    const material_t* mat = material_of(grain);
    const uint8_t mat_id = CELL_MATERIAL(grain);
    const size_t at = (size_t)y * (size_t)w + (size_t)x;

    /* A material that burns only while lit counts its VARIANT down at its
     * own rate, rather than the movement table's `decay` - which stays 0
     * for it, because wood is not a transient. It does not disappear on
     * its own; it disappears because it burned. */
    const reaction_t* rx = reaction_of(grain);
    const bool lit_state = rx->burn_decay != 0;
    const int burn_rate = (s->decay >= 0) ? s->decay : rx->burn_decay;

    if (lit_state ? !tick_decay_at(s, row, x, y, &grain, rx, burn_rate)
                  : !tick_decay(s, row, x, y, &grain, mat, mat_id)) {
        /* Burned out. `tick_decay()`/`tick_decay_at()` cleared cell, woke it.
         * `grain` holds previous byte. `find_lit_two_by_two()` checks if
         * species matches. `explodes` BURN-OUT read: decides blast. Lit 2x2
         * corner detonates, else burns to plain fire. Delays blasts in piles. */
        if (rx->explodes != 0) {
            REACTION_DOC(explodes, "at burn-out, if it is one corner of a 2x2 that is all lit and the board's blast cooldown has run out");
            int dx = 0, dy = 0;
            if (s->impulse_buf != NULL && s->fuse_blast_wait == 0 &&
                find_lit_two_by_two(s, x, y, w, h, grain, rx, &dx, &dy)) {
                s->fuse_blast_wait = (uint8_t)((s->fuse_cooldown >= 0) ? s->fuse_cooldown
                                                                       : SAND_GUNPOWDER_BLAST_COOLDOWN);
                spend_lit_two_by_two(s, x, y, w, dx, dy);
                sand_explode(s, x, y, rx->explodes);
            } else {
                place_reacted(s, x, y, at, MAT_FIRE);
            }
            return true;
        }
        /* MAT_SMOKE, not MAT_STEAM: avoids kettle-steam confusion. Hardcoded
         * due to uniform residue; use `smokes_to` field if needed. */
        const uint8_t residue = rx->residue;
        if (residue != 0 && (int)(rng_next(&s->rng) & 0xFF) < residue) {
            place_reacted(s, x, y, at, MAT_SMOKE);
        }
        return true;
    }

    /* No liquid on board; single field test. neighbor_quenches() reads
     * material table, 9.3% of full-screen-of-fire, 10% of fire cascade
     * benchmarks. may_have_liquid set by latch_content_flags(), cleared by
     * full cross-flow sweep. */
    if (s->may_have_liquid) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if (neighbor_quenches(s, nx, ny, w, h)) {
                const uint8_t quench_to = rx->quench_to;
                if (lit_state) {
                    /* Water douses log, leaving wet (`rx->tones != 0`). Ember
                     * needs distinct state. Quenching retains moisture,
                     * unlike dry code 0. Gunpowder, wet but non-explosive,
                     * exemplifies. Use `with_moisture()`/`cell_with_code()`
                     * in `place_cell()` to preserve identity bits, ensuring
                     * moisture latches correctly. */
                    if (rx->tones != 0) {
                        place_cell(s, x, y, at, with_moisture(grain, rx->moist_max, rx));
                    } else {
                        row[x] = CELL_MAKE(mat_id, 0);
                        mark_rows(s, y, y);
                        wake_block_and_neighbors(s, x, y);
                    }
                } else if (quench_to != 0) {
                    /* FIRE's quench_to (MAT_STEAM, material.c) models LIQUID
                     * flash-boiling at contact, already handled by
                     * conduct_heat()'s boiling branch (reaction_t.boils_to).
                     * Unlike lava's quench_to (MAT_STONE), fire substitutes
                     * the quenching liquid's boils_to. 0 defaults to
                     * MAT_STEAM, water's figure. */
                    uint8_t product      = quench_to;
                    bool leaves_residue  = true;
                    if (mat_id == MAT_FIRE) {
                        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
                        const uint8_t liquid_boils_to = reaction_of(s->cells[nat])->boils_to;
                        if (liquid_boils_to == MAT_GAS) {
                            /* Acid putting a flame out is not water's
                             * clean, deterministic flash to steam - see
                             * SAND_ACID_QUENCH_RESIDUE_CHANCE/
                             * SAND_ACID_QUENCH_SMOKE_CHANCE's own comment
                             * (sand.h) for why this is two rolls, not
                             * one. */
                            leaves_residue = (int)(rng_next(&s->rng) & 0xFF)
                                             < SAND_ACID_QUENCH_RESIDUE_CHANCE;
                            product = ((int)(rng_next(&s->rng) & 0xFF)
                                       < SAND_ACID_QUENCH_SMOKE_CHANCE)
                                          ? MAT_SMOKE
                                          : MAT_GAS;
                        } else {
                            product = liquid_boils_to ? liquid_boils_to : MAT_STEAM;
                        }
                    }
                    if (leaves_residue) {
                        place_reacted(s, x, y, at, product);
                        /* TRIGGER B of cool_off_chain(): A burning LIQUID
                         * cell quenched takes a same-liquid neighbor. This
                         * allows SUSTAINED pour to progress, freezing the
                         * crust. Gated on KIND_LIQUID, not just quench_to !=
                         * 0. Fire (MAT_STEAM) is KIND_GAS, does not chain.
                         * Only lava quenches AS liquid. */
                        if (mat->kind == KIND_LIQUID) {
                            const int lava_cooloff = (s->lava_cooloff >= 0)
                                                          ? s->lava_cooloff
                                                          : SAND_LAVA_COOLOFF_CHANCE;
                            cool_off_chain(s, x, y, w, h, product, lava_cooloff);
                        }
                    } else {
                        row[x] = CELL_EMPTY;
                        mark_rows(s, y, y);
                        wake_block_and_neighbors(s, x, y);
                    }
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
     * doing the burning.
     *
     * The burst gate below (covered_at(), sand_priv.h - bd esp32c6-mqt/
     * esp32c6-a2j) needs a DIFFERENT question for a burning LIQUID than
     * this smothered() call answers, and does not share it: smothered()
     * can never be true for any cell in a lava pool wider than one cell,
     * because a lava neighbour is KIND_LIQUID and neighbor_smothers()
     * never counts one - the pool's own sides and floor are always more
     * of the same liquid, never covering, no matter how completely a
     * crust seals its surface. covered_at()'s gravity-relative lid
     * (cover_mask(), sand_priv.h) is what actually answers "is there a
     * lid over it" for a wide pool.
     *
     * SKIPPED OUTRIGHT for an `explodes` material - gunpowder carries its
     * own oxidiser, unlike wood or a candle, which both need outside air
     * to keep burning. A fuse buried in the middle of its own pile has to
     * keep burning or nothing inside a pile would ever reach burn-out at
     * all, and every blast would be stillborn at the one place a pile
     * actually has enough neighbours lit to detonate. */
    if (mat->kind != KIND_LIQUID && rx->explodes == 0 && smothered(s, x, y, w, h, mat->density)) {
        /* Burying a burning log smothers the BURN. cell_with_code(grain, 0)
         * used instead of CELL_MAKE(mat_id, 0) because rx->explodes == 0
         * prevents gunpowder, and cell_with_code() matches every material
         * seen here, including MATX_ICE. */
        row[x] = lit_state ? cell_with_code(grain, 0) : CELL_EMPTY;
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return true;
    }

    bool acted = false;

    /* A SUFFICIENTLY COVERED LAVA CELL CAN BURST - bd esp32c6-mqt, the
     * chosen replacement for the vent machinery that used to sit right
     * here (covered_from_above()/try_vent()/try_vent_chunk(), reaction_t.
     * vent_chance - removed by bd esp32c6-0f2, once this replacement had
     * shipped and proved out). Where venting used to throw the lid away,
     * this converts the lava itself:
     * the cell becomes MAT_STONE via place_reacted(), then sand_explode()
     * fires at that same spot. sand_explode() FIRST fills a core of
     * radius `radius / SAND_EXPLODE_CORE_DIVISOR` with fire (sand.h) - at
     * SAND_LAVA_BURST_RADIUS(12) and divisor 5 that core radius is 2, so
     * the stone cell just placed at the centre is immediately overwritten
     * by fresh fire. That is expected, pinned behaviour, not a bug to
     * chase - see test_buried_lava_bursts_into_stone_and_fire
     * (suite_sand_lava_burial.c), which asserts the centre's real final
     * material
     * rather than assuming it.
     *
     * A WHOLE-CELL EVENT, NOT A PER-NEIGHBOUR PROBE - sits out here rather
     * than inside the STAGE 2 cascade walk below, the same reason the
     * vent block that used to precede it sat out here too: this asks one
     * question about the cell itself, not one question per neighbour
     * direction.
     *
     * GATED ON THE SAME SHAPE cool_off_chain()'s own trigger (below) uses
     * - burning KIND_LIQUID with a non-zero quench_to (lava's own
     * MAT_STONE, material.c) - so only lava ever reaches this, resolved
     * ONCE, here, before any neighbour work: a board with no lava (or any
     * other burning liquid) costs one predicted-false compare and draws
     * no random number at all.
     *
     * THE ROLL COMES BEFORE covered_at(), not after: at
     * SAND_LAVA_BURST_CHANCE's odds (sand.h, deliberately the rarest a
     * single byte-wide roll can express) the roll is the cheap rejection,
     * the 3-neighbour cover_mask() walk is not - so the overwhelmingly
     * common case, a covered cell that simply loses this step's roll,
     * never pays for the walk.
     *
     * covered_at() (sand_priv.h), NOT smothered()'s own all-4 == test -
     * bd esp32c6-a2j, replacing cover_count() (bd esp32c6-mqt), which was
     * wrong twice over: it counted four SCREEN-fixed cardinals instead of
     * gravity-relative ones, and being built on neighbor_smothers()
     * (which never counts a liquid neighbour) meant an interior cell of a
     * pool wider than one cell could have at most one neighbour that
     * ever counted - the crust directly above it - so a wide pool could
     * never reach the threshold no matter how completely a crust sealed
     * it. covered_at()'s gravity-relative lid fixes that: the two
     * diagonal crust cells beside "straight up" count too, so a crust
     * alone lids an interior pool cell - see
     * test_a_wide_pool_under_a_crust_bursts (suite_sand_lava_burial.c),
     * the case that could not fire before this change. Those three cells and ONLY
     * those: the two perpendiculars beside the cell never count, because
     * a vessel's sides wall lava in rather than cover it, and letting
     * them count blew the sides out of every hand-drawn basin (see
     * test_lava_in_a_wall_notch_never_bursts, suite_sand_lava_burial.c, and
     * cover_mask()'s own comment for the notch that did it). A pocket
     * with an open side still qualifies, as long as its lid is complete.
     *
     * NOT GATED ON s->impulse_buf, UNLIKE try_ignite_given()'s own
     * gas_ignite_confined() caller above, which skips straight to a plain
     * ignition when impulses are off so gas still catches fire either
     * way. There is no equivalent fallback needed here: with impulses
     * off, sand_explode() is a documented no-op (its own first line,
     * sand.c) and this cell simply becomes stone with nothing thrown -
     * exactly right, since no impulses means no explosions anywhere else
     * in the simulation either, and a bare conversion to stone is not a
     * wrong answer on its own (see
     * test_buried_lava_still_becomes_stone_with_impulses_off,
     * suite_sand_lava_burial.c). */
    const bool is_lava = mat->kind == KIND_LIQUID && rx->quench_to != 0;
    /* NATURAL rate or override; difference decides if second gate applies.
     * step_one_dissolver_cell() handles 'evaporates'. Test at 255 means 'fire
     * on every cell'; hidden 1-in-N complicates testing. */
    const bool burst_natural = s->lava_burst < 0;
    const int burst_chance = burst_natural ? SAND_LAVA_BURST_CHANCE : s->lava_burst;
    if (is_lava && burst_chance != 0 &&
        (int)(rng_next(&s->rng) & 0xFF) < burst_chance &&
        (!burst_natural || (rng_next(&s->rng) % SAND_LAVA_BURST_GATE) == 0) &&
        covered_at(s, x, y, w, h, mat->density)) {
        /* rx->quench_to, not a hardcoded MAT_STONE: `is_lava` above IS
         * "a burning liquid with a quench product", so the product is
         * already named right there, and a second burning liquid with
         * a different one would keep working. The same reasoning
         * cool_off_chain() gives for not carrying a material id. */
        place_reacted(s, x, y, at, rx->quench_to);
        sand_explode(s, x, y, SAND_LAVA_BURST_RADIUS);
        return true;
    }

    /* STAGE 2 OF bd esp32c6-iu5's pair-matrix restructure: THE CASCADE.
     * Bounds-check, load and classify each neighbour exactly ONCE per
     * iteration - one cell load, one pair_bits[mat_id][theirs] byte load -
     * and feed both probes from that one classification, instead of each
     * of try_ignite()/try_heat_transform() separately re-deriving the same
     * three facts about the same cell (their own former bodies, still
     * intact in try_heat_transform()'s wrapper for its other three callers
     * - see that function's own comment). try_ignite_given()/
     * try_heat_transform_given() (both above) are exactly the OLD try_
     * ignite()/try_heat_transform() bodies with that shared prologue cut
     * away - nothing past this point differs from before, so the RNG draw
     * sequence is unchanged cell by cell, direction by direction: ignite
     * is still tried before heat transform, for every direction, in the
     * same reaction_dirs[] order as always. See sand_step_reactions()'s
     * own comment for why this is safe to gate on the pair byte alone:
     * PAIR_IGNITABLE/PAIR_HEAT_RESPONSIVE mean exactly what try_ignite_
     * given()'s/try_heat_transform_given()'s own first real checks would
     * have decided anyway - the fingerprint gate this stage is committed
     * under proves it, not just this comment.
     *
     * Explicitly still NOT merged with the quench walk above or the
     * conduct_heat() walk below - see this file's own top comment on why
     * those stay separate passes over the same four neighbours: merging
     * them would reorder RNG draws between the three walks, which the bd
     * issue calls out as reordering-territory, not this stage's job.
     *
     * my_pair_row HOISTED OUT OF THE LOOP: mat_id is loop-invariant (this
     * cell's own material, fixed for all four directions), so pair_bits
     * [mat_id] is one row-base address good for the whole loop, rather
     * than a fresh pair_bits[mat_id][...] index - and therefore a fresh
     * runtime multiply - every iteration. Tried BECAUSE the host probe
     * (best of 7, twice) measured lava stress/four liquids/smoke+steam a
     * couple percent SLOWER against stage 1 despite this stage's own
     * objdump showing step_one_burning_cell() genuinely smaller - the
     * multiply was the obvious suspect, since pair_theirs_bits()'s own
     * MAT_EMPTY-row trick (stage 1) is a compile-time-zero offset the
     * compiler folds away for free, and a runtime row index is not free
     * the same way. Measured, not assumed to have fixed it: hoisting
     * moved the host numbers by well under a percent, so the multiply was
     * not the (or not the whole) explanation. Left in regardless - it is
     * strictly no worse, plainly correct, and one less thing to suspect
     * next time - but the regression itself is reported as-is below,
     * unexplained, rather than claimed fixed. Per this project's own
     * "host numbers mispredicting the device" lesson (docs/Sand/
     * Performance-Tuning-Attempts.md): this stage removes real RNG-
     * avoiding and load-avoiding work, which is exactly the shape of
     * change host and device have disagreed on before - a device capture
     * settles this, a host number alone does not. */
    /* TRIGGER A of cool_off_chain() (above): doing the WORK of actually
     * converting a neighbour costs a burning LIQUID a small chance of
     * freezing itself. Resolved once, here, before the loop starts - not
     * because the chance can change mid-loop, but so a cell that is not
     * even a burning liquid (wood, ember, fire) never pays for this at
     * all: 0 short-circuits the check inside the loop below before it
     * ever reads the RNG, one predicted-false compare per burning cell on
     * a board with no lava, the same "check whether this could ever
     * matter before rolling" discipline the lava-burst gate and
     * try_ignite() both already document above. */
    const int lava_cooloff = (mat->kind == KIND_LIQUID && rx->quench_to != 0)
                                  ? ((s->lava_cooloff >= 0) ? s->lava_cooloff
                                                             : SAND_LAVA_COOLOFF_CHANCE)
                                  : 0;
    const uint8_t* my_pair_row = pair_bits[mat_id];
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
        const uint8_t pair = my_pair_row[CELL_MATERIAL(n)];
        if ((pair & PAIR_IGNITABLE) != 0 && try_ignite_given(s, nx, ny, w, h, nat, n)) {
            acted = true;
        }
        /* Separate from ignition, and reached whether or not that fired:
         * a neighbour is either fuel or something heat merely changes, and
         * nothing is both today, but there is no reason one could not be. */
        if ((pair & PAIR_HEAT_RESPONSIVE) != 0) {
            /* Captured BEFORE probe. `try_heat_transform_given()` returning
             * true doesn't mean neighbor's material changed. Heat-ramping
             * materials (stone, glass) store heat. Relying solely on return
             * value to gate `cool_off_chain()` would drain it almost every
             * step lava touches wall. Only genuine CELL_MATERIAL change or
             * thermal-shock crack triggers it. */
            const uint8_t before_mat = CELL_MATERIAL(n);
            bool changed = try_heat_transform_given(s, nx, ny, w, h, nat, n);
            /* MELTING - door opens with LIQUID. See reaction_t.melts. Fire
             * and lava are indistinguishable. Source known here. Check
             * source, neighbors, then RNG. Flame near root pays three
             * predicted-false checks, never touches stream. */
            if (!changed && mat->kind == KIND_LIQUID) {
                const reaction_t* nr = reaction_of(n);
                if (nr->melts != 0 && nr->heats_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < nr->melts) {
                    place_reacted(s, nx, ny, nat, nr->heats_to);
                    changed = true;
                }
            }
            if (changed) {
                acted = true;
                if (lava_cooloff != 0 && CELL_MATERIAL(s->cells[nat]) != before_mat &&
                    (int)(rng_next(&s->rng) & 0xFF) < lava_cooloff) {
                    place_reacted(s, x, y, at, rx->quench_to);
                    cool_off_chain(s, x, y, w, h, rx->quench_to, lava_cooloff);
                    return true;
                }
            }
        }
    }

    if (conduct_heat(s, x, y, w, h)) {
        acted = true;
    }

    if (try_flare(s, x, y, w, h, mat, reaction_of(grain)->flare)) {
        acted = true;
    }

    return acted;
}

/* Tiny chance for four cells to merge into one of reaction_t.condenses_to.
 * Ignores temp and surface. (x, y) marks top-left. Called per cell by
 * step_one_reacting_row(), scanning L-to-R, T-to-B. Other corners also
 * checked but no complete square found. */
static inline bool
step_one_condensing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at    = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r  = at + 1;
    const size_t at_d  = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t mat_id = CELL_MATERIAL(s->cells[at]);

    if (CELL_IS_EMPTY(s->cells[at_r]) || CELL_MATERIAL(s->cells[at_r]) != mat_id
        || CELL_IS_EMPTY(s->cells[at_d]) || CELL_MATERIAL(s->cells[at_d]) != mat_id
        || CELL_IS_EMPTY(s->cells[at_dr]) || CELL_MATERIAL(s->cells[at_dr]) != mat_id) {
        return false;
    }

    const int condenses = (s->condenses >= 0) ? s->condenses : r->condenses;
    if (condenses == 0 || (int)(rng_next(&s->rng) & 0xFF) >= condenses) {
        return false;
    }

    place_reacted(s, x, y, at, r->condenses_to);
    place_cell(s, x + 1, y, at_r, CELL_EMPTY);
    place_cell(s, x, y + 1, at_d, CELL_EMPTY);
    place_cell(s, x + 1, y + 1, at_dr, CELL_EMPTY);
    return true;
}

/* ACID RAIN - uses 2x2/4-cells-into-1 "fake collapse" like
 * step_one_condensing_cell(). Keyed on MAT_GAS and MAT_STEAM. Only 2-and-2
 * splits. Survivor resolves 50/50 to Acid or Water. Caller reports
 * FOUND_DISSOLVER | FOUND_MOISTURE | FOUND_CONDENSING on success. */
static inline bool
step_one_acid_rain_cell(sand_t* s, int x, int y, int w, int h) {
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at    = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r  = at + 1;
    const size_t at_d  = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t m0 = CELL_MATERIAL(s->cells[at]);
    const uint8_t m1 = CELL_MATERIAL(s->cells[at_r]);
    const uint8_t m2 = CELL_MATERIAL(s->cells[at_d]);
    const uint8_t m3 = CELL_MATERIAL(s->cells[at_dr]);

    if ((m0 != MAT_STEAM && m0 != MAT_GAS) || (m1 != MAT_STEAM && m1 != MAT_GAS)
        || (m2 != MAT_STEAM && m2 != MAT_GAS) || (m3 != MAT_STEAM && m3 != MAT_GAS)) {
        return false;
    }
    const int steam_count = (m0 == MAT_STEAM) + (m1 == MAT_STEAM)
                             + (m2 == MAT_STEAM) + (m3 == MAT_STEAM);
    if (steam_count != 2) {
        return false;
    }

    const int acid_rain = (s->acid_rain >= 0) ? s->acid_rain : SAND_ACID_RAIN_CHANCE;
    if (acid_rain == 0 || (int)(rng_next(&s->rng) & 0xFF) >= acid_rain) {
        return false;
    }

    /* A coin flip, not a certainty - see this function's own header
     * comment for why acid is no likelier than plain water here. */
    const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_ACID : MAT_WATER;
    place_reacted(s, x, y, at, residue);
    place_cell(s, x + 1, y, at_r, CELL_EMPTY);
    place_cell(s, x, y + 1, at_d, CELL_EMPTY);
    place_cell(s, x + 1, y + 1, at_dr, CELL_EMPTY);
    return true;
}

/* One row's scan sends only burning cells. Kind checks are skipped to avoid
 * treating static materials as heat sources. Non-reacting burning cells are
 * tracked in may_have_burning. Returns a bitmask to distinguish between
 * burning and dissolving cells, preventing incorrect board states. */
#define FOUND_BURNING     1u
#define FOUND_DISSOLVER   2u
#define FOUND_TEMPERATURE 4u
#define FOUND_MOISTURE    8u
#define FOUND_FALLER      16u
#define FOUND_WITHERING   32u
#define FOUND_CONDENSING  64u

/* THE REACTION-STAGE DISPATCH TABLE - a computed-goto walk of the ladder
 * below, skipping a row's statically-dead PREFIX instead of testing every
 * field in order. Water, oil and metal match none of the fifteen fields
 * and today walk all fifteen to find that out; this jumps them straight
 * to the end. */

/* The RSTAGE_* enum and reaction_first_stage() itself live in
 * sand_priv.h, not here - a host test needs to call
 * reaction_first_stage() directly to pin it against the walk below. */

/* Two tables, not one: the key is NOT the material nibble - root, leaf,
 * plant, ice and gunpowder share MAT_EXTENDED but carry sixteen
 * different rows apiece. material_first_stage[] keys CELL_MATERIAL(c);
 * extended_first_stage[] keys CELL_VARIANT(c), same split as
 * reaction_of(). */

/* Rebuilt once per PASS in sand_step_reactions(), beside pair_bits - see
 * that rebuild's comment for the full reasoning: attempt 12's per-CELL
 * mask went stale against a cell created mid-pass; a per-PASS rebuild
 * from the live tables does not. */
static uint8_t material_first_stage[MATERIAL_MAX];
static uint8_t extended_first_stage[MATERIAL_EXTENDED_CODES];

static unsigned
step_one_reacting_row(sand_t* s, int y, int w, int h) {
    uint8_t* row = s->cells + (size_t)y * (size_t)w;

    /* One indirect jump per cell, straight to the first stage this row
     * could ever match - see the dispatch-table comment above this
     * function. Everything from the landed label on is the unmodified
     * stage walk this function has always run. */
    static void* const stage_labels[RSTAGE_COUNT] = {
        &&stage_burn_any,  &&stage_burn_always, &&stage_burn_check, &&stage_dissolve, &&stage_acid_rain,
        &&stage_condense,  &&stage_heat_ramp,   &&stage_chill,      &&stage_warm,     &&stage_soak_dry,
        &&stage_fall,      &&stage_wither,      &&stage_drink,      &&stage_root,     &&stage_grow,
        &&stage_sprout,    &&stage_bud,         &&stage_end,
    };

    unsigned found = 0;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        /* The one decode branch reaction_of() already paid for, now also
         * yielding the stage byte - a second, separate branch here to
         * look up the stage table would spend exactly what this
         * dispatcher exists to save. */
        const reaction_t* r;
        uint8_t stage;
        if (CELL_MATERIAL(c) == MAT_EXTENDED) {
            const uint8_t variant = CELL_VARIANT(c);
            r = &extended_reactions[variant];
            stage = extended_first_stage[variant];
        } else {
            const uint8_t mat = CELL_MATERIAL(c);
            r = &reactions[mat];
            stage = material_first_stage[mat];
        }
        goto* stage_labels[stage];

    stage_burn_always:
        found |= FOUND_BURNING;
        step_one_burning_cell(s, row, x, y, w, h);
        continue;

    stage_burn_check:
        /* burns == 0 here, or dispatch would have landed above - so
         * cell_is_burning()'s own OR collapses to its second half. */
        if (cell_code(c) >= r->lit_from) {
            found |= FOUND_BURNING;
            step_one_burning_cell(s, row, x, y, w, h);
            continue;
        }
        /* Past the safe default, not into it: cell_is_burning() below is
         * provably false for a row that reached here (burns == 0, and the
         * lit_from test just failed), so falling through would cost every
         * unlit wood and gunpowder cell that test twice. */
        goto stage_dissolve;

    stage_burn_any:
        /* The safe default (RSTAGE_BURN_ANY == 0) lands here rather than
         * on stage_burn_always/stage_burn_check above - both skip half of
         * cell_is_burning()'s own test, and a slot that fell back from
         * one of those would misreport instead of just walking on. */
        if (cell_is_burning(c)) {
            found |= FOUND_BURNING;
            step_one_burning_cell(s, row, x, y, w, h);
            continue;
        }

    stage_dissolve:
        if (r->dissolves) {
            found |= FOUND_DISSOLVER;
            /* MAT_ACID specifically, not "anything that dissolves" - see
             * acid_bubble()'s own comment above for why this lives here.
             * A future second dissolver would not automatically want to
             * bubble too. */
            if (CELL_MATERIAL(c) == MAT_ACID) {
                acid_bubble(s, x, y);
            }
            step_one_dissolver_cell(s, row, x, y, w, h, r);
            continue;
        }
        /* ACID RAIN - see SAND_ACID_RAIN_CHANCE's own comment (sand.h).
         * MAT_GAS and MAT_STEAM are named directly here rather than
         * gated behind a reaction_t field of their own - the same
         * reasoning acid_bubble() above already gives for naming
         * MAT_ACID directly in the dissolves branch: this is inherently
         * about these two specific materials, not a trait a future
         * material could opt into. Checked ahead of the ordinary
         * condensing branch below since, with both windows now the same
         * 2x2 size, a window this fires on can never also satisfy
         * condensation (that needs four IDENTICAL cells; this needs a
         * 2-and-2 split) - the ordering costs nothing, it just has to
         * pick one.
         *
         * found |= ... on a hit is NOT optional the way it looks: the
         * survivor (Acid or Water) is written at (x, y), the walk's own
         * current position, and the walk never revisits a cell it has
         * already passed this pass - so unlike every other branch here,
         * nothing else is going to give the new cell its own turn to
         * report FOUND_DISSOLVER/FOUND_MOISTURE, and the steam this
         * collapse just consumed cannot report FOUND_CONDENSING either.
         * Skipping this - as an earlier version of this branch did -
         * left may_have_dissolver/may_have_moisture/may_have_condenser
         * all cleared at the end of THIS SAME pass, stranding the new
         * cell inert (no bubbling, no boiling, no further dissolving)
         * until something unrelated re-armed one of the three; measured
         * directly, a rained acid cell sealed against stone ate nothing
         * across 200 steps where an identical sand_set()-placed acid
         * cell in the same box ate normally. Over-arms by one bit when
         * the flip lands on Water rather than Acid (FOUND_DISSOLVER is
         * irrelevant to a water cell) - harmless, the same direction
         * step_one_condensing_cell()'s own FOUND_CONDENSING report below
         * already errs in (set before the roll is even known to hit). */
    stage_acid_rain:
        if (CELL_MATERIAL(c) == MAT_GAS || CELL_MATERIAL(c) == MAT_STEAM) {
            if (step_one_acid_rain_cell(s, x, y, w, h)) {
                found |= FOUND_DISSOLVER | FOUND_MOISTURE | FOUND_CONDENSING;
                continue;
            }
        }
        /* A steam cell that finds no complete square or rolls a miss still
         * exists and must be found again. This needs its own flag due to
         * may_have_temperature already being armed by `warms` and not
         * re-armed in boards with no heat-holder. */
    stage_condense:
        if (r->condenses != 0) {
            found |= FOUND_CONDENSING;
            if (step_one_condensing_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
        /* Only cells ALREADY holding heat need a turn. A cold pane is
         * heated from the fire's side by try_heat_transform(), the same as
         * any other neighbour of a flame, so the common case - a board full
         * of glass and one candle - walks past nearly all of it on a
         * variant test. */
    stage_heat_ramp:
        if (r->heat_ramp != 0) {
            if (CELL_VARIANT(c) != SAND_AMBIENT_HEAT && step_one_tempered_cell(s, row, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* A cold cell keeps the flag set whether or not it melts this
         * step: a drift on dry ground has nothing to do now and still has
         * to be found later, when a liquid reaches it. */
    stage_chill:
        if (r->chills != 0) {
            if (step_one_cold_cell(s, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* Hot gas warms what it touches if there's a heat holder.
         * `may_have_temperature` alone can't close this as smoke and steam
         * `warm` and re-arm it. Gated on `may_have_heat_holder` to check if
         * any heat_ramp cell exists, avoiding unnecessary neighbour scans on
         * boards with only gas and fire. */
    stage_warm:
        if (r->warms != 0 && s->may_have_temperature && s->may_have_heat_holder) {
            step_one_warming_cell(s, x, y, w, h, r);
            found |= FOUND_TEMPERATURE;
            continue;
        }
        /* Soaking and drying. Reached by sand and dirt, which are on most
         * boards, so the cheap tests come first: the field check, then
         * may_have_liquid inside, and only then a neighbour scan. */
    stage_soak_dry:
        if ((r->soaks != 0 || r->dries != 0) && step_one_soaking_cell(s, row, x, y, w, h, r)) {
            found |= FOUND_MOISTURE;
            continue;
        }
        /* Falling. First, because a seed still in the air has nothing to
         * grow from and no soil to look at - and because the cheapest
         * answer for everything else on the board is one zero field. */
    stage_fall:
        if (r->falls != 0) {
            /* Armed by cell EXISTING, not movement. A landed seed reported
             * nothing, clearing the flag and stopping the pass. Dissolve
             * ground with acid and plants hang in air, similar to snow on dry
             * ground bug. */
            found |= FOUND_FALLER;
            if (step_one_falling_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
        /* Withering. Not gated on may_have_moisture, deliberately: the
         * cells this is for are the ones with no water anywhere near
         * them, on boards that may have none at all. */
    stage_wither:
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
    stage_drink:
        if (r->drinks != 0 && s->may_have_liquid) {
            if (step_one_drinking_cell(s, x, y, w, h, r, c)) {
                found |= FOUND_MOISTURE;
            }
        }
        /* Root growth - PART 2, step_one_rooting_cell(). Gated on `c ==
         * r->roots_to`, not `r->roots` being nonzero, to exclude plant and
         * wood cells. */
    stage_root:
        if (r->roots != 0 && c == (cell_t)r->roots_to && s->may_have_moisture) {
            /* Conduct first, then eat: the level a root carries down this
             * step is the level the tip beneath it can grow into next. */
            if (step_one_conducting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
            if (step_one_rooting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
            continue;
        }
        /* Growing. Reached only where there is soil with water in it,
         * which is what may_have_moisture already tracks - a plant on dry
         * ground costs one field test and nothing else. */
    stage_grow:
        if (r->grows != 0 && s->may_have_moisture) {
            step_one_growing_cell(s, x, y, w, h, r);
            found |= FOUND_MOISTURE;
            continue;
        }
        /* Budding. Same gate as growing, and reached by unlit wood, which
         * falls through every branch above it. */
    stage_sprout:
        if (r->sprouts != 0 && s->may_have_moisture) {
            if (step_one_sprouting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
        }
        /* Budding, on the same gate. Reached by wood, which falls through
         * every branch above it. */
    stage_bud:
        if (r->buds != 0 && s->may_have_moisture) {
            if (step_one_budding_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
        }
    stage_end:;
    }
    return found;
}

/* Takes only `s` again. It briefly took (gx, gy) as well, for
 * a since-deleted surface walk to climb against gravity;
 * boiling now happens at the heat source instead and the steam finds its
 * own way up by bubbling, so nothing in this pass has any interest in
 * which way gravity points. */
void
sand_step_reactions(sand_t* s) {
    /* One tick of the fuse-blast cooldown per pass, before any cell gets a
     * turn - so the step a blast fires on is the step it starts waiting
     * from, so a cooldown of 1 lets the very next step blast again. */
    if (s->fuse_blast_wait != 0) {
        s->fuse_blast_wait--;
    }
    /* Dissolving, not a fire reaction, must run independently. Heat and
     * condensation are separate reasons to run, unrelated to fire. */
    if (!s->may_have_burning && !s->may_have_dissolver && !s->may_have_temperature && !s->may_have_moisture
        && !s->may_have_faller && !s->may_have_withering && !s->may_have_condenser) {
        return;
    }

    /* pair_bits, REBUILT HERE, EVERY PASS - see this file's own top comment
     * for the bit definitions and the honesty note on which ones are
     * genuinely pairwise. Same discipline s->heat_mask/s->wet_mask used to
     * document in this exact spot, unchanged by widening from two bits to
     * five: written once per PASS from the live tables, read only by
     * neighbour probes inside that same pass, never cached across steps -
     * this is NOT the retired per-cell "can this material react at all"
     * mask (docs/Sand/Performance-Tuning-Attempts.md, "Never retry",
     * attempt 12), which went stale against a cell created behind the scan
     * pointer in the SAME pass because it was written once per CELL. A
     * sand_set_* table override between steps is picked up on the very
     * next entry with nothing to invalidate.
     *
     * theirs_bits[] holds the five theirs-only bits (this file's own top
     * comment: everything except PAIR_DENSER, which is not stored at all)
     * per ordinary material id, exactly the two loops heat_mask/wet_mask
     * used to run separately, now merged into one pass over reactions[]
     * plus the same MAT_EXTENDED aggregate heat_mask always needed -
     * PAIR_IGNITABLE and PAIR_DISSOLVABLE both need it too, since plant/
     * leaf/metal (extended_reactions[]) carry real flammability/dissolvable
     * figures the way ice's heats_to always did. PAIR_WETS and
     * PAIR_QUENCHES need no such pass: every extended material shares
     * materials[MAT_EXTENDED].kind == KIND_STATIC (material.c; see
     * material.h's own comment on the one shared physics row), so neither
     * bit can ever be true for MAT_EXTENDED, exactly what the tests they
     * replace already decided.
     *
     * The final nested loop broadcasts theirs_bits[] into every row of
     * pair_bits[][] - all sixteen rows read identically for a
     * pair_theirs_bits() caller, which is the whole point (this file's own
     * top comment on pair_theirs_bits()). At sixteen materials this is
     * O(256), one to two cache lines of reactions[]/materials[] read once
     * per pass rather than once per PROBE - cheaper than the single
     * reaction_of() load any one of the five bits replaces on the very
     * first neighbour it is asked about. */
    uint8_t theirs_bits[MATERIAL_MAX] = {0};
    for (int m = 1; m < MAT_COUNT; m++) {
        const reaction_t* r = &reactions[m];
        /* `melts` opens the bit too: a material lava alone can change has
         * to reach the burning walk at all, and this bit is that walk's
         * front door. Fire paying one cheap, RNG-free probe per such
         * neighbour (try_heat_transform_given() rejects it at its
         * heat_chance gate before rolling) is the price. */
        if (r->heat_ramp != 0 || (r->heats_to != 0 && (r->heat_chance != 0 || r->melts != 0))) {
            theirs_bits[m] |= PAIR_HEAT_RESPONSIVE;
        }
        if (material_by_id((material_id_t)m)->kind == KIND_LIQUID) {
            if (r->wets != 0) {
                theirs_bits[m] |= PAIR_WETS;
            }
            if (r->flammability == 0 && r->burns == 0) {
                theirs_bits[m] |= PAIR_QUENCHES;
            }
        }
        if (r->flammability != 0) {
            theirs_bits[m] |= PAIR_IGNITABLE;
        }
        if (r->dissolvable != 0) {
            theirs_bits[m] |= PAIR_DISSOLVABLE;
        }
    }
    /* MATERIAL_EXTENDED_CODES (16), not _COUNT (8): codes in nibble 15 use
     * theirs_bits[MAT_EXTENDED]. PROBE knows "MAT_EXTENDED", not specific
     * half. Gunpowder's row is active but widening is safe.
     * theirs_bits[MAT_EXTENDED] ORs across nibble; STATICS set all bits this
     * loop can produce: ice (PAIR_HEAT_RESPONSIVE), plant/leaf
     * (PAIR_IGNITABLE), plant/leaf/metal (PAIR_DISSOLVABLE). Gunpowder's OR
     * does not change existing bits. */
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        const reaction_t* r = &extended_reactions[k];
        if (r->heat_ramp != 0 || (r->heats_to != 0 && (r->heat_chance != 0 || r->melts != 0))) {
            theirs_bits[MAT_EXTENDED] |= PAIR_HEAT_RESPONSIVE;
        }
        if (r->flammability != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_IGNITABLE;
        }
        if (r->dissolvable != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_DISSOLVABLE;
        }
    }
    for (int mine = 0; mine < MATERIAL_MAX; mine++) {
        for (int theirs = 0; theirs < MATERIAL_MAX; theirs++) {
            pair_bits[mine][theirs] = theirs_bits[theirs];
        }
    }

    /* material_first_stage[]/extended_first_stage[] - REBUILT HERE, EVERY
     * PASS, the same discipline as pair_bits just above: written fresh
     * from reactions[]/extended_reactions[] every pass, read only by
     * step_one_reacting_row() within that same pass, never cached across
     * steps. */
    for (int m = 0; m < MAT_COUNT; m++) {
        const bool is_acid_rain_material = (m == MAT_GAS || m == MAT_STEAM);
        material_first_stage[m] = reaction_first_stage(&reactions[m], is_acid_rain_material);
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        extended_first_stage[k] = reaction_first_stage(&extended_reactions[k], false);
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
    if (!(found & FOUND_CONDENSING)) {
        s->may_have_condenser = false;
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
