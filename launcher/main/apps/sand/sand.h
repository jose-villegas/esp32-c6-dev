/*=============================================================================
 * sand - a falling-sand cellular automaton.
 *
 * Pure logic. It knows nothing about the panel, the accelerometer or the frame
 * loop: the caller passes in a grid to work on and a gravity vector, and gets
 * grains moved around. That is what lets the whole simulation be tested on a
 * host machine, where a "frame" costs microseconds and the grid can be four
 * cells wide.
 *
 * The grid is caller-owned. On this board the framebuffer already claims 322 of
 * ~424 KiB, so the app decides how coarse the grid must be to fit in what is
 * left - see apps/app_sand.c.
 *
 * Coordinates follow the screen: x grows right, y grows DOWN. So ordinary
 * gravity is (0, +1).
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "material.h"
#include "util/rng.h"

/* See material.h for cell encoding. Variant travels with the cell, not the
 * position, to prevent shimmering in a moving pile. */
#define SAND_EMPTY        CELL_EMPTY
#define SAND_SHADE_COUNT  MATERIAL_VARIANTS
#define SAND_FIRST_SHADE  CELL_MAKE(MAT_SAND, 0)
#define SAND_LAST_SHADE   CELL_MAKE(MAT_SAND, MATERIAL_VARIANTS - 1)

/* Cells per block, on each axis, for the settled-block tracking behind
 * sand_enable_sleeping() - see the comment there. 32x64 is the only shape,
 * of several measured on real hardware, that clears the settled-screen
 * frame budget - see docs/Sand/Simulation-Lessons.md for the sweep. */
#define SAND_BLOCK_W 32
#define SAND_BLOCK_H 64

/* How many persistent emitters sand_t can carry - see sand_add_emitter(). A
 * fixed cap keeps the list a small inline array; 16 is more taps than this
 * board can usefully tell apart as distinct streams. */
#define SAND_MAX_EMITTERS 16

/* One grain in flight from sand_impulse() - not explosion-specific despite
 * sand_explode() being the one caller today. `cell` is the exact byte
 * thrown, checked before moving so a stale entry (its cell touched since by
 * something else) drops rather than flying whatever is there now. `speed`
 * is both this turn's chance in 256 of moving and how much flight is left
 * (see SAND_IMPULSE_SPEED_RAMP); `ramp` is a per-entry override of its decay
 * rate (see sand_impulse_dislodge()), ignored for water/acid. */
typedef struct {
    uint16_t index;   /* y*w+x */
    cell_t   cell;
    uint8_t  dir;     /* ring_dir() index, sand_priv.h */
    uint8_t  speed;
    uint8_t  ramp;
} impulse_t;

typedef struct {
    uint8_t *cells;      /* w * h, row-major, caller-owned */
    int      w, h;
    rng_t    rng;        /* seeded explicitly, so every run repeats exactly */
    /* Drifts the shade band random_cell() spawns with, so two separate pours
     * read as two shades rather than one flat fill. */
    uint32_t pour_phase;

    bool     sweep_flip; /* alternates the sweep direction between steps */
    bool     liquid_flip;/* alternates which way liquids share sideways */
    bool     gas_flip;   /* same idea as liquid_flip, for sand_step_gas()'s
                           * own spread pass - kept separate so gas's
                           * alternation isn't coupled to whether water also
                           * moved this step */

    /* Whether the grid might hold any liquid at all. Conservative: set the
     * moment a liquid is placed, and only ever cleared by a pass that has
     * looked everywhere and found none. When it is false the whole cross-flow
     * pass is skipped, so a screen of sand never pays for water. */
    bool     may_have_liquid;

    /* Same idea as may_have_liquid, for gas - see sand_step_gas() in
     * sand_gas.c. */
    bool     may_have_gas;

    /* For anything that burns - see sand_step_reactions() in sand_reactions.c.
     * Keyed on reaction_t.burns, not kind == KIND_STATIC: stone shares that
     * kind with ember and is poured often. */
    bool     may_have_burning;

    /* Same idea again, for a material that dissolves others
     * (reaction_t.dissolves - acid is the only one today). Separate from
     * may_have_burning because dissolving is not a fire reaction: acid
     * has to work on a board with no flame anywhere, so
     * sand_step_reactions() runs when EITHER flag is set. */
    bool     may_have_dissolver;

    /* For cells with a TEMPERATURE (glass, snow) - own flag since cooling
     * duration needs tracking independent of fire. */
    bool     may_have_temperature;

    /* Moisture-holding cells or soakers near liquid are armed. "A soaker
     * exists" alone does not arm it. Sand soaks but is common, so
     * step_one_reacting_row() checks actual wetness or adjacency to liquid. */
    bool     may_have_moisture;

    /* Reactions pass may run on a board with no fire, acid, heat, or water if
     * a seed falls from the screen to the floor - see reaction_t.falls. */
    bool     may_have_faller;

    /* Checks if grid has heat-interactive elements (e.g., stone, glass, ice)
     * to gate convection in step_one_reacting_row(). Not a pass gate but a
     * branch gate within the pass. Needed to prevent scanning four neighbours
     * for every gas cell on smoke/steam-only boards. */
    bool     may_have_heat_holder;

    /* Own flag for withering, like may_have_dissolver and
     * may_have_temperature. Plant was the only material that both fell and
     * withered. Foliage never falls, so it arms nothing above, but it does
     * wither, requiring the pass to visit it. Leaf on a bare board would be
     * skipped forever. */
    bool     may_have_withering;

    /* Condensing flag for steam; unlike boiling, it's independent and must
     * persist. Without it, steam would never condense if no other reactions
     * occur. */
    bool     may_have_condenser;

    /* See sand_set_soak(). 0, the default, means nothing soaks. */
    int      soak;

    /* Steps before another fuse blast may fire: set to cooldown on explosion,
     * ticked down each reactions pass. Cross-step state for fuse model,
     * bounds burst cost per frame. See SAND_GUNPOWDER_BLAST_COOLDOWN in
     * sand_reactions.c and sand_set_fuse_cooldown() for details. */
    uint8_t  fuse_blast_wait;

    int      fuse_cooldown; /* see sand_set_fuse_cooldown() */

    /* Optional, caller-owned, h bytes: which rows changed since it was last
     * cleared. NULL disables tracking entirely. See sand_track_dirty_rows(). */
    uint8_t *dirty_rows;

    /* Optional, caller-owned, block_cols*block_rows bytes: which blocks of
     * the grid to consider. NULL means check every block. Always walks grid
     * in blocks of SAND_BLOCK_W x SAND_BLOCK_H (see step_one_row()).
     * block_cols/block_rows are sand_init()-derived values, never zero. See
     * sand_enable_sleeping(). */
    uint8_t *block_state;
    int      block_cols, block_rows;   /* set once, by sand_init() */

    /* Caller-owned `impulse_max` entries: grains in flight from
     * sand_impulse(). NULL disables the mechanic. `impulse_count` tracks live
     * entries in `impulse_buf`, always <= `impulse_max`. */
    impulse_t *impulse_buf;
    int        impulse_max;
    int        impulse_count;

    /* Decaying trigger chance for splash_displace() (sand_liquid.c) - see
     * SAND_SPLASH_RADIUS_WATER's own comment above for why this lives
     * per-instance. WATER only - acid does not use splash_displace() at
     * all any more, see acid_bubble()'s own comment in sand_liquid.c. */
    uint8_t    splash_chance;

    /* Decaying splash RADIUS, water only, alongside splash_chance above -
     * see SAND_SPLASH_RADIUS_WATER's own comment for why these are two
     * independent decays (whether a bounce splashes at all, versus how
     * big the ones that do are) rather than one. */
    uint8_t    splash_radius_water;

    /* ROLLING-MODULO CLUMP in reaction_t.flaw_to (material.h) for
     * try_heat_transform() mechanism. Shared across materials setting flaw_to
     * (dirt today). heat_flaw_seq counts triggers; heat_flaw_is_flawed
     * decides run of HEAT_FLAW_CLUMP. */
    uint16_t heat_flaw_seq;
    bool     heat_flaw_is_flawed;

    int      last_load_dx, last_load_dy;

    /* The dithered direction, unlike last_load_d{x,y}'s nearest direction -
     * see sand_gravity_direction_dithered(). */
    int      last_step_dx, last_step_dy;

    int      scatter;      /* see sand_set_scatter() */
    int      decay;        /* see sand_set_decay() */
    int      evaporates;   /* see sand_set_evaporates() */
    int      mobility;     /* see sand_set_mobility() */
    int      flammability; /* see sand_set_flammability() */
    int      conduction;   /* see sand_set_conduction() */
    int      boils;        /* see sand_set_boils() */
    int      condenses;    /* see sand_set_condenses() */
    int      lava_cooloff; /* see sand_set_lava_cooloff() */
    int      lava_burst;   /* see sand_set_lava_burst() */
    int      acid_rain;    /* see sand_set_acid_rain() */
    int      acid_dilute_mass_bias; /* see sand_set_acid_dilute_mass_bias() */

    /* Persistent point sources via sand_add_emitter() - stepped once per
     * sand_step() by emit_from_emitters() (sand.c), which writes through
     * sand_spawn_cell() so an emitter reads exactly like an ordinary pour. */
    struct {
        int16_t x, y;
        cell_t  cell;
    } emitters[SAND_MAX_EMITTERS];
    int      emitter_count;
} sand_t;

/* `cells` must have room for w * h bytes and is cleared. */
void sand_init(sand_t *s, uint8_t *cells, int w, int h, uint32_t seed);

void sand_clear(sand_t *s);

/* Record rows in caller-owned `h` array. Opt-in; useful if acted upon. Caller
 * avoids redrawing unchanged rows and sending unchanged bands, saving frame
 * cost. See gfx_mark_dirty(). Rows set to 1 and never cleared here; clearing
 * is caller's responsibility. Functions marking changes: settling, spawning,
 * sand_set, sand_clear. */
void sand_track_dirty_rows(sand_t *s, uint8_t *rows);

/* Skip settled BLOCKS entirely - without this, a settled grain still fails
 * its gravity-ward move, draws a random number and fails both slides, every
 * step, to conclude nothing; see docs/Sand/Simulation-Lessons.md.
 *
 * `blocks` is caller-owned, ceil(w/SAND_BLOCK_W) * ceil(h/SAND_BLOCK_H)
 * bytes, one flag per block. NULL disables sleeping. A shake, a gravity
 * change, or sand landing in a block wakes it. */
void sand_enable_sleeping(sand_t *s, uint8_t *blocks);

/* Diagnostic: whether block (bx, by) is settled under dithered direction. Bit
 * layout private to sand.c/sand_priv.h. Used by app_sand.c's count_awake().
 * False if block-sleeping disabled. */
bool sand_block_settled(const sand_t *s, int bx, int by);

/* Caller-owned `buf` holds up to `max` in-flight impulse_t entries (not
 * per-cell - one explosion can queue hundreds). NULL disables the mechanic
 * entirely, making sand_impulse()/sand_explode() no-ops. */
void sand_enable_impulses(sand_t *s, impulse_t *buf, int max);

/* Out-of-bounds reads return STONE to make walls solid without bounds checks.
 * Stone is used because walls must be dense and non-displaceable. */
cell_t sand_at(const sand_t *s, int x, int y);

/* Ignores out-of-bounds writes. */
void sand_set(sand_t *s, int x, int y, cell_t cell);

int sand_count(const sand_t *s);

/* Fill a disc with `material`, at random variants. Returns how many cells it
 * filled, which is less than the disc's area when it overlaps the edge or
 * anything already there. */
int sand_spawn(sand_t *s, int cx, int cy, int radius, material_id_t material);

/* Paints a whole CELL. Extended materials use MATX() for identity, cannot use
 * material_id_t. For ordinary materials, use CELL_MAKE(id, 0); variant is
 * ignored. */
int sand_spawn_cell(sand_t *s, int cx, int cy, int radius, cell_t spec);

/* Remove all grains and emitters in a disc. Returns number of CELLS removed.
 * Not a mirror of sand_spawn, which only places grains. Also switches off any
 * emitter in the disc to prevent a trap. Emitter removal is not included in
 * the count. */
int sand_erase(sand_t *s, int cx, int cy, int radius);

/* EMITTERS: grid points producing materials continuously. See `emitters`
 * field in `sand_t` and `emit_from_emitters()` in `sand.c`. */

/* Place or replace an emitter. Returns false if list is at SAND_MAX_EMITTERS
 * or (x, y) is off grid. An existing emitter at (x, y) is replaced. `cell` is
 * written exactly as given, like `sand_spawn_cell()`'s `spec`. */
bool sand_add_emitter(sand_t *s, int x, int y, cell_t cell);

/* Remove every emitter within `radius` of (cx, cy) - the same disc test
 * sand_erase() uses, so "within radius" means the same thing in both
 * places. Returns how many were removed. Does not touch the grid itself:
 * whatever an emitter already placed stays put, only the tap stops. */
int sand_remove_emitters(sand_t *s, int cx, int cy, int radius);

int sand_emitter_count(const sand_t *s);

/* Read emitter `i` back - for drawing a marker at it, say. False, leaving
 * the outputs untouched, if `i` is not currently a valid emitter index. */
bool sand_emitter_at(const sand_t *s, int i, int *x, int *y, cell_t *cell);

/* Queue one grain at (x, y) for the flight pass at the tail of sand_step()
 * (step_impulses(), sand.c - must run last, after anything else that can
 * move or replace a cell). No-op if disabled, off-grid, already empty, the
 * buffer is full, or the cell is KIND_STATIC - a wall is never thrown
 * through this function, by any caller; sand_explode() and
 * sand_impulse_dislodge() (below) each dislodge a static cell their own
 * separate, explicit way instead. */
void sand_impulse(sand_t *s, int x, int y, int dir, int speed);

/* A single-cell push that bypasses the density-scaled toughness roll for
 * guaranteed dislodgement of KIND_STATIC targets. See queue_flying_grain() in
 * sand.c for details. `ramp` is the entry's speed decay; use
 * SAND_IMPULSE_SPEED_RAMP for standard decay or a custom value for different
 * throw distances. */
void sand_impulse_dislodge(sand_t *s, int x, int y, int dir, int speed,
                           int ramp);

/* How much `speed` (impulse_t) loses every step - a linear ramp, matching
 * this file's other chance-in-256 rates. Because gravity itself never
 * accelerates (a falling grain drops exactly one cell a step), decaying only
 * the horizontal push is what turns a queued throw into a visible ARC rather
 * than a straight line - shallow at first, steepening as `speed` runs out. */
#define SAND_IMPULSE_SPEED_RAMP  2

/* How many cells one successful push-roll moves: 1 + speed / this, uncapped.
 * Before this existed every displacing move covered one cell per roll, the
 * same rate gravity falls at, so a thrown grain could never outrun its own
 * fall. 104 gives a full-speed (255) entry 3 cells and a near-spent one
 * still exactly 1 - see suite_sand_impulse.c's divisor tests. No separate
 * cap: the result never exceeds 3 at this value. */
#define SAND_IMPULSE_CELLS_PER_STEP_DIVISOR  104

/* EXTRA speed charged per non-empty cell a KIND_STATIC/KIND_POWDER mover
 * displaces, on top of density-based drag (impulse_drag_of(), sand_priv.h) -
 * `density << this`, KIND_POWDER only. Density alone gives every solid the
 * same drag shape, which is wrong: packed grain jams a mover, a liquid
 * MEDIUM barely slows one, same as a liquid MOVER always has. Keeps a
 * full-speed chunk stopping near a bank's rim, not tunnelling through - see
 * test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank. */
#define SAND_IMPULSE_DRAG_POWDER_SHIFT  2

/* Below this post-drag speed a KIND_STATIC entry is SPENT: its unconditional
 * gravity-drift (step_impulses()) may only enter an empty cell, not swap
 * through an occupant - the drift pays no drag, ever, so without this floor
 * a spent chunk swaps down through a whole bank, never stopping. 1 means
 * "speed is zero" (`speed` saturates, never wraps). Not kind-aware: a spent
 * chunk also rests mid-liquid rather than sinking - a deliberate trade-off. */
#define SAND_IMPULSE_SINK_MIN_SPEED  1

/* RESTITUTION FLOOR for the wall-bounce (step_impulses()'s blocked branch) -
 * below this a blocked entry just waits; above it, it reflects off the
 * blocking surface's approximate normal and pays restitution for the
 * privilege. Not a polish knob: unfloored, undamped, a piece could keep
 * finding just enough energy to bounce forever instead of settling. Not
 * lowered further regardless: speed is also the per-step move chance, so an
 * entry too far under this just crawls instead of ricocheting. */
#define SAND_IMPULSE_BOUNCE_MIN_SPEED  32

/* TRANSFER - what a struck cell inherits from the mover displacing it
 * (step_impulses(), the move site), so struck material can fly clear of a
 * bank. Direction is a backward cone (dir+3/4/5): along the mover's own
 * heading was tried first and just drove struck material deeper in. This
 * constant is how much of the ARRIVAL speed the child keeps, in 256ths, not
 * a divisor - below 256 keeps a strike from minting energy; 213 is a device
 * figure. */
#define SAND_IMPULSE_TRANSFER_KEEP  213

/* Below this post-drag speed, a mover does not queue a transfer at all - a
 * nearly-spent mover's transfer would be too faint to ever visibly move,
 * the same shape SAND_IMPULSE_BOUNCE_MIN_SPEED floors restitution at. This
 * is the cheap first gate; the real budget protection against a long plow
 * queuing one transfer per cell is SAND_CASCADE_TRANSFER_MAX_PER_STEP
 * (this file). */
#define SAND_IMPULSE_TRANSFER_MIN_SPEED  64

/* sand_explode()'s own choice of speed to hand every entry it queues - not a
 * property of sand_impulse() itself, which just takes speed as a parameter.
 * At 255, the uint8_t ceiling: there is nowhere left to raise it, so a
 * future round wanting more reach has to retune SAND_IMPULSE_SPEED_RAMP or
 * the blast radius instead. */
#define SAND_EXPLODE_INITIAL_SPEED  255

/* Bounds a single cool_off_chain() walk, independent of rolls. Smaller scale
 * than CRACK_MAX. Prevents a single lucky roll from emptying a pool, avoiding
 * "dry bowl paving" failure. PUBLIC; named in Reaction-Table.md and used in
 * tests. Chain frequency controlled by SAND_LAVA_COOLOFF_CHANCE. */
#define SAND_LAVA_COOLOFF_MAX_CHAIN 8

/* Radius and decaying trigger CHANCE for splash_displace() (sand_liquid.c) -
 * a WATER grain landing hard throws a small splash, exaggerated past a real
 * splash's reach to read clearly here. WATER ONLY, NOT ACID - see
 * acid_bubble() in sand_liquid.c. A grain landing back in the liquid would
 * otherwise re-trigger the call, so CHANCE and RADIUS_WATER both step down
 * on every trigger, independently, settling a bounce chain rather than
 * rattling on. Neither recovers; both are per-sand_t, not globals. */
#define SAND_SPLASH_RADIUS_WATER       20
#define SAND_SPLASH_RADIUS_WATER_FLOOR 2
#define SAND_SPLASH_RADIUS_WATER_STEP  8
#define SAND_SPLASH_CHANCE_START       255
#define SAND_SPLASH_CHANCE_FLOOR       24
#define SAND_SPLASH_CHANCE_STEP        140

/* How fast a WATER/ACID impulse's `speed` decays per CELL OF TRAVEL
 * (impulse_decay(), sand.c) - a right-shift, not the linear
 * SAND_IMPULSE_SPEED_RAMP every other material uses. BIGGER SHIFT MEANS
 * SLOWER DECAY, NOT FASTER: `speed -= speed >> SHIFT`, so 1 leaves half
 * per cell, 2 leaves 3/4 - easy to get backwards (a stale comment here
 * once did). Scoped to water/acid only. */
#define SAND_SPLASH_SPEED_DECAY_SHIFT  2

/* CASCADE - a WATER/ACID impulse that moves relays its push into the SAME
 * material one step BEHIND where it started, so connected liquid moves as a
 * chain rather than one grain flying off alone. Behind, not ahead: the cell
 * ahead is close to definitionally open (that is why the move just
 * succeeded), so relaying there finds nothing. Each hop's speed is the last
 * one's divided by DIVISOR; MIN_SPEED gates whether a hop even queues, kept
 * at 1 so a roll's own exhaustion, not this gate, stops a chain. */
#define SAND_CASCADE_SPEED_DIVISOR 2
#define SAND_CASCADE_MIN_SPEED     1

/* SHARED DEFERRED ARRAY SIZE (step_impulses(), sand.c) - Holds CASCADE (max
 * once per water/acid entry) and TRANSFER (max once per HOP, up to
 * SAND_IMPULSE_CELLS_PER_STEP_DIVISOR). Single counter gates both, causing
 * starvation. Reserve for RELAY avoids this. Revisit reserve if needed. */
#define SAND_CASCADE_MAX_PER_STEP  64
/* RESERVATION, not split. Transfer fires once per hop, relay once per liquid
 * entry. Half-and-half prevents transfer starvation but shrinks lone
 * mechanisms. Relay gets floor, not ceiling. Transfer fills to everything
 * except SAND_CASCADE_RELAY_RESERVE; relay takes remainder. Single-mechanism
 * scenes untouched, relay cannot starve below reserve. */
#define SAND_CASCADE_RELAY_RESERVE          16
#define SAND_CASCADE_TRANSFER_MAX_PER_STEP  (SAND_CASCADE_MAX_PER_STEP - \
                                             SAND_CASCADE_RELAY_RESERVE)

/* See acid_bubble() (sand_reactions.c). SPEED stays well below
 * SAND_EXPLODE_INITIAL_SPEED so a bubble reads smaller than a splash;
 * range is logarithmic in this value (the fine adjustment - see
 * SAND_SPLASH_SPEED_DECAY_SHIFT for the coarse one, shared with water).
 * CHANCE governs how many bubbles fire, not how high they arc. */
#define SAND_ACID_BUBBLE_CHANCE 40
#define SAND_ACID_BUBBLE_SPEED  180

/* DILUTION - water touching acid rolls to decide who wins (the winner boils
 * into vapour, the loser converts to the winner's material - see the ladder
 * in step_one_dissolver_cell()). Chance-in-256 that WATER wins, before
 * SAND_ACID_DILUTE_MASS_BIAS adjusts for local backing. 118, not 128:
 * SAND_ACID_DILUTE_EVAPORATE_CHANCE's 20-in-256 comes off the roll first, so
 * 118 is half of the 236 left (20+118+118=256) - an even split, with any
 * lean left to the mass bias. */
#define SAND_ACID_DILUTE_TO_WATER_CHANCE 118

/* MASS MATTERS - a fixed-split roll alone can't tell a drop of acid on a
 * lake from a poured slab, so step_one_dissolver_cell() counts each side's
 * cardinal same-material neighbours (0-3) and adjusts
 * SAND_ACID_DILUTE_TO_WATER_CHANCE by the DIFFERENCE, not either count
 * alone - measuring one side lets a deep pool always read "backed"
 * regardless of its neighbour. Known asymmetry, not fixed here: density
 * makes acid disperse when poured on water but not the reverse. */
#define SAND_ACID_DILUTE_MASS_BIAS 24

/* EVAPORATION ON DILUTION - a flat roll in step_one_dissolver_cell(),
 * sand_reactions.c, gives acid/water bites a chance to boil acid into
 * MAT_GAS. Chance-in-256, checked before SAND_ACID_DILUTE_MASS_BIAS. Distinct
 * from r->evaporates and the win/lose split's mass sink. */
#define SAND_ACID_DILUTE_EVAPORATE_CHANCE 20




/* OIL BOILS OFF, NOT MORE ACID - bitten oil mostly turns to MAT_GAS
 * (SAND_ACID_OIL_TO_GAS_CHANCE) or dies (SAND_ACID_OIL_DEATH_CHANCE). Acid
 * eating oil can consume itself, not breed more. */
#define SAND_ACID_OIL_TO_GAS_CHANCE 220
#define SAND_ACID_OIL_DEATH_CHANCE  128

/* Sand/wood/stone fall through to one shared branch of
 * step_one_dissolver_cell() paying only a flat one-unit chip per bite, so a
 * freely dissolvable material let a full acid cell land many bites before
 * running out, reading as barely spending itself for how much it destroys.
 * Same fix as oil's SAND_ACID_OIL_DEATH_CHANCE (a further roll to die
 * outright), picked lower than oil's 128 because sand/wood/stone dissolve
 * far more readily - oil's rate here would kill acid on nearly every bite. */
#define SAND_ACID_EAT_DEATH_CHANCE 40

/* ACID RAIN - a 2x2 block of exactly two MAT_GAS and two MAT_STEAM cells has
 * a small chance to collapse to one cell, 50/50 Acid or Water (see
 * step_one_acid_rain_cell()) - an extension of the steam-to-water
 * condensation trick, sized the same way. Only the corner survives: acid is
 * a big gas/steam producer, so a full-count yield would keep a lake topped
 * up indefinitely. 1 is already the floor a byte-wide roll allows; a pocket
 * reading too frequent needs a mechanism change, not this number. */
#define SAND_ACID_RAIN_CHANCE 1

/* QUENCHING A FLAME - Acid snuffs fire unlike water's crisp steam. Two rolls:
 * "visible effect?" then "what is it?" SAND_ACID_QUENCH_RESIDUE_CHANCE
 * (1/256) checks for residue. Miss means clean quench.
 * SAND_ACID_QUENCH_SMOKE_CHANCE (1/256) checks if residue is smoke, likely.
 * Tune chances as needed. */
#define SAND_ACID_QUENCH_RESIDUE_CHANCE 96
#define SAND_ACID_QUENCH_SMOKE_CHANCE   180

/* How much of the blast radius sand_explode() fills with fire first: filled
 * radius is `radius / SAND_EXPLODE_CORE_DIVISOR`. Needed because a packed
 * medium blocks every queued entry's first move; fire, not a hole, since
 * being lighter than nearly anything it detonates into lets the medium
 * swap through via the ordinary density rule. Kept small relative to the
 * disc, clamped to a minimum core_radius of 1 for radius >= 2 - a
 * zero-radius core cannot seed the density-swap collapse it depends on. */
#define SAND_EXPLODE_CORE_DIVISOR  5

/* How many of fire's sixteen shades a blast core sheds from centre to rim -
 * see fill loop in sand_explode() (sand.c). 8 ensures clear gradient, keeping
 * outer ring alight. Raising past MATERIAL_VARIANTS - 2 is redundant; floor
 * of 1 clamps it. 0 restores flat disc. */
#define SAND_EXPLODE_CORE_FADE     8

/* THE PURE DISPLACEMENT PRIMITIVE, NO MATERIAL CONVERSION - split from
 * sand_explode() so a caller wanting the push without fire has somewhere to
 * call. Queues an outward entry per occupied annulus cell, in RING order so
 * an undersized buffer degrades to a smaller, even disc, not a lopsided
 * crescent. Every cell is seeded, not sampled - sparse seeding measured
 * worse, an unseeded cell just sitting still. KIND_STATIC can be dislodged
 * here via a density-scaled chance, unlike sand_impulse()'s refusal. */
void sand_displace(sand_t *s, int cx, int cy, int radius);

/* Same as sand_displace(), but only cells whose material is exactly
 * `mat_id` are ever queued - see its own comment in sand.c. Used by
 * splash_displace() (sand_liquid.c) so a liquid's splash cannot fling
 * unrelated material (dirt under a pool of water, say) along with it. */
void sand_displace_material(sand_t *s, int cx, int cy, int radius,
                            uint8_t mat_id);

/* A THIN WRAPPER AROUND sand_displace(), ABOVE - fills a core of `radius /
 * SAND_EXPLODE_CORE_DIVISOR` with fire, then hands the rest to
 * sand_displace(). A no-op if sand_enable_impulses() was never called - the
 * core is left unfilled too. CONSERVATION IS BOUNDED, NOT EXACT, unlike
 * sand_displace() alone: filling an empty core cell with fire is a real
 * increase, exactly once - the count can fall further afterwards, but never
 * exceeds (count before) + (empty cells the core filled). */
void sand_explode(sand_t *s, int cx, int cy, int radius);

/* Chance in 256 that a grain with exactly one grain above it may still
 * slide; halves for each additional grain, so a pile locks up quickly with
 * depth. Falling itself is not affected by burial. */
#define SAND_SLIP_CHANCE 96

/* Beyond this much load a grain cannot slide at all. Without a hard floor the
 * chance only ever approaches zero, and at 60 steps a second "almost never"
 * still visibly creeps. */
#define SAND_LOAD_CAP 5

/* How far a liquid seeks a shallower spot non-locally - local rules alone
 * level a pool far too slowly (a 184-cell pool: ~5k steps for a 6-cell
 * height difference) for water pressure to read as believable. A larger
 * sight levels faster but leaves a wider band looking momentarily uneven.
 * Flow halts at a non-matching liquid. */
#define SAND_LIQUID_SIGHT 8

/* sand_step_gas() now uses per-material sight (material.h's `sight` field)
 * instead of a global constant. Fire, sharing KIND_GAS, requires a tighter
 * dispersal than gas. See tuned figures in material.c for details. */


/* Cells stacked against gravity above (x, y), capped at SAND_LOAD_CAP. (dx,
 * dy) is gravity direction. Exposed for friction model: off-grid counts as
 * sky, not solid. */
int sand_load_above(const sand_t *s, int x, int y, int dx, int dy);

/* Grain fall randomness (1/256) to mimic real sand dispersion. Zero makes it
 * deterministic. Scattered grains either lag or drift. Uses
 * SAND_SCATTER_PER_MATERIAL for material-specific values. Any other value
 * overrides. */
void sand_set_scatter(sand_t *s, int chance);
#define SAND_SCATTER_PER_MATERIAL (-1)

/* Material life ticks down by a chance in 256, controlled by `decay` in
 * material.h. Zero makes materials immortal, defaulting off for test
 * consistency. Use SAND_DECAY_PER_MATERIAL for material-specific decay or
 * another value for specific test decay. */
void sand_set_decay(sand_t *s, int chance);
#define SAND_DECAY_PER_MATERIAL (-1)

/* How often a cell turns into MAT_GAS, as a chance in 256. Zero turns it off.
 * Pass SAND_EVAPORATES_PER_MATERIAL to use each material's figure, or any
 * other value to override. */
void sand_set_evaporates(sand_t *s, int chance);
#define SAND_EVAPORATES_PER_MATERIAL (-1)

/* Soaking: sand absorbs liquid. Off by default like decay. Tests assume sand
 * sinks based on density, not chemistry. Enable by passing
 * SAND_SOAK_PER_MATERIAL. */
void sand_set_soak(sand_t *s, int chance);
#define SAND_SOAK_PER_MATERIAL (-1)

/* Flame catch chance per adjacent burning cell per step, 1 in 256.
 * Flammability in material.h's reaction_t. Defaults to material-specific
 * value (SAND_FLAMMABILITY_PER_MATERIAL), unlike scatter/decay (default OFF).
 * Ignition only occurs next to burning cells. Tests can override for specific
 * catch rates. */
void sand_set_flammability(sand_t *s, int chance);
#define SAND_FLAMMABILITY_PER_MATERIAL (-1)

/* Heat conduction chance per cell, per burning cell per step, in 256. See
 * material.h's reaction_t. Defaults to SAND_CONDUCTION_PER_MATERIAL, same as
 * sand_set_flammability(): conduction only occurs next to burning cells, no
 * background chance. Setting to 255 makes conduction deterministic. */
void sand_set_conduction(sand_t *s, int chance);
#define SAND_CONDUCTION_PER_MATERIAL (-1)

/* Heat reaching liquid boils into steam with 1/256 chance, using material.h's
 * reaction_t.boils. Not a replacement for conduct_heat() roll. Defaults to
 * SAND_BOILS_PER_MATERIAL. Tests may override for deterministic boiling. */
void sand_set_boils(sand_t *s, int chance);
#define SAND_BOILS_PER_MATERIAL (-1)

/* 2x2 material collapses to one cell every 256 steps (material.h's
 * reaction_t), condensing in step_one_condensing_cell() (sand_reactions.c),
 * defaulting to SAND_CONDENSES_PER_MATERIAL. For deterministic one-step
 * condensation, set chance to 255. Acid rain (SAND_ACID_RAIN_CHANCE)
 * transforms 2x2 gas/steam to 50% Acid/50% Water in step_one_reacting_row().
 * Tests needing "no mechanic destroys cells" should disable acid rain if
 * board supports MAT_GAS and MAT_STEAM. */
void sand_set_condenses(sand_t *s, int chance);
#define SAND_CONDENSES_PER_MATERIAL (-1)

/* A QUENCHING liquid (PAIR_QUENCHES) speeds up a heat-ramping cell's cooling
 * in the ABOVE-ambient branch, per step_one_tempered_cell(). This prevents
 * water from dropping below room temperature. The `cools` field already
 * indicates heat-shedding rate, and wet surfaces cool faster. No distinction
 * is made for stone pane cooling rates. Use one figure for water and acid,
 * the relevant coolants. */
#define SAND_WET_COOLING_FACTOR 8

/* How often a burning LIQUID (lava) that has just done real WORK -
 * converting a neighbour, not merely banking heat (see
 * step_one_burning_cell()/cool_off_chain()) - freezes ITSELF as the cost;
 * the same chance cool_off_chain() reaches for at each further pool link.
 * Chance in 256 PER EVENT, not per step, since this only rolls on something
 * already rare. Raising this, not SAND_LAVA_COOLOFF_MAX_CHAIN, keeps pour
 * rate, not chain length, the thing that sets how fast a pool dies. */
void sand_set_lava_cooloff(sand_t *s, int chance);
#define SAND_LAVA_COOLOFF_CHANCE 32

/* The sentinel sand_set_lava_cooloff(s, chance < 0) uses
 * SAND_LAVA_COOLOFF_CHANCE, not per-material values, because no per-material
 * figure exists. Named _DEFAULT to indicate a single constant, unlike other
 * sentinels that answer "whose rate". */
#define SAND_LAVA_COOLOFF_DEFAULT (-1)

/* 1-in-256 chance PER COVERED CELL, PER STEP to convert lava to MAT_STONE and
 * burst, unlike vent_chance. vent_chance hit 255, causing instant material
 * pop. 1 is rarest single byte roll; tune aggregate, not single cell. If
 * still too frequent, add second gate in step_one_dissolver_cell(), not new
 * field. */
#define SAND_LAVA_BURST_CHANCE   1

/* A SECOND GATE above chance < 1/256 triggers another roll. Follow
 * step_one_dissolver_cell() in sand_reactions.c for `evaporates`. 4 for
 * 1/1024 rate, 75% reduction. Only applies to natural rate;
 * sand_set_lava_burst() bypasses. */
#define SAND_LAVA_BURST_GATE     4

/* Radius for the fire a lava burst ignites via sand_explode(). Tuned via
 * this radius, not SAND_EXPLODE_CORE_DIVISOR: that divisor is shared by
 * every explosion, so retuning it for one caller would rescale detonate
 * mode and the confined-gas burst too. Smaller than gunpowder's own blast
 * radius (material.h) - the material whose whole point is to go off should
 * own the biggest blast, not a vessel's side effect. */
#define SAND_LAVA_BURST_RADIUS   12

/* Overrides SAND_LAVA_BURST_CHANCE for lava cells, similar to
 * sand_set_lava_cooloff(). Ensures deterministic bursts for testing. Clamped
 * to [0, 255]. */
void sand_set_lava_burst(sand_t *s, int chance);

/* Overrides SAND_GUNPOWDER_BLAST_COOLDOWN: steps between fuse blasts,
 * board-wide. 0 lifts limit, negative restores default. Exists because
 * cooldown is compile-time constant, preventing test without rebuild. */
void sand_set_fuse_cooldown(sand_t *s, int steps);

/* The sentinel sand_set_lava_burst(s, chance < 0) restores. sand_init() sets
 * every sand_t to use SAND_LAVA_BURST_CHANCE, named _DEFAULT rather than
 * _PER_MATERIAL because there's no per-material table figure, only a
 * constant. */
#define SAND_LAVA_BURST_DEFAULT (-1)

/* Overrides SAND_ACID_RAIN_CHANCE. Matches
 * sand_set_lava_cooloff()/sand_set_lava_burst(). Necessary for deterministic
 * gas/steam pocket collapse in tests, similar to
 * test_a_2x2_block_of_steam_condenses_into_one_water_cell in
 * suite_sand_metal.c. Clamped to [0, 255]. */
void sand_set_acid_rain(sand_t *s, int chance);

/* The sentinel sand_set_acid_rain(s, chance < 0) restores, and sand_init()
 * starts every sand_t at - "use SAND_ACID_RAIN_CHANCE", named _DEFAULT like
 * SAND_LAVA_COOLOFF_DEFAULT: acid rain is not read from any material's row,
 * only the constant. */
#define SAND_ACID_RAIN_DEFAULT (-1)

/* Overrides SAND_ACID_DILUTE_MASS_BIAS for testing mass-bias effect
 * isolation. Forced to 0, it simulates an unbiased contest. Not clamped to
 * [0, 255] as it's a per-neighbour multiplier, not a roll threshold. */
void sand_set_acid_dilute_mass_bias(sand_t *s, int bias);

/* The sentinel sand_set_acid_dilute_mass_bias(s, bias < 0) restores and
 * sand_init() starts every sand_t at - "use SAND_ACID_DILUTE_MASS_BIAS",
 * similar to _DEFAULT naming used for SAND_LAVA_COOLOFF_DEFAULT and
 * SAND_ACID_RAIN_DEFAULT. */
#define SAND_ACID_DILUTE_MASS_BIAS_DEFAULT (-1)


/* Gas grain rise attempt frequency as a chance in 256, default 255 ensures
 * deterministic rise per step. Use SAND_MOBILITY_PER_MATERIAL for
 * material-specific figures or another value to override for specific drift
 * tests. */
void sand_set_mobility(sand_t *s, int chance);
#define SAND_MOBILITY_PER_MATERIAL (-1)

/* Advance one frame. (gx, gy) is a gravity vector, direction matters. Zero
 * vector means free fall. `jostle` (0-255) makes grains slide sideways and
 * overrides friction. */
void sand_step(sand_t *s, int gx, int gy, int jostle);

/* The eight-way quantisation: the NEAREST of the eight directions.
 * Writes the unit direction to (*dx, *dy), or (0, 0) for a zero vector.
 *
 * Deterministic, and not what sand_step uses - see below. */
void sand_gravity_direction(int gx, int gy, int *dx, int *dy);

/* sand_step uses random bracketing, averaging true angles. Grains move to
 * neighbors, making slow tilts rigid. Dithering varies direction over time,
 * smoothing this. At 17 degrees, 62% of frames fall straight down, 38% fall
 * down-right, integrating to a smooth 70 fps flow. Exactly-aligned input is
 * never dithered, ensuring gravity (0, 1) is always straight down and tests
 * are deterministic. */
void sand_gravity_direction_dithered(sand_t *s, int gx, int gy,
                                     int *dx, int *dy);
