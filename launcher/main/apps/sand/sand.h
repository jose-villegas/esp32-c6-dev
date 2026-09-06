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

/* A cell is one byte: material in high nibble, variant in low. See material.h
 * for encoding and properties. Variant travels with cell, not position, to
 * prevent shimmering in moving piles. Names retained for test readability. */
#define SAND_EMPTY        CELL_EMPTY
#define SAND_SHADE_COUNT  MATERIAL_VARIANTS
#define SAND_FIRST_SHADE  CELL_MAKE(MAT_SAND, 0)
#define SAND_LAST_SHADE   CELL_MAKE(MAT_SAND, MATERIAL_VARIANTS - 1)

/* Cells per block, on each axis, for the settled-block tracking behind
 * sand_enable_sleeping() - see the comment there. A tunable, like LEAF_SUB
 * in gfx_dirty.h: needs real-device measurement before it's treated as
 * settled - and now has it. At the shipped 184x224 grid, 32x64 gives
 * ceil(184/32) x ceil(224/64) = 6x4 = 24 blocks, half the 48 the previous
 * 16x64 gave, which is most of why it wins: a mass wake's memset() cost
 * scales with block count.
 *
 * MEASURED, six (SAND_BLOCK_W, SAND_BLOCK_H) pairs on real hardware
 * (settled-screen avg us / flip avg us / water avg us):
 *   8x32   (161 blocks): 610 / 9186 / 16374
 *   16x32  (84 blocks):  375 / 8869 / 16055
 *   8x64   (92 blocks):  528 / 9950 / 21642
 *   16x64  (48 blocks):  333 / 9638 / 16540  (previous default)
 *   32x64  (24 blocks):  234 / 9209 / 16238  (shipped default)
 *   32x128 (12 blocks):  215 / 9658 / 16888
 * 32x64 is the only pair that clears the settled-screen budget (300us) at
 * all, and beats the old 16x64 default on every metric - not a landslide
 * against every alternative, though: 16x32 beats it on flip (~4% - the
 * one steady-vs-flipped-gravity case) and water (~1%), trading that for
 * failing settled-screen by a wider margin (375us, 25% over budget) than
 * 16x64 did. Settled-screen won the tie-break here because it is the one
 * budget nothing but 32x64 actually clears, and the case this project's
 * very first documented lesson exists for (a resting pile costing 20x an
 * empty grid) - not because the other two numbers do not matter. See
 * docs/Sand/Simulation-Lessons.md for the full sweep methodology and the
 * two real bugs (a device-only stack overflow, two test fixtures broken
 * by this same tuning) it surfaced along the way. */
#define SAND_BLOCK_W 32
#define SAND_BLOCK_H 64

/* How many persistent emitters sand_t can carry - see sand_add_emitter(). A
 * fixed cap allows the list to be a small inline array. 16 is more taps than
 * a 368x448 board can accommodate with distinct streams. The cap bounds the
 * array, not a limit anyone expects to hit. */
#define SAND_MAX_EMITTERS 16

/* One grain currently in flight from sand_impulse() - see
 * sand_enable_impulses() and sand_impulse() for the mechanic this belongs
 * to, and sand_explode() for the one caller that exists today.
 *
 * NOT explosion-specific, on purpose, and not named as though it were:
 * this is a sparse, bounded, transient list of cells carrying a directed
 * displacement, nothing more. An explosion is one way to seed it - queue a
 * ring of these radiating outward from a point - but nothing about the
 * entry itself, or the pass that moves it (step_impulses(), in sand.c),
 * knows or cares that an explosion is where it came from. Naming this
 * blast_t when it is really an impulse was the same overloading trap
 * material.h documents for `mobility` and `sight`, which "came to mean
 * different things to different kinds without saying so" - a name that
 * describes one caller standing in for what the thing actually is - and
 * the rename was free while sand_explode() remained the only caller.
 *
 * `index` is y*w+x - the same row-major index crack_run()'s frontier
 * (sand_reactions.c) already uses uint16_t for, and for the same reason: this
 * grid is never more than 65,536 cells. `cell` is the exact byte (material
 * and variant) that was thrown, kept so the flight pass can tell, before
 * touching anything, whether the grain it threw is still the one sitting at
 * `index` - ordinary gravity in the sweep, a reaction, or an external
 * sand_set()/sand_erase() can all have touched that exact cell since this
 * entry's last turn, and a stale entry must drop rather than fly whatever now
 * happens to be there. `dir` is which of the eight ring directions (see
 * ring_dir() in sand_priv.h) it keeps flying.
 *
 * `speed` is BOTH the chance in 256 this turn's outward move happens AND
 * the thing that ramps down every turn to make that chance shrink - see
 * SAND_IMPULSE_SPEED_RAMP's own comment in this file for why one byte
 * carries both jobs instead of a separate step counter, and why the ramp
 * rate itself belongs to this generic mechanism rather than to any one
 * caller.
 *
 * Five bytes, not the three a bare (index, dir) pair would need - the
 * `cell` byte is what makes the identity check above possible at all, and
 * `speed` is what makes an arc read as a curve rather than a bent line
 * (again, see SAND_IMPULSE_SPEED_RAMP's own comment) - and on top of that
 * costs almost nothing: a uint16_t plus three uint8_ts rounds up to six
 * bytes for alignment, only one more than the four the struct needed
 * before `speed` existed. Even a generous few hundred of these is still
 * three orders of magnitude under a real per-cell velocity field - see
 * docs/Sand/Explosion-Plan.md for the full comparison.
 *
 * RE-CHECKED FOR SLACK, NOT JUST ASSUMED MINIMAL, when a memory-budget
 * bug (see SAND_IMPULSE_BUDGET_BYTES in app_sand.c) made every byte here
 * worth questioning again: there is none to find. `index` genuinely needs
 * all 16 bits at this grid's real size (184*224 = 41,216, over halfway to
 * uint16_t's own 65,536 ceiling, so nothing can be borrowed from it for
 * `dir` even though `dir` itself only needs three). `cell` and `speed`
 * each need their own full byte for the reasons above. And reordering the
 * fields buys nothing: the struct's alignment is fixed at 2 by `index`
 * alone, so the five logical bytes any ordering produces still round up
 * to six - the pad moves, it does not shrink.
 *
 * `ramp` FILLS THAT EXISTING PAD BYTE, so it is not a size increase -
 * still six bytes, the same "true floor" the paragraph above measured,
 * just with what used to be unused padding now doing something. Carries
 * queue_flying_grain()'s own `ramp` parameter (see its own comment in
 * sand.c) with the entry for as long as it flies: how fast `speed`
 * decays every turn (step_impulses(), this file) is PER-ENTRY rather than
 * one shared SAND_IMPULSE_SPEED_RAMP constant, so a caller wanting a
 * throw to travel farther (or less far) than that shared, already-
 * measured figure can ask for it via sand_impulse_dislodge() without
 * changing what every OTHER caller of this same mechanism gets for free -
 * see that function's own comment for the one caller today that needs a
 * non-default figure, and why. Ignored entirely for water/acid, which
 * keep their own separate geometric decay (SAND_SPLASH_SPEED_DECAY_SHIFT)
 * regardless of what this field holds. */
typedef struct {
    uint16_t index;
    cell_t   cell;
    uint8_t  dir;
    uint8_t  speed;
    uint8_t  ramp;
} impulse_t;

typedef struct {
    uint8_t *cells;      /* w * h, row-major, caller-owned */
    int      w, h;
    rng_t    rng;        /* seeded explicitly, so every run repeats exactly */
    /* Counts steps for random_cell() grain shade. Two pours yield different
     * shades as shade stays with cell. No mechanism; no pass, flag, test, or
     * extra draw. Shade chosen at spawn, changing the center. Alternative
     * crusting method cost 4.4 microseconds vs 1.0 and was reverted; current
     * method costs one increment per step. */
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

    /* For anything that burns, see sand_step_reactions() in sand_reactions.c.
     * Renamed from may_have_fire to accommodate wood and ember. Keyed on
     * reaction_t.burns in material.h, not kind == KIND_STATIC, as stone
     * shares that kind with ember and is frequently poured. */
    bool     may_have_burning;

    /* Same idea again, for a material that dissolves others
     * (reaction_t.dissolves - acid is the only one today). Separate from
     * may_have_burning because dissolving is not a fire reaction: acid
     * has to work on a board with no flame anywhere, so
     * sand_step_reactions() runs when EITHER flag is set. */
    bool     may_have_dissolver;

    /* For cells with TEMPERATURE - glass or snow. Own flag for cooling
     * duration, unlike may_have_burning. Named for temperature, not heat. */
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

    /* Material-pair classification table (pair_bits[][], sand_reactions.c) is
     * now a single file-scope static, not a sand_t field. At 256 bytes, it is
     * affordable once, not per sand_t, as each sand_t rebuilds it identically
     * from global reactions[]/materials[]. */

    int      last_load_dx, last_load_dy;

    /* DITHERED direction for smooth tilts. Suitable for gravity-based growth
     * over time, avoiding rigid angles. */
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

    /* Persistent point sources via sand_add_emitter(). Not cells. Grid: one
     * byte per cell (material in high nibble, variant in low - see
     * CELL_MAKE). Extended range used. Emitters in side list, stepped via
     * emit_from_emitters() in sand.c. Writes to empty points using
     * sand_set(). No material id, no per-cell cost, indistinguishable from
     * manual placement. Inline in sand_t for size. */
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

/* Let settled BLOCKS and dry-of-liquid ROWS be skipped entirely.
 *
 * Without this, resting sand is the MOST expensive thing the simulation can
 * hold, not the least. A settled grain fails its gravity-ward move, draws a
 * random number, walks its load column and then fails both slides - the whole
 * decision path, every step, to conclude nothing. Measured on a host, a screen
 * full of motionless sand cost twenty times an empty one.
 *
 * `blocks` is caller-owned, ceil(w/SAND_BLOCK_W) * ceil(h/SAND_BLOCK_H)
 * bytes - one settled flag per SAND_BLOCK_W x SAND_BLOCK_H block of the
 * grid. A block is worth processing only if it, or a block touching it,
 * saw movement on the previous step: a grain can only move one cell, so
 * nothing else can have changed what any grain in a quiet block is resting
 * on. Spawning, erasing and sand_set all count as movement, so sand poured
 * onto a sleeping pile wakes the blocks it lands in. Block-shaped rather
 * than row-shaped, unlike an earlier version of this: a whole row forced a
 * settled grain to pay full per-grain cost merely for sharing a row with
 * something active elsewhere on it, and row-shaped wake propagation only
 * ever reached vertically, which stopped meaning much once gravity tilted
 * towards horizontal and most movement became sideways within a row.
 *
 * NULL disables sleeping entirely.
 *
 * There used to be a second, row-shaped buffer here as well. It outlived
 * the settled bits that first justified it (those moved to `blocks`) and
 * ended up carrying exactly one flag - sand_liquid.c's ROW_NO_LIQUID, a
 * per-row "proved dry" cache for the cross-flow pass - before being removed
 * outright: keeping that one bit honest cost more, measured on device, than
 * the row scans it saved. See docs/Sand/Performance-Tuning-Attempts.md.
 *
 * Everything wakes when the gravity direction changes or the grid is shaken,
 * since either can free a grain that had nothing to do with its neighbours. */
void sand_enable_sleeping(sand_t *s, uint8_t *blocks);

/* Diagnostic: whether block (bx, by) is settled under dithered direction. Bit
 * layout private to sand.c/sand_priv.h. Used by app_sand.c's count_awake().
 * False if block-sleeping disabled. */
bool sand_block_settled(const sand_t *s, int bx, int by);

/* `sand_impulse()` queues a flying grain with a caller-provided shape,
 * similar to `sand_enable_sleeping()` and `sand_track_dirty_rows()`. Boards
 * without impulses have no extra cost. Tests can use small buffers. `max`
 * limits `buf` entries in flight, like `CRACK_MAX` in `crack_run()`, not
 * per-cell. Explosions generate many grains, so `max` controls hundreds of
 * `impulse_t` entries. NULL disables impulses, making `sand_impulse()` and
 * `sand_explode()` no-ops. */
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

/* THE PRIMITIVE. Queue one grain at (x, y) to be moved by the flight pass
 * at the tail of sand_step() - see sand.c's step_impulses() and its own
 * comment on why that pass has to run LAST, after everything else that can
 * move or replace a cell. `dir` is which of the eight ring directions (see
 * ring_dir() in sand_priv.h) it keeps flying, and `speed` is both this
 * turn's chance in 256 of moving and how much flight is left - see
 * SAND_IMPULSE_SPEED_RAMP's own comment below for why one number does both
 * jobs, and for the arc that falls out of it for free.
 *
 * NO MATERIAL OWNS THIS AND NO REACTION FIRES IT - it is a primitive, the
 * same family as sand_spawn()/sand_erase() above in that nothing here
 * decides when it happens, only what happens once something else decides
 * to call it. sand_explode(), below, is the one caller that exists today:
 * it fills a core with fire, then calls this once per occupied cell in an
 * annulus around it, outward-facing. A future caller wanting a different
 * shape of push - a single directed shove, a cone, a recoil off an impact
 * - would call this the same way, with its own choice of which cells, in
 * which directions, at what speed; nothing about the primitive itself
 * assumes a centre or a radius.
 *
 * A no-op if sand_enable_impulses() was never called, if (x, y) is off the
 * grid, if the cell there is already empty (nothing to throw), if the cell
 * there is KIND_STATIC (a wall cannot be thrown any more than a flying
 * grain can enter one - see can_impulse_enter()'s own comment in sand.c
 * for the entering half of this and this function's own body for why the
 * throwing half needed its own, separate check), or if the buffer is
 * already at capacity - the last one is graceful degradation, exactly
 * like CRACK_MAX truncating a crack rather than failing: past the cap,
 * calls simply queue nothing further, without the caller needing to track
 * the count itself or stop calling once it fills.
 *
 * THIS PRIMITIVE'S OWN KIND_STATIC REFUSAL HAS NO EXCEPTIONS - a wall
 * cannot be thrown through THIS function, by any caller, ever. sand_
 * explode()'s own seeding loop reaches a static cell through queue_
 * flying_grain() (sand.c) with a density-scaled chance to override that
 * refusal - a blast should read as tougher against stone than sand, not
 * indestructible - but that override is explicit, opt-in, and asked for
 * BY NAME at that one call site; it is not a hidden exception buried in
 * this shared primitive that some future caller (gunpowder, whatever
 * comes next) could trip over by accident. Calling sand_impulse() itself
 * always gets the safe default; sand_impulse_dislodge() (below) is the
 * one primitive that gets to skip it, for a caller that already knows
 * its own target is KIND_STATIC and wants it dislodged unconditionally
 * rather than with the density-scaled chance every other wall-dislodging
 * caller keeps. */
void sand_impulse(sand_t *s, int x, int y, int dir, int speed);

/* A single-cell push that bypasses the density-scaled toughness roll for
 * guaranteed dislodgement of KIND_STATIC targets. See queue_flying_grain() in
 * sand.c for details. `ramp` is the entry's speed decay; use
 * SAND_IMPULSE_SPEED_RAMP for standard decay or a custom value for different
 * throw distances. */
void sand_impulse_dislodge(sand_t *s, int x, int y, int dir, int speed,
                           int ramp);

/* Chance in 256, per step, that a queued grain's outward move happens THIS
 * turn - see sand_impulse()'s `speed` parameter, which is this chance at
 * the moment an entry is queued, and impulse_t's own `speed` field, which
 * is where it lives and ramps down from there.
 *
 * A per-entry step counter was the obvious alternative to a chance at all,
 * and was rejected for the same reason mobility/falls/scatter/flare/
 * heat_chance are all already chances in 256 rather than counters: this
 * project already expresses "how often does X happen this step" that way
 * everywhere a rate is needed, so a roll here reads like the rest of the
 * file instead of introducing a second idiom.
 *
 * SAND_IMPULSE_SPEED_RAMP is how much `speed` loses every step - moved,
 * blocked, or about to be dropped, it ages regardless (see step_impulses()'s
 * own comment on why the ramp cannot make an exception for a wedged
 * entry). This is what turns a queued grain's outward push into an actual
 * ARC rather than a bent line: gravity in this simulation does not
 * accelerate - a falling grain drops at a constant one cell per step,
 * forever - so a parabola needs the HORIZONTAL half of the motion to
 * change instead, since the vertical half never will. Early on, `speed` is
 * whatever the caller queued it at and the grain moves outward nearly
 * every step - shallow. As it ramps down, outward moves come only
 * occasionally - steeper. Once `speed` reaches zero, rng_chance() with a
 * zero numerator never succeeds, so the grain never moves outward again -
 * vertical, falling straight under gravity alone like any other grain, and
 * dropped from the flight list that same turn, since a roll that can never
 * again succeed has nothing left to track.
 *
 * BE HONEST about what this is not, though: the vertical component is
 * still exactly one cell per step, the whole time. This is not a
 * physically accurate parabola; it is a curve that reads as an arc because
 * only the horizontal half of it decays. Shallow by construction, not by
 * choice.
 *
 * ONE KNOB, GENERIC TO EVERY CALLER, not per-call configurable - unlike
 * the initial `speed` a caller passes in (which is exactly the point:
 * "how far this particular impulse reaches" belongs to whoever is calling
 * sand_impulse() and choosing what to queue; "how quickly any impulse's
 * push fades" is a property of the flight mechanism itself, and every
 * caller shares it). WAS 14, RAISED TO 4, in the round that first gave a
 * device a real-radius detonation to look at: a grain's push was fading
 * out before it had travelled far enough to be seen, on top of the cap
 * and core problems fixed alongside it. 4 was itself only a STARTING
 * POINT, never measured on its own.
 *
 * WAS 4, LOWERED TO 2 - the first of these three constants to actually be
 * swept rather than guessed, against the host measurement this mechanic
 * had been missing until suite_sand_dune_blast.c's dune scene arrived:
 * grains landing outside a settled dune's own footprint, averaged over independent
 * sand_init() seeds (a single hardcoded-seed host test cannot tell a
 * genuine improvement from a lucky roll). Swept one knob at a time first
 * (20 seeds each, this constant against 200/250/255 SAND_EXPLODE_INITIAL_
 * SPEED and against 3/4/5 SAND_EXPLODE_CORE_DIVISOR): RAMP 2 alone raised
 * "outside" by roughly 20% over RAMP 4 at every SPEED/DIVISOR pairing
 * tried, the largest swing any one of the three constants produced -
 * unsurprising once stated plainly: a slower ramp means more steps spent
 * above zero speed, which is more steps in which the outward roll can
 * still succeed at all. Confirmed rather than assumed to still hold in
 * COMBINATION, not just alone: an 18-way grid over all three constants
 * together (30 seeds each) found RAMP 2 beating RAMP 4 at every single
 * SPEED/DIVISOR pairing in the grid, never just on average - see
 * SAND_EXPLODE_CORE_DIVISOR's own comment for the full table and why 5,
 * not 3, is the DIVISOR this ships paired with. At the shipped combination
 * (255, 2, 5) "outside" measured 80.0 average versus baseline (250, 4, 3)'s
 * 56.9 - both n=30, same seed set - a mechanism-only change, with no
 * changes yet to WHICH cells a flying grain may enter (see
 * can_impulse_enter()'s own comment in sand.c for that half of the story,
 * measured separately since it landed as its own commit).
 *
 * Paired with a `speed` of 255 (see SAND_EXPLODE_INITIAL_SPEED), this
 * reaches zero in ceil(255/2) = 128 steps - almost exactly double the old
 * RAMP-4 bound of 63, and still a small fraction of a frame's worth of
 * wall-clock time at this project's step rate. That determinism is the
 * property worth keeping regardless of where either number lands: the
 * design this replaced (a single fixed chance-in-256 rolled fresh every
 * turn, with no memory of how long a grain had already been flying) only
 * ever shrank the PROBABILITY of surviving another turn, never actually
 * bounded how long that could take.
 *
 * RECONFIRMED, NOT ASSUMED, WHEN THE RADIUS DOUBLED. A device pass on this
 * combination asked for a much bigger blast - see DETONATE_RADIUS_PX in
 * app_sand.c, which doubled from 24 to 48 cells - and a grain now has
 * twice as far to travel to clear a disc twice as wide, which is exactly
 * the kind of change that could have moved this constant's optimum. It
 * did not: re-running the full 18-way combined grid at the new radius put
 * RAMP 2 ahead of RAMP 4 at every SPEED/DIVISOR pairing again, same as at
 * the old radius, and by a similar relative margin (108-122 "outside" at
 * RAMP 2 across the three divisors, versus 76-89 at RAMP 4, all at SPEED
 * 255). Still not confirmed on a device at this new radius - see
 * docs/Sand/Explosion-Plan.md's "Device" section for what to look at
 * first once it is. */
#define SAND_IMPULSE_SPEED_RAMP  2

/* HOW MANY CELLS THE PUSH MOVE COVERS IN ONE STEP, once the per-step roll
 * (rolled_move, step_impulses(), sand.c) says this is a turn it moves at
 * all. That roll's own meaning is UNCHANGED by this constant - it still
 * decides only whether the push happens this step, exactly as it always
 * has. What used to be fixed at exactly one cell per successful roll is
 * now 1 + speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR, uncapped - see
 * "NO SEPARATE CAP" below for why a full-speed entry needs none.
 *
 * THE PROBLEM THIS FIXES: every displacing move in this file, before this
 * existed, advanced an entry exactly one cell per successful roll -
 * identical to how far the ordinary gravity sweep moves a falling grain in
 * that same step (SAND_IMPULSE_SPEED_RAMP's own comment above: "the
 * vertical component is still exactly one cell per step, the whole time").
 * An impulse could therefore never outrun gravity: a horizontal throw sank
 * at close to 45 degrees, one cell of push for every one cell of fall, and
 * ejecta thrown off a powder volume could only ever reposition material,
 * never visibly leave it - nothing this engine threw ever moved sideways
 * faster than gravity pulled it down. Measured, seeds 1..40, an 8-cell-wide
 * stone chunk thrown at an angle into a settled sand bed: BEFORE this
 * constant existed, 26 of 40 seeds ever produced any airborne sand (a bed
 * cell with all eight neighbours empty) at all, peaking at 3 simultaneously
 * airborne cells; AFTER, all 40 seeds show airborne sand, peaking at 8 -
 * material rearranging became material visibly flying. See
 * test_a_stone_chunk_thrown_into_a_sand_bed_launches_sand_airborne
 * (suite_sand_impulse.c), which pins this exact scene and these exact figures.
 *
 * 104, SO A FULL-SPEED (255) ENTRY COVERS THREE CELLS AND A SPENT ONE
 * STILL COVERS EXACTLY ONE, UNCHANGED - 1 + 255/104 = 3, and integer
 * division means anything under 104 rounds to 1 + 0 = 1, the same single
 * cell every entry already moved before this existed. THAT IS THE POINT,
 * not a side effect to work around: it is what keeps a slow-moving
 * entry's own behaviour, and every test written against the old
 * one-cell-per-roll design, byte-for-byte unchanged - see
 * test_a_sub_divisor_speed_impulse_never_moves_more_than_one_cell_a_step
 * (suite_sand_impulse.c), which pins exactly this.
 *
 * NO SEPARATE CAP - there used to be one, SAND_IMPULSE_CELLS_PER_STEP_MAX,
 * standing on its own so a future retune of this divisor could not
 * silently move it too. An adversarial review of step_impulses() (bd
 * esp32c6-w2h) measured what it was actually worth at this divisor's own
 * shipped value: 1 + 255/104 = 3, always strictly under the cap's 4, at
 * every speed a uint8_t can hold - the cap could never fire, so it was
 * dead weight standing guard over nothing. Deleted rather than kept as
 * insurance: a future divisor change that would make the cap matter again
 * has to reintroduce it deliberately, with the same reasoning this
 * paragraph records, not inherit a number nobody re-checked. */
#define SAND_IMPULSE_CELLS_PER_STEP_DIVISOR  104

/* EXTRA speed charged at the move site, on top of the ordinary ramp above,
 * per non-empty cell a KIND_STATIC or KIND_POWDER mover displaces - see
 * step_impulses()'s own comment at the charge site for why there rather
 * than folded into the ramp, and impulse_drag_of() (sand_priv.h) for the
 * one place this is actually computed. The base cost IS `density`, no shift
 * applied to it at all - open air (nothing displaced) costs nothing, water
 * (30) costs 30, dirt or sand (60-62) cost 60-62 - and THIS constant is an
 * additional per-kind adjustment on top of that, `density << this`, applied
 * only for KIND_POWDER. Density alone gave every non-liquid medium the same
 * SHAPE of resistance, which is the thing that was wrong: packed grain jams
 * against itself and stops a mover dead, while a fluid parts around one and
 * barely slows it. So powder multiplies, and LIQUID CHARGES NOTHING AT ALL -
 * a liquid mover was always exempt, and now a liquid MEDIUM is too, which
 * puts a chunk falling into a pool back to exactly the behaviour it had
 * before any of this existed. That was the device call: sinking to the floor
 * had always looked right, so there was nothing here for drag to fix.
 *
 * USED TO BE TWO CONSTANTS - a general SAND_IMPULSE_DRAG_SHIFT applied to
 * every non-liquid kind's density, and this one layered on top for
 * KIND_POWDER only. The general shift shipped at 2 (dirt/sand cost 15,
 * water cost 7) and a real throw against a real bank of sand on device did
 * not stop at that figure: a chunk at full speed still drove roughly 15
 * cells into a bank, plainly not "the first few layers" the maintainer
 * asked for. Dropping it to 0 - `density >> 0` is the identity, drag IS the
 * density, honestly - fixed that for KIND_STATIC, and an adversarial
 * architecture review (bd esp32c6-w2h) later pointed out the shift, sitting
 * at its own identity value, was adding a second lever over the same
 * density with no effect left to have: folded away here rather than kept as
 * a second constant with nothing left to say. A future STATIC-only
 * adjustment, if one is ever needed, is a new constant scoped to that kind,
 * not this one reopened.
 *
 * RE-SWEPT AGAINST THE ENERGY EXIT (step_impulses()'s hop loop, sand.c),
 * not inherited from the distance-only-budget regime the 3.0 figure above
 * was measured under. This constant's own comment carried 12.5 / 11.9 /
 * 0.8 once (one cell a step), then 27.8 / 25.0 / 3.0 (multi-cell travel,
 * no energy exit) - both stale the moment the loop that produced them
 * changed again, which is exactly why this number gets re-measured rather
 * than trusted to still be right. Swept over the shift itself, 32 seeds,
 * cells of travel (plow_total_distance(), suite_sand_impulse.c), air and water
 * unaffected by this constant at any value (air displaces nothing; water
 * is KIND_LIQUID and impulse_drag_of() charges liquids nothing regardless
 * of shift), so only dirt moves:
 *
 *     shift   air    water   dirt
 *     0       27.8   25.0    3.81   (identity - the pre-powder-drag figure)
 *     1       27.8   25.0    1.88
 *     2       27.8   25.0    0.91   <- kept
 *     3       27.8   25.0    0.81
 *     4       27.8   25.0    0.81
 *     5       27.8   25.0    0.81
 *     6       27.8   25.0    0.81
 *     8       27.8   25.0    0.81
 *
 * KEPT AT 2, THE SHIPPED VALUE - not carried over unexamined, but
 * reconfirmed against the loop that actually governs it now: 0.91 cells is
 * the closest any candidate gets to the stated intent ("stop at the rim",
 * roughly one) without already being on the far side of it. Shift 3 and
 * every value above it plateau at 0.81 - dirt's density (62), doubled
 * three times or more, already exceeds a single hop's own speed budget
 * before the energy exit even has to fire twice, so further shifting buys
 * nothing: the loop is stopping on hop 0 regardless. Shift 1 (1.88) is the
 * only candidate closer to "two cells" than to "one", and shift 0 (3.81 -
 * worse than the OLD, energy-exit-less 3.0 figure, since a full-speed
 * entry with dirt's UNSHIFTED cost of 62 a cell needs four hops rather
 * than one to exhaust its own energy budget) is the pre-powder-drag
 * baseline, kept in the table only to show the shape the sweep moves
 * across, not as a candidate. See
 * test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank (suite_sand_impulse.c) for
 * the pin this figure is checked against. */
#define SAND_IMPULSE_DRAG_POWDER_SHIFT  2

/* THE FLOOR BELOW WHICH A KIND_STATIC ENTRY IS SPENT, for the gravity-drift
 * move AND the settled check right after it in step_impulses() (sand.c,
 * the "AIRBORNE SOLIDS FALL TOO" block and the has_opening loop just below
 * it) - see can_impulse_enter_gravity_ward()'s own comment (sand.c) for the
 * one predicate both now call, so they can never again disagree about what
 * "open" means the way they once did before impulse_gravity_candidates()
 * unified the candidate list itself.
 *
 * WHY THIS HAS TO EXIST AT ALL: drag (impulse_drag_of(), sand_priv.h) only
 * charges at the PUSH move site - a KIND_STATIC entry's unconditional
 * gravity-drift pays no drag at all, ever, by design ("gating this on
 * speed would tie 'still falling' to 'still has outward energy left', which
 * is backwards" - that block's own comment). Stronger drag alone therefore
 * stops the sideways travel but does nothing to the drift, which keeps
 * swapping a spent chunk downward through an entire bank one row a step
 * until it reaches the bottom - reported on device as "a thrown chunk
 * entering a powder bank does not stop." An entry below this floor is
 * SPENT - genuinely out of push, not merely between rolls - and a spent
 * entry may only continue into a cell that is genuinely CELL_IS_EMPTY(),
 * never swap through an occupant the way an energetic entry still can.
 * Above the floor, nothing changes: an energetic chunk still swaps through
 * powder and liquid exactly as it always has, so a hard impact still
 * buries itself - this only ends the drift's own free ride once the push
 * that justified it is gone.
 *
 * 1, SO IT MEANS EXACTLY "speed is zero" - the narrowest floor that is
 * still a floor at all, deliberately not a bigger number: this is meant to
 * catch an entry that has nothing left, not one that merely has little,
 * and `speed` already saturates at 0 rather than wrapping (SAND_IMPULSE_
 * SPEED_RAMP's own comment), so 1 is the first value genuine exhaustion
 * can never reach.
 *
 * THE KNOWN, CHOSEN TRADE: this floor is not kind-aware, so a spent chunk
 * now comes to rest inside a liquid too, not only a powder - roughly eight
 * cells down in water at drag's own present figure, rather than
 * sinking all the way to the floor of a deep pool. The maintainer measured
 * liquid behaviour as already looking fine and chose this one simple rule
 * over a second, kind-aware version anyway - simplicity over splitting
 * powder from liquid, accepted with the trade-off named rather than
 * discovered later on device. If a chunk stalling mid-pool ever looks
 * wrong in practice, the fix is to make this predicate ask a different
 * question for KIND_LIQUID than for KIND_POWDER, not to raise or lower
 * this number. */
#define SAND_IMPULSE_SINK_MIN_SPEED  1

/* RESTITUTION FLOOR for the wall-bounce (step_impulses()'s
 * blocked branch, sand.c) - below this, a blocked entry just waits, same
 * as it always has; at or above it, it reflects off the blocking surface's
 * approximate normal (blocker_normal()/reflect_off_normal(), sand.c) and
 * pays restitution for the privilege. Not a polish knob: see
 * step_impulses()'s own roll comment (its "A DETERMINISTIC, NEVER-ROLLED
 * VARIANT..." paragraph) for the reverted-attempt history this rung can
 * reopen if bouncing were left undamped and unfloored - pieces that never
 * settle because they keep finding just enough energy to bounce again.
 * Every bounce ALSO costs restitution (half the speed on a head-on
 * reflection, a quarter on a glancing one - see the charge site), so nothing
 * here is immortal even without this floor, but at SAND_IMPULSE_SPEED_RAMP
 * 2 the plain linear ramp alone takes 255 / 2 ~= 128 steps to exhaust - a
 * long time for something to keep visibly rattling in a corner before
 * restitution alone brings it under a floor.
 *
 * 32, LOWERED FROM 64, AND THE MEASUREMENT SAYS IT BARELY MATTERS. Run
 * against the two-wall scene (test_the_two_wall_explosion_scene_...,
 * suite_sand_impulse.c, 100 seeds, 284 tracked entries) the two floors give:
 *
 *     floor 64   >=1 bounce 205   >=2 45   >=3 0
 *     floor 32   >=1 bounce 208   >=2 48   >=3 3
 *
 * about one percent, plus the first entries ever to reach a third bounce.
 * Kept because it is free and strictly more lively, NOT because it is the
 * lever it was expected to be - the arithmetic said an entry crossing back
 * at speed 43 would newly clear a floor of 32, and that is true and almost
 * never happens, because THE BINDING CONSTRAINT IS FALL TIME, not energy.
 * A tracked cell drops one row a step unconditionally ("AIRBORNE SOLIDS
 * FALL TOO", step_impulses()), so in an arena 40 tall it is on the ground
 * inside 40 steps while crossing a 42-cell gap costs it 42 - most entries
 * land before finishing even one crossing, whatever speed they still have.
 * What actually buys bounces is GEOMETRY: a gap narrower than the fall
 * height. Not lowered further regardless: speed IS the per-step move
 * chance, so an entry much under this crawls a step in eight rather than
 * ricocheting - the rattling this floor exists to prevent, just slower. */
#define SAND_IMPULSE_BOUNCE_MIN_SPEED  32

/* TRANSFER - what a struck cell inherits from the mover that just displaced
 * it, at the move site in step_impulses() (sand.c), right after the swap.
 * Today a flying cell simply swaps with whatever it displaces and the
 * medium closes behind it with no further effect; this is what lets a
 * struck cell pick up impulse of its own instead, including flying clean
 * out of the volume it was sitting in.
 *
 * DIRECTION IS A BACKWARD CONE, NOT THE MOVER'S OWN `dir` - one of `dir+3`,
 * `dir+4` (straight back) or `dir+5`, picked with rng_below(). Queuing the
 * struck cell along the mover's own heading was tried first and reported
 * as the reason nothing ever visibly sprayed off a bank: pushing struck
 * material further ALONG the impact just drives it deeper into whatever it
 * was already part of, the opposite of the wanted crater. What a real
 * impact actually does is squeeze material back out through the surface it
 * came in by, roughly backward and outward - straight back is also where
 * the open room is, since the mover just came through those exact cells, so
 * the ejecta has somewhere to go instead of wedging further into the bank.
 * See the transfer site's own comment for the rest of the scope
 * (KIND_STATIC/KIND_POWDER movers only, matching drag's own scope just
 * above; any non-static displaced cell, powder or liquid alike).
 *
 * SAND_IMPULSE_TRANSFER_KEEP is HOW MUCH OF THE ARRIVAL SPEED THE CHILD
 * KEEPS, in 256ths - not what that speed is divided by. The arrival speed,
 * not what the mover has left, which is the whole point of capturing
 * impact_speed before drag takes its cut (see the transfer site).
 *
 * A KEEP FACTOR RATHER THAN A DIVISOR because an integer divisor can only
 * express 1, 2, 3 - and the useful range turned out to sit between the
 * first two. At 1 a strike hands over everything; at 2 it halves; on device
 * the first was too strong and the second too weak, and there was no way to
 * say so. In 256ths the whole range is reachable, using the same "in 256"
 * vocabulary this file already speaks everywhere else, and a shift instead
 * of a divide.
 *
 *     keep 256   divisor 1.00   a 255 parent hands over 255
 *     keep 213   divisor 1.20   a 255 parent hands over 212
 *     keep 171   divisor 1.50   a 255 parent hands over 170
 *     keep 128   divisor 2.00   a 255 parent hands over 127
 *
 * ANY VALUE BELOW 256 SATISFIES THE RULE THIS EXISTS FOR, which is worth
 * being exact about because the review that raised it named 2 specifically.
 * The problem at keep 256 is that a strike MINTS energy: the child carries
 * the parent's full arrival speed while the parent loses nearly all of it
 * to drag, so nothing bounds a chain except the backward cone usually
 * finding open air - geometry and luck rather than a rule. Below 256 each
 * generation is strictly smaller than the last, so a chain decays
 * geometrically whatever the geometry does. 2 buys a faster decay, not the
 * only bound; 1.2 is a weaker bound that is still a bound.
 *
 * 213 is a device figure, picked between a 1 that sprayed too hard and a 2
 * that sprayed too softly. */
#define SAND_IMPULSE_TRANSFER_KEEP  213

/* THE TRANSFER FLOOR - below this post-drag speed, a mover does not queue a
 * transfer at all, the same shape SAND_IMPULSE_BOUNCE_MIN_SPEED already
 * floors restitution at (a nearly-spent mover queuing a transfer too faint to
 * ever visibly move is not worth the entry). 64 matches that floor's own
 * starting figure for the same reason - not a coincidence to be tuned
 * independently without cause. THE REAL BUDGET REASON THIS EXISTS AT ALL:
 * impulse_buf is a FIXED-SIZE buffer (APP_IMPULSE_MAX, app_sand.c - 2048
 * entries, 12 KiB) shared with whatever explosion or splash is already using
 * it, and sand_impulse() silently refuses once it is full - graceful, but
 * silent. A chunk plowing through a wide bank, uncapped, would queue one
 * transfer per cell it displaces; TRANSFER's own share of the deferred
 * array's total capacity (SAND_CASCADE_TRANSFER_MAX_PER_STEP, this file - see
 * SAND_CASCADE_MAX_ PER_STEP's own comment for why that total is now split
 * rather than one counter shared with the cascade relay) is what actually
 * protects the buffer, but this floor is the first, cheaper gate: a plow
 * through a LOW-density medium (most cells pass the floor easily) still
 * queues plenty, so both gates matter - this one trims the faintest transfers
 * before they even compete for the per-step cap. */
#define SAND_IMPULSE_TRANSFER_MIN_SPEED  64

/* sand_explode()'s OWN choice of what speed to hand every entry it queues -
 * not a property of sand_impulse() itself, which takes speed as a plain
 * parameter and assumes nothing about what any particular caller wants.
 * WAS 200, RAISED TO 250 (near the uint8_t ceiling) alongside slowing
 * SAND_IMPULSE_SPEED_RAMP from 14 to 4 - a starting point, not a
 * measurement, picked from a single device report rather than a sweep.
 *
 * WAS 250, RAISED TO 255 - the uint8_t ceiling outright, once there was a
 * real measurement to raise it TOWARD. See SAND_IMPULSE_SPEED_RAMP's own
 * comment for the sweep methodology (grains landing outside a settled
 * dune's own footprint, averaged over independent seeds); the same sweep
 * ran this constant against 200/250/255 and found 255 beating 250 at
 * every RAMP/DIVISOR pairing tried, one-at-a-time and in the later 18-way
 * combined grid alike - a smaller effect than SAND_IMPULSE_SPEED_RAMP's
 * own (a few points of "outside" rather than dozens), but consistently in
 * the same direction, so there is no combination in the grid where
 * stepping back to 250 would have won. There is nowhere left to raise
 * this TO - 255 is every bit of range a uint8_t speed has - so a future
 * round wanting more reach has to look at SAND_IMPULSE_SPEED_RAMP or the
 * radius instead. Paired with a bigger or smaller radius this is still
 * "how far things fly" in the sense the old SAND_BLAST_DECAY used to mean
 * it, just relocated to belong to the explosion that actually decides it,
 * rather than living inside the generic flight mechanism as though every
 * future caller would want the same number.
 *
 * RECONFIRMED AT DOUBLE THE RADIUS, same as SAND_IMPULSE_SPEED_RAMP's own
 * comment describes: the 18-way combined grid re-run at 48 cells (was 24)
 * found 255 still beating 200 and 250 at every RAMP/DIVISOR pairing - at
 * RAMP 2, DIVISOR 5 specifically, 37.6 (200) / 103.2 (250) / 122.1 (255)
 * "outside", the same ordering and a similar relative gap to before. There
 * is still nowhere to raise this to, so a bigger radius did not change
 * that either. */
#define SAND_EXPLODE_INITIAL_SPEED  255

/* Bounds a single cool_off_chain() walk, independent of rolls. Smaller scale
 * than CRACK_MAX. Prevents a single lucky roll from emptying a pool, avoiding
 * "dry bowl paving" failure. PUBLIC; named in Reaction-Table.md and used in
 * tests. Chain frequency controlled by SAND_LAVA_COOLOFF_CHANCE. */
#define SAND_LAVA_COOLOFF_MAX_CHAIN 8

/* Radius and decaying trigger CHANCE for splash_displace() (sand_liquid.c)
 * - a WATER grain landing hard, either falling onto an already-occupied
 * surface or rebounding off a wall, throws a small, MASKED sand_displace_
 * material() (only the same material gets thrown - see splash_displace()'s
 * own comment for why), exaggerated well past a real splash's reach so the
 * effect reads clearly at this display size.
 *
 * WATER ONLY, NOT ACID ANY MORE - acid used to share this exact mechanism
 * (a smaller, non-decaying radius, see git history from before 2026-09-01
 * if that is ever worth reviving), and no longer does: see acid_bubble()'s
 * own comment in sand_liquid.c for what replaced it and why. Oil and lava
 * are not wired into this either - oil has no gameplay reason to scatter,
 * and lava is a heat source whose spread timing this same exaggerated
 * radius visibly disrupted when tried (see this constant's own commit
 * history if that is ever revisited).
 *
 * A displaced grain falling back into the liquid lands hard too, which
 * would re-trigger the same call that threw it - an unconditional trigger
 * on every landing bounces indefinitely, and a real splash does not keep
 * re-splashing itself. Gating whether the call fires at all fixes that:
 * CHANCE (a chance-in-256 roll, the same idiom tick_decay() and dislodge
 * use elsewhere) starts at START - guaranteeing the first splash - and
 * drops by STEP on every successful trigger, so a bounce chain's own
 * echoes are suppressed almost immediately rather than rattling on.
 * Per-sand_t (sand_t::splash_chance below), not a shared global, so one
 * simulation's splash history never bleeds into another's.
 *
 * WATER'S RADIUS ALSO STEPS DOWN on every successful trigger (sand_t::
 * splash_radius_water below), independently of the chance above - the
 * chance decides WHETHER a bounce chain's echo still gets to splash at
 * all, this decides how BIG the ones that do get to are, so a chain that
 * does keep landing hits reads as a bounce settling down rather than a
 * string of identically-sized pops. RADIUS_WATER is both the starting
 * value (fresh from sand_init()) and the ceiling; floors at
 * RADIUS_WATER_FLOOR, stepping by RADIUS_WATER_STEP each time - never
 * recovers on its own, matching splash_chance's own one-way decay.
 *
 * RAISED WELL PAST ACID'S OWN RADIUS, 2026-08-31 - reported as still
 * reading like a quiet merge rather than a repel: sand_impulse()'s own
 * speed is already at its ceiling (SAND_EXPLODE_INITIAL_SPEED, shared
 * with explosions, measured with nowhere left to raise it - see that
 * constant's own comment), so radius is the one lever actually left for
 * "hits harder". STEP raised to match, not just FLOOR left alone to
 * stretch the ramp out longer - the ask was a stronger INITIAL punch that
 * still settles down at roughly the same pace, not the same shape held
 * for more bounces. 10 -> 6 -> 2 -> floor.
 *
 * DOUBLED AGAIN, 2026-08-31, same session, once the cascade's own
 * cross-flow re-acquisition loss was fixed (see step_impulses()'s comment
 * in sand.c) and the effect was still judged too small to read as a real
 * repel. Speed has nowhere left to go - see SAND_EXPLODE_INITIAL_SPEED's
 * own comment, still true, still the uint8_t ceiling - so radius remains
 * the only knob this feature has for "reaches further". FLOOR and STEP
 * scaled with it, same 10:1:4 shape as before, so the decay still reads as
 * the same settling-down curve, just starting from a bigger first hit: 20
 * -> 12 -> 4 -> floor. Not yet confirmed on device at this size - the next
 * on-device pass should look at whether SAND_CASCADE_MAX_PER_STEP still
 * holds at a wider blast, the same way SAND_EXPLODE_CORE_DIVISOR needed
 * reconfirming when DETONATE_RADIUS_PX doubled (see that constant's own
 * comment). */
#define SAND_SPLASH_RADIUS_WATER       20
#define SAND_SPLASH_RADIUS_WATER_FLOOR 2
#define SAND_SPLASH_RADIUS_WATER_STEP  8
#define SAND_SPLASH_CHANCE_START       255
#define SAND_SPLASH_CHANCE_FLOOR       24
#define SAND_SPLASH_CHANCE_STEP        140

/* How fast a WATER or ACID impulse's own `speed` decays per step, in the
 * flight pass (step_impulses(), sand.c) - a right-shift, not the linear
 * SAND_IMPULSE_SPEED_RAMP subtraction every other material still uses.
 * ADDED 2026-08-31, same session as RADIUS_WATER's own doubling: reported
 * that the splash needed to hit hard and then die out fast, a shape a flat
 * per-step subtraction cannot give - subtracting a fixed amount from 255
 * takes ~128 steps to reach zero regardless of how big the amount is tuned
 * (raising it shortens the tail linearly, at best), where a shift of 1
 * halves whatever is left EVERY step and reaches zero from 255 in about 8 -
 * most of the loss happens in the first couple of steps, which is what
 * "dies out fast" actually means as a shape, not just a smaller number.
 * The same halving idiom SAND_CASCADE_SPEED_DIVISOR already uses for the
 * cascade's own hop-to-hop decay, reused here rather than inventing a
 * second one.
 *
 * SCOPED TO WATER/ACID ONLY, matching the wall-bounce and the cascade
 * itself (both in step_impulses()'s own comment) - sand_explode() and
 * every other caller of sand_impulse() keeps the original linear
 * SAND_IMPULSE_SPEED_RAMP, so this does not disturb that mechanism's own
 * extensively swept tuning (see SAND_IMPULSE_SPEED_RAMP's and
 * SAND_EXPLODE_CORE_DIVISOR's comments for that history).
 *
 * A STARTING POINT, NOT YET MEASURED - 1 (halving) was picked as the
 * gentlest shift that still qualifies as "much stronger" against the old
 * ~128-step linear tail, specifically to leave the cascade some room:
 * step_impulses()'s own comment on this decay explains the interaction -
 * a faster base decay means fewer relayed hops clear the cascade's
 * SAND_CASCADE_MIN_SPEED * SAND_CASCADE_SPEED_DIVISOR gate before dying,
 * on top of the cascade's own halving. A shift of 2 or higher would die
 * out even faster but was not tried first, on the reasoning that starving
 * the cascade entirely in the same round that is meant to make the splash
 * hit harder would make it hard to tell which change caused what on
 * device. Raise this if 1 still does not read as fast enough once tested;
 * lower SAND_CASCADE_MIN_SPEED instead if the cascade reads as cut too
 * short by this. */
#define SAND_SPLASH_SPEED_DECAY_SHIFT  3

/* CASCADE - a WATER or ACID impulse that successfully moves relays its
 * push into whatever of the SAME material sits one step BEHIND where it
 * started (opposite its own direction of travel), so that cell can now
 * advance into the gap this one just left - a chain of connected liquid
 * moves together, one advancing into the last one's vacancy, rather than
 * just the one grain that happened to be queued flying off alone. See
 * step_impulses()'s own comment (sand.c) for why this queues into NEXT
 * step's pass rather than this one's, and for why "behind", not "one
 * further step ahead" (the first version of this checked ahead, and
 * found almost nothing - the cell ahead of a mover is close to
 * definitionally open, that is why the move just succeeded).
 *
 * The ramp lives here, not in a separate mechanism: each relay hop's
 * speed is the previous hop's speed divided by DIVISOR, so the wave loses
 * energy geometrically as it travels and dies out on its own.
 *
 * EXAGGERATED, 2026-08-31 - reported as "there but extremely subtle" at
 * the previous MIN_SPEED (32): from a full 255 push, DIVISOR 2 only
 * cleared the `speed >= MIN_SPEED * DIVISOR` gate for 2 hops (255 -> 127
 * -> 63, stopped there since 63 < 64) - a genuine cascade, but short
 * enough to barely read as one. Dropping the floor is the direct lever:
 * MIN_SPEED 4 clears the same gate (>= 8) for 5 hops instead (255 -> 127
 * -> 63 -> 31 -> 15 -> 7, stops there), a visibly longer, slower-fading
 * wave through connected water rather than a two-cell nudge.
 *
 * DROPPED AGAIN, TO 1, 2026-08-31 same session - this table's own "255 ->
 * 127 -> 63 -> ..." math was never the whole story: it is only what a
 * relayed hop's speed IS the instant it gets queued, not what it decays
 * to on every step afterward while waiting for its own roll to succeed
 * (rng_chance(&s->rng, entry.speed) in step_impulses()). Once
 * SAND_SPLASH_SPEED_DECAY_SHIFT made that per-step decay geometric
 * instead of linear (own comment, this file), a relayed entry that takes
 * more than one step to actually roll a move can fall under this gate
 * before it ever gets the chance to relay again - confirmed the hard way,
 * not just reasoned about: test_a_cascading_impulse_moves_more_than_one_
 * cell (suite_sand_materials.c), a fixed-seed scene that cascaded reliably before,
 * started failing the moment SPEED_DECAY_SHIFT landed. MIN_SPEED 1 (gate
 * >= 2) makes the cascade's own artificial cutoff almost never the
 * reason a chain stops - the roll's own exhaustion (rng_chance with a
 * near-zero numerator) becomes the real stopping point instead, the same
 * way it already is for a lone, non-cascading entry. Tune DIVISOR first
 * if the cascade still needs a different FALLOFF shape once this is
 * confirmed on device; this constant now has nowhere lower to usefully
 * go. */
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

/* ACID BUBBLES - see acid_bubble()'s own comment in sand_reactions.c for the
 * full account of what this replaced and why (2026-09-01): the old "landed
 * hard on already-occupied liquid" trigger splash_displace() shared with
 * water turned out to concentrate acid's activity wherever ordinary cross-
 * flow physics happened to pile material up first, reading as a strong,
 * unwanted spatial preference rather than the ambient "bubbling, almost
 * carbonated" look this was meant to have.
 *
 * CHANCE is a flat, non-decaying chance-in-256, rolled once per acid cell
 * this pass visits that has open space directly against gravity from it -
 * the "rim" of an exposed surface - checked every step regardless of
 * history, the same way mobility or flare are. No per-step cap, no
 * decaying budget: unlike the old trigger, this one has no bulk-pour
 * pathology to guard against in the first place, because it is never
 * fired many times from the same *event* - only ever once per rim cell
 * per step, and a step's worth of rim cells is already a small, bounded
 * set.
 *
 * RAISED SHARPLY, 2026-09-02 - the starting values (6, 40) were reachable
 * in a host test but reported as invisible on device even once the real
 * bug (acid_bubble() living behind block-sleeping, fixed the same round)
 * was gone. The reason was never traced to one exact line, but the ARITHMETIC
 * alone explains most of it: SPEED 40 only has ~15.6% chance to move on its
 * OWN FIRST roll, and decays (SAND_SPLASH_SPEED_DECAY_SHIFT, geometric,
 * scoped to water/acid) fast enough that most queued bubbles likely settled
 * without ever visibly moving even once - a silent near-miss, not a
 * visible pop, on the majority of rolls. CHANCE 6-in-256 (~2.3%) compounds
 * that: on a modest rim a bubble is already rare, and most of the rare
 * ones then failed to ever actually move. Both raised well past a
 * literal reading of "small bubble, occasional pop": CHANCE to 40 (~15.6%,
 * several times a second on an ordinary rim) so the trigger itself is
 * common, and SPEED to 220 (near SAND_EXPLODE_INITIAL_SPEED's own ceiling,
 * not equal to it - a bubble should still read as smaller than a splash)
 * so a fired bubble is very likely to actually move, more than once,
 * before its geometric decay catches it - an actual multi-cell arc rather
 * than a single-frame flicker easy to miss.
 *
 * THE RAISE DESCRIBED ABOVE NEVER ACTUALLY LANDED - caught 2026-08-31 while
 * chasing a device report of "no upward shots at all": this comment already
 * argued for CHANCE 40 / SPEED 220, but the #defines below still held 20/120,
 * the exact starting values the paragraph above says were already confirmed
 * invisible on device. One commit, one omission - the write-up shipped, the
 * numbers it describes did not. Ruled out first with temporary per-call
 * counters (exposure-check passes, chance-roll passes, successful
 * impulse_buf enqueues - since removed, see ACID_BUBBLE_INVESTIGATION.md)
 * read back over screenshot.sh's device-state JSON: the roll rate matched
 * CHANCE=20 to within rounding and every fired roll reached impulse_buf, so
 * the mechanism itself was never broken - it was only ever running at the
 * values this same comment already knew were too subtle to see. */
#define SAND_ACID_BUBBLE_CHANCE 40
#define SAND_ACID_BUBBLE_SPEED  220

/* DILUTION - water touching acid rolls a chance to decide who wins,
 * reusing the same trigger acid's ordinary eating already has: the
 * dissolves/dissolvable pair in step_one_dissolver_cell()
 * (sand_reactions.c). No new field for "does this happen at all" -
 * MAT_WATER's own `dissolvable` (material.c) answers that exactly the
 * way sand's or wood's already does, and this constant only decides the
 * OUTCOME once that roll has already landed. Chance-in-256 that WATER
 * wins, BEFORE SAND_ACID_DILUTE_MASS_BIAS below adjusts it for this
 * particular acid cell.
 *
 * BOTH cells change on either outcome now, symmetrically - see the
 * ladder's own comment in step_one_dissolver_cell() for the mechanism
 * (the winner boils into its own vapour, the loser converts into the
 * winner's material) - so this constant is now a close-to-even coin
 * flip, not a strong lean: 192 (3 in 4) toned down to 160, then to a
 * 55/45 split at 141 while the winning side's cell was still left
 * untouched (a free cell of whichever material won, every single time,
 * which is what actually needed fixing - see SAND_ACID_DILUTE_MASS_BIAS
 * below and the ladder's own comment for the full story). With that
 * fixed, the split itself only needed a small lean rather than a strong
 * one - 134 (roughly 52.3%) for one round - and then, once the win/lose
 * split was no longer the only thing standing between either liquid and
 * unbounded growth, no lean at all.
 *
 * 118, NOT 128 - the naive "half of 256" - because this constant is not
 * read against the full 256-wide roll. SAND_ACID_DILUTE_EVAPORATE_CHANCE
 * is checked FIRST in the ladder (step_one_dissolver_cell(),
 * sand_reactions.c) and its 20-in-256 comes out of the roll's low end
 * before the water/acid split is even reached, so this constant is only
 * ever compared against the REMAINING (256 - 20) = 236-wide range. 128
 * shipped there for one round and was quietly a 128:108 lean toward
 * water (54%/46%) at zero mass bias, not the even split its own comment
 * claimed - caught by review, not by a test, since nothing pinned the
 * unbiased split's exact ratio. 118 is genuinely half of the 236 that is
 * actually on offer once evaporate has taken its cut: 20 + 118 + 118 =
 * 256, splitting the water/acid ladder into two equal 118-wide bands.
 * Whichever side wins a given bite is now decided entirely by
 * SAND_ACID_DILUTE_MASS_BIAS below (local backing) rather than a fixed
 * preference baked into the base rate. Starting bias, not a measured
 * one - tune on device like every other constant here. */
#define SAND_ACID_DILUTE_TO_WATER_CHANCE 118

/* MASS MATTERS - a single roll at the fixed split above can't tell a lone
 * drop of acid resting on a lake from a whole poured-on slab of it; every
 * bite looked the same, so a huge amount of acid dumped on a small puddle
 * never read as different from a trickle, and the same was true in
 * reverse for a lake poured onto a puddle of acid. Fixed directly:
 * step_one_dissolver_cell() counts how many of the ACID cell's own
 * cardinal neighbours are themselves acid, and separately how many of
 * the WATER cell's own cardinal neighbours are themselves water (0 to 3
 * each), and rolls against SAND_ACID_DILUTE_TO_WATER_CHANCE adjusted by
 * the DIFFERENCE between the two, water_backing minus acid_backing,
 * times this constant.
 *
 * The difference matters, not either count alone - a first version only
 * ever measured the acid side and subtracted, which is NOT actually
 * symmetric: since only acid cells ever roll this reaction, a deep, pure
 * acid pool always reads as "backed" from its own side even while an
 * equally deep pool of water sits right next to it, so water could pour
 * onto an acid puddle forever and still not reliably win. With the
 * difference: two equally deep pools facing each other net to zero bias
 * and the base split above holds exactly as it always did; a lone grain
 * of either material still reads as 0 on its own side, so an isolated
 * drop in a big lake gets pushed even further toward diluting (the
 * lake's own water_backing pulls the split up), and it is only once one
 * side's local mass genuinely outweighs the other's that the roll tips.
 *
 * That fixes a pour of ACID onto a pool of water cleanly -
 * test_a_relentless_pour_of_acid_overwhelms_a_pool_of_water
 * (suite_sand_reaction_encoding.c)
 * checks it with an A/B (bias on vs off) comparison and passes. The
 * reverse direction does NOT get the same clean win, and this constant
 * alone cannot fix it: acid's density (38) is higher than water's (30),
 * so a poured acid grain sinks into and disperses through the pool it
 * lands in, reading as an isolated, weakly-backed intruder the way this
 * mechanism assumes - but water sits and floats on TOP of acid instead,
 * so the acid cells actually being bitten stay backed by the deep acid
 * pool underneath them the whole time, never reading as isolated at all.
 * test_a_relentless_pour_of_water_overwhelms_a_pool_of_acid
 * (suite_sand_reaction_encoding.c)
 * documents this directly: biasing the roll measurably HURTS that
 * direction rather than helping it (3451 tap-side acid cells biased vs
 * 3623 unbiased at the same step count), so that test asserts on sheer
 * volume of conversion instead of an A/B comparison. Flagged, not
 * silently patched - fixing it for real would need something that reads
 * density/position, not just local backing counts, and that is a real
 * design decision rather than a tuning knob.
 *
 * Purely local either way - no global concentration tracking, up to 8
 * bounds-checked reads total (4 for each cell's own neighbours), the
 * same kind of local rule the rest of this automaton already relies on
 * to produce a macro-scale effect. Starting bias, not a measured one -
 * tune on device like every other constant here. */
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

/* THE GENERIC EAT ALSO HAS TO COST SOMETHING - reported directly: pouring
 * sand over acid barely shrinks the acid, even though a lot of sand gets
 * eaten in the process. Sand (dissolvable=200), wood (160) and stone (60)
 * all fall through to the SAME shared branch at the bottom of
 * step_one_dissolver_cell() (sand_reactions.c, unlike water and oil which
 * both get their own dedicated ones) and have always paid
 * pay_quench_cost()'s flat one-unit chip per bite - a full MASS_MAX-unit
 * acid cell can eat up to MASS_MAX cells of anything on that path before
 * running out, and against a material as freely dissolvable as sand,
 * landing that many bites does not take long, so the acid reads as
 * barely spending itself for how much it visibly destroys.
 *
 * Same fix as oil's SAND_ACID_OIL_DEATH_CHANCE, same shape: after the
 * normal fizz/residue handling, a further roll gives the acid a real
 * chance to die outright - its whole remaining mass gone in this one
 * bite - instead of always just the one-unit chip. Picked lower than
 * oil's 128: oil is dissolved rarely (dissolvable=16, "one bite in
 * sixteen") so a near coin-flip death rate there still reads as acid
 * mostly surviving contact with it, but sand/wood/stone are dissolved
 * MUCH more readily (dissolvable up to 200) - the same death rate here
 * would let a single bite of ordinary sand kill a full acid cell about
 * as often as not, which reads as acid being unable to eat sand at all
 * rather than eating it at a real cost. Starting bias, not a measured
 * one - tune on device like every other constant here. */
#define SAND_ACID_EAT_DEATH_CHANCE 40

/* ACID RAIN - a mixed pocket of MAT_GAS and MAT_STEAM, sitting together
 * in a 2x2 block (exactly two cells of each - the only composition four
 * cells and an "at least two of each" requirement can ever satisfy), has
 * a small chance to collapse - see step_one_acid_rain_cell()
 * (sand_reactions.c) for the mechanism itself. An extension of the
 * existing steam-to-water condensation trick (reaction_t.condenses,
 * material.c) that already fakes "rain" by collapsing a settled pocket
 * of steam into water: same 2x2 window, same 4-cells-into-1 collapse
 * ratio, generalised to a mixed two-material pocket instead of a
 * uniform one - acid rain, sized exactly like plain rain, not a bigger
 * production of its own. Only the block's own top-left corner survives,
 * not the whole pocket: acid is one of this simulation's biggest
 * producers of gas and steam in the first place (dilution, eating oil,
 * eating sand/wood/stone, its own ambient boil-off all leave one or the
 * other), so a full-count yield would be a feedback loop that could keep
 * a lake topped up indefinitely.
 *
 * The surviving cell resolves 50/50 to Acid or Water, not always Acid -
 * a pocket that is HALF gas and HALF steam has no more claim on being
 * acid's own rain than plain water's, and always-Acid would make this
 * the same one-way tap on the board's acid budget the full-pocket yield
 * already was.
 *
 * 1, the rarest a single byte-wide roll can express, and (deliberately)
 * the same figure steam's own condense-to-water roll uses
 * (reaction_t.condenses, MAT_STEAM's row in material.c) - pinning both
 * "rain" mechanics to the same per-window floor. This constant is
 * already at that floor: a big enough vapor pocket offers many
 * overlapping qualifying windows every step, so it will still convert
 * fairly readily even here - that scaling comes from how many windows a
 * large pocket offers, not from this constant, so there is no lower
 * value left to try if it still reads too frequent on device; the fix
 * at that point has to be the mechanism, not this number.
 *
 * Chance-in-256, checked only once a full 2x2 has already been confirmed
 * to hold nothing but gas and steam, two of each. Starting bias, not a
 * measured one - tune on device like every other constant here.
 *
 * Chance-in-256, checked only once a full 2x2 has already been confirmed
 * to hold nothing but gas and steam, two of each. */
#define SAND_ACID_RAIN_CHANCE 1

/* QUENCHING A FLAME - Acid snuffs fire unlike water's crisp steam. Two rolls:
 * "visible effect?" then "what is it?" SAND_ACID_QUENCH_RESIDUE_CHANCE
 * (1/256) checks for residue. Miss means clean quench.
 * SAND_ACID_QUENCH_SMOKE_CHANCE (1/256) checks if residue is smoke, likely.
 * Tune chances as needed. */
#define SAND_ACID_QUENCH_RESIDUE_CHANCE 96
#define SAND_ACID_QUENCH_SMOKE_CHANCE   180

/* How much of the blast radius sand_explode() fills with fire before it
 * queues a single flight entry - the filled radius is `radius /
 * SAND_EXPLODE_CORE_DIVISOR`. Explosion-specific, unlike the two constants
 * above: nothing about sand_impulse() itself has a "core", so this stays
 * named for the one caller that has one.
 *
 * Without this, sand_explode() only ever SEEDS entries; it never makes
 * room for them. In an incompressible medium - a packed bed, a body of
 * water - every cell inside the radius starts out surrounded by more of
 * the same material, so the very first move every entry attempts is
 * blocked. A blocked entry now waits rather than dying (see
 * step_impulses()'s own comment), but waiting for a gap that nothing will
 * ever open is still nothing happening: reported from a device, this is
 * exactly what read as "in water nothing happens, in sand also no holes"
 * before this existed.
 *
 * FIRE, not a hole. An earlier version of this simply erased the core -
 * correct for making room, wrong for what an explosion actually is: it
 * flashes and leaves a plume, it does not silently delete whatever was
 * standing there. Fire is far LIGHTER than almost anything it might be
 * detonating into (density 15 against sand's 60, water's 30), so
 * can_enter()'s ordinary "a denser mover displaces a lighter fluid" rule -
 * the same one that already lets sand sink through water or gas, with no
 * special-casing added for this - lets the surrounding medium swap straight
 * through the fire with no need to wait for it to move or decay away
 * first. Measured, not assumed: a 20,000-seed sweep of a fully packed bed,
 * with the flight pass disabled, found the resulting cavity reaching well
 * outside the original radius on literally the first step, every time. The
 * mechanism this replaced - a plain hole - relied on exactly the same
 * neighbouring-cell collapse; filling with fire costs it nothing.
 *
 * A fraction of the blast's OWN radius, not an independent constant, so a
 * bigger blast gets a bigger flash automatically instead of the same
 * fixed-size fireball no matter how large the outer radius grows.
 *
 * THE RULE THIS RATIO ANSWERS TO: a fireball is small; a pressure wave is
 * large. A real explosion is two effects at very different scales, not
 * one - a small, hot kernel of actual combustion, and a much wider shock
 * that does the displacing. sand_explode() draws exactly that as three
 * concentric zones: the CORE (radius / SAND_EXPLODE_CORE_DIVISOR)
 * converts to fire - that is the fireball, and it should read as small;
 * the ANNULUS between the core and the full radius is thrown outward -
 * that is the pressure wave, and it should read as most of the disc;
 * beyond the full radius, nothing. This is the reasoning test for the
 * constant, not the arithmetic alone: a divisor near 1 makes the
 * "fireball" as big as the blast itself, which is a bomb made of fire,
 * not a bomb that starts one. A divisor picked to keep the core's AREA a
 * small fraction of the disc's - see the measurement below - is what
 * keeps the two zones reading as the different-scaled effects they are
 * meant to be, and is the test any future retuning of this number should
 * be held to, not just "does it look a bit bigger or smaller".
 *
 * WAS 2 (half the radius), MEASURED WRONG. Half the radius is a QUARTER of
 * the disc's area (area scales with the square of the radius), and a
 * device pass on the first real-radius detonation confirmed exactly that
 * reads as deletion, not a flash: "most particles are just being
 * removed". 3 drops the converted area to about 11% - big enough that
 * even radius 1 still fills its own centre cell (any divisor does; the
 * centre is always within a core radius of zero), small enough that a
 * wide blast keeps most of its own disc as real material for the flight
 * pass to throw, rather than a quarter of it never existing to be thrown
 * at all. Was itself a starting point, not a fully tuned measurement - the
 * device pass that caught the old value never confirmed this one either.
 *
 * WAS 3, RAISED TO 5 - now an actual measurement, and the one of these
 * three constants where the numbers argued hardest for a specific value
 * rather than just a direction. See SAND_IMPULSE_SPEED_RAMP's own comment
 * for the sweep methodology. Independently, divisor alone barely moved
 * "grains outside the footprint" at all (3 vs 4 vs 5 landed within a
 * couple of points of each other at every fixed SPEED/RAMP pairing) - it
 * is not a throw-distance knob, which makes sense: it decides how much of
 * the disc becomes fire before anything is thrown, not how far the
 * annulus that IS thrown then travels. What it moves instead is
 * DESTRUCTION, hard, because the core's own area is what it directly
 * controls: at the combination this ships with (255 speed, ramp 2), the
 * dune scene's own "material destroyed" reading came in at 184.4 for
 * divisor 3, 106.5 for 4, and 47.7 for 5 - divisor 5 destroys barely a
 * QUARTER of what divisor 3 does, for statistically the same throw (80.0
 * outside vs 81.4, 1.8 max-throw either way, n=30). That is exactly the
 * trade worth making: the standing complaint driving this round was too
 * much material vanishing into the core, not too little being thrown, and
 * this constant turned out to be the one that answers that complaint
 * almost for free. Confirmed in the full 18-way combined grid too, not
 * just at the shipped SPEED/RAMP pair - divisor 5 was the cheapest of the
 * three divisors for destruction at every other pairing in the grid as
 * well, never only on average. Not yet confirmed on a device.
 *
 * RECONFIRMED, WITH A BIGGER EFFECT, AT DOUBLE THE RADIUS. The core's own
 * area scales with the SQUARE of the radius, so doubling the radius from
 * 24 to 48 cells (see DETONATE_RADIUS_PX in app_sand.c) roughly quadruples
 * everything divisor decides - measured, not merely expected: at SPEED
 * 255, RAMP 2, "destroyed" came in at 730.5 for divisor 3, 411.6 for 4,
 * and 237.4 for 5, each almost exactly what the r=24 figures above times
 * four would predict. Unlike at the old radius, divisor here also nudged
 * "outside" UP as it rose (108.5 / 116.1 / 122.1 across 3/4/5) rather than
 * sitting flat - a bigger core apparently costs a little of the annulus's
 * own throw too, not just what it converts - so 5 is no longer merely the
 * cheapest-for-the-same-throw option, it is the best of the three on BOTH
 * numbers at once at this radius. Still not confirmed on a device at this
 * new radius.
 *
 * PLAIN DIVISION IS CLAMPED TO A MINIMUM OF 1 for any radius >= 2 - see
 * sand_explode()'s own comment on `core_radius` in sand.c. Raising the
 * divisor to 3 made a small enough radius round down to a bare
 * single-cell core (radius 2, for instance: 2 / 3 = 0), and a 20,000-seed
 * sweep found that single cell genuinely insufficient to seed the
 * density-swap collapse a packed medium depends on - stuck on about 11%
 * of seeds, not merely slow, however many further steps it was given. The
 * clamp restores exactly the core shape the OLD divisor of 2 already gave
 * at every radius small enough for a later divisor to have zeroed it out.
 * Raising the divisor again, to 5, changes nothing about that: 24 / 5 = 4
 * is still far above the floor at the radius a real detonation actually
 * uses, so the floor engages at exactly the same small radii it always
 * did, for exactly the same reason. */
#define SAND_EXPLODE_CORE_DIVISOR  5

/* How many of fire's sixteen shades a blast core sheds from centre to rim -
 * see fill loop in sand_explode() (sand.c). 8 ensures clear gradient, keeping
 * outer ring alight. Raising past MATERIAL_VARIANTS - 2 is redundant; floor
 * of 1 clamps it. 0 restores flat disc. */
#define SAND_EXPLODE_CORE_FADE     8

/* ONE CALLER OF sand_impulse(), seeding many radially. Queue an outward-
 * facing flight entry - at SAND_EXPLODE_INITIAL_SPEED - for every occupied
 * cell in the annulus between the core and the full radius, direction
 * quantised the same way sand_gravity_direction() already quantises
 * gravity into eight directions. Everything about HOW a queued grain then
 * moves - the flight pass, the arc, the cap, re-acquisition - belongs to
 * sand_impulse() and step_impulses(); this function's only job is
 * deciding WHICH cells get queued and in which direction, which is the
 * one thing genuinely specific to a displacement's shape.
 *
 * THE PURE DISPLACEMENT PRIMITIVE, WITH NO MATERIAL CONVERSION OF ITS
 * OWN - split out from sand_explode() (below) specifically so a caller
 * that wants the push without the fire has somewhere to call. A banked
 * idea makes this concrete: a stone shield over lava, breached by trapped
 * steam PRESSURE rather than heat, must not set anything alight just
 * because it pushed material around - folding fire into this primitive
 * would make that impossible to ask for. There is a second reason beside
 * that correctness one: converting a cell to fire latches
 * `may_have_burning` (see sand_explode()'s own comment in sand.c), which
 * keeps the whole reactions pass active every step until that fire burns
 * out - a real, ongoing cost a caller that fires often and never wanted
 * fire (a chain of confined-steam bursts, say) has no reason to pay. This
 * function never touches fire, smoke, or the burning flag at all, so it
 * costs neither the correctness risk nor the ongoing expense.
 *
 * QUEUED BY RING, OUTWARD FROM THE CENTRE - not by row. An earlier version
 * scanned dy-outer, dx-inner, which reads as an ordinary nested loop right
 * up until the buffer is smaller than the disc: a device pass on the
 * first real-radius detonation found that order had handed the entire cap
 * to the top nine or ten rows of a forty-nine-row disc before the scan
 * ever reached the core or the lower half, so almost nothing below the
 * centre ever received an impulse at all - not a size problem alone (see
 * APP_IMPULSE_MAX's own comment in app_sand.c), a BIAS problem: any cap
 * smaller than the disc truncates in whatever order the scan visits
 * cells, and top-to-bottom is the least fair order there is. Ring order
 * - every cell at Chebyshev distance 0 from the centre, then every cell
 * at distance 1, and so on outward - means a truncated cap still yields a
 * complete, symmetric, smaller disc instead of a lopsided crescent. This
 * is why the cap being large enough barely matters on its own: whatever
 * caller's buffer, however tight, degrades the same way this one
 * would if a future radius ever outgrew it again.
 *
 * Same signature shape as sand_spawn() and sand_erase() (a centre and a
 * radius) and, like sand_explode() below, belongs to their family: no
 * material owns it and no reaction fires it, so tests call it directly.
 * See docs/Sand/Explosion-Plan.md for the design this and sand_explode()
 * both implement and why neither needs a per-cell velocity field.
 *
 * A no-op if sand_enable_impulses() was never called - there is nowhere
 * to queue an entry. Also a no-op for any cell with no defined outward
 * direction, which is exactly the centre cell itself; every other
 * occupied cell in the annulus between the core and the full radius gets
 * one.
 *
 * Past the buffer's own capacity, sand_impulse() itself already queues
 * nothing further - see its own comment - so this simply keeps scanning
 * the rest of the disc without incident. A push bigger than the buffer is
 * a smaller, but still evenly-shaped, one - see the ring-order comment
 * above for why "smaller" no longer means "missing its entire lower
 * half" - not a bug.
 *
 * CONSERVATION: EXACT, not merely bounded - unlike sand_explode() below,
 * which fills a core with fire first (see its own comment on why that
 * fill is a real, deliberate increase). This function alone neither
 * creates nor destroys a single cell of material; every grain it queues
 * is relocated, never conjured or deleted, whatever direction it ends up
 * flying or whether it was already there. That holds even for a
 * dislodged wall cell (see the density-toughness paragraph below) - the
 * same material, just moved.
 *
 * EVERY OCCUPIED ANNULUS CELL IS SEEDED, NOT A SAMPLE OF THEM - TRIED AND
 * REJECTED, not left unconsidered. Once a flying grain could displace what
 * it hit (see can_impulse_enter() in sand.c), it was reasonable to guess
 * that seeding only a fraction of the annulus might be enough: a pushed
 * grain shoulders its neighbours aside on the way, so maybe motion
 * propagates through the medium on its own and the rest never needed
 * their own entry. Measured against the dune scene at this file's shipped
 * constants and DETONATE_RADIUS_PX's 48-cell radius: full seeding put
 * "grains outside the footprint" at 122.1 (n=30); seeding every OTHER
 * annulus cell (a spatial checkerboard, not a scan-order stride, so
 * coverage stayed uniform) dropped it to 90.6; a quarter dropped it
 * further, to 65.7 - a clear, monotonic decline, not noise. "Destroyed"
 * barely moved (237.4 / 236.4 / 235.8), which makes sense: that number is
 * the core's own area, untouched by how the annulus is seeded. The
 * hypothesis does not hold, and the reason is structural, not a tuning
 * miss: a cell displaced as someone ELSE's neighbour gets shoved exactly
 * once, into whatever cell that mover just vacated, and then sits still -
 * it was never itself given a direction or a speed, so nothing carries it
 * any further unless a second, separately-seeded entry happens to reach
 * it again. Sparse seeding was hoping for a chain reaction that the
 * design, correctly, does not produce (see step_impulses()'s own "no
 * cascade" discipline in sand.c) - the fix for a deep grain having
 * nowhere to go was letting it be PUSHED, not letting one push propagate
 * indefinitely through everything nearby. Full seeding stayed the
 * deliberate default because of the bound this project already holds
 * every part of this mechanic to - not a manual, caller-chosen sparsity
 * that trades density for radius, which loses to a smaller full-density
 * disc for the structural reason above.
 *
 * WHAT DID CHANGE: NOT THE CONCLUSION, THE QUESTION. The comparison above
 * asked which DELIBERATE seeding density throws better, at a radius
 * someone had already fixed - it never asked what a caller should do
 * when the buffer it was actually given cannot hold the disc a radius
 * implies at all. That second question got asked for real when
 * DETONATE_RADIUS_PX doubled to 96 px (48 cells) and the buffer this
 * mechanic's own caller-provided sizing used to derive FROM that radius
 * could not be allocated on real hardware - see SAND_IMPULSE_BUDGET_
 * BYTES's own comment in app_sand.c for the failed malloc and the silent
 * no-op it produced. The fix is not a manual sparse mode a caller opts
 * into (that was tried above and lost) and not a smaller radius chosen
 * by hand to dodge the failure (that only relocates the same bug to
 * whatever radius is requested next) - it is this function itself
 * degrading its OWN density automatically, only when a disc's true cell
 * count (exact_disc_count() in sand.c) exceeds the buffer it was
 * actually given, spread evenly across the whole disc via the same
 * digital-differential-analyser accumulator queue_outward_impulse() uses
 * for everything else - see that function's own comment. Below the
 * buffer's capacity, this is unobservable: every existing small-radius
 * caller and test still gets full seeding, unchanged, because `keep`
 * equals the true count exactly when the disc fits. Above it, thinning
 * costs exactly what the measurement above predicts it would - at
 * 96 px against the real, device-corrected 2,048-entry budget (see
 * SAND_IMPULSE_BUDGET_BYTES's own comment in app_sand.c for why this
 * number is smaller than an earlier host-only estimate - a live device
 * capture, not more arithmetic, is what fixed it): ~28% of that radius's
 * true 7,213-cell disc, and "grains outside the footprint" measured 67.1
 * against build_sand_dune_scene() - well below the 106-107 a full-density
 * 24-25-cell blast reaches at this same budget, though still ahead of
 * that smaller blast on reach and destruction. A real device confirmed
 * 96 px thinned detonating successfully, at a visibly small result - and
 * given that exact tradeoff, DETONATE_RADIUS_PX was retuned down to 25
 * cells (50 px) instead, trading the extra reach and destruction back
 * for the full-density number, a choice this mechanic's design leaves
 * entirely open and is not this comment's place to relitigate - see
 * DETONATE_RADIUS_PX's own comment in app_sand.c for the full account
 * and the figure that decision actually landed on. The conclusion above
 * did not change either way: seeding sparsely is still worse, cell for
 * cell, than seeding fully. What changed is that a caller no longer has
 * to choose between honouring that conclusion and fitting in the memory
 * it was actually given - this function spends every entry the buffer
 * allows before it starts thinning at all, whatever radius it is asked
 * to thin.
 *
 * A WALL CAN NOW BE DISLODGED, TOUGHER RATHER THAN INVISIBLE. Every
 * occupied annulus cell being seeded, above, used to mean "every occupied
 * NON-STATIC cell" in practice - sand_impulse() itself refuses a
 * KIND_STATIC source unconditionally (see its own comment), so stone,
 * glass and wood sat inside a blast's own annulus completely untouched
 * regardless of how much force reached them. This function's own seeding
 * loop (queue_outward_impulse(), sand.c) now reaches a static candidate
 * through a separate, explicit path (queue_flying_grain() there, with its
 * `allow_dislodge_static` opt-in) that rolls a density-scaled chance
 * instead of refusing outright - see that function's own comment for the
 * formula and why lower density means an easier dislodge. sand_impulse()
 * itself did not change at all: it still refuses every static source,
 * every time, for every caller that has not explicitly asked otherwise,
 * which today is every caller except this one. The toughness applies to
 * displacement as a whole, not specifically to fire's own edge - a
 * future pure-pressure caller (the confined-steam idea above) pushes
 * against the exact same density-scaled resistance an explosion does,
 * because both reach it through this one shared function. */
void sand_displace(sand_t *s, int cx, int cy, int radius);

/* Same as sand_displace(), but only cells whose material is exactly
 * `mat_id` are ever queued - see its own comment in sand.c. Used by
 * splash_displace() (sand_liquid.c) so a liquid's splash cannot fling
 * unrelated material (dirt under a pool of water, say) along with it. */
void sand_displace_material(sand_t *s, int cx, int cy, int radius,
                            uint8_t mat_id);

/* A THIN WRAPPER AROUND sand_displace(), ABOVE, adding exactly one thing
 * to it: a core of fire. Fill a disc of `radius` around (cx, cy) with
 * fire at its core (see SAND_EXPLODE_CORE_DIVISOR), then hand the rest -
 * the annulus, the density thinning, the wall-toughness roll, everything
 * about a displacement that is not specific to fire - to sand_displace()
 * entirely; see its own comment for all of it. This split exists for two
 * reasons, not one - see sand_displace()'s own comment for both in full -
 * and this function is what still needs the fire half: an explosion, as
 * opposed to a bare push, is specifically the case that wants combustion.
 *
 * FIRST fills a core with fire - see SAND_EXPLODE_CORE_DIVISOR's own
 * comment for why an explosion that only ever queued flight entries could
 * never actually move anything once the medium it is detonating in has no
 * gaps of its own, and for why fire rather than emptiness. That fill is a
 * real, immediate write - every cell in the core becomes fire, occupied or
 * already empty alike - not something sand_impulse() or the flight pass it
 * feeds has any part in, and it is why sand_explode() is no longer purely
 * additive to the grain count the way sand_spawn()/sand_erase()
 * individually are: see the comment on conservation this implies, below.
 * Placing fire is a normal write like any other in this file - it latches
 * the content flags a burning cell arms (so the reactions pass notices it),
 * marks rows dirty and wakes blocks - so fire near fuel ignites it and
 * fire touching water boils it to steam, exactly as painted fire would.
 *
 * Same signature shape as sand_spawn() and sand_erase() above - a centre and
 * a radius - because it belongs to that family: no material owns it and no
 * reaction fires it, so tests call it directly and the app calls it from a
 * temporary mode (see app_sand.c). See docs/Sand/Explosion-Plan.md for the
 * design this implements and why it needs no per-cell velocity field.
 *
 * A no-op if sand_enable_impulses() was never called - there is nowhere to
 * queue an entry, and the core is left unfilled too, so a disabled
 * mechanic costs the board nothing at all, not even the fire. Everything
 * past that check is sand_displace()'s own set of no-ops (an occupied
 * centre cell with no direction to throw it in, a buffer already at
 * capacity) - see its own comment.
 *
 * CONSERVATION, NOW BOUNDED RATHER THAN EXACT - unlike sand_displace()
 * alone (see its own comment), because of the fire this wrapper adds.
 * sand_spawn() and sand_erase() are each individually exact - every cell
 * they touch is accounted for in what they return. sand_explode() is
 * not, in EITHER direction it might first seem to move: filling an
 * already-empty core cell with fire is a real, deliberate increase,
 * exactly once, right here - not a bug to guard against, just what "the
 * core flashes into fire even where there was nothing to convert" means.
 * From that point on the count can still fall further: fire is a real
 * burning cell now, and if the medium around it ever traps it with no
 * denser neighbour able to sink through and no escape upward,
 * smothered() (sand_reactions.c) puts it out for good, the same as any
 * other buried flame. What can never legitimately happen, from either
 * the fill or anything after it, is the count exceeding (count before
 * the call) + (empty cells the core just filled) - that half is exact,
 * always. */
void sand_explode(sand_t *s, int cx, int cy, int radius);

/* Grain friction modelled by burial count, halving slide chance. Falling not
 * affected by burial. */

/* Chance in 256 that a grain with exactly one grain above it may still slide.
 * Halves for each additional grain, so a pile locks up quickly with depth. */
#define SAND_SLIP_CHANCE 96

/* Beyond this much load a grain cannot slide at all. Without a hard floor the
 * chance only ever approaches zero, and at 60 steps a second "almost never"
 * still visibly creeps. */
#define SAND_LOAD_CAP 5

/* How far a liquid seeks a shallower spot non-locally. Local rules limit
 * levelling speed (184-cell pool: 5k steps for 6-cell height), simulating
 * water pressure in falling-sand games. Flow halts at non-matching liquids.
 * Search distance (sight): 4 -> 1.6, 8 -> 0.8, 16 -> 0.5, 32 -> 0.2 cells
 * off-level, looks incorrect temporarily. */
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

/* How often a burning LIQUID (lava, today) that has just done the WORK of
 * actually converting a neighbour into something else - a real melt, not
 * merely banking one more level of heat, see step_one_burning_cell()'s
 * and cool_off_chain()'s own comments (sand_reactions.c) for the check
 * that tells the two apart - freezes ITSELF as the cost, and the same
 * chance a frozen cell's own cool_off_chain() reaches for at each further
 * link into the pool beside it.
 *
 * Chance in 256, per EVENT, not per step - a coarser gate than an
 * ordinary chance-in-256-per-step field. This only ever rolls on
 * something that is already rare
 * (a genuine material change, or a successful water quench), so a small
 * figure here still adds up to real, visible progress under a sustained
 * pour without needing anywhere near the once-a-step rates the heat ramp
 * itself is tuned to. Starting point, tune on device like every other
 * constant in this file.
 *
 * NO SECOND ROLL TO SKIP - nothing stacks on top of this one, so
 * sand_set_lava_cooloff(255) gets a single, deterministic firing on
 * every qualifying event, exactly like every other value this accepts.
 *
 * 12 -> 32 after watching a real pour on device. The first figure was
 * chosen to be deliberately rare against events that are themselves
 * common, and it undershot: a pour read as barely biting. Raised here,
 * rather than by letting one event reach further (SAND_LAVA_COOLOFF_
 * MAX_CHAIN, tried at 16 and put back) - MORE EVENTS, each still short,
 * is what makes the pour rate the thing that sets how fast a pool dies,
 * which is the whole shape this mechanic was designed around. */
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

/* SAND_GAS_IGNITE_BLAST_RADIUS's own figure (sand_reactions.c) - the only
 * other reaction-driven sand_explode() caller that exists today, so
 * there is no reason yet for this one to differ from it. A starting
 * point to tune on device, not a considered figure of its own. */
/* RAISED 8 -> 16 to make the fire actually visible, reported on device as
 * 'barely noticeable'. The fire is not tuned by this number directly: it
 * is the CORE that burns (sand_explode() fills radius /
 * SAND_EXPLODE_CORE_DIVISOR with fire before handing the rest to
 * sand_displace()), and with the divisor at 5 a radius of 8 gave a core
 * radius of ONE - about five cells of flame, which is why a detonation
 * read as a flicker. 16 gives a core radius of 3, near thirty cells.
 *
 * THEN 16 -> 12, swapping places with SAND_GUNPOWDER_BLAST_RADIUS
 * (material.h) once gunpowder existed: the one material whose whole point
 * is to go off should own the biggest reaction-driven blast on the board,
 * and a lava burst is a side effect of a vessel, not a charge. Core radius
 * 2 here now, about thirteen cells of flame - still well past the flicker
 * that 8 gave.
 *
 * Raising this rather than lowering SAND_EXPLODE_CORE_DIVISOR on purpose:
 * the divisor is shared by every explosion in the app and carries its own
 * measured tuning table, so moving it to fix one caller's fireball would
 * silently rescale the hand-fired detonate mode and the confined-gas burst
 * as well.
 *
 * The cost is a disc walk of about four times the cells, paid only when a
 * burst actually fires - 1 in 1024 per covered cell per step
 * (SAND_LAVA_BURST_GATE) - and the disc still fits the impulse budget
 * without thinning, so this buys visibility without changing what the
 * blast is allowed to do. Starting point, tune on device. */
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
