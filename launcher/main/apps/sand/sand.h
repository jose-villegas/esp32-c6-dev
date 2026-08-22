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

typedef struct {
    uint8_t *cells;      /* w * h, row-major, caller-owned */
    int      w, h;
    rng_t    rng;        /* seeded explicitly, so every run repeats exactly */
    bool     sweep_flip; /* alternates the sweep direction between steps */
    bool     liquid_flip;/* alternates which way liquids share sideways */

    /* Whether the grid might hold any liquid at all. Conservative: set the
     * moment a liquid is placed, and only ever cleared by a pass that has
     * looked everywhere and found none. When it is false the whole cross-flow
     * pass is skipped, so a screen of sand never pays for water. */
    bool     may_have_liquid;

    /* Optional, caller-owned, h bytes: which rows changed since it was last
     * cleared. NULL disables tracking entirely. See sand_track_dirty_rows(). */
    uint8_t *dirty_rows;

    /* Optional, caller-owned, h bytes: which rows are worth looking at at all.
     * NULL means look at every row, every step. See sand_enable_sleeping(). */
    uint8_t *row_state;
    int      last_load_dx, last_load_dy;

    int      scatter;   /* see sand_set_scatter() */
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

/* Let settled rows be skipped entirely, using a caller-owned array of `h`
 * bytes.
 *
 * Without this, resting sand is the MOST expensive thing the simulation can
 * hold, not the least. A settled grain fails its gravity-ward move, draws a
 * random number, walks its load column and then fails both slides - the whole
 * decision path, every step, to conclude nothing. Measured on a host, a screen
 * full of motionless sand cost twenty times an empty one.
 *
 * A row is worth processing only if it, or a row touching it, saw movement on
 * the previous step: a grain can only move one row, so nothing else can have
 * changed what any grain in a quiet row is resting on. Spawning, erasing and
 * sand_set all count as movement, so sand poured onto a sleeping pile wakes it.
 *
 * Everything wakes when the gravity direction changes or the grid is shaken,
 * since either can free a grain that had nothing to do with its neighbours. */
void sand_enable_sleeping(sand_t *s, uint8_t *rows);

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
