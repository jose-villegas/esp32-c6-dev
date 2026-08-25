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
#include "rng.h"

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

typedef struct {
    uint8_t *cells;      /* w * h, row-major, caller-owned */
    int      w, h;
    rng_t    rng;        /* seeded explicitly, so every run repeats exactly */
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

    int      last_load_dx, last_load_dy;

    int      scatter;      /* see sand_set_scatter() */
    int      decay;        /* see sand_set_decay() */
    int      mobility;     /* see sand_set_mobility() */
    int      flammability; /* see sand_set_flammability() */
    int      conduction;   /* see sand_set_conduction() */
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

/* Remove every grain in a disc. Returns how many it removed.
 *
 * The exact mirror of sand_spawn, down to reporting a count that only includes
 * cells it actually changed - so neither can quietly drift the grain total. */
int sand_erase(sand_t *s, int cx, int cy, int radius);

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
