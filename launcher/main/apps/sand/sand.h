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

/* A cell is one byte - material in the high nibble, variant in the low one.
 * See material.h, which owns the encoding and the properties.
 *
 * The variant travels with the cell rather than being derived from position,
 * which matters more than it sounds: position-derived colour makes a moving
 * pile shimmer, because each grain changes colour as it falls.
 *
 * These names are kept because most of this module's tests are about sand
 * specifically, and "SAND_FIRST_SHADE" reads better in them than a CELL_MAKE
 * incantation. */
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

/* How many persistent emitters sand_t can carry at once - see
 * sand_add_emitter() below.
 *
 * A fixed cap, not a caller-owned count, is what lets the list be a small
 * inline array rather than something the app has to allocate and pass in -
 * see the comment on the `emitters` field itself for why that matters. 16
 * is far more taps than a 368x448 board has room for at any placement a
 * person would actually choose: spaced so each one's stream is visually
 * distinct, that board does not have 16 sensible places to put a source at
 * once. The cap exists to bound the array, not because 16 is a number
 * anyone is expected to reach. */
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
    /* Counts steps, and is read by exactly one thing: which part of its
     * shade band a freshly poured grain takes - see random_cell(). Two
     * pours a few seconds apart therefore come out as two different
     * shades, and because the shade lives in the cell it stays put when
     * the first pile is buried under the second.
     *
     * Deliberately not a mechanism. There is no pass, no flag, no
     * per-cell test and no extra draw: the shade was always chosen by one
     * random number at spawn, and this only changes what that number is
     * centred on. A version that instead crusted surfaces in place cost
     * 4.4 microseconds a step against 1.0 and was reverted; this costs
     * one increment per step, whatever is on the board. */
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

    /* Same idea again, for anything that burns - see sand_step_reactions()
     * in sand_reactions.c. Named may_have_fire until wood and ember
     * arrived; renamed once a second material could set it, since the
     * flag was never really about MAT_FIRE specifically - see
     * reaction_t.burns in material.h. Keyed on that field wherever it is
     * set, NOT on kind == KIND_STATIC - stone shares that kind with ember
     * and is poured far too often to accidentally re-arm this on every
     * touch. */
    bool     may_have_burning;

    /* Same idea again, for a material that dissolves others
     * (reaction_t.dissolves - acid is the only one today). Separate from
     * may_have_burning because dissolving is not a fire reaction: acid
     * has to work on a board with no flame anywhere, so
     * sand_step_reactions() runs when EITHER flag is set. */
    bool     may_have_dissolver;

    /* And again for cells with a TEMPERATURE - glass holding heat in its
     * variant, or anything cold enough to `chill`, which is snow.
     *
     * Its own flag rather than riding may_have_burning, for the same
     * reason may_have_dissolver has one: a pane goes on cooling long
     * after the fire that heated it is out, and cooling is the half of
     * the ramp that makes it mean duration. Gated behind the fire flag it
     * would freeze mid-ramp instead - and snow would sit in a pond
     * forever on a board where nothing happened to be burning.
     *
     * Named for temperature rather than heat because cold is half of it.
     * It was may_have_temperature while glass was the only thing that had one. */
    bool     may_have_temperature;

    /* And again for anything WET, or anything that could become wet: a
     * cell holding moisture, or a soaker sitting where there is liquid to
     * soak. Dirt drying out has to keep happening after the puddle that
     * wetted it is gone, which is why holding moisture arms it on its own.
     *
     * Deliberately NOT armed by "a soaker exists" alone. Sand soaks, and
     * sand is on almost every board, so that would run the reactions pass
     * always - see step_one_reacting_row(), which only reports this found
     * when a cell is actually wet or actually beside a liquid. */
    bool     may_have_moisture;

    /* Something on the board falls in the COLD pass rather than in the
     * sweep - see reaction_t.falls. Its own flag because it is the one
     * reason the reactions pass may need to run on a board with no fire,
     * no acid, no heat and no water anywhere on it: a seed dropped onto a
     * bare screen still has to reach the floor. */
    bool     may_have_faller;

    /* Whether anything a hot gas could ACT on exists anywhere on the
     * grid: something that banks heat and can climb a level (stone,
     * glass), or something cold that cannot bank it and thaws outright
     * instead (ice, snow).
     *
     * A different question from may_have_temperature above: that one asks
     * whether something is currently OFF ambient and needs its ramp
     * ticked; this one asks whether there is anything to warm in the
     * first place, which is what gates convection in
     * step_one_reacting_row() - see the comment on that branch, and on
     * step_one_warming_cell(), for why a board of nothing but smoke and
     * steam (both `warms`) needs this to stay closed rather than paying a
     * four-neighbour scan on every gas cell, every step, forever.
     *
     * NOT one of the flags sand_step_reactions() checks to decide whether
     * to run at all - it is a gate on one branch inside the pass, not a
     * reason for the pass to exist. A board can need the reactions pass
     * for fire, acid, moisture or a faller with no heat-holder on it
     * anywhere, and this flag has nothing to say about any of that. */
    bool     may_have_heat_holder;

    /* And its own flag for withering, rather than riding the one above -
     * the same reasoning that gave may_have_dissolver and
     * may_have_temperature theirs. They looked like one flag while the
     * plant was the only material that both fell and withered. Foliage
     * does neither-and-both: it never falls, so it arms nothing above, and
     * it does wither, so the pass has to keep visiting it. A leaf on an
     * otherwise bare board would simply have been skipped for ever. */
    bool     may_have_withering;

    /* And its own flag again for condensing (reaction_t.condenses - steam
     * is the only one today), for the same reason may_have_dissolver and
     * may_have_withering each got theirs: condensing is not a rider on
     * anything else. `boils` needs no such flag - it only ever gets read
     * from inside conduct_heat(), which is already reached through
     * may_have_burning - but condensing has to keep being checked for as
     * long as any steam exists on the board, whether or not anything is
     * burning, dissolving, tempered, wet, falling or withering. Without
     * its own flag, a board holding nothing but a drifting cloud of steam
     * would have sand_step_reactions() stop running the moment nothing
     * else gave it a reason to, and the steam would sit there forever,
     * never getting a chance to condense. */
    bool     may_have_condenser;

    /* See sand_set_soak(). 0, the default, means nothing soaks. */
    int      soak;

    /* Optional, caller-owned, h bytes: which rows changed since it was last
     * cleared. NULL disables tracking entirely. See sand_track_dirty_rows(). */
    uint8_t *dirty_rows;

    /* Optional, caller-owned, block_cols*block_rows bytes: which blocks of
     * the grid are worth looking at at all. NULL means look at every
     * block, every step - the main sweep always walks the grid in blocks
     * of SAND_BLOCK_W x SAND_BLOCK_H (see step_one_row()), whether or not
     * this is set, so block_cols/block_rows are always real, sand_init()-
     * derived values, never zero. See sand_enable_sleeping(). */
    uint8_t *block_state;
    int      block_cols, block_rows;   /* set once, by sand_init() */

    /* Optional, caller-owned, `impulse_max` entries: grains currently in
     * flight from sand_impulse() - see sand_enable_impulses(). NULL
     * disables the whole mechanic, the same way dirty_rows/block_state
     * above disable theirs. impulse_count is how many of impulse_buf's
     * entries are live right now, always <= impulse_max. */
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

    /* THE ROLLING-MODULO CLUMP behind reaction_t.flaw_to (material.h) - see
     * try_heat_transform()'s own comment (sand_reactions.c) for the
     * mechanism. Shared across every material that ever sets flaw_to
     * (dirt is the only one today), deliberately: this is what makes
     * consecutive smelt successes come out as a run of the same
     * material - a nodule - instead of an independent per-cell coin flip.
     * heat_flaw_seq counts triggers; heat_flaw_is_flawed is the decision
     * currently being shared across the run of HEAT_FLAW_CLUMP of them. */
    uint16_t heat_flaw_seq;
    bool     heat_flaw_is_flawed;

    /* The material-pair classification table (pair_bits[][], sand_
     * reactions.c) that used to live here as two separate 16-bit masks
     * (heat_mask, wet_mask) is now a single file-scope static in
     * sand_reactions.c instead of a sand_t field - see that table's own
     * top comment for why: at 256 bytes it is affordable exactly once,
     * not once per sand_t, and every sand_t rebuilds it identically from
     * the same global reactions[]/materials[] tables every pass anyway, so
     * per-instance storage bought nothing the two masks were not already
     * paying for out of habit. */

    int      last_load_dx, last_load_dy;

    /* The DITHERED direction of the last step, as opposed to the nearest
     * one above. A tilt that falls between two of the eight directions
     * spends some steps on each, in proportion - which is how a pile flows
     * at its true angle instead of snapping to the nearest eighth.
     *
     * Anything that wants to point along gravity over TIME rather than
     * within one step wants this one. Growth does: a stem built from the
     * nearest direction is a rigid straight line at one of eight angles,
     * and the board it is growing on does not work that way. */
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

    /* Persistent point sources - see sand_add_emitter() below.
     *
     * Deliberately NOT cells. The grid is one byte per cell (material in
     * the high nibble, variant in the low one - see CELL_MAKE above) and
     * docs/Sand/Sand-Simulation.md says it always will be; the material
     * nibble has no spare id to spend on a MAT_SOURCE, and the extended
     * range that would otherwise hide one is already spoken for by real
     * materials. Giving an emitter a material id would also make it cost
     * something on every cell it did NOT occupy - material_of() is read
     * per cell per step by the sweep, and a sixteenth entry in that table
     * existing only to be recognised and skipped is a tax paid by every
     * other cell on the board, forever, for a feature most boards will not
     * even use.
     *
     * So an emitter lives here instead: a small side list, stepped once per
     * sand_step() (see emit_from_emitters() in sand.c), that writes an
     * ordinary cell through sand_set() when its own point is empty. No
     * material id consumed, no per-cell cost anywhere else on the grid, and
     * the byte at an emitter's own point is indistinguishable from one a
     * person poured there by hand.
     *
     * Inline, unlike `cells`/`dirty_rows`/`block_state` above, which are
     * caller-owned because they are sized by the grid and can run to tens
     * of kilobytes. SAND_MAX_EMITTERS entries at a few bytes apiece is
     * small enough to just carry inside sand_t, and doing so means a
     * caller that never places an emitter need not allocate a second
     * buffer, pass it in, or even know the feature exists. */
    struct {
        int16_t x, y;
        cell_t  cell;
    } emitters[SAND_MAX_EMITTERS];
    int      emitter_count;
} sand_t;

/* `cells` must have room for w * h bytes and is cleared. */
void sand_init(sand_t *s, uint8_t *cells, int w, int h, uint32_t seed);

void sand_clear(sand_t *s);

/* Record which rows change, into a caller-owned array of `h` bytes.
 *
 * Opt-in, because it only pays for itself if someone acts on it. On this board
 * the caller does: a row that did not change need not be redrawn, and a screen
 * band containing no changed rows need not be sent to the panel at all - which
 * is most of a frame's cost. See gfx_mark_dirty().
 *
 * Rows are set to 1 and never cleared here; clearing is the caller's job, once
 * it has acted on them. Everything that can alter a cell marks it: settling,
 * spawning, sand_set and sand_clear. */
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

/* Diagnostic only: whether block (bx, by) - in SAND_BLOCK_W x SAND_BLOCK_H
 * units, not pixels or cells - is currently settled under either dithered
 * direction. The bit layout behind this is private to sand.c/sand_priv.h;
 * this exists only so a caller instrumenting real-device performance (see
 * app_sand.c's count_awake()) can ask the question without reaching into
 * it. Always false if block-sleeping was never enabled. */
bool sand_block_settled(const sand_t *s, int bx, int by);

/* Let sand_impulse() queue a flying grain instead of doing nothing.
 *
 * The same caller-provided-buffer shape as sand_enable_sleeping() and
 * sand_track_dirty_rows() above, and for the same reason: a board that
 * never uses an impulse should not pay for the mechanic, not even the
 * struct space, and a test can size `buf` deliberately small to exercise
 * the cap on purpose (see sand_impulse()'s own comment on what happens
 * once it fills).
 *
 * `max` is how many of `buf`'s entries may be in flight at once - a bounded
 * transient list, the same shape crack_run() already uses for CRACK_MAX
 * (sand_reactions.c), not a per-cell flag: an explosion throws at most a few
 * hundred grains for a few dozen steps each, so this is a few hundred
 * entries of impulse_t, not another byte on every one of the grid's cells.
 *
 * NULL disables the mechanic entirely - sand_impulse() becomes a no-op,
 * and so does anything built on it, sand_explode() included. */
void sand_enable_impulses(sand_t *s, impulse_t *buf, int max);

/* Out-of-bounds reads return STONE, not empty.
 *
 * That is deliberate: it makes the walls solid for free, so the movement code
 * never needs a bounds check before asking what is in the next cell. Stone
 * specifically, rather than any old occupied value, because the walls must also
 * be too dense to displace - otherwise a heavy material would sink through the
 * floor. */
cell_t sand_at(const sand_t *s, int x, int y);

/* Ignores out-of-bounds writes. */
void sand_set(sand_t *s, int x, int y, cell_t cell);

int sand_count(const sand_t *s);

/* Fill a disc with `material`, at random variants. Returns how many cells it
 * filled, which is less than the disc's area when it overlaps the edge or
 * anything already there. */
int sand_spawn(sand_t *s, int cx, int cy, int radius, material_id_t material);

/* The same, but taking a whole CELL as the thing to paint.
 *
 * An extended material has no variant to choose - its low nibble is its
 * identity (MATX() in material.h) - so it cannot be named by a
 * material_id_t at all, and this is how it gets onto the board. For an
 * ordinary material pass CELL_MAKE(id, 0); the variant is ignored and
 * chosen the usual way, so sand_spawn() is just this with the id wrapped. */
int sand_spawn_cell(sand_t *s, int cx, int cy, int radius, cell_t spec);

/* Remove every grain in a disc, and every emitter centred in it too - see
 * sand_add_emitter() below. Returns how many CELLS it removed.
 *
 * No longer the exact mirror of sand_spawn that it once was: sand_spawn only
 * ever places grains, and this also switches off any emitter in the same
 * disc, because a running tap that sand_erase() could not turn off would
 * make the whole feature a trap - there would be no way to stop it short of
 * rebuilding the scene.
 *
 * The returned count still means exactly what it always has, though: cells
 * this call actually changed. An emitter removed is not folded into it - see
 * sand_remove_emitters() below if that count is what a caller wants - so
 * neither sand_spawn nor sand_erase can quietly drift the grain total. */
int sand_erase(sand_t *s, int cx, int cy, int radius);

/* EMITTERS
 *
 * A point on the grid that keeps producing one material on its own, so the
 * user can place a water tap or a gas vent and leave it running instead of
 * holding a finger down. See the `emitters` field of sand_t above for why
 * this is a side list rather than a material, and emit_from_emitters() in
 * sand.c for how it is stepped. */

/* Place (or replace) an emitter. Returns false if the list is already at
 * SAND_MAX_EMITTERS or (x, y) is off the grid - either way nothing changes.
 *
 * An emitter already at (x, y) has its cell replaced rather than a second
 * one being added at the same point, so calling this again at a point the
 * caller is not sure is already a tap is always safe: it either creates one
 * or retunes the one already there, never both. `cell` is written exactly
 * as given, the same as sand_spawn_cell()'s `spec` - so an extended
 * material can be an emitter's cell too, not just an ordinary one built
 * with CELL_MAKE(). */
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

/* A single-cell push that skips the density-scaled toughness roll every
 * other wall-dislodging caller keeps - for a caller that already knows
 * its target is KIND_STATIC and wants it dislodged GUARANTEED, not
 * merely with a chance. See queue_flying_grain()'s own comment in sand.c
 * for the roll this bypasses, and why.
 *
 * Originally reaction_t.vent_chance's own single-cell push (try_vent(),
 * sand_reactions.c - removed by bd esp32c6-0f2 once the covered-lava
 * burst, bd esp32c6-mqt, replaced venting outright). Kept as a general
 * primitive rather than removed alongside it - a deterministic dislodge
 * is a real, reusable capability, not something specific to the
 * mechanism that first needed it - and now exercised directly by
 * test_a_dislodged_wall_keeps_falling_even_if_its_first_push_roll_fails
 * (suite_sand.c), which uses it to place a KIND_STATIC cell airborne on
 * demand without needing a real explosion or reaction to get one there,
 * so it can pin down step_impulses()'s own gravity-drift behaviour in
 * isolation.
 *
 * `ramp` is the entry's own per-turn speed decay (impulse_t's own field,
 * this file) - pass SAND_IMPULSE_SPEED_RAMP for the same decay every
 * other caller gets, or a different figure for a throw that should
 * travel farther (or less far) than that shared, already-measured
 * constant without retuning it for every other caller of this same
 * mechanism. */
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
 * had been missing until suite_sand.c's dune scene arrived: grains landing
 * outside a settled dune's own footprint, averaged over independent
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

/* How far a single cool_off_chain() (sand_reactions.c) walk reaches
 * before it stops on its own, independent of how many rolls in a row it
 * wins. The same reasoning CRACK_MAX (sand_reactions.c) gives, at a much
 * smaller scale: this only ever has to bound ONE cold pass, not claim
 * anything about how far a chain could really go, and a sustained pour
 * re-triggers this every step anyway - the pour rate is what should set
 * how fast a pool actually dies, not this cap. A generous number here
 * would let a single lucky roll eat a whole pool in one step, which is
 * exactly the "dry bowl paving itself" failure mode the design has to
 * avoid.
 *
 * PUBLIC, unlike CRACK_MAX - docs/Sand/Reaction-Table.md already names
 * this constant as part of the described contract, and test_the_cool_
 * off_chain_is_bounded (suite_sand.c) has to assert against its real
 * value rather than a hand-copied literal that silently goes stale the
 * next time this is retuned.
 *
 * Briefly doubled to 16 and put back: reaching FURTHER per event turned
 * out to be the wrong dial for "the pour bites harder". How OFTEN a
 * chain gets going at all is SAND_LAVA_COOLOFF_CHANCE's job, and that
 * is what moved instead. */
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
 * cell (suite_sand.c), a fixed-seed scene that cascaded reliably before,
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
#define SAND_CASCADE_MAX_PER_STEP  64

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
 * unbounded growth, no lean at all: 128 is an exact 50/50 coin flip.
 * Whichever side wins a given bite is now decided entirely by
 * SAND_ACID_DILUTE_MASS_BIAS below (local backing) rather than a fixed
 * preference baked into the base rate. Starting bias, not a measured
 * one - tune on device like every other constant here. */
#define SAND_ACID_DILUTE_TO_WATER_CHANCE 128

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
 * onto an acid puddle forever and still not reliably win - see
 * test_a_relentless_pour_of_water_overwhelms_a_pool_of_acid
 * (suite_sand.c), which is what caught it. With the difference: two
 * equally deep pools facing each other net to zero bias and the base
 * split above holds exactly as it always did; a lone grain of either
 * material still reads as 0 on its own side, so an isolated drop in a
 * big lake gets pushed even further toward diluting (the lake's own
 * water_backing pulls the split up), and it is only once one side's
 * local mass genuinely outweighs the other's that the roll tips.
 *
 * Purely local either way - no global concentration tracking, up to 8
 * bounds-checked reads total (4 for each cell's own neighbours), the
 * same kind of local rule the rest of this automaton already relies on
 * to produce a macro-scale effect. Starting bias, not a measured one -
 * tune on device like every other constant here. */
#define SAND_ACID_DILUTE_MASS_BIAS 24

/* EVAPORATION ON DILUTION - a flat slice of the SAME roll that decides
 * water-wins-vs-acid-wins (see the single-roll ladder in
 * step_one_dissolver_cell(), sand_reactions.c - deliberately one roll,
 * not the two independent ones an earlier version of this used, to keep
 * the interaction from growing another moving part) gives every acid/
 * water bite a small, unconditional chance to boil the ACID cell off
 * into MAT_GAS before the win/lose split even runs, regardless of who
 * would otherwise have won. Distinct from r->evaporates (material.c),
 * which is acid's own ambient boil-off and fires whether or not water is
 * anywhere nearby - this one only fires at the moment of an actual
 * acid/water bite. Distinct too from the mass sink the win/lose split
 * itself now carries on both its own outcomes (see that split's own
 * comment) - this bucket existed before that fix landed and is kept
 * as its own separate, smaller chance rather than folded in, since it is
 * the one outcome that skips the water cell entirely. Chance-in-256 out
 * of the full roll, checked first in the ladder, ahead of
 * SAND_ACID_DILUTE_MASS_BIAS's adjustment - so evaporating is exactly as
 * likely for a lone drop as for a poured-on slab. Starting bias, not a
 * measured one - tune on device like every other constant here. */
#define SAND_ACID_DILUTE_EVAPORATE_CHANCE 20

/* OIL BOILS OFF, IT DOES NOT BREED MORE ACID - reported directly: a
 * whole pool of oil was ending up entirely acid. The old rule always
 * converted a bitten oil cell into a FRESH, full-mass acid cell
 * (place_reacted() gives every new ordinary material a full life - see
 * its own comment, sand_reactions.c) while the acid doing the eating
 * only ever paid pay_quench_cost()'s ordinary one-unit nick, the same
 * price eating sand or wood costs. One unit spent to mint an entire new
 * full acid cell, on nearly every bite that landed - net acid could only
 * grow, never actually shrink, and a burning pool of oil was in effect a
 * (slow) acid factory. Two independent fixes in step_one_dissolver_cell()
 * (sand_reactions.c):
 *
 * - the bitten oil cell mostly turns to MAT_GAS now, not acid -
 *   SAND_ACID_OIL_TO_GAS_CHANCE, chance-in-256 that it does. Acid still
 *   spreading into the oil is the minority outcome that survives, not
 *   the only one, the same "mostly, not always" shape water's own
 *   dilution keeps for its own minority branch.
 * - the acid cell separately rolls SAND_ACID_OIL_DEATH_CHANCE for a MUCH
 *   higher chance to die OUTRIGHT - spend its whole remaining mass in
 *   this one bite, cell cleared the same way pay_quench_cost() already
 *   clears a cell whose last unit just went - instead of the ordinary
 *   one-unit chip every other dissolve pays. Explicitly asked for:
 *   eating oil is supposed to be something that can consume the acid
 *   doing it, not a near-free way to breed more of it. */
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

/* QUENCHING A FLAME - acid putting out fire is not water's clean,
 * deterministic flash to steam (see step_one_burning_cell(),
 * sand_reactions.c): unconditionally leaving a cell of gas or smoke
 * behind every single time read as too busy on a board where acid keeps
 * meeting fire, the same over-frequent complaint that shaped `evaporates`
 * and `fizz` down to their own current, much rarer figures. Two
 * independent rolls, not one - "does anything visible happen at all"
 * first, and only then "which of the two it is":
 *
 * SAND_ACID_QUENCH_RESIDUE_CHANCE - chance in 256 that quenching a flame
 * with acid leaves ANY residue behind, instead of the flame simply going
 * out with nothing left to see (the same silent-clear fallback fire's own
 * `residue` field already uses when ITS roll misses). A miss here is not
 * a bug - it is acid putting a fire out cleanly, which is allowed to be
 * the common case.
 *
 * SAND_ACID_QUENCH_SMOKE_CHANCE - chance in 256 that, given residue DOES
 * happen, it is smoke rather than gas - biased toward smoke rather than
 * fizz's even coin flip (step_one_dissolver_cell()'s residue pick),
 * because this puff comes from a flame going out, and smoke is what a
 * dying flame is already understood to leave (reaction_t.residue, just
 * above) - gas is still possible, just the less likely of the two here.
 *
 * Both starting points, not measured - tune on device like every other
 * constant here. */
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

/* How many of fire's sixteen shades a blast core sheds between its centre
 * and its rim - see the fill loop in sand_explode() (sand.c) for why a
 * core written at one uniform life had a colour ramp it could never
 * show.
 *
 * 8, so the rim starts at half the centre's life: a clear gradient that
 * still leaves the outer ring alight long enough to be seen and to set
 * fire to what it touches. Raising this past MATERIAL_VARIANTS - 2 buys
 * nothing - the floor of 1 clamps it - and would only make the fringe
 * die sooner. 0 restores the old flat disc exactly. */
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

/* FRICTION
 *
 * A grain is held in place by the weight of whatever is stacked on top of it.
 * Without modelling that, the bottom of a pile slides as freely as the top -
 * a whole floor of sand skating sideways on the faintest tilt, because nothing
 * in the rules knows it is buried.
 *
 * Real granular friction is subtle (the load on a deep grain does not grow
 * without limit - the Janssen effect - and the angle of repose depends on grain
 * shape). None of that is needed here. What is needed is that burial resists
 * sliding, and that shaking overcomes it.
 *
 * So: a grain counts how many grains are stacked directly against gravity above
 * it, and that count halves its chance of making a SLIDE move. Falling is never
 * affected - if the cell gravity-ward is empty, the grain falls whatever is on
 * top of it, which is what "unsupported" means. */

/* Chance in 256 that a grain with exactly one grain above it may still slide.
 * Halves for each additional grain, so a pile locks up quickly with depth. */
#define SAND_SLIP_CHANCE 96

/* Beyond this much load a grain cannot slide at all. Without a hard floor the
 * chance only ever approaches zero, and at 60 steps a second "almost never"
 * still visibly creeps. */
#define SAND_LOAD_CAP 5

/* How far along its own surface a liquid will look for somewhere shallower to
 * send mass.
 *
 * Everything else about a liquid is strictly local, and this deliberately is
 * not. It has to be: a local rule moves information one cell per step, so
 * levelling a pool 184 cells wide by neighbour-to-neighbour diffusion alone
 * takes tens of thousands of steps. Measured, a real-width pool was still six
 * cells proud after five thousand. That is not a flaw in the rule, it is the
 * bound on any rule of that shape.
 *
 * Real water levels quickly because pressure travels through it far faster
 * than water does. This is the cheap stand-in for that, and it is why every
 * falling-sand game has some version of it.
 *
 * Flow stops at anything that is not the same liquid, so it cannot reach
 * through a wall, and a settled pool finds nothing to do and goes to sleep -
 * which is what keeps the search off the bill.
 *
 * KEEP IT SHORT. Mass handed to a cell eight away skips everything in
 * between, so water disappears from one place and reappears in another - and
 * since the direction alternates every step, it slops straight back the next.
 * At thirty-two that read as great waves surging across the screen and pours
 * flinging themselves sideways before collapsing into specks. Measured on a
 * real-width pool, the levelling barely suffers for the reduction:
 *
 *      sight 4  -> 1.6 cells out of level
 *      sight 8  -> 0.8
 *      sight 16 -> 0.5
 *      sight 32 -> 0.2, and looks wrong while it gets there
 */
#define SAND_LIQUID_SIGHT 8

/* sand_step_gas()'s own spread pass (sand_gas.c) used to have a single
 * global sight constant here (SAND_GAS_SIGHT), the same idea as
 * SAND_LIQUID_SIGHT above - how far along the perpendicular a grain
 * will look for an empty cell to hop into. Now per-material
 * (material.h's `sight` field), since fire shares KIND_GAS with gas
 * but needs to disperse tighter, not the same amount - see gas's and
 * fire's own rows in material.c for the tuned figures and their
 * reasoning. */

/* THE WALL-REBOUND SPLASH, REMOVED 2026-08-30 - a bulk-momentum-driven
 * mass kick off a wall on a hard flick (sand_set_flick(), sand_momentum(),
 * SAND_MOMENTUM_DECAY/SAND_REBOUND_THRESHOLD/SAND_REBOUND_GAIN/SAND_
 * REBOUND_MAX, rebound_wall()/rebound_one_cell() in sand_liquid.c). Set
 * SAND_REBOUND_GAIN to 0 first as a reversible on-device test; confirmed
 * imperceptible at this display size and cell resolution, so removed
 * outright rather than left as a permanently-disabled mechanism still
 * paying for momentum tracking every step. Search git history for
 * "SAND_REBOUND_GAIN" if this is ever worth revisiting - a real per-grain
 * splash on a genuine wall hit exists instead, see splash_displace()'s
 * own comment in sand_liquid.c. */

/* How many cells are stacked directly against gravity above the one at (x, y),
 * capped at SAND_LOAD_CAP. (dx, dy) is a unit gravity direction.
 *
 * Exposed because it is the whole of the friction model and is worth pinning
 * down on its own - in particular that off the grid counts as open sky rather
 * than as load, which sand_at's solid-walls convention would get backwards. */
int sand_load_above(const sand_t *s, int x, int y, int dx, int dy);

/* How often a grain falling through open air does something other than fall
 * straight down, as a chance in 256. Zero, the default, makes falling exactly
 * deterministic.
 *
 * Without it, a falling stream is a rigid block: every grain in open air takes
 * the same move on the same step, so a poured blob keeps its shape all the way
 * down and lands as a blob. Real sand disperses, because no two grains fall at
 * quite the same rate or in quite the same line.
 *
 * A scattered grain either lags a step - which spreads the stream vertically -
 * or drifts to one side, which spreads it horizontally. Neither invents a move
 * that was not already legal, so nothing here can push a grain through a wall
 * or into another grain.
 *
 * Off by default because most tests want to say "a grain falls one cell per
 * step" and mean it. The randomness is an aesthetic choice, so the caller
 * makes it.
 *
 * Pass SAND_SCATTER_PER_MATERIAL to use each material's own figure from the
 * table instead of one value for everything - which is what the app wants,
 * since water and sand do not disperse alike. Any other value overrides all of
 * them, which is what a test wants. */
void sand_set_scatter(sand_t *s, int chance);
#define SAND_SCATTER_PER_MATERIAL (-1)

/* How often a transient material's life ticks down by one, as a chance in
 * 256 - see material.h's `decay` field. Zero, the default, makes every
 * material immortal regardless of what the table says, for the same reason
 * scatter defaults off: most tests want to place gas and reason about it
 * without a background chance of it quietly vanishing out from under them
 * (test_gas_grain_count_is_conserved is exactly that kind of test).
 *
 * Pass SAND_DECAY_PER_MATERIAL to use each material's own figure from the
 * table instead - what the app wants, since only a transient material
 * should ever fade. Any other value overrides all of them, which is what a
 * test that specifically wants to watch something decay wants instead. */
void sand_set_decay(sand_t *s, int chance);
#define SAND_DECAY_PER_MATERIAL (-1)

/* How often a cell spontaneously turns into MAT_GAS, as a chance in 256 -
 * see material.h's `evaporates` field. Zero, the default, turns it off
 * entirely regardless of what the table says, for the same reason decay
 * defaults off: nothing here needs a neighbour or a trigger to fire, so
 * without this override every test with a standing pool of acid would
 * quietly lose cells to it whether or not the test had any opinion about
 * evaporation - exactly what happened to the acid mass-accounting, fizz
 * and metal-eating-budget tests the day this field was added, before this
 * override existed to shield them from it.
 *
 * Pass SAND_EVAPORATES_PER_MATERIAL to use each material's own figure
 * instead - what the app wants, since only acid sets it. Any other value
 * overrides all of them, which is what a test that specifically wants to
 * watch a cell evaporate wants instead. */
void sand_set_evaporates(sand_t *s, int chance);
#define SAND_EVAPORATES_PER_MATERIAL (-1)

/* How readily anything SOAKS UP a liquid it is touching - sand turning to
 * dirt, dirt taking on moisture.
 *
 * Off by default, like decay, and for the same reason it turned out to
 * need to be: sand soaks, and half the tests in the suite put sand in
 * water to check that sand SINKS. Those are about density and have no
 * opinion about chemistry, and they all broke the moment soaking became a
 * property of sand rather than of the scene. A mechanic that arrives
 * switched on rewrites every scene that already existed.
 *
 * Pass SAND_SOAK_PER_MATERIAL for each material's own figure. */
void sand_set_soak(sand_t *s, int chance);
#define SAND_SOAK_PER_MATERIAL (-1)

/* How often a flammable neighbour actually catches, per adjacent burning
 * cell per step, as a chance in 256 - see material.h's reaction_t.
 * flammability field. Defaults to SAND_FLAMMABILITY_PER_MATERIAL (each
 * material's own table figure), unlike scatter/decay which default OFF -
 * ignition already only ever happens next to an actual burning cell, so
 * there is no "background chance of it quietly happening" to guard
 * against the way there is for decay ticking on its own or a falling
 * grain scattering on its own. Without an override, a test that wants
 * to watch something NOT catch (wood beside a single flame, one step)
 * uses the real slow-catching figure; a test that wants to watch it
 * catch forces this to 255 instead, rather than looping hundreds of
 * steps and hoping. */
void sand_set_flammability(sand_t *s, int chance);
#define SAND_FLAMMABILITY_PER_MATERIAL (-1)

/* How often heat crosses one cell of a conductor, per adjacent burning
 * cell per step, as a chance in 256 - see material.h's reaction_t.
 * conducts field and conduct_heat()'s own comment in sand_reactions.c
 * for the walk this actually drives. Defaults to
 * SAND_CONDUCTION_PER_MATERIAL, same reasoning as
 * sand_set_flammability()'s own default: conduction only ever happens
 * next to an actual burning cell, so there is no background chance to
 * guard tests against. Forcing this to 255 is what turns "wait several
 * dozen steps and hope a thick, hand-drawn stone slab eventually boils
 * something" into a one-step, deterministic assertion. */
void sand_set_conduction(sand_t *s, int chance);
#define SAND_CONDUCTION_PER_MATERIAL (-1)

/* How often conducted heat that has already reached a liquid (see
 * conduct_heat(), sand_reactions.c) actually boils it into steam that
 * same step, as a chance in 256 - see material.h's reaction_t.boils
 * field. A second roll on top of the `conducts` roll that got the heat
 * there in the first place, not a replacement for it. Defaults to
 * SAND_BOILS_PER_MATERIAL (each material's own table figure). Without an
 * override, a test that wants to watch water resist boiling for a while
 * uses the real, deliberately low figure it is tuned to; a test that
 * wants a deterministic one-step boil forces this to 255 instead, the
 * same reasoning sand_set_conduction()'s own override gives. */
void sand_set_boils(sand_t *s, int chance);
#define SAND_BOILS_PER_MATERIAL (-1)

/* How often a 2x2 square of one material collapses into a single cell of
 * another, per step, as a chance in 256 - see material.h's reaction_t.
 * condenses field and step_one_condensing_cell() (sand_reactions.c).
 * Defaults to SAND_CONDENSES_PER_MATERIAL (each material's own table
 * figure - steam's own deliberately rare one, today). A test that wants
 * a deterministic one-step condensation forces this to 255 instead of
 * looping a number of steps scaled to however rare the real figure ends
 * up being tuned. */
void sand_set_condenses(sand_t *s, int chance);
#define SAND_CONDENSES_PER_MATERIAL (-1)

/* How much faster a heat-ramping cell's own `cools` drain runs while a
 * QUENCHING liquid (PAIR_QUENCHES, sand_reactions.c's own top comment -
 * water and acid, never lava or oil) is touching it - see
 * step_one_tempered_cell()'s own comment for the neighbour test this
 * multiplies and why it only ever applies in the ABOVE-ambient branch,
 * which is what stops water from chilling anything past room temperature
 * the way snow does. Not a new per-material field: `cools` already means
 * "how fast this material sheds heat", and a wet surface is the same
 * drain, just faster - there is no reason a stone pane's own cooling rate
 * and its cooling rate WHILE WET would ever want to be two independently
 * tuned numbers. One figure, because water is the only coolant anyone
 * actually pours - acid also satisfies PAIR_QUENCHES, and gets the same
 * speed-up as a side effect, but nobody has asked for acid to cool a wall
 * differently from water, and inventing a second multiplier before that
 * is a real request would be tuning a knob nobody turns. */
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

/* The sentinel sand_set_lava_cooloff(s, chance < 0) restores, and what
 * sand_init() itself starts every sand_t at - "use SAND_LAVA_COOLOFF_
 * CHANCE", not "use each material's own row", since (unlike
 * flammability/conduction/boils/condenses) this has no per-material
 * figure to fall back to; SAND_LAVA_COOLOFF_CHANCE IS the only figure.
 * Named _DEFAULT rather than _PER_MATERIAL for exactly that reason - the
 * other five sentinels above answer "whose rate", this one only ever
 * answers "which constant". */
#define SAND_LAVA_COOLOFF_DEFAULT (-1)

/* Chance in 256 a sufficiently-covered lava cell converts to MAT_STONE
 * and bursts, PER COVERED CELL, PER STEP - not per pool, not per event.
 * This is the exact multiplier mistake the vent mechanism this feature
 * replaced (reaction_t.vent_chance, removed by bd esp32c6-0f2) made
 * TWICE: a figure that reads as "extremely low" in isolation is anything
 * but, once a large sealed pool has dozens of covered cells each
 * independently rolling this every single step they stay covered.
 * vent_chance's own combined rate briefly hit 255 with a maxed-out
 * second roll on top of it, and a covered cell got thrown away the very
 * next step, before any more crust could build on top of it - "material
 * pops the instant water touches lava", not "a sealed slab breaks free".
 * 1 is deliberately the rarest a single byte-wide roll (rng_next(&s->rng)
 * & 0xFF, this file's usual shape) can express - start at the floor, not
 * at whatever "feels right" in isolation, and tune upward on device with
 * the aggregate in mind, not the single-cell figure. If device play says
 * this is still too frequent even at 1, the documented next move is a
 * SECOND, independent gate in step_one_dissolver_cell()'s own
 * 1-in-60-on-`.evaporates` style (this file), not a new field alongside
 * this one - vent_chance already tried "add a knob" once, in the form of
 * its own second roll, and that is not a pattern to repeat casually. */
#define SAND_LAVA_BURST_CHANCE   1

/* A SECOND, INDEPENDENT GATE stacked on the chance above, and the only
 * way to express a rate below it: 1 in 256 is already the rarest a single
 * byte-wide roll can say, so getting rarer needs another roll rather than
 * a smaller number. The same route step_one_dissolver_cell() takes for
 * `evaporates` (sand_reactions.c), for the same reason.
 *
 * 4, so the effective rate is 1 in 1024 per covered cell per step - a 75%
 * cut, asked for on device as 'at least 50%' after the first flash of the
 * burst read as firing too often. Remember what makes a per-cell figure
 * misleading: a sealed pool has MANY covered cells all rolling every step,
 * so the rate a POOL bursts at is this multiplied by however many cells
 * are covered, which is why the honest number here looks absurdly small.
 * Raise this constant to make bursts rarer still; it is the dial to turn
 * before touching SAND_LAVA_BURST_CHANCE, which has no room left to fall.
 *
 * APPLIED ONLY TO THE NATURAL RATE. sand_set_lava_burst() bypasses it
 * entirely, so a test that pins the chance gets exactly what it asked
 * for. */
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
#define SAND_LAVA_BURST_RADIUS   16

/* Overrides SAND_LAVA_BURST_CHANCE for every lava cell alike - the same
 * shape as sand_set_lava_cooloff() just above, for the same reason: a
 * test that wants a burst to fire (or never fire) deterministically
 * cannot wait out a 1-in-256 roll and stay fast. Clamped to [0, 255]
 * exactly like every other chance-in-256 setter in this file. */
void sand_set_lava_burst(sand_t *s, int chance);

/* The sentinel sand_set_lava_burst(s, chance < 0) restores, and what
 * sand_init() itself starts every sand_t at - "use
 * SAND_LAVA_BURST_CHANCE", named _DEFAULT rather than _PER_MATERIAL for
 * the same reason SAND_LAVA_COOLOFF_DEFAULT is: this has no per-material
 * table figure to fall back to, only the one constant. */
#define SAND_LAVA_BURST_DEFAULT (-1)

/* How often a gas grain attempts its spontaneous rise/slide at all, as a
 * chance in 256 - see material.h's `mobility` field. 255, the default,
 * makes gas rise exactly one cell per step it can, the same deterministic
 * guarantee sand's own fall makes - most tests that place a gas grain and
 * step once want to reason about exactly where it lands, the same reason
 * scatter defaults off (test_gas_rises_straight_up_under_ordinary_gravity
 * is exactly that kind of test, which is why this is NOT off by default
 * the way scatter and decay are - "off" for a rise-gate means "never
 * rises", the opposite of a safe default here).
 *
 * Pass SAND_MOBILITY_PER_MATERIAL to use each material's own figure from
 * the table instead - what the app wants, for gas's actual lazy drift.
 * Any other value overrides all of them, which is what a test that
 * specifically wants to watch that drift wants instead. */
void sand_set_mobility(sand_t *s, int chance);
#define SAND_MOBILITY_PER_MATERIAL (-1)

/* Advance one frame.
 *
 * (gx, gy) is a gravity vector in any units - only its direction matters. A
 * zero vector means free fall, where nothing settles and so nothing moves.
 *
 * `jostle` is 0-255, how hard the device is being shaken. It does two things,
 * both of which real shaking does: it makes grains prefer sliding sideways over
 * falling straight down, and it overrides friction, fluidising a pile that
 * would otherwise be locked solid. */
void sand_step(sand_t *s, int gx, int gy, int jostle);

/* The eight-way quantisation: the NEAREST of the eight directions.
 * Writes the unit direction to (*dx, *dy), or (0, 0) for a zero vector.
 *
 * Deterministic, and not what sand_step uses - see below. */
void sand_gravity_direction(int gx, int gy, int *dx, int *dy);

/* What sand_step actually uses: the two directions bracketing the true angle,
 * chosen between at random, weighted so the LONG-RUN AVERAGE is the true angle.
 *
 * Grains can only move to one of eight neighbours - that is what a grid is -
 * so a single step can never express "17 degrees off vertical". Snapping to
 * the nearest of eight instead makes a slow tilt arrive in 45-degree jerks,
 * which is most of what makes tilt-controlled sand feel rigid.
 *
 * Dithering moves the quantisation into TIME, where there is room for it. At
 * 17 degrees the pile spends about 62% of its frames falling straight down and
 * 38% falling down-right, and at 70 fps the eye integrates that into a smooth
 * 17-degree flow. The same trick as dithering a colour ramp, applied to a
 * direction.
 *
 * Exactly-aligned input is never dithered, so gravity of (0, 1) is always
 * straight down and tests stay deterministic. */
void sand_gravity_direction_dithered(sand_t *s, int gx, int gy,
                                     int *dx, int *dy);
