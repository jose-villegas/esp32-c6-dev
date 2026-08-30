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

/* AXIS_HYSTERESIS_PCT and DEPTH_DIAGONAL_DEADZONE_PCT are OWNED by
 * app_sand.c's LOCAL DEPTH mechanism (update_local_depth_axis() and
 * DEPTH_DIAGONAL_DEADZONE_PCT's own comment there) - declared here, not
 * there, purely so suite_sand.c's host tests can reference the real values
 * instead of a hand-copied duplicate that could silently drift. The same
 * reason MATERIAL_LIQUID_DEPTH_BAND lives in material.h rather than
 * staying private to material.c.
 *
 * The two are independent - AXIS_HYSTERESIS_PCT prevents its own chatter
 * regardless of the dead zone's width, they do not need to relate. */
#define AXIS_HYSTERESIS_PCT 15

/* +/-1.2 degrees, ~2.3 degrees total - see DEPTH_DIAGONAL_DEADZONE_PCT's
 * own comment in app_sand.c. */
#define DEPTH_DIAGONAL_DEADZONE_PCT 4

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
 * to six - the pad moves, it does not shrink. Six bytes is this struct's
 * true floor for what it does, not merely the number nobody has
 * revisited. */
typedef struct {
    uint16_t index;
    cell_t   cell;
    uint8_t  dir;
    uint8_t  speed;
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

    /* See sand_set_soak(). 0, the default, means nothing soaks. */
    int      soak;

    /* Bulk momentum: how hard gravity's DIRECTION is currently swinging, not
     * where it currently points. See the comment above SAND_REBOUND_GAIN. Q8
     * fixed point; (dir_x_q8, dir_y_q8) is the previous step's normalised
     * gravity direction, kept only to measure the turn against. */
    int32_t  mom_x_q8, mom_y_q8;
    int32_t  dir_x_q8, dir_y_q8;
    bool     mom_primed;
    int      flick;      /* 0-255, see sand_set_flick() */

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
     * SAND_SPLASH_RADIUS's own comment above for why this lives
     * per-instance. */
    uint8_t    splash_chance;

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
    int      mobility;     /* see sand_set_mobility() */
    int      flammability; /* see sand_set_flammability() */
    int      conduction;   /* see sand_set_conduction() */

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
 * explode()'s own seeding loop reaches a static cell through a separate,
 * internal path (queue_flying_grain() in sand.c) with a density-scaled
 * chance to override that refusal - a blast should read as tougher
 * against stone than sand, not indestructible - but that override is
 * explicit, opt-in, and local to the one caller that asked for it; it is
 * not a hidden exception buried in this shared primitive that some future
 * caller (gunpowder, gas, whatever comes next) could trip over by
 * accident. Calling sand_impulse() itself always gets the safe default. */
void sand_impulse(sand_t *s, int x, int y, int dir, int speed);

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

/* Radius and decaying trigger CHANCE for splash_displace() (sand_liquid.c)
 * - a WATER or ACID grain landing hard, either falling onto an already-
 * occupied surface or rebounding off a wall, throws a small, MASKED
 * sand_displace_material() at RADIUS (only the same material gets thrown -
 * see splash_displace()'s own comment for why), exaggerated well past a
 * real splash's reach so the effect reads clearly at this display size.
 * Oil and lava are not wired into this - oil has no gameplay reason to
 * scatter, and lava is a heat source whose spread timing this same
 * exaggerated radius visibly disrupted when tried (see this constant's own
 * commit history if that is ever revisited).
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
 * simulation's splash history never bleeds into another's. */
#define SAND_SPLASH_RADIUS       5
#define SAND_SPLASH_CHANCE_START 255
#define SAND_SPLASH_CHANCE_FLOOR 24
#define SAND_SPLASH_CHANCE_STEP  90

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

/* THE WALL-REBOUND SPLASH
 *
 * Everything above reacts to where gravity POINTS. Nothing reacts to how fast
 * it is CHANGING - so flicking the board hard and a slow tilt to the same
 * angle look identical once settled, and a wave that has just piled against a
 * wall has no reason to do anything but sit there.
 *
 * A real wave hitting a wall bounces some of itself back. The cheap stand-in:
 * track a bulk momentum vector from how much gravity's direction has turned,
 * step over step - not from where it points, which is already fully handled
 * above. When that turn is large and pointed into a wall, cells touching that
 * wall kick a little mass back into the grid; the kick fades as the turn
 * fades, over a few steps.
 *
 * Deliberately not a per-cell velocity: that would be another byte per cell,
 * 41 KB against the ~90 KB actually free once the grid and its row state are
 * allocated. One shared vector costs nothing measurable and produces the
 * same visible effect, since the whole board is being shaken together, not
 * grain by grain.
 *
 * THE DIRECTION AND THE SPEED COME FROM DIFFERENT PLACES, ON PURPOSE
 *
 * (gx, gy) is already smoothed before it ever reaches here - it has to be, or
 * every grain would jitter with the sensor's noise. That smoothing is exactly
 * what makes it the wrong thing to measure SPEED from: an exponential filter
 * can only close a fraction of the gap to a new reading each step, so its own
 * frame-to-frame delta is capped by the filter's time constant rather than by
 * how fast the device actually moved. Turning that up just makes ordinary
 * tilting cross the threshold too.
 *
 * So direction still comes from (gx, gy) - which way it turned is a question
 * the smoothed signal answers just fine, a step or two late. HOW FAR to push
 * comes from sand_set_flick() instead: the caller's own gyroscope reading,
 * which was never run through that filter and exists for exactly this. See
 * sand_set_flick().
 */

/* How much of a step's turn survives to the next one, out of 256. Higher
 * lingers longer. */
#define SAND_MOMENTUM_DECAY     220

/* Below this much accumulated turn (Q8 units - 256 is a full quarter-turn in
 * one step) no wall reacts at all. Ordinary tilting, even briskly, stays under
 * this; it takes an actual flick to cross it. */
#define SAND_REBOUND_THRESHOLD  160

/* Mass kicked off a wall per cell per step, per unit of turn past the
 * threshold. Zero disables the whole effect with no other change needed -
 * the momentum is still tracked, but nothing ever reads it.
 *
 * Calibrated against a real capture, not guessed: a genuine flick on this
 * board measured 162-737 past the threshold, but MOST of that range sits at
 * the low end (162-350) - so the previous value of 3 gave a typical flick a
 * kick of only 1-2 out of 15, barely visible. 8 puts a typical flick's kick
 * in the 4-6 range instead; only the hardest flicks in the sample now reach
 * SAND_REBOUND_MAX on their own. */
#define SAND_REBOUND_GAIN         8

/* However hard the flick, no more than this much mass moves per cell per
 * step - it is a splash, not a teleport. Raised alongside the gain above so
 * the hardest flicks in the same capture (up to 737 past the threshold)
 * still read as visibly stronger than a merely brisk one, rather than both
 * capping out at the same number. */
#define SAND_REBOUND_MAX         10

/* How hard the device is being turned RIGHT NOW, 0-255 - not jostle (linear
 * shake) and not derived from (gx, gy) (smoothed, and rate-limited by that
 * smoothing - see the comment above). Meant to be read straight from a
 * gyroscope once a frame and handed in here unchanged; sand_step() uses it to
 * scale the wall-rebound kick, and nothing else. Not sticky: whatever was set
 * last is what the next sand_step() sees, so a caller with nothing to report
 * should pass 0 rather than assume it decays on its own. Defaults to 0, so a
 * caller that never calls this sees no rebound at all, ever. */
void sand_set_flick(sand_t *s, int flick);

/* The momentum vector as it stands after the last sand_step(), Q8 fixed
 * point. Read-only, and not needed for the simulation itself - it exists so
 * a caller can watch what real handling actually does to it, which is the
 * only honest way to calibrate SAND_REBOUND_THRESHOLD against a device
 * rather than a synthetic test. */
void sand_momentum(const sand_t *s, int32_t *mx_q8, int32_t *my_q8);

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
