/*=============================================================================
 * sand_impulse - the public API for grains, chunks and splashes in flight:
 * explosions, thrown debris, splash pushback. Split out of sand.h for the
 * same reason sand_impulse.c is split out of sand.c - see that file's own
 * banner.
 *
 * Included FROM sand.h, not instead of it - a caller anywhere else in the
 * app keeps doing `#include "sand.h"` and sees this API without change.
 * impulse_t has to be defined before sand_t (sand.h's own struct embeds an
 * `impulse_t *`), so this header forward-declares sand_t itself; every
 * function below only ever takes a sand_t*, never looks inside it, so the
 * incomplete type is enough here. sand.h completes the definition.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#include "material.h"

typedef struct sand_s sand_t;

/* One grain in flight from sand_impulse() - not explosion-specific despite
 * sand_explode() being the one caller today. `cell` is the exact byte
 * thrown, checked before moving so a stale entry drops rather than flying
 * whatever is there now. `speed` is both this turn's chance in 256 of
 * moving and how much flight is left (see SAND_IMPULSE_SPEED_RAMP); `ramp`
 * is a per-entry override of its decay rate, ignored for water/acid. */
typedef struct {
    uint16_t index;   /* y*w+x */
    cell_t   cell;
    uint8_t  dir;     /* ring_dir() index, sand_priv.h */
    uint8_t  speed;
    uint8_t  ramp;
} impulse_t;

/* Caller-owned `buf` holds up to `max` in-flight impulse_t entries (not
 * per-cell - one explosion can queue hundreds). NULL disables the mechanic
 * entirely, making sand_impulse()/sand_explode() no-ops. */
void sand_enable_impulses(sand_t *s, impulse_t *buf, int max);

/* Queue one grain at (x, y) for the flight pass at the tail of sand_step()
 * (step_impulses(), sand_impulse.c - must run last, after anything else
 * that can move or replace a cell). No-op if disabled, off-grid, already
 * empty, the buffer is full, or the cell is KIND_STATIC - a wall is never
 * thrown through this function; sand_explode() and sand_impulse_dislodge()
 * (below) each dislodge a static cell their own separate way instead. */
void sand_impulse(sand_t *s, int x, int y, int dir, int speed);

/* A single-cell push that bypasses the density-scaled toughness roll for
 * guaranteed dislodgement of KIND_STATIC targets. See queue_flying_grain()
 * in sand_impulse.c for details. `ramp` is the entry's speed decay; use
 * SAND_IMPULSE_SPEED_RAMP for standard decay or a custom value for
 * different throw distances. */
void sand_impulse_dislodge(sand_t *s, int x, int y, int dir, int speed,
                           int ramp);

/* How much `speed` (impulse_t) loses every step - a linear ramp, matching
 * this file's other chance-in-256 rates. Because gravity itself never
 * accelerates (a falling grain drops exactly one cell a step), decaying
 * only the horizontal push is what turns a queued throw into a visible
 * ARC rather than a straight line - shallow at first, steepening as
 * `speed` runs out. */
#define SAND_IMPULSE_SPEED_RAMP  2

/* How many cells one successful push-roll moves: 1 + speed / this,
 * uncapped. Before this existed every displacing move covered one cell
 * per roll, the same rate gravity falls at, so a thrown grain could never
 * outrun its own fall. 104 gives a full-speed (255) entry 3 cells and a
 * near-spent one still exactly 1 - see suite_sand_impulse.c's divisor
 * tests. */
#define SAND_IMPULSE_CELLS_PER_STEP_DIVISOR  104

/* EXTRA speed charge per non-empty cell a KIND_STATIC/KIND_POWDER mover
 * displaces, on top of density-based drag (`density << this`, KIND_POWDER
 * only). Density alone gives every solid the same drag shape, which is
 * wrong: packed grain jams a mover, a liquid barely slows one. Ensures
 * full-speed chunks stop near a bank's rim, not tunneling through - see
 * `test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank`. */
#define SAND_IMPULSE_DRAG_POWDER_SHIFT  2

/* Below this post-drag speed a KIND_STATIC entry is SPENT: its
 * unconditional gravity-drift may only enter an empty cell, not swap
 * through an occupant - the drift pays no drag, ever, so without this
 * floor a spent chunk swaps down through a whole bank, never stopping. A
 * spent chunk also rests mid-liquid rather than sinking - a deliberate
 * trade-off. */
#define SAND_IMPULSE_SINK_MIN_SPEED  1

/* RESTITUTION FLOOR for the wall-bounce - below this a blocked entry just
 * waits; above it, it reflects off the blocking surface's normal and pays
 * restitution for the privilege. Not a polish knob: unfloored, undamped,
 * a piece could keep finding just enough energy to bounce forever instead
 * of settling. */
#define SAND_IMPULSE_BOUNCE_MIN_SPEED  32

/* TRANSFER - what a struck cell inherits from the mover displacing it, so
 * struck material can fly clear of a bank. Direction: backward cone
 * (dir+3/4/5) - along the mover's own heading was tried first and just
 * drove struck material deeper in. Constant: ARRIVAL speed in 256ths, not
 * a divisor - below 256 keeps a strike from minting energy; 213 is a
 * device figure. */
#define SAND_IMPULSE_TRANSFER_KEEP  213

/* Below this post-drag speed, a mover does not queue a transfer at all -
 * a nearly-spent mover's transfer would be too faint to ever visibly
 * move. This is the cheap first gate; the real budget protection against
 * a long plow is SAND_CASCADE_TRANSFER_MAX_PER_STEP. */
#define SAND_IMPULSE_TRANSFER_MIN_SPEED  64

/* sand_explode()'s own choice of speed to hand every entry it queues. At
 * 255, the uint8_t ceiling: a future round wanting more reach has to
 * retune SAND_IMPULSE_SPEED_RAMP or the blast radius instead. */
#define SAND_EXPLODE_INITIAL_SPEED  255

/* THE PURE DISPLACEMENT PRIMITIVE, NO MATERIAL CONVERSION - split from
 * sand_explode() so a caller wanting the push without fire has somewhere
 * to call. Queues an outward entry per occupied annulus cell, in RING
 * order so an undersized buffer degrades to a smaller, even disc, not a
 * lopsided crescent. Every cell is seeded, not sampled - sparse seeding
 * measured worse. KIND_STATIC can be dislodged here via a density-scaled
 * chance, unlike sand_impulse()'s refusal. */
void sand_displace(sand_t *s, int cx, int cy, int radius);

/* Same as sand_displace(), but only cells whose material is exactly
 * `mat_id` are ever queued - see its own comment in sand_impulse.c. Used
 * by splash_displace() (sand_liquid.c) so a liquid's splash cannot fling
 * unrelated material (dirt under a pool of water, say) along with it. */
void sand_displace_material(sand_t *s, int cx, int cy, int radius,
                            uint8_t mat_id);

/* How much of the blast radius sand_explode() fills with fire first:
 * filled radius is `radius / SAND_EXPLODE_CORE_DIVISOR`. Needed because a
 * packed medium blocks every queued entry's first move; fire, being
 * lighter than nearly anything it detonates into, lets the medium swap
 * through via the ordinary density rule. Clamped to a minimum core_radius
 * of 1 for radius >= 2 - a zero-radius core cannot seed the density-swap
 * collapse it depends on. */
#define SAND_EXPLODE_CORE_DIVISOR  5

/* How many of fire's sixteen shades a blast core sheds from centre to rim -
 * see fill loop in sand_explode() (sand_impulse.c). 8 ensures clear
 * gradient, keeping outer ring alight. Raising past MATERIAL_VARIANTS - 2
 * is redundant; floor of 1 clamps it. 0 restores flat disc. */
#define SAND_EXPLODE_CORE_FADE     8

/* A THIN WRAPPER AROUND sand_displace(), ABOVE - fills a core of
 * `radius / SAND_EXPLODE_CORE_DIVISOR` with fire, then hands the rest to
 * sand_displace(). A no-op if sand_enable_impulses() was never called -
 * the core is left unfilled too. CONSERVATION IS BOUNDED, NOT EXACT,
 * unlike sand_displace() alone: filling an empty core cell with fire is a
 * real increase, exactly once. */
void sand_explode(sand_t *s, int cx, int cy, int radius);
