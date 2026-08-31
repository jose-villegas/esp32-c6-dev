/*=============================================================================
 * sand - a falling-sand cellular automaton.
 *
 * The whole simulation is one rule applied to every grain: try to move the way
 * gravity points; failing that, try the two directions either side of it. Piles
 * with a natural angle of repose, heaps that collapse when undermined and sand
 * that pours through a gap all fall out of those three attempts. Nothing here
 * models them explicitly.
 *
 * The one subtlety is sweep order - see the comment on sand_step().
 *
 * A liquid's DOWN-AND-SLIDE movement is here too, in move_liquid_grain() -
 * called from the same sweep, because it obeys the same gravity-ward
 * guarantee every other move in this file does. Everything else about a
 * liquid - the two things that are NOT gravity-ward - lives in
 * sand_liquid.c. See sand_priv.h for why they need to share a few small
 * helpers, and sand_step_liquids() for where the two meet.
 *===========================================================================*/

#include "sand_priv.h"

#include <string.h>

#include "util/fixed.h"
#include "util/intmath.h"


/* tan(22.5 deg) is the boundary between "straight down" and "diagonal"; its
 * reciprocal, 2.4142, is approximated as 29/12 to keep this in integers.
 * The largest operand is a raw accelerometer reading, so 32767 * 29 stays well
 * inside 32 bits. */
#define AXIS_NUM 29
#define AXIS_DEN 12

/* How long the poured shade lingers on one part of the band before
 * drifting on: 64 steps, about two seconds, so a single brushful is one
 * shade and two separate pours are two. */
#define POUR_BAND_SHIFT 6

/* And how far it JUMPS each time, rather than stepping to the next
 * shade along. Walking the band one shade at a time put consecutive
 * pours two shades apart - about twenty points of luminance, which is
 * a layer you have to look for. Five is coprime with both 12 (sand's
 * dune band) and 16 (snow's), so it still visits every shade before
 * repeating, it just does not visit them in order. */
#define POUR_BAND_STRIDE 5u

/* `band` is where in the shade band this pour is centred, worked out
 * ONCE by the caller. It is the same for every cell of a brushful, and
 * the modulo that produces it is a real division - MATERIAL_SHADE_SPAN is
 * a runtime ternary, so the compiler cannot turn it into a mask. Computed
 * per cell it cost about 11% of the spawn path; hoisted, a brushful pays
 * for one instead of thirty. */
static cell_t random_cell(sand_t *s, material_id_t material, int band)
{
    /* A liquid's variant is an amount, not a shade, so a fresh cell is a full
     * one. Giving it a random level would be pouring random quantities. */
    if (materials[material].kind == KIND_LIQUID) {
        return CELL_MAKE(material, MASS_MAX);
    }
    /* A transient material's variant is life remaining, not a shade either -
     * see material.h's top comment and the `decay` field it documents.
     * Fresh gas starts at full life so it fades from vivid to gone, rather
     * than spawning already partway decayed. */
    if (materials[material].decay != 0) {
        return CELL_MAKE(material, MATERIAL_VARIANTS - 1);
    }
    /* A heat-ramping material's variant is a TEMPERATURE, not a shade
     * either, and a fresh cell of it is at ROOM temperature - not at the
     * bottom of its range, which now means frosted. Random would hand the
     * player a pane that is already half melted, and MATERIAL_VARIANTS - 1
     * would hand them one that melts on the next step. */
    if (reactions[material].heat_ramp != 0) {
        return CELL_MAKE(material, SAND_AMBIENT_HEAT);
    }
    /* A material that burns only while lit has HOW MUCH IS LEFT TO BURN in
     * its variant, and a fresh one is not on fire. A random shade would
     * hand the player a log that is already half burnt - and, at variant
     * 0 being the only unlit value, mostly one that is already alight. */
    if (reactions[material].burn_decay != 0) {
        return CELL_MAKE(material, 0);
    }
    /* A material that dries has MOISTURE in its variant - but only in the
     * low three bits of it. The top bit is a carried tone, and that one
     * IS random, which is the whole point: it is what makes a poured bank
     * of soil keep the pattern it was poured with instead of sliding
     * under a texture pinned to the screen.
     *
     * So this is the one variant that is part random shade and part
     * something else, and the two halves have to be picked separately:
     * random tone, bone-dry moisture. Fresh soil that arrived already
     * watered would hand the player fertile ground for free. */
    if (reactions[material].dries != 0) {
        /* Banded by the moment it was poured, exactly as a shade is
         * below. Soil has only two tones, so a band IS a tone and
         * consecutive pours simply alternate - which is the whole of
         * what two tones can do, and enough to lay down a line. The
         * draw is kept so a bank is not perfectly uniform. */
        /* An eighth of grains take the other tone rather than a quarter.
         * With two tones a "speckle" IS the neighbouring band bleeding in,
         * so the jitter that keeps sand looking like grains is the same
         * thing that washes a soil layer out - and soil has one bit to
         * lose, where sand has twelve shades to spare. */
        const unsigned tone =
            ((unsigned)band + rng_below(&s->rng, 8) / 7u) % SOIL_TONES;
        return CELL_SOIL(material, (uint8_t)tone, 0);
    }
    /* Not the whole range: sand keeps its top four shades for cullet, so
     * a painted dune can never accidentally contain grains that claim to
     * have been a window. */
    /* Centred on where the band has drifted to, rather than spread across
     * the whole of it. A brushful comes out nearly one shade; a brushful
     * poured a few seconds later comes out another; and a pile built from
     * several pours has visible layers in it, with the older surfaces
     * still legible after they are buried.
     *
     * The jitter is what keeps it from being flat. Plus or minus one
     * shade, which is enough that a bank still reads as grains rather
     * than as paint, and narrow enough that the layers survive it.
     *
     * Same single draw it always was. */
    const int span = MATERIAL_SHADE_SPAN(material);
    int shade = band + (int)rng_below(&s->rng, 3) - 1;
    if (shade < 0) {
        shade = 0;
    } else if (shade >= span) {
        shade = span - 1;
    }
    return CELL_MAKE(material, (uint8_t)shade);
}

/*---------------------------------------------------------------------------
 * Grid access
 *-------------------------------------------------------------------------*/

void sand_init(sand_t *s, uint8_t *cells, int w, int h, uint32_t seed)
{
    s->cells      = cells;
    s->w          = w;
    s->h          = h;
    rng_seed(&s->rng, seed);
    s->pour_phase = 0;
    s->sweep_flip = false;
    s->liquid_flip = false;
    s->gas_flip   = false;
    /* The THIRD copy of this list, and the one that made the other two
     * hard to see. Four of the five flags were reset here by hand and
     * may_have_temperature was not, so a sand_t reused across tests
     * carried it in from whatever ran before - which meant the tests
     * written to catch the brush latching bug could not see it, because
     * the flag they were checking was already true on both sides.
     *
     * On a fresh board the same omission reads as the opposite bug: the
     * simulation is a static, so the flag starts false and stays false,
     * and painted snow never wakes the reactions pass at all. */
    clear_content_flags(s);
    s->dirty_rows = NULL;
    s->block_state = NULL;
    s->impulse_buf   = NULL;
    s->impulse_max   = 0;
    s->impulse_count = 0;
    s->splash_chance = SAND_SPLASH_CHANCE_START;
    s->splash_radius_water = SAND_SPLASH_RADIUS_WATER;
    s->heat_flaw_seq        = 0;
    s->heat_flaw_is_flawed  = false;
    /* Computed here, unconditionally, rather than only when sleeping is
     * enabled: the main sweep always walks block-columns (see
     * step_one_row()), whether or not block_state exists, so block_cols/
     * block_rows must be real grid-derived values from the start - never
     * zero, which would make that walk cover nothing. */
    s->block_cols  = (w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    s->block_rows  = (h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;
    s->last_load_dx = 0;
    s->last_load_dy = 0;
    s->last_step_dx = 0;
    s->last_step_dy = 0;
    s->scatter      = 0;
    s->decay        = 0;
    s->evaporates   = 0;    /* see sand_set_evaporates() */
    s->soak         = 0;    /* nothing soaks unless asked - see
                             * sand_set_soak() */
    s->mobility     = 255;  /* full speed by default - see sand_set_mobility() */
    s->flammability = SAND_FLAMMABILITY_PER_MATERIAL;  /* see sand_set_flammability() */
    s->conduction   = SAND_CONDUCTION_PER_MATERIAL;    /* see sand_set_conduction() */
    s->vent_chance  = SAND_VENT_CHANCE_PER_MATERIAL;   /* see sand_set_vent_chance() */
    /* The array itself need not be touched - every reader below goes
     * through emitter_count, so an entry past it is simply never looked
     * at, the same way sand_spawn_cell()'s clipped cells are never looked
     * at rather than being separately zeroed. */
    s->emitter_count = 0;
    sand_clear(s);
}

/* wake_blocks_range()/wake_block_and_neighbors() and mark_rows()/
 * mark_move() are shared with sand_liquid.c and live in sand_priv.h now -
 * see the comment there for why they are `static inline` in a header
 * rather than ordinary functions declared extern.
 * BLOCK_SETTLED_NEAREST/OTHER/ACTIVE (block_state) live there too, next to
 * the code that reads them. */

void sand_enable_sleeping(sand_t *s, uint8_t *blocks)
{
    s->block_state = blocks;
    if (blocks != NULL) {
        /* Nothing is known about the grid yet, so nothing may be assumed
         * settled. */
        memset(blocks, 0, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    s->last_load_dx = 0;
    s->last_load_dy = 0;
}

bool sand_block_settled(const sand_t *s, int bx, int by)
{
    if (s->block_state == NULL) {
        return false;
    }
    return (s->block_state[by * s->block_cols + bx] &
           (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) != 0;
}

void sand_enable_impulses(sand_t *s, impulse_t *buf, int max)
{
    s->impulse_buf   = buf;
    s->impulse_max   = (buf != NULL) ? max : 0;
    /* Nothing can already be in flight against a buffer that was just handed
     * over - the same reasoning sand_enable_sleeping() gives for zeroing
     * `blocks`, applied to a count rather than a memset since there is no
     * grid content of `buf`'s own to know anything about yet. */
    s->impulse_count = 0;
}

void sand_track_dirty_rows(sand_t *s, uint8_t *rows)
{
    s->dirty_rows = rows;
    if (rows != NULL) {
        /* Nothing is known about what is already on screen, so assume all of
         * it needs redrawing once. */
        memset(rows, 1, (size_t)s->h);
    }
}

void sand_clear(sand_t *s)
{
    memset(s->cells, SAND_EMPTY, (size_t)s->w * (size_t)s->h);
    if (s->dirty_rows != NULL) {
        memset(s->dirty_rows, 1, (size_t)s->h);
    }
    if (s->block_state != NULL) {
        memset(s->block_state, 0, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    /* Any entry still in flight names a cell this memset just wiped, so its
     * stored `cell` byte can no longer match what is actually there - the
     * flight pass would drop every one of them on its next turn anyway (see
     * step_impulses()'s verify-before-moving check). Dropping them here
     * instead reaches the same outcome without paying for it: no stale
     * index survives into a grid that has just been handed back empty. */
    s->impulse_count = 0;
}

cell_t sand_at(const sand_t *s, int x, int y)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        /* Outside the grid reads as STONE, which makes the four walls solid
         * without a single bounds check in the movement code - and, being the
         * densest thing there is, too solid for anything heavy to displace its
         * way through the floor. */
        return CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT);
    }
    return s->cells[y * s->w + x];
}

void sand_set(sand_t *s, int x, int y, cell_t cell)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return;
    }
    s->cells[y * s->w + x] = cell;
    latch_content_flags(s, cell);
    mark_move(s, x, y, x, y);
}

int sand_count(const sand_t *s)
{
    int n = 0;
    const int total = s->w * s->h;
    for (int i = 0; i < total; i++) {
        n += (s->cells[i] != SAND_EMPTY) ? 1 : 0;
    }
    return n;
}

/* Attempt to place `material` at (x, y). Returns whether it did - off the
 * grid or already occupied is not an error, just nothing to do. */
static bool try_spawn_one(sand_t *s, int x, int y, cell_t spec, int band)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return false;
    }
    if (s->cells[y * s->w + x] != SAND_EMPTY) {
        return false;   /* never overwrite, so the count cannot drift */
    }
    /* Latched from the finished cell, not from the material id, because
     * one of the flags depends on the variant - and through the SAME
     * helper sand_set() uses, because this list existing twice is what
     * let the brush and the setter disagree about snow.
     *
     * An extended material is written exactly as given: its low nibble is
     * its identity, so there is no variant for random_cell() to pick and
     * picking one would change which material it is. */
    const cell_t cell = cell_is_extended(spec)
                        ? spec
                        : random_cell(s, (material_id_t)CELL_MATERIAL(spec),
                                      band);
    s->cells[y * s->w + x] = cell;
    latch_content_flags(s, cell);
    mark_move(s, x, y, x, y);
    return true;
}

int sand_spawn(sand_t *s, int cx, int cy, int radius, material_id_t material)
{
    return sand_spawn_cell(s, cx, cy, radius, CELL_MAKE(material, 0));
}

int sand_spawn_cell(sand_t *s, int cx, int cy, int radius, cell_t spec)
{
    int filled = 0;
    const int r2 = radius * radius;
    /* Once for the whole brushful - see random_cell(). */
    const int span = MATERIAL_SHADE_SPAN((material_id_t)CELL_MATERIAL(spec));
    const int band = (int)(((s->pour_phase >> POUR_BAND_SHIFT) *
                            POUR_BAND_STRIDE) % (unsigned)span);

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            if (try_spawn_one(s, cx + dx, cy + dy, spec, band)) {
                filled++;
            }
        }
    }
    return filled;
}

int sand_erase(sand_t *s, int cx, int cy, int radius)
{
    int removed = 0;
    const int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const int x = cx + dx;
            const int y = cy + dy;
            if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
                continue;
            }
            if (s->cells[y * s->w + x] == SAND_EMPTY) {
                continue;   /* already empty, so nothing changed here */
            }
            s->cells[y * s->w + x] = SAND_EMPTY;
            mark_move(s, x, y, x, y);
            removed++;
        }
    }

    /* Also switches off any emitter in the same disc - see this function's
     * own comment in sand.h for why. Not folded into `removed`: that count
     * means cells changed, and an emitter is not a cell. */
    sand_remove_emitters(s, cx, cy, radius);

    return removed;
}

/* Integer floor(sqrt(v)), for exact_disc_count() below - Newton's method,
 * which converges in a handful of iterations for anything this small (v
 * is at most a grid dimension squared, a few hundred thousand at the
 * largest quality this app offers). Not the same isqrt64() tilt.c already
 * has: that one is `static` to its own file and built around int64_t
 * magnitudes an accelerometer reading produces, neither of which this
 * caller needs - a second, smaller copy for a second, smaller domain
 * beats reaching across an unrelated file for one function. */
static int isqrt_floor(int v)
{
    if (v <= 0) {
        return 0;
    }
    int x = v;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

/* THE EXACT number of lattice cells inside a disc of this radius (dx*dx +
 * dy*dy <= r*r), not an upper-bound estimate - see sand_explode()'s own
 * comment for why exactness matters here specifically. For each row `dy`
 * of the disc, the widest `dx` still inside it is floor(sqrt(r*r -
 * dy*dy)), so that row holds exactly 2*dx + 1 cells (dx on either side of
 * the centre column, plus the centre column itself); summing that over
 * every row from -radius to radius gives the disc's true cell count in
 * O(radius) integer square roots, far cheaper than walking the cells
 * themselves. */
static int exact_disc_count(int radius)
{
    if (radius < 0) {
        return 0;
    }
    const int r2 = radius * radius;
    int count = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        const int rem = r2 - dy * dy;
        if (rem < 0) {
            continue;
        }
        count += 2 * isqrt_floor(rem) + 1;
    }
    return count;
}

/* Forward-declared: the shared implementation behind both sand_impulse()
 * and this file's own annulus seeding lives right next to sand_impulse()
 * itself, further down, not up here next to its other caller - see that
 * function's own comment for why one body serves both. */
static void queue_flying_grain(sand_t *s, int x, int y, int dir, int speed,
                               bool allow_dislodge_static, int mat_filter,
                               bool guaranteed_dislodge);

/* One candidate cell of sand_explode()'s own annulus scan, below - split
 * out so the ring-order loop that calls it (see that function's own
 * comment on why ring order rather than row order) does not have to repeat
 * this body once per edge of every ring it walks. (dx, dy) is the offset
 * from the centre, exactly as sand_explode() itself receives it; `r2` is
 * the outer radius squared, so this still enforces the true circular disc
 * even though the ring loop that reaches it walks square Chebyshev shells,
 * not circles - the shell order only decides SEQUENCE, this decides
 * MEMBERSHIP, and the two are independent.
 *
 * `disc_count`, `keep` and `*accum` are what make this disc SELF-LIMITING
 * to whatever buffer the caller actually has, instead of trusting the
 * caller to have picked a radius small enough - see sand_explode()'s own
 * comment on why that trust already failed once. `disc_count` is
 * exact_disc_count()'s own exact count of how many true disc members this
 * call's radius contains; `keep` is how many of them this blast may
 * actually queue (min(disc_count, the buffer's own capacity)); `*accum`
 * is a fixed-point accumulator, shared across every candidate this blast
 * visits, that decides WHICH ones. This is a digital differential
 * analyser - the same technique that rasterises a line one evenly-spaced
 * pixel at a time, not a modulo stride: a stride aliases with the ring
 * loop's own edge lengths (four edges per ring, of varying length) and
 * can clump or gap in ways a flat "every Nth candidate" cannot see
 * coming, where an accumulator that only advances on true disc members
 * spreads them out as evenly as `keep`-out-of-`disc_count` can be spread,
 * in scan order and therefore radially AND angularly at once. The bound
 * this needs to hold: over any P true disc members actually visited (P is
 * always <= disc_count, since disc_count IS the true total - not merely
 * an upper bound on it, see exact_disc_count()'s own comment for why that
 * distinction matters here), the number of times this fires is
 * floor(P * keep / disc_count), which cannot exceed `keep` however P
 * compares to disc_count, and is EXACTLY `keep` once the whole disc has
 * been visited (P == disc_count). Never more selections than the
 * caller's own buffer allows, and never fewer than the buffer can hold
 * either - degrading the DENSITY evenly instead of truncating the SHAPE,
 * which is what running out of room used to mean before this existed
 * (see the ring-order comment's own note on that). */
static void queue_outward_impulse(sand_t *s, int cx, int cy, int dx, int dy,
                                  int r2, int disc_count, int keep, int *accum,
                                  int mat_filter)
{
    if (dx * dx + dy * dy > r2) {
        return;
    }

    *accum += keep;
    if (*accum < disc_count) {
        return;
    }
    *accum -= disc_count;

    /* (dx, dy) IS the vector from the centre to this cell, so handing it
     * straight to the same quantiser gravity uses gives "away from the
     * centre" in one of the eight directions the rest of the simulation
     * already works in - no separate angle math needed. The one input
     * this can never resolve is the centre cell itself, where (dx, dy) is
     * (0, 0) - sand_gravity_direction() reports that as no direction at
     * all, and this simply leaves that cell where it is rather than throw
     * it nowhere in particular. (The centre cell still spends one unit of
     * `keep` here even though it never queues anything - a fixed,
     * one-time rounding cost identical at every radius, not worth a
     * special case to refund.) */
    int qdx, qdy;
    sand_gravity_direction(dx, dy, &qdx, &qdy);
    if (qdx == 0 && qdy == 0) {
        return;
    }

    /* EVERY CANDIDATE GETS THE SAME SAND_EXPLODE_INITIAL_SPEED,
     * DELIBERATELY, not because distance-scaled speed was never tried.
     * "full push at the fireball's edge, decaying toward the outer
     * radius" reads right and was measured, twice - once linear in the
     * squared distance (free, since d2 is already computed two lines
     * up), once a true linear falloff via sqrt - against the dune scene
     * in suite_sand.c (test_the_sand_dune_scene_throws_grains_beyond_
     * its_own_footprint). Both made the blast markedly WORSE by the
     * scene's own numbers, not better: grains outside the footprint fell
     * from an average of 57 to 4 and 3 respectively, and average throw
     * distance fell from 71 to 55 and 48. The cells that actually
     * produce "escaped the footprint" are disproportionately the ones
     * near the outer radius - they have the least distance left to
     * travel - and any falloff that reduces their push specifically
     * guts the exact evidence a working blast is supposed to produce;
     * the core-adjacent cells getting full speed does not compensate,
     * because SAND_IMPULSE_SPEED_RAMP decays their push over TIME
     * before they can cross the same distance from further inside.
     * Flat speed stays until a falloff is found that does not trade the
     * whole outer annulus for a marginally hotter core. */
    /* `true` HERE, AND ONLY HERE - see queue_flying_grain()'s own comment
     * for the density-scaled roll this unlocks and why a blast is the
     * one caller that gets to make a wall's KIND_STATIC refusal a chance
     * instead of a certainty. Every other detail of a dislodged wall
     * cell's flight - speed, direction, everything after this call - is
     * identical to any other entry; the toughness lives entirely in this
     * one boolean and the roll behind it. */
    queue_flying_grain(s, cx + dx, cy + dy, ring_of(qdx, qdy),
                       SAND_EXPLODE_INITIAL_SPEED, true, mat_filter, false);
}

/* THE SHARED IMPLEMENTATION BEHIND sand_impulse() AND sand_explode()'s OWN
 * ANNULUS SEEDING - one body, not two, so the bounds/empty/buffer-full
 * checks that have nothing to do with walls stay in exactly one place.
 * `allow_dislodge_static` is the ONE thing that differs between the two
 * callers, and it is deliberately a parameter here rather than a second
 * copy of this function: sand_impulse() (below) always passes false, so
 * the PUBLIC primitive keeps its hard default - a wall cannot be thrown -
 * for any caller that has not explicitly asked otherwise, while sand_
 * explode()'s own seeding loop (queue_outward_impulse(), above) is the
 * one caller that has, and passes true. A future caller of sand_impulse()
 * itself (gunpowder, gas, whatever comes next) inherits the SAFE default
 * automatically, the same as today - it does not inherit wall-breaking
 * just because this function grew the capability somewhere inside it. */
static void queue_flying_grain(sand_t *s, int x, int y, int dir, int speed,
                               bool allow_dislodge_static, int mat_filter,
                               bool guaranteed_dislodge)
{
    /* Disabled, or already full - see sand_impulse()'s own comment in
     * sand.h on why both are silent no-ops rather than something a caller
     * has to check for itself first. */
    if (s->impulse_buf == NULL || s->impulse_count >= s->impulse_max) {
        return;
    }
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return;
    }

    const size_t at = (size_t)y * (size_t)s->w + (size_t)x;
    const cell_t cell = s->cells[at];
    if (CELL_IS_EMPTY(cell)) {
        return;   /* nothing there to throw */
    }

    /* mat_filter < 0 means "any material", the ordinary case for a plain
     * sand_impulse() or an unmasked sand_displace() - every existing
     * caller. A masked displacement (sand_displace_material()) is the one
     * exception: it exists specifically so a liquid's own splash cannot
     * fling whatever happens to be sitting nearby (dirt under a pool of
     * water, say) along with it. */
    if (mat_filter >= 0 && CELL_MATERIAL(cell) != (uint8_t)mat_filter) {
        return;
    }

    /* A WALL CANNOT BE THROWN BY DEFAULT, any more than one can be
     * entered - the missing HALF of can_impulse_enter()'s own rule
     * (step_impulses(), this file), which only ever gated the
     * DESTINATION a flying grain tries to move into. Nothing gated the
     * SOURCE: this used to be "is there something here to throw", and a
     * stone wall cell is very much something, so it queued exactly like
     * a grain of sand would - and once queued, step_impulses() moved any
     * entry into whatever open (or, since displacement, non-static) cell
     * sat ahead of it, with no idea the thing it was moving happened to
     * itself be a wall. A vessel wall thick enough that no earlier,
     * smaller blast's radius ever reached it hid this for every round
     * before it was fixed: the annulus simply never touched a wall cell
     * to queue in the first place. Doubling the radius (see DETONATE_
     * RADIUS_PX in app_sand.c) was what finally reached one, and the
     * result was exactly what it sounds like - a chunk of the vessel's
     * own wall, given an outward push like anything else in the annulus,
     * walking itself into the genuinely empty space just outside the
     * vessel and leaving the "sealed" box with a hole in it.
     *
     * KIND_STATIC, the same test can_impulse_enter() uses for the
     * opposite half of this rule, because the reasoning is identical
     * either direction: this simulation defines a wall as the one thing
     * with no leverage to be moved BY anything, so BY DEFAULT it has none
     * to be moved either.
     *
     * "BY DEFAULT" IS NO LONGER "ALWAYS", for the one caller that opts in
     * (`allow_dislodge_static`) - a blast should read as TOUGHER against
     * stone or glass than sand, not as an invisible wall no explosion can
     * ever touch. `density` (material.h) is what already measures how
     * hard this simulation treats something as being to move - can_enter()
     * (sand_priv.h) gates whether a moving grain displaces what it lands
     * on by comparing densities the exact same way - so reusing it here
     * instead of inventing a second "toughness" field keeps one number
     * meaning one thing everywhere it appears. The chance is `255 -
     * density`, the same chance-in-256 idiom this project already uses
     * for flammability, mobility and heat_chance (see tick_decay()'s own
     * roll in sand_priv.h for the identical `(rng_next(&s->rng) & 0xFF)`
     * pattern) rather than a new roll shape invented for one case. LOWER
     * density means a HIGHER chance of being dislodged - not the reverse -
     * because density is a measure of how much has to move, and a blast's
     * push is finite: stone and glass (both density 200) are this board's
     * heaviest materials and get only a 55-in-256 chance (about 21%),
     * while wood (150, lighter, and correctly so - a beam gives way more
     * easily than a stone wall) gets 105-in-256 (about 41%), nearly double
     * - a real, visible difference in toughness rather than a coin flip
     * that reads the same for every wall regardless of what it is built
     * from. A failed roll is IDENTICAL to today's unconditional refusal:
     * the wall holds, nothing else about this cell or this call changes.
     * A cell that succeeds falls straight through to the same write every
     * other entry gets, below - same speed, same direction, no other
     * special-casing once it is airborne; the toughness lives entirely in
     * whether it gets thrown at all. */
    if (material_of(cell)->kind == KIND_STATIC) {
        if (!allow_dislodge_static) {
            return;
        }
        /* `guaranteed_dislodge` SKIPS THE ROLL ENTIRELY, rather than
         * every other detail of a dislodged wall's flight staying
         * shared - for reaction_t.vent_chance's own push (sand_impulse_
         * dislodge(), below) specifically: the density-scaled toughness
         * roll exists to make an ORDINARY explosion read as tougher
         * against stone than sand, a real but RANDOM resistance to
         * arbitrary shrapnel reaching an arbitrary wall. A vent's push is
         * not that - it is pressure from directly beneath the exact
         * material sealing it in, not a stray annulus cell that happens
         * to graze a wall a few cells away. Leaving that same ~21%-per-
         * cell coin flip on a THIN, single-cell-deep crust (the realistic
         * result of water quenching lava, and the case this feature is
         * actually judged against) meant most of a covering barely moved
         * even when vent_chance itself finally rolled true - a rare event
         * that then MOSTLY DID NOTHING was the worst of both. The seal a
         * vent is relieving should give way every time the vent fires,
         * the same way covered_from_above() already guarantees IT fires
         * rather than leaving that to chance too. */
        if (!guaranteed_dislodge) {
            const int chance = 255 - (int)material_of(cell)->density;
            if ((int)(rng_next(&s->rng) & 0xFF) >= chance) {
                return;   /* the roll failed - the wall holds, same as always */
            }
        }
    }

    /* REFUSE A SECOND ENTRY FOR A CELL ALREADY TRACKED - two entries both
     * claiming to BE the one grain of material sitting at `at` is always
     * a bookkeeping error, not a feature: only one physical cell exists
     * there, so only one entry can ever legitimately move it. Callers
     * that queue once per event (a single sand_explode()'s own annulus,
     * a single splash) never hit this - each candidate cell is visited at
     * most once per call, and step_impulses()'s per-step retry already
     * covers "try again next step" for whatever is still blocked. The
     * caller this actually guards is one that RE-QUEUES the same cells
     * on a schedule of its own regardless of whether earlier entries for
     * them are still in flight - reaction_t.vent_chance (material.h) at a
     * high roll rate is exactly that: covered_from_above() stays true for
     * as long as ANY covering cell remains, so try_vent() (sand_
     * reactions.c) can fire again on literally the next step, and without
     * this check every such re-fire would add a FRESH duplicate for
     * every cell still blocked from a PRIOR firing - measured filling the
     * impulse buffer to within a few percent of impulse_max on a 20-pod
     * vent-cap test before this existed, with real entries then silently
     * refused once it filled. A linear scan of the buffer, not a per-cell
     * flag - see the "spoken for" comment on step_impulses()'s own
     * re-acquisition just above for why this file avoids the latter, and
     * impulse_count is small enough in every real use of this system
     * (bounded by design intent, not merely by observation so far) that
     * the scan costs nothing worth avoiding.
     *
     * MATCHED ON `cell` TOO, NOT INDEX ALONE - an existing entry's stored
     * index can be STALE at this exact point: step_impulses()'s own
     * "verify before moving" re-acquisition (its own comment) only runs
     * when THAT entry is next processed, which is later in the same
     * step, not retroactively the moment something else touches its
     * cell. Queuing happens during REACTIONS, earlier in the step, so an
     * existing entry here may already no longer describe what is
     * actually sitting at `at`. Treating a stale index as "still tracked"
     * would refuse a genuinely fresh, accurate entry forever - measured
     * turning "some pods still sealed" into "every pod stays sealed"
     * outright when this check compared index alone. Requiring the
     * stored cell to still match the byte just read into `cell` above
     * confirms the existing entry is still an accurate description of
     * this same physical grain, not simply an old index nobody has
     * cleaned up yet. */
    for (int existing = 0; existing < s->impulse_count; existing++) {
        if (s->impulse_buf[existing].index == (uint16_t)at &&
            s->impulse_buf[existing].cell == cell) {
            return;
        }
    }

    impulse_t *entry = &s->impulse_buf[s->impulse_count++];
    entry->index = (uint16_t)at;
    entry->cell  = cell;
    entry->dir   = (uint8_t)dir;
    entry->speed = (uint8_t)speed;
}

void sand_impulse(sand_t *s, int x, int y, int dir, int speed)
{
    queue_flying_grain(s, x, y, dir, speed, false, -1, false);
}

/* reaction_t.vent_chance's (material.h) own single-cell dislodge - the
 * static-wall equivalent of sand_impulse() itself, with `allow_dislodge_
 * static` forced true and `guaranteed_dislodge` set - see queue_flying_
 * grain()'s own comment on why a vent's push skips the density-scaled
 * toughness roll every other wall-dislodging caller keeps. */
void sand_impulse_dislodge(sand_t *s, int x, int y, int dir, int speed)
{
    queue_flying_grain(s, x, y, dir, speed, true, -1, true);
}

/* THE SHARED IMPLEMENTATION BEHIND sand_displace() AND sand_displace_
 * material() - one body, not two, for the same reason queue_flying_grain()
 * above is one body behind sand_impulse() and sand_explode()'s seeding: the
 * annulus math, buffer-sharing math and ring-order scan below have nothing
 * to do with whether a caller wants every candidate or only ones matching
 * one material, and duplicating all of that just to add a filter would be
 * exactly the kind of second copy this file already avoids elsewhere.
 * `mat_filter` is threaded straight through to queue_outward_impulse() and,
 * ultimately, queue_flying_grain() - see its own comment for what -1 means.
 * `guaranteed_dislodge` is the same kind of pass-through - see queue_
 * flying_grain()'s own comment on why sand_impulse_dislodge() (below) skips
 * the usual density-scaled toughness roll entirely rather than sharing it
 * with an ordinary explosion's random resistance. */
static void displace_disc(sand_t *s, int cx, int cy, int radius,
                          int mat_filter, bool guaranteed_dislodge)
{
    if (s->impulse_buf == NULL) {
        return;   /* sand_enable_impulses() was never called - see its comment */
    }

    /* THE ONE CALLER OF sand_impulse(), seeding many radially. Everything
     * about how a queued grain then moves - the flight pass, the arc, the
     * cap, re-acquisition - is sand_impulse()'s and step_impulses()'s job,
     * not this loop's; all this does is decide which cells qualify and
     * which direction each one gets, which is the one part of a
     * displacement that is genuinely specific to its shape.
     *
     * BY RING, OUTWARD FROM THE CENTRE - not by row. See this loop's own
     * comment in sand.h ("QUEUED BY RING, OUTWARD FROM THE CENTRE") for
     * why: a device pass on the first real-radius detonation found that
     * scanning dy-then-dx handed the ENTIRE cap to the top nine or ten
     * rows of the disc before the scan ever reached the core or the lower
     * half, because that is what a top-to-bottom scan truncated by a cap
     * does. `ring` is the Chebyshev distance from the centre -
     * max(|dx|, |dy|) - not the true Euclidean one, because a SQUARE
     * ring's border can be walked directly (four edges, no interior
     * re-scan) where a true circular one would need either a sort or a
     * repeated full-box scan per ring; the r2 check inside
     * queue_outward_impulse() still enforces the real circular disc
     * regardless of which square ring a cell's Chebyshev distance puts it
     * in, so this only changes the ORDER cells are offered to
     * sand_impulse() in, never which cells qualify. Every (dx, dy) with
     * max(|dx|, |dy|) == ring is visited exactly once: ring 0 is the
     * centre alone; each ring after that is its own square's top edge,
     * bottom edge, then the left and right edges with the shared corners
     * left out (already covered by the top/bottom pass). SUPERSEDED BY
     * queue_outward_impulse()'s own accumulator, below, for what happens
     * when the disc does not fit the buffer: this used to mean the buffer
     * filling up partway through, truncating to a smaller-but-complete
     * disc with an untouched outer band. That was safe, but it made
     * DETONATE_RADIUS_PX and the buffer's own capacity (APP_IMPULSE_MAX in
     * app_sand.c) two numbers a caller had to keep in sync by hand -
     * exactly the step that got skipped once already, when the radius
     * doubled without anyone re-deriving the buffer to match, and
     * detonating silently stopped doing anything at all on real hardware.
     * The ring order this comment describes is unchanged and still
     * matters - it is what makes the accumulator's even thinning land
     * radially AND angularly evenly instead of only within one ring - but
     * "what happens when it doesn't fit" is now this function's own job,
     * not a constraint on whoever picks the radius. */
    const int r2 = radius * radius;

    /* THE BUFFER'S CAPACITY IS NOW FIXED, INDEPENDENT OF radius - see
     * APP_IMPULSE_MAX's own comment in app_sand.c, sized once from the
     * device's real heap budget and never touched again when the radius
     * changes. That inversion is what this pair of locals implements:
     * `disc_count` is exact_disc_count()'s own EXACT total for this call's
     * radius, computed here at RUNTIME from whatever radius this call
     * actually received, instead of once at compile time from a single
     * constant radius. `keep` is how much of that disc this call can
     * actually queue right now: the whole disc when it fits in what is
     * actually free (every existing small-radius test and caller gets
     * EXACTLY today's full-density behaviour, unchanged), or whatever
     * room is left when it does not - see `room`'s own comment just
     * below for why that has to be the buffer's REMAINING space rather
     * than its total capacity, and queue_outward_impulse()'s own comment
     * for how `keep`-out-of-`disc_count` turns into an even thinning
     * rather than a truncation.
     *
     * EXACT, NOT A SAFE OVER-ESTIMATE - deliberately, and this is the one
     * place that distinction actually bites. APP_IMPULSE_MAX used to be
     * SIZED from `(355*r*r)/113 + 5*r + 3`, a disc-lattice-point formula
     * proven to always overshoot the true count (see that constant's old
     * derivation, now folded into this history), which is exactly the
     * right shape for a BUFFER ALLOCATION - wasting a little headroom on
     * an over-estimate is harmless, and it must never come up short. A
     * THINNING RATIO has the opposite tolerance: the same formula
     * overshoots small discs badly enough to matter (radius 1's true
     * count is 5, the formula's own estimate is 11 - more than double),
     * and computing `keep` from an inflated total throws away density
     * the buffer had room for. A blast whose buffer could hold every one
     * of its 5 true neighbours would have thinned down to 3 anyway,
     * exactly the regression that surfaced as two failing host tests the
     * first time this used the old formula here - both expected a small,
     * fully-buffered blast to queue EVERY neighbour, and got fewer.
     * exact_disc_count() costs a handful of integer square roots more
     * than the formula did, entirely negligible next to the per-cell work
     * the rest of this function already does once per detonation. */
    /* AGAINST REMAINING ROOM, NOT TOTAL CAPACITY - `s->impulse_max` is how
     * big the buffer IS, not how much of it is FREE right now. Nothing
     * here resets `s->impulse_count` to zero on entry, so a SECOND
     * displacement fired while a FIRST one's grains are still mid-arc
     * (SAND_IMPULSE_SPEED_RAMP hasn't decayed them out yet - see that
     * constant's own comment) finds `s->impulse_count` already above
     * zero. Sizing `keep` from `s->impulse_max` there would compute a
     * density as if the WHOLE buffer were free, seed that many entries
     * into queue_outward_impulse()'s accumulator, and then watch
     * sand_impulse() itself silently refuse every entry past the
     * buffer's REAL remaining room (its own
     * `impulse_count >= impulse_max` guard - safe, no crash, no
     * overflow, but exactly the lopsided, one-sided truncation this
     * whole mechanism exists to avoid, reintroduced via contention
     * between two blasts instead of bias within one: the first rings
     * queued would still land, the later ones would not, because they
     * physically run out of buffer, not because the density math ever
     * knew to expect that. `room` is what closes that gap - the
     * buffer's ACTUAL free space at the moment THIS call runs, so `keep`
     * never promises more than what is really left, however many other
     * in-flight entries got there first. A single, uncontended
     * displacement (`impulse_count` already 0 on entry, the only case a
     * manual DETONATE tap can ever produce today) sees
     * `room == s->impulse_max` and this is a no-op change; it starts
     * mattering the moment a second caller can trigger sand_displace()
     * (directly, or through sand_explode(), below) while a first is
     * still resolving - a chain of igniting gas pockets or confined-
     * steam bursts, say, not yet wired up but exactly the shape of
     * caller this generality was always meant to survive. Do not
     * simplify this back to `s->impulse_max` - see
     * test_two_overlapping_blasts_share_the_buffer_evenly (suite_sand.c)
     * for a test that fails immediately if someone does. */
    const int disc_count = exact_disc_count(radius);
    const int room = s->impulse_max - s->impulse_count;
    const int keep = (disc_count < room) ? disc_count : room;
    int accum = 0;

    queue_outward_impulse(s, cx, cy, 0, 0, r2, disc_count, keep, &accum,
                          mat_filter);
    for (int ring = 1; ring <= radius; ring++) {
        for (int dx = -ring; dx <= ring; dx++) {
            queue_outward_impulse(s, cx, cy, dx, -ring, r2, disc_count, keep,
                                  &accum, mat_filter);
            queue_outward_impulse(s, cx, cy, dx,  ring, r2, disc_count, keep,
                                  &accum, mat_filter);
        }
        for (int dy = -ring + 1; dy <= ring - 1; dy++) {
            queue_outward_impulse(s, cx, cy, -ring, dy, r2, disc_count, keep,
                                  &accum, mat_filter);
            queue_outward_impulse(s, cx, cy,  ring, dy, r2, disc_count, keep,
                                  &accum, mat_filter);
        }
    }
}

void sand_displace(sand_t *s, int cx, int cy, int radius)
{
    displace_disc(s, cx, cy, radius, -1, false);
}

/* Same as sand_displace(), but only cells whose material is exactly
 * `mat_id` are ever queued - everything else within the radius is left
 * untouched, unlike an ordinary displacement which throws whatever it
 * finds. For a liquid's own splash: water landing hard should throw water,
 * not the dirt sitting under it. */
void sand_displace_material(sand_t *s, int cx, int cy, int radius,
                            uint8_t mat_id)
{
    displace_disc(s, cx, cy, radius, (int)mat_id, false);
}

void sand_explode(sand_t *s, int cx, int cy, int radius)
{
    if (s->impulse_buf == NULL) {
        return;   /* sand_enable_impulses() was never called - see its comment */
    }

    /* FILL a cavity with fire, before a single flight entry is queued -
     * see SAND_EXPLODE_CORE_DIVISOR's own comment in sand.h for why an
     * explosion that only ever seeded entries could never move anything
     * once the medium it detonates in has no gap of its own to offer, and
     * for why fire is what fills that gap now rather than emptiness. An
     * explosion flashes and leaves a plume; it does not silently delete
     * whatever was standing there.
     *
     * FIRE IS sand_explode()'s OWN ADDITION, NOT sand_displace()'s -
     * exactly the reason this function still exists as a thin wrapper
     * (below) around the shared displacement primitive rather than the
     * whole mechanism living in one place. Two separate reasons, not one:
     * CORRECTNESS - a future caller wanting the displacement without the
     * combustion (a banked idea: a stone shield over lava, breached by
     * trapped steam PRESSURE rather than heat) must not set anything
     * alight just because it pushed material around; steam is explicitly
     * not fire, and folding fire into the shared primitive would make
     * that impossible to ask for. And COST -
     * converting a cell to MAT_FIRE latches `may_have_burning` (see
     * latch_content_flags()), which keeps the ENTIRE reactions pass
     * active every step until that fire burns itself out, plus whatever
     * conducted-heat propagation follows from it; a caller that fires
     * often and never wanted fire (a chain of confined-steam bursts,
     * exactly the case above) would otherwise pay that ongoing simulation
     * cost for a side effect it never asked for. sand_displace() never
     * touches fire, smoke, or the burning flag at all, so it costs
     * neither the correctness risk nor the ongoing expense - see its own
     * tests for proof it produces neither.
     *
     * Every cell within the core radius is written, unconditionally -
     * occupied or already empty alike. sand_erase() skips an already-empty
     * cell because removing nothing is a no-op worth avoiding; there is no
     * equivalent shortcut here; an empty cell becoming fire is exactly as
     * real a change as an occupied one converting, and skipping it would
     * leave a ring of untouched holes inside the fireball on any board that
     * was not already packed solid.
     *
     * Written by hand rather than through a shared helper: place_cell() in
     * sand_reactions.c is the worked example of what every write like this
     * owes the simulation (latch the content flags a fresh cell arms, mark
     * its row dirty, wake its block and neighbours), but it is `static` to
     * that file, so this repeats the same four things directly instead of
     * exporting it for one caller.
     *
     * CONSEQUENCES, NOT BUGS. Fire ignites flammable neighbours (wood, oil,
     * gas) and boils adjacent water to steam, same as it always has - an
     * explosion starting fires and boiling water is the feature, not a
     * regression to guard against. And the fresh fire cells this loop just
     * wrote are themselves occupied, non-empty cells inside `radius`, so
     * sand_displace()'s own annulus loop, below, queues THEM as flight
     * entries too, exactly like any other grain in the disc - the
     * fireball's own edge gets thrown outward along with everything else,
     * which is one more reason a real explosion's flash reads as an
     * expanding thing rather than a static disc. */
    /* A BARE SINGLE CELL turned out not to be enough of a seed once
     * SAND_EXPLODE_CORE_DIVISOR grew from 2 to 3 - measured, not assumed:
     * a 20,000-seed sweep of a fully packed radius-2 detonation (division
     * gives core_radius 0 there, versus 1 at the old divisor) found the
     * density-swap collapse simply never reaching far enough to open a
     * cavity on about 11% of seeds, no matter how many further steps it
     * was given - genuinely stuck, not merely slow, so more steps was not
     * the fix. `radius >= 2` clamped to a minimum of 1 restores exactly
     * the old, already-proven core shape at every radius small enough for
     * plain division to have zeroed it out, while changing nothing at the
     * radii that motivated raising the divisor in the first place: at a
     * real detonation's scale (radius 24), 24 / 3 = 8 is already far
     * above this floor, so the floor never engages there at all. radius
     * 1 is deliberately excluded from the clamp - it stays at core_radius
     * 0, the single centre cell every test and comment already assumes,
     * since nothing measured that case as broken. */
    const int core_radius_raw = radius / SAND_EXPLODE_CORE_DIVISOR;
    const int core_radius = (core_radius_raw == 0 && radius >= 2)
                                 ? 1 : core_radius_raw;
    const int core_r2 = core_radius * core_radius;
    for (int fdy = -core_radius; fdy <= core_radius; fdy++) {
        for (int fdx = -core_radius; fdx <= core_radius; fdx++) {
            if (fdx * fdx + fdy * fdy > core_r2) {
                continue;
            }
            const int fx = cx + fdx;
            const int fy = cy + fdy;
            if (fx < 0 || fx >= s->w || fy < 0 || fy >= s->h) {
                continue;
            }
            const size_t fat = (size_t)fy * (size_t)s->w + (size_t)fx;
            /* Fresh, full-life fire - the same MATERIAL_VARIANTS - 1
             * convention random_cell() (above in this file) uses for any
             * transient material, so a blast's core reads exactly like
             * fire painted by hand would: brightest right after ignition,
             * fading from there via its own ordinary decay. */
            const cell_t fire = CELL_MAKE(MAT_FIRE, MATERIAL_VARIANTS - 1);
            s->cells[fat] = fire;
            latch_content_flags(s, fire);
            mark_rows(s, fy, fy);
            wake_block_and_neighbors(s, fx, fy);
        }
    }

    sand_displace(s, cx, cy, radius);
}

/*---------------------------------------------------------------------------
 * Emitters - see the `emitters` field of sand_t and the EMITTERS section of
 * sand.h for the design. What is here is just list management; the actual
 * per-step write lives in emit_from_emitters() below, next to sand_step().
 *-------------------------------------------------------------------------*/

bool sand_add_emitter(sand_t *s, int x, int y, cell_t cell)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return false;
    }

    /* Retune in place if one is already here, rather than adding a second
     * - see sand_add_emitter()'s own comment in sand.h. */
    for (int i = 0; i < s->emitter_count; i++) {
        if (s->emitters[i].x == x && s->emitters[i].y == y) {
            s->emitters[i].cell = cell;
            return true;
        }
    }

    if (s->emitter_count >= SAND_MAX_EMITTERS) {
        return false;
    }

    s->emitters[s->emitter_count].x    = (int16_t)x;
    s->emitters[s->emitter_count].y    = (int16_t)y;
    s->emitters[s->emitter_count].cell = cell;
    s->emitter_count++;
    return true;
}

int sand_remove_emitters(sand_t *s, int cx, int cy, int radius)
{
    const int r2 = radius * radius;
    int removed = 0;
    int kept = 0;

    /* Compact in place: every emitter that survives the disc test is
     * copied down to the next free slot, so the surviving emitters end up
     * contiguous at the front with no gap for a later sand_emitter_at() to
     * trip over. */
    for (int i = 0; i < s->emitter_count; i++) {
        const int dx = s->emitters[i].x - cx;
        const int dy = s->emitters[i].y - cy;
        if (dx * dx + dy * dy <= r2) {
            removed++;
            continue;
        }
        if (kept != i) {
            s->emitters[kept] = s->emitters[i];
        }
        kept++;
    }
    s->emitter_count = kept;
    return removed;
}

int sand_emitter_count(const sand_t *s)
{
    return s->emitter_count;
}

bool sand_emitter_at(const sand_t *s, int i, int *x, int *y, cell_t *cell)
{
    if (i < 0 || i >= s->emitter_count) {
        return false;
    }
    *x    = s->emitters[i].x;
    *y    = s->emitters[i].y;
    *cell = s->emitters[i].cell;
    return true;
}

/* One pass over every emitter, run once per sand_step() - see below for
 * where and why. Each tries to place its own cell at its own point, but
 * only if that point is currently empty.
 *
 * `s->emitters[i].cell` is NOT the exact byte to write. It comes straight
 * from the app's brush table (see brushes[] in app_sand.c), and every entry
 * there is CELL_MAKE(material, 0) - a placeholder whose variant is not yet
 * meaningful. What variant 0 MEANS depends on the material (see material.h's
 * top comment and random_cell() above): for most materials it is a fine
 * fresh value, but for a KIND_LIQUID it is zero mass - a degenerate, invisible
 * cell, since a liquid's variant is how much of it is there. Writing it raw
 * is exactly the bug this function used to have: a lava emitter placed a
 * MAT_LAVA cell with nothing in it, which the reactions pass could still
 * quench into steam - MAT_LAVA was really there - but which never rendered
 * or flowed, because it carried no mass to render or flow.
 *
 * sand_spawn_cell() is what resolves that placeholder everywhere else a
 * brush is used (pouring), through random_cell(), so placing through it here
 * too is what makes an emitter agree with a pour about what a fresh cell of
 * a material looks like. For a liquid that resolution is not random at all -
 * random_cell() always hands back a FULL cell (CELL_MAKE(material,
 * MASS_MAX)), because a fresh cell is a full one. A radius of 0 places
 * exactly one cell, not a disc: the disc loop's only offset with
 * dx*dx + dy*dy <= 0 is (0, 0). A disc emitted every step would be a
 * firehose, not a tap - and, worse, would bury its own source: the centre
 * cell would never see empty again once the disc around it filled in.
 *
 * The "only if empty" rate control is no longer written out here as an
 * explicit check - sand_spawn_cell() goes through try_spawn_one(), which
 * already refuses to overwrite an occupied cell ("never overwrite, so the
 * count cannot drift"), so the same self-limiting refill falls out of that
 * refusal for free. A material that flows away quickly - water - refills
 * every step and reads as a steady stream; a viscous one - oil - clears its
 * own doorway slowly and refills just as slowly, reading as a drip. The
 * material table already encodes how fast each one moves, so it already
 * encodes how fast its own tap can run: there is no separate rate parameter
 * to tune, and no way for a tap to flood the board faster than its own
 * material disperses, because it can never place a second cell while the
 * first is still sitting there.
 *
 * Going through sand_spawn_cell() also gets the CRITICAL part for free, the
 * same as sand_set() did before it: try_spawn_one() latches the cell's
 * content flags and calls mark_move(), which marks the row dirty for the
 * renderer AND wakes the block the point is in (see mark_move()'s comment
 * in sand_priv.h). Without that wake, material appearing inside a block
 * that had gone to sleep would never be swept - it would sit exactly
 * where it was written, forever, and look like the emitter was broken
 * rather than like the wake was missing. */
static void emit_from_emitters(sand_t *s)
{
    for (int i = 0; i < s->emitter_count; i++) {
        sand_spawn_cell(s, s->emitters[i].x, s->emitters[i].y, 0,
                        s->emitters[i].cell);
    }
}

/*---------------------------------------------------------------------------
 * Movement
 *-------------------------------------------------------------------------*/

/* The sign/magnitude split both gravity_direction functions below start
 * with. Returns false for a zero vector, in which case the direction is
 * undefined and the caller must stop rather than divide by it. */
static bool gravity_axes(int gx, int gy, int *ax, int *ay, int *sx, int *sy)
{
    *ax = im_abs(gx);
    *ay = im_abs(gy);

    if (*ax == 0 && *ay == 0) {
        return false;
    }

    *sx = im_sign(gx);
    *sy = im_sign(gy);
    return true;
}

void sand_gravity_direction(int gx, int gy, int *dx, int *dy)
{
    int ax, ay, sx, sy;
    if (!gravity_axes(gx, gy, &ax, &ay, &sx, &sy)) {
        *dx = 0;
        *dy = 0;
        return;
    }

    if (ay * AXIS_DEN > ax * AXIS_NUM) {
        *dx = 0;         /* within 22.5 deg of vertical */
        *dy = sy;
    } else if (ax * AXIS_DEN > ay * AXIS_NUM) {
        *dx = sx;        /* within 22.5 deg of horizontal */
        *dy = 0;
    } else {
        *dx = sx;        /* the diagonal octant */
        *dy = sy;
    }
}

/* dest_row() is shared with sand_liquid.c and lives in sand_priv.h now. */

/* How many grains are stacked directly against gravity above this one, capped.
 *
 * Note this does NOT use sand_at(), which reports out-of-bounds as occupied so
 * that the walls are solid for free. That convention is exactly wrong here: a
 * grain resting against the ceiling would count as buried and lock up, when in
 * fact nothing is on top of it at all. Off the grid is open sky. */
int sand_load_above(const sand_t *s, int x, int y, int dx, int dy)
{
    int n  = 0;
    int cx = x - dx;
    int cy = y - dy;

    while (n < SAND_LOAD_CAP) {
        if (cx < 0 || cx >= s->w || cy < 0 || cy >= s->h) {
            break;
        }
        if (s->cells[cy * s->w + cx] == SAND_EMPTY) {
            break;
        }
        n++;
        cx -= dx;
        cy -= dy;
    }
    return n;
}

/* slide_chance() moved to sand_priv.h (still static inline), alongside the
 * rest of the grain-movement primitive stack it is part of - see that
 * header's own comment above can_enter() for why. */

/* driven_by_gravity() - whether a grain may slide in direction (mx, my) at
 * all, given gravity and its material's angle of repose - moved to
 * sand_priv.h (still static inline) since sand_gas.c needs it too, to
 * build its own driven[][] table against a reversed gravity vector. See
 * its comment there for the physics and the full reasoning for the move. */

/* How strongly the true angle leans toward the diagonal, as 0-256.
 *
 * `r` is the ratio of the smaller component to the larger, scaled to 0-256, so
 * it runs from 0 on an axis to 256 at 45 degrees. What we want is the angle's
 * position in that range, which is atan(r/256) / 45deg - and that is NOT r
 * itself: at 22.5 degrees r is 106, not 128.
 *
 * The correction is Rajan's approximation, atan(x) ~= x*pi/4 + 0.273*x*(1-x),
 * rearranged and scaled. 0.3477 * 256 = 89. It lands within a degree across the
 * whole range, which is far finer than anything visible in falling sand. */
static int diagonal_weight(int r)
{
    return r + ((89 * r * (256 - r)) >> 16);
}

void sand_gravity_direction_dithered(sand_t *s, int gx, int gy,
                                     int *dx, int *dy)
{
    int ax, ay, sx, sy;
    if (!gravity_axes(gx, gy, &ax, &ay, &sx, &sy)) {
        *dx = 0;
        *dy = 0;
        return;
    }

    const int lo = ax < ay ? ax : ay;
    const int hi = ax < ay ? ay : ax;

    /* hi is non-zero here, since not both components are zero. */
    const int r = (int)(((int64_t)lo * 256) / hi);

    if (rng_chance(&s->rng, diagonal_weight(r))) {
        *dx = sx;              /* the diagonal between the two axes */
        *dy = sy;
    } else if (ax > ay) {
        *dx = sx;              /* the dominant axis */
        *dy = 0;
    } else {
        *dx = 0;
        *dy = sy;
    }
}

void sand_set_scatter(sand_t *s, int chance)
{
    /* Negative means "each material's own figure", which is what the app
     * wants; anything else overrides every material alike, which is what a
     * test wants. */
    if (chance < 0) {
        s->scatter = SAND_SCATTER_PER_MATERIAL;
    } else {
        s->scatter = chance > 255 ? 255 : chance;
    }
}

void sand_set_soak(sand_t *s, int chance)
{
    if (chance < 0) {
        s->soak = SAND_SOAK_PER_MATERIAL;
    } else {
        s->soak = chance > 255 ? 255 : chance;
    }
}

void sand_set_decay(sand_t *s, int chance)
{
    if (chance < 0) {
        s->decay = SAND_DECAY_PER_MATERIAL;
    } else {
        s->decay = chance > 255 ? 255 : chance;
    }
}

void sand_set_evaporates(sand_t *s, int chance)
{
    if (chance < 0) {
        s->evaporates = SAND_EVAPORATES_PER_MATERIAL;
    } else {
        s->evaporates = chance > 255 ? 255 : chance;
    }
}

void sand_set_mobility(sand_t *s, int chance)
{
    if (chance < 0) {
        s->mobility = SAND_MOBILITY_PER_MATERIAL;
    } else {
        s->mobility = chance > 255 ? 255 : chance;
    }
}

void sand_set_flammability(sand_t *s, int chance)
{
    if (chance < 0) {
        s->flammability = SAND_FLAMMABILITY_PER_MATERIAL;
    } else {
        s->flammability = chance > 255 ? 255 : chance;
    }
}

void sand_set_conduction(sand_t *s, int chance)
{
    if (chance < 0) {
        s->conduction = SAND_CONDUCTION_PER_MATERIAL;
    } else {
        s->conduction = chance > 255 ? 255 : chance;
    }
}

void sand_set_vent_chance(sand_t *s, int chance)
{
    if (chance < 0) {
        s->vent_chance = SAND_VENT_CHANCE_PER_MATERIAL;
    } else {
        s->vent_chance = chance > 255 ? 255 : chance;
    }
}

/* can_enter()/cell_open()/move_to() moved to sand_priv.h (still
 * static inline) - see that header's own comment for why the whole
 * grain-movement primitive stack lives there now. pour_into()/room_in()
 * are the liquid-specific siblings and stayed in sand_liquid.c; sand.c's
 * own movement never splits a grain, so nothing here needed them once
 * the liquid branch did. */


/* Whether each slide is driven at this tilt, for each material - depends
 * only on the direction and the material's angle of repose, so it is worked
 * out once per step for all sixteen materials rather than recomputed for
 * every one of 41,000 cells. */
static void compute_driven(bool driven[MATERIAL_MAX][2], const int *slide_a,
                           const int *slide_b, int gx, int gy)
{
    for (int m = 0; m < MATERIAL_MAX; m++) {
        const int repose = materials[m].repose;
        driven[m][0] = driven_by_gravity(slide_a[0], slide_a[1], gx, gy, repose);
        driven[m][1] = driven_by_gravity(slide_b[0], slide_b[1], gx, gy, repose);
    }
}

/* Which column order to sweep this step, against the direction of travel -
 * see the comment on sand_step() for why that direction matters. Alternates
 * when gravity has no horizontal component, so piles do not lean
 * consistently one way.
 *
 * Only the step direction comes out now, not a from/to range: since the
 * sweep walks block-columns (see step_one_row()/block_x_order()), each
 * block works out its own cell range from x_step and its own bounds, so a
 * single row-wide from/to pair has no reader left. */
static int sweep_x_order(sand_t *s, int dx)
{
    int x_step;
    if (dx > 0) {
        x_step = -1;
    } else if (dx < 0) {
        x_step = 1;
    } else if (s->sweep_flip) {
        x_step = -1;
    } else {
        x_step = 1;
    }
    s->pour_phase++;
    s->sweep_flip = !s->sweep_flip;
    return x_step;
}

/* try_scatter()/pick_slide_order()/try_slide_pair(), and the _impl forms
 * of try_fall_or_scatter()/try_slide() - one grain's whole turn: try to
 * fall, then the two slides either side of it, with friction and shaking
 * deciding whether the slides are allowed at all - all moved to
 * sand_priv.h (still static inline). See that header's own comment above
 * try_fall_or_scatter_impl() for the measured reason, and for why the
 * ordinary (non-inline) try_fall_or_scatter()/try_slide() sand_gas.c
 * actually calls are defined below instead, alongside step_one_grain()
 * rather than in the header. */

static bool step_one_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                           uint8_t *arow, uint8_t *brow, int x, int y, int w,
                           int dx, int dy, const int *slide_a,
                           const int *slide_b, int load_dx, int load_dy,
                           int jostle, bool driven[MATERIAL_MAX][2])
{
    const cell_t grain = row[x];
    const material_t *mat = material_of(grain);
    if (mat->kind == KIND_STATIC || mat->kind == KIND_GAS) {
        /* Static never moves, so it costs a single comparison - which is
         * what makes a wall of stone free to have on screen.
         *
         * Gas is skipped here for the same reason liquid's cross-flow is:
         * rising means moving AGAINST this sweep's own direction, into
         * cells not yet visited, which would let it move several times in
         * one step and teleport to the ceiling. Handled by its own pass,
         * sand_step_gas() in sand_gas.c, called from sand_step() after
         * this sweep finishes. */
        return false;
    }

    const uint8_t mat_id  = CELL_MATERIAL(grain);
    const uint8_t density = mat->density;

    /* Liquids move an AMOUNT rather than a whole grain - see
     * move_liquid_grain() in sand_liquid.c, and sand_step_liquids() below
     * for the rest of a liquid's behaviour, which is NOT gravity-ward and
     * so cannot join this sweep. */
    if (mat->kind == KIND_LIQUID) {
        return move_liquid_grain(s, row, prow, x, y, dx, dy,
                                 slide_a, slide_b, grain, mat_id);
    }

    if (jostle == 0) {
        const int scatter = (s->scatter >= 0) ? s->scatter : mat->scatter;
        /* _impl, called directly, not the ordinary try_fall_or_scatter()
         * below - this is the hottest call site in the whole simulation,
         * and it needs to stay inlined. See sand_priv.h's own comment
         * above try_fall_or_scatter_impl() for why there are two forms
         * of this function at all. */
        if (try_fall_or_scatter_impl(s, row, prow, arow, brow, x, y, w, dx,
                                     dy, slide_a, slide_b, grain, density,
                                     scatter)) {
            return true;
        }
    }

    return try_slide_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a,
                          slide_b, load_dx, load_dy, jostle, grain, mat_id,
                          density, mat, driven);
}

/* The ordinary, non-inline forms - see sand_priv.h's own comment above
 * try_fall_or_scatter_impl() for why these exist alongside the inline
 * versions step_one_grain() calls directly above. sand_gas.c calls
 * these, not the _impl versions, so gas movement costs one ordinary
 * function call rather than a second full inlined copy of this chain. */
bool try_fall_or_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                         uint8_t *arow, uint8_t *brow, int x, int y,
                         int w, int dx, int dy, const int *slide_a,
                         const int *slide_b, cell_t grain,
                         uint8_t density, int scatter)
{
    return try_fall_or_scatter_impl(s, row, prow, arow, brow, x, y, w, dx,
                                    dy, slide_a, slide_b, grain, density,
                                    scatter);
}

bool try_slide(sand_t *s, uint8_t *row, uint8_t *prow, uint8_t *arow,
               uint8_t *brow, int x, int y, int w, int dx, int dy,
               const int *slide_a, const int *slide_b, int load_dx,
               int load_dy, int jostle, cell_t grain, uint8_t mat_id,
               uint8_t density, const material_t *mat,
               bool driven[MATERIAL_MAX][2])
{
    return try_slide_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a,
                          slide_b, load_dx, load_dy, jostle, grain, mat_id,
                          density, mat, driven);
}

/* Sleeping is off entirely when block_state does not exist. Otherwise, wakes
 * every block when the grid is being shaken or the settle-relevant direction
 * has changed underneath it - either can free a grain that had nothing to do
 * with what its neighbours were doing, so no block's settled state survives -
 * and returns which of the two dithered directions this step is using, so a
 * block can be marked settled against the right one.
 *
 * The NEAREST direction is what is compared, not the dithered one - that
 * changes almost every step by design, and comparing it would mean nothing
 * ever slept.
 *
 * Also clears BLOCK_ACTIVE for every block, every step (unless the full
 * reset above already did): that bit gets set fresh as this step's sweep
 * and liquid pass touch blocks, and is read once, at the very end of
 * sand_step(), to decide which blocks earned the settled bit this step -
 * see the finalisation pass there. */
static uint8_t compute_settled_bit(sand_t *s, int jostle, int dx, int dy,
                                   int load_dx, int load_dy)
{
    if (s->block_state == NULL) {
        return 0;
    }

    const int n = s->block_cols * s->block_rows;
    const uint8_t bit = (dx == load_dx && dy == load_dy)
                      ? BLOCK_SETTLED_NEAREST : BLOCK_SETTLED_OTHER;
    if (jostle > 0 ||
        load_dx != s->last_load_dx || load_dy != s->last_load_dy) {
        /* A mass wake leaves nothing settled, so the sweep will walk every
         * block and re-establish BLOCK_HAS_LIQUID for all of them - clearing
         * it here along with everything else is exactly right. */
        memset(s->block_state, 0, (size_t)n);
    } else {
        for (int i = 0; i < n; i++) {
            uint8_t v = (uint8_t)(s->block_state[i] & ~BLOCK_ACTIVE);
            /* BLOCK_HAS_LIQUID is the sweep's own observation, so it is
             * cleared for exactly the blocks the sweep is about to make it
             * afresh. A block it will SKIP keeps last time's answer, which is
             * still true: nothing in a settled block moved. See the invariant
             * above BLOCK_HAS_LIQUID in sand_priv.h. */
            if ((v & bit) == 0) {
                v &= (uint8_t)~BLOCK_HAS_LIQUID;
            }
            s->block_state[i] = v;
        }
    }
    return bit;
}

/* Which way to step through a row's blocks, mirroring sweep_x_order()'s
 * cell-level x_from/x_to/x_step - derived from x_step's sign rather than
 * computed independently, since the cell order within a row is already
 * decided once per sand_step() call and the block order must agree with
 * it (a block swept back-to-front while its cells go front-to-back would
 * not change correctness, since each block's own cell loop is still self-
 * consistent, but would visit blocks in a confusing order for no reason -
 * kept aligned for clarity, not because it is load-bearing). */
static void block_x_order(int block_cols, int x_step, int *bx_from,
                          int *bx_to, int *bx_step)
{
    if (x_step > 0) {
        *bx_from = 0;            *bx_to = block_cols; *bx_step = 1;
    } else {
        *bx_from = block_cols - 1; *bx_to = -1;        *bx_step = -1;
    }
}

/* Everything step_one_block() needs that stays the same across every
 * block-column in one row's sweep, bundled into one struct and passed by
 * pointer - so the hot per-block call only needs two arguments (this and
 * bx) rather than the dozen-plus that would otherwise have to go through
 * argument registers, or the stack once they run out. Measured to matter:
 * with the flat parameter list, forcing every block-column in a full-grid
 * sweep through a real call (instead of a skip) regressed the full-
 * occupancy frame budget by several hundred microseconds on real
 * hardware - RISC-V has 8 argument registers and the flat version needed
 * 18. */
typedef struct {
    sand_t     *s;
    uint8_t    *row, *prow, *arow, *brow;
    int         y, w, dx, dy, x_step;
    const int  *slide_a, *slide_b;
    int         load_dx, load_dy, jostle;
    int         by;
    /* Which materials are liquid, as a bitmask over the nibble - the same
     * trick, and the same reason, as sand_liquid.c's liquid_mask(): the sweep
     * has to answer "is this cell liquid?" per occupied cell to maintain
     * BLOCK_HAS_LIQUID, and a shift-and-mask on a register answers it without
     * a second read of the material table. */
    uint16_t    is_liquid;
    bool      (*driven)[2];
} sweep_ctx_t;

/* One block's x-span within one row of the gravity sweep - the unit
 * step_one_row() below can skip entirely when settled. Marks the block
 * BLOCK_ACTIVE the moment anything in it moves, for compute_settled_bit()'s
 * later finalisation pass to read; does nothing if block_state does not
 * exist (sleeping disabled), since then there is nothing to mark. */
static void step_one_block(const sweep_ctx_t *ctx, int bx)
{
    int lo = bx * SAND_BLOCK_W;
    int hi = lo + SAND_BLOCK_W;
    if (hi > ctx->w) {
        hi = ctx->w;
    }

    int cx_from, cx_to;
    if (ctx->x_step > 0) {
        cx_from = lo;     cx_to = hi;
    } else {
        cx_from = hi - 1; cx_to = lo - 1;
    }

    bool moved_here = false;
    unsigned saw_liquid = 0;
    for (int x = cx_from; x != cx_to; x += ctx->x_step) {
        const cell_t c = ctx->row[x];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        /* Accumulated in a register and stored once per block below, the same
         * shape as moved_here - the whole point of BLOCK_HAS_LIQUID is that
         * keeping it true costs O(blocks) per step rather than O(moves), the
         * question docs/Sand/Performance-Tuning-Attempts.md's ninth attempt
         * says to ask of any skip structure before building it. */
        saw_liquid |= (unsigned)(ctx->is_liquid >> CELL_MATERIAL(c)) & 1u;
        if (step_one_grain(ctx->s, ctx->row, ctx->prow, ctx->arow, ctx->brow,
                           x, ctx->y, ctx->w, ctx->dx, ctx->dy, ctx->slide_a,
                           ctx->slide_b, ctx->load_dx, ctx->load_dy,
                           ctx->jostle, ctx->driven)) {
            moved_here = true;
        }
    }

    if ((moved_here || saw_liquid) && ctx->s->block_state != NULL) {
        ctx->s->block_state[ctx->by * ctx->s->block_cols + bx] |=
            (uint8_t)((moved_here ? BLOCK_ACTIVE : 0) |
                      (saw_liquid ? BLOCK_HAS_LIQUID : 0));
    }
}

/* One row of the gravity sweep, walked by block-column rather than by
 * cell: a block whose settled bit is already set for this step's direction
 * is skipped entirely - none of its cells are even read - since it was
 * already examined under this exact direction with nothing to do, and
 * nothing has moved next to it since, so it cannot have anywhere to go.
 * When sleeping is disabled (block_state is NULL), settled_bit is always 0
 * (see compute_settled_bit()), so the skip check never fires and every
 * block still gets walked - the same total work as a flat per-cell walk,
 * just partitioned into SAND_BLOCK_W-wide chunks. */
static void step_one_row(sand_t *s, int y, int w, int dx, int dy,
                         const int *slide_a, const int *slide_b, int x_step,
                         int load_dx, int load_dy, int jostle,
                         uint8_t settled_bit, uint16_t is_liquid,
                         bool driven[MATERIAL_MAX][2])
{
    sweep_ctx_t ctx = {
        .s = s,
        .row  = s->cells + (size_t)y * (size_t)w,
        .prow = dest_row(s, y + dy),
        .arow = dest_row(s, y + slide_a[1]),
        .brow = dest_row(s, y + slide_b[1]),
        .y = y, .w = w, .dx = dx, .dy = dy, .x_step = x_step,
        .slide_a = slide_a, .slide_b = slide_b,
        .load_dx = load_dx, .load_dy = load_dy, .jostle = jostle,
        .by = y / SAND_BLOCK_H,
        .is_liquid = is_liquid,
        .driven = driven,
    };

    int bx_from, bx_to, bx_step;
    block_x_order(s->block_cols, x_step, &bx_from, &bx_to, &bx_step);

    for (int bx = bx_from; bx != bx_to; bx += bx_step) {
        if (settled_bit != 0 &&
            (s->block_state[ctx.by * s->block_cols + bx] & settled_bit)) {
            continue;
        }
        step_one_block(&ctx, bx);
    }
}

/* Finalise a step's settling: a block only earns the settled bit if
 * nothing marked it BLOCK_ACTIVE anywhere in the step just finished -
 * neither the sweep nor the liquid pass - and none of its up to 8
 * neighbours did either (any_neighbor_active(), sand_priv.h - the
 * pull-based replacement for the old per-move wake mechanism; see that
 * function's own comment). Deferred to here, once, rather than decided
 * per-row as the old row-shaped design could: a block spans
 * SAND_BLOCK_H rows, each swept by a separate step_one_row() call, so
 * whether anything moved in it cannot be known until every row
 * belonging to it has been visited - which, for a block, only happens
 * once across the entire sweep. */
static void finalize_settling(sand_t *s, uint8_t settled_bit)
{
    if (s->block_state == NULL) {
        return;
    }
    for (int by = 0; by < s->block_rows; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            const int i = by * s->block_cols + bx;
            if (s->block_state[i] & BLOCK_ACTIVE) {
                continue;
            }
            if (any_neighbor_active(s, bx, by)) {
                s->block_state[i] &=
                    (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
            } else {
                s->block_state[i] |= settled_bit;
            }
        }
    }
}

/* The two rays a liquid levels along, and what one step of each costs in
 * gravitational potential. See xflow_t.
 *
 * Whichever of gx/gy has the larger magnitude picks the major ray: `ax`
 * always runs perpendicular to that dominant axis. `dg`, the diagonal
 * partner, is the ring neighbour on the side the tilt actually leans - it
 * is built from the same two signs as `ax` rather than looked up, so it is
 * always the correct one of the two diagonals next to that axis, never the
 * one on the far side.
 *
 * `q_q8` is the raw ratio of the smaller gravity component to the larger,
 * scaled to 0-256 - the TANGENT of the tilt, which is exactly the slope of
 * the level line. This is deliberately the raw ratio and not
 * diagonal_weight()'s angle correction above: diagonal_weight() answers
 * "where does the true angle sit between two octants", which is what
 * dithering gravity's own direction needs, but what is wanted here is the
 * line's slope, not the angle's position - and the raw ratio already IS
 * that slope.
 *
 * Both biases are built from one shared pair of per-axis constants, `bx`
 * and `by` - the gravity direction scaled to a mass unit - rather than each
 * being worked out separately for its own ray. That is what makes the level
 * field exactly consistent: find_shallowest() walks a loop of comparisons
 * across cells that can be reached by either ray, and a shared (bx, by)
 * guarantees those comparisons agree with each other no matter which ray
 * got each cell there, the way two independently-rounded biases could not
 * promise.
 *
 * `im_len()`'s ~4% approximation (see intmath.h) is fine here for the same
 * reason it is fine everywhere else in this file: nothing reads a bias to
 * that precision, only its sign and rough size. The one thing that DOES
 * matter - a bias of exactly zero when a ray is exactly level - comes from
 * bx*ax0 + by*ax1 (or the dg equivalent) landing on exactly zero by simple
 * cancellation, a property of the dot product that does not depend on the
 * length at all. */
static void build_xflow(xflow_t *f, int gx, int gy)
{
    const int ax = im_abs(gx), ay = im_abs(gy);
    const int sx = im_sign(gx), sy = im_sign(gy);

    if (ay >= ax) {
        /* Gravity is mostly vertical: the level surface runs mostly across. */
        f->ax[0] = (sx >= 0) ? 1 : -1;  f->ax[1] = 0;
        f->dg[0] = f->ax[0];            f->dg[1] = (sy >= 0) ? -1 : 1;
        f->q_q8 = (ay != 0) ? (ax * 256) / ay : 0;
    } else {
        /* Gravity is mostly sideways: the level surface runs mostly up. */
        f->ax[0] = 0;                   f->ax[1] = (sy >= 0) ? -1 : 1;
        f->dg[0] = (sx >= 0) ? 1 : -1;  f->dg[1] = f->ax[1];
        f->q_q8 = (ay * 256) / ax;
    }

    const int len = im_len(gx, gy);
    if (len == 0) {
        f->bias_ax_q8 = 0;
        f->bias_dg_q8 = 0;
        return;
    }
    const int bx = (MASS_MAX * 256 * gx) / len;
    const int by = (MASS_MAX * 256 * gy) / len;
    f->bias_ax_q8 = bx * f->ax[0] + by * f->ax[1];
    f->bias_dg_q8 = bx * f->dg[0] + by * f->dg[1];
}

/* Whether a FLYING grain may swap into the cell currently holding `target`,
 * as opposed to can_enter() (sand_priv.h), which answers the same question
 * for ORDINARY gravity-driven movement.
 *
 * DELIBERATELY NOT can_enter(). That function says a mover displaces a
 * target only if the target is a fluid (KIND_LIQUID/KIND_GAS) AND the mover
 * is denser - the right rule for a grain settling under its own weight,
 * because weight is the only force involved and a powder has no business
 * sinking through another powder just because gravity is pulling on both of
 * them identically. An impulse entry is not settling, though - something
 * has already forced it into motion - and reusing can_enter() as-is turned
 * out to make the flight pass nearly powerless exactly where the round that
 * added displacement was aimed: a grain buried in an ordinary sand pile is
 * surrounded by MORE SAND, same material, same density, and can_enter()'s
 * mover > target test can never be true between two cells of identical
 * density however hard either one is being pushed. That is not a fluid
 * edge case, it is the ENTIRE INTERIOR of every powder scene this mechanic
 * exists for - so a density gate copied from can_enter() would have looked
 * reasonable while doing nothing for the one case that motivated this.
 *
 * THE ANSWER: density does not gate this at all - a flying grain may swap
 * into ANY occupied, non-static cell, regardless of which of the two is
 * denser. Speed does not need its own separate threshold either: this is
 * only ever consulted from the branch below that already required this
 * turn's rng_chance(entry.speed) roll to succeed, so every call here is
 * already gated on the entry currently carrying enough of the blast's own
 * force to be moving AT ALL this step - a roll that can never succeed once
 * `speed` has ramped to zero (see SAND_IMPULSE_SPEED_RAMP's own comment in
 * sand.h). A second, hand-picked "how much speed buys a push" constant
 * would only be reproducing a threshold the ramp already enforces for free.
 *
 * STATIC IS STILL A WALL - the one thing this deliberately does NOT loosen.
 * A shove has no leverage against something this simulation defines as
 * immovable regardless of any arithmetic - stone, wood, glass, an extended
 * material's structure - so KIND_STATIC never yields here any more than it
 * does to can_enter(). This is what keeps a blast inside a sealed vessel
 * from tunnelling its own way out through the wall: the wall was never the
 * obstacle this rule exists to open, only the packed interior it contains.
 * sand_at()'s out-of-bounds-reads-as-STONE convention folds the grid edge
 * into the same guarantee for free, exactly as it already did for
 * can_enter()'s own callers. */
static inline bool can_impulse_enter(cell_t target)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }
    return material_of(target)->kind != KIND_STATIC;
}

/* The flight pass: every entry in s->impulse_buf either moves exactly one
 * cell along the direction it was queued with, waits another turn for its
 * way to clear, or is finally dropped. Called from sand_step(), immediately
 * before finalize_settling() - see docs/Sand/Explosion-Plan.md's "Where the
 * pass runs, and why it must be LAST" for the two reasons that position is
 * not a preference. In short: running after every other pass that can move
 * or replace a cell is what keeps an entry's stored position honest -
 * nothing else can have touched it again before this pass's own next turn -
 * and it is what turns a plain outward push into a ballistic arc for
 * nothing extra, since gravity has already pulled in the sweep above by the
 * time this runs and "down" plus "out, decaying" simply add.
 *
 * BLOCKED MEANS WAIT, NOT STOP. A cell in the way used to drop the entry on
 * the spot - fine for open air, wrong for anything packed: an explosion
 * into a bed of sand or a body of water starts with every queued cell
 * surrounded by more of the same material, so that rule dropped nearly
 * everything on its very first turn and only the annulus already touching
 * open space (or the fire-filled core sand_explode() now writes - see
 * SAND_EXPLODE_CORE_DIVISOR in sand.h - which a denser neighbour can swap
 * straight through) ever went anywhere. Keeping a blocked entry instead
 * lets it try again next step, once whatever was ahead of it has had a
 * chance to move out of the way - which is what lets the disturbance the
 * core's fire starts unpack outward over several steps instead of being a
 * single frozen ring.
 *
 * WAIT STILL HAPPENS, BUT ONLY AGAINST A TRUE WALL NOW - see
 * can_impulse_enter()'s own comment just above this function for why a
 * flying grain shoulders aside any non-static occupant it meets instead of
 * only ever moving into a genuinely empty cell. That shrinks "blocked" down
 * to KIND_STATIC and the grid edge specifically, but does not remove the
 * need for this branch: a wall is still a wall, and the wait-then-retry
 * behaviour this comment describes is exactly what keeps an entry pinned
 * against one bounded rather than dropped the instant it arrives.
 *
 * `dx`/`dy` is this step's own dithered gravity direction, the same one
 * sand_step()'s main sweep just used - needed for RE-ACQUISITION, below,
 * which is what makes a thrown grain arc at all rather than flying dead
 * straight for exactly one cell. The sweep runs BEFORE this pass, every
 * step, on every ordinary cell including ones this list still has an eye
 * on: an airborne grain sitting in open air is not special to the sweep,
 * so gravity moves it down one cell before this pass ever gets a turn on
 * it that step. Naively, the entry's stored index then names a cell the
 * grain no longer occupies, the identity check below fails, and the entry
 * is dropped - meaning a grain gets exactly one outward hop, ever, then
 * spends the rest of its fall as an ordinary grain with no more push. That
 * reads as lateral scatter out of a crater, not an arc, and it is not a
 * hypothetical: it is what "the crater works now but the grains don't
 * arc" was.
 *
 * A single `if` with nothing queued, which is every step on a board with
 * nothing in flight - the same shape sand_step_gas()'s own may_have_gas
 * gate gives a board with no gas on it. */
static void step_impulses(sand_t *s, int dx, int dy)
{
    if (s->impulse_count == 0) {
        return;
    }

    const int w = s->w;
    const int h = s->h;
    int kept = 0;

    /* CASCADE candidates, collected here and appended only AFTER the main
     * loop below finishes - see SAND_CASCADE_SPEED_DIVISOR's own comment
     * in sand.h for why. Queuing into s->impulse_buf mid-loop would grow
     * s->impulse_count while this same loop's own bound (`i <
     * s->impulse_count`) is still reading it, so a freshly-queued relay
     * could be revisited in this SAME pass - an unbounded same-step chain
     * through however much connected liquid happens to be there, not the
     * one-ring-per-step ripple this is meant to be. Collecting here and
     * queuing once the loop's own compaction (`kept`) has already
     * finished keeps every relay exactly one step behind the hop that
     * caused it. */
    impulse_t cascade[SAND_CASCADE_MAX_PER_STEP];
    int cascade_count = 0;

    for (int i = 0; i < s->impulse_count; i++) {
        impulse_t entry = s->impulse_buf[i];

        /* Verify before moving. Nothing marks this index as "spoken for" -
         * that would be the per-cell flag this whole design exists to
         * avoid - so ordinary gravity in the sweep above, a reaction, or an
         * external sand_set()/sand_erase() may already have touched this
         * exact cell since the entry's last turn.
         *
         * RE-ACQUIRE before giving up. The sweep just ran and can only have
         * moved this exact grain to one of three cells: one step along
         * gravity, or one of the two diagonal slides either side of it -
         * the same three destinations step_one_grain() itself ever writes
         * to. Check those for a byte-for-byte match before concluding the
         * grain is truly gone.
         *
         * THIS IS SOUND, NOT A HACK, because of what "byte-for-byte match"
         * already means everywhere else in this file: two cells holding
         * the same material and the same variant are the same GRAIN as far
         * as anything here can tell or cares - move_to()'s own swap logic
         * already trusts that equivalence for the cell it displaces. If
         * re-acquisition latches onto some OTHER grain that merely happens
         * to carry an identical byte, nothing observable changes: the same
         * material continues flying from a position gravity actually just
         * put a matching grain at, and conservation still holds exactly,
         * because nothing here creates or destroys a cell - it only decides
         * which already-identical cell the entry's own bookkeeping follows.
         * What it must NOT do, and does not: adopt a DIFFERENT byte. If
         * nothing among the three candidates matches, the entry is still
         * dropped exactly as before - see
         * test_a_dropped_entry_never_moves_someone_elses_cell.
         *
         * LIQUIDS GET A SECOND CHANCE, MATCHED ON MATERIAL - measured, not
         * theoretical: tracing a water-settling scene showed 44.5% of
         * queued water entries lost to exactly this gap, which is coin-flip
         * odds against a splash/cascade entry surviving even one step. The
         * fix stays scoped to water/acid - the only materials that ever
         * carry a cascade - so the solid path above, and its byte-exact
         * identity guarantee, is untouched. A material-only match is the
         * right test here specifically because cross-flow moves MASS, not
         * grains: it can drain or refill a cell's amount without moving it
         * to one of the three swap-shaped candidates above, and it can
         * change the variant byte (mass amount) without changing the
         * material at all. Checked at the original cell first (cross-flow
         * often leaves mass sitting right where it was, just a different
         * amount), then its 8 neighbours (equalise_liquids() moves mass by
         * at most one cell per step - sand_liquid.c). Re-anchoring
         * entry.cell to whatever byte is actually found keeps its stored
         * mass/variant truthful for every check after this one. */
        if (s->cells[entry.index] != entry.cell) {
            bool reacquired = false;

            /* HEAT-RAMPING MATERIALS FIRST, AND CHECKED AT THEIR OWN
             * POSITION ONLY, before even trying the movement candidates
             * below - a heat-ramping material's variant nibble IS a
             * TEMPERATURE, not a shade (see random_cell()'s own comment),
             * and it keeps drifting every step for as long as the cell
             * sits next to whatever is heating it. A BLOCKED entry never
             * actually moves - can_impulse_enter() (below) is what keeps
             * it waiting at the exact index it was queued with - so a
             * mismatch here is guaranteed to be drift, never motion, and
             * the entry's own position is the only place that drift could
             * have happened. Trying the movement candidates FIRST would
             * risk exactly the failure this exists to prevent: several
             * cells of the same heat-ramping material sitting near the
             * same heat source drift in step with each other, so a
             * neighbour in the gravity direction can easily carry the
             * exact same stale byte by coincidence, and the byte-exact
             * candidate check below cannot tell that apart from a real
             * move - silently handing the entry a neighbouring wall
             * cell's identity instead of its own. Exactly the case a
             * blast's own wall-dislodge roll (queue_outward_impulse(),
             * this file) hits hardest via reaction_t.vent_chance
             * (material.h): the stone try_vent() (sand_reactions.c)
             * blasts is, by definition, touching the lava it was sealing
             * in, so it is ALWAYS actively ramping while it waits for a
             * turn to actually move. */
            const uint8_t lost_mat = CELL_MATERIAL(entry.cell);
            if (reactions[lost_mat].heat_ramp != 0) {
                const cell_t here = s->cells[entry.index];
                if (!CELL_IS_EMPTY(here) && CELL_MATERIAL(here) == lost_mat) {
                    entry.cell = here;
                    reacquired = true;
                }
            }

            const int ox = (int)((unsigned)entry.index % (unsigned)w);
            const int oy = (int)((unsigned)entry.index / (unsigned)w);
            const int i_dir = ring_of(dx, dy);
            const int *slide_a = ring_dir(i_dir + 7);
            const int *slide_b = ring_dir(i_dir + 1);
            const int cand[3][2] = {
                { ox + dx,         oy + dy         },
                { ox + slide_a[0], oy + slide_a[1] },
                { ox + slide_b[0], oy + slide_b[1] },
            };

            for (int c = 0; c < 3 && !reacquired; c++) {
                const int cx = cand[c][0];
                const int cy = cand[c][1];
                if ((unsigned)cx >= (unsigned)w ||
                    (unsigned)cy >= (unsigned)h) {
                    continue;
                }
                const size_t cat = (size_t)cy * (size_t)w + (size_t)cx;
                if (s->cells[cat] == entry.cell) {
                    entry.index = (uint16_t)cat;
                    reacquired = true;
                }
            }

            if (!reacquired) {
                if (lost_mat == MAT_WATER || lost_mat == MAT_ACID) {
                    const cell_t here = s->cells[entry.index];
                    if (!CELL_IS_EMPTY(here) &&
                        CELL_MATERIAL(here) == lost_mat) {
                        entry.cell = here;
                        reacquired = true;
                    }
                    for (int c = 0; c < 8 && !reacquired; c++) {
                        const int *rd = ring_dir(c);
                        const int cx = ox + rd[0];
                        const int cy = oy + rd[1];
                        if ((unsigned)cx >= (unsigned)w ||
                            (unsigned)cy >= (unsigned)h) {
                            continue;
                        }
                        const size_t cat = (size_t)cy * (size_t)w + (size_t)cx;
                        const cell_t found = s->cells[cat];
                        if (!CELL_IS_EMPTY(found) &&
                            CELL_MATERIAL(found) == lost_mat) {
                            entry.index = (uint16_t)cat;
                            entry.cell  = found;
                            reacquired = true;
                        }
                    }
                }
            }

            if (!reacquired) {
                continue;
            }
        }

        /* Read once, after re-acquisition (which can rewrite entry.cell -
         * see its own comment above) has had its say, and reused for every
         * material check below instead of re-deriving it three times. */
        const uint8_t mat_id = CELL_MATERIAL(entry.cell);

        /* AIRBORNE SOLIDS FALL TOO. A KIND_STATIC material never moves on
         * its own - the main sweep (sand_step(), this file) skips it
         * outright, which is exactly what makes stone or glass hold its
         * shape. That is also why a THROWN chunk of it never arced the
         * way a thrown grain of sand or splash of water does: those get
         * gravity for free from the ordinary sweep EVERY step in addition
         * to whatever this loop does, while a flying KIND_STATIC entry
         * only ever got this loop's own directional push - a straight
         * line, not a fall. This is the sweep's missing half for exactly
         * the one case that needs it: while an entry is tracked here at
         * all (thrown, not yet resting), it ALSO gets one unconditional
         * gravity-ward attempt every step, same candidate order as an
         * ordinary grain's own fall (straight down first, then the two
         * diagonal slides either side) and the same can_impulse_enter()
         * rule every other move in this loop already uses. Combined with
         * the outward push below - which fades as `speed` decays while
         * this does not - the two together turn "moves outward for a
         * while, then drops straight down" into a visible arc, the same
         * way the sweep+impulse combination already does for non-static
         * material; this just extends that combination to the one kind
         * that never got it. UNCONDITIONAL, not rolled: a thrown chunk
         * should fall every bit as reliably as a grain the sweep touches
         * every step, and gating this on `speed` would tie "still
         * falling" to "still has outward energy left", which is
         * backwards - the whole point is that it keeps falling well
         * after the outward push has spent itself. */
        if (materials[mat_id].kind == KIND_STATIC) {
            const int gi_dir = ring_of(dx, dy);
            const int *g_slide_a = ring_dir(gi_dir + 7);
            const int *g_slide_b = ring_dir(gi_dir + 1);
            const int gx = (int)((unsigned)entry.index % (unsigned)w);
            const int gy = (int)((unsigned)entry.index / (unsigned)w);
            const int gcand[3][2] = {
                { gx + dx,           gy + dy           },
                { gx + g_slide_a[0], gy + g_slide_a[1] },
                { gx + g_slide_b[0], gy + g_slide_b[1] },
            };
            for (int c = 0; c < 3; c++) {
                const int cx = gcand[c][0];
                const int cy = gcand[c][1];
                if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                    continue;
                }
                /* NOT can_impulse_enter() - that lets a flying entry
                 * shoulder aside a LIQUID same as any other non-static
                 * occupant, which is fine for a deliberate outward THROW
                 * but wrong for this unconditional per-step fall: a
                 * covering cell dislodged from directly beside lava has
                 * one of its own settling-slide candidates land right
                 * back on the lava cell it just came from (the two are
                 * diagonally adjacent by construction), and lava is
                 * KIND_LIQUID, so can_impulse_enter() would happily let
                 * the falling chunk swap straight into it - overwriting
                 * the very lava this feature exists to never touch (see
                 * step_one_burning_cell()'s "a burning LIQUID is never
                 * smothered" invariant). A falling solid should rest ON a
                 * liquid surface, not sink into it - the same principle
                 * already applied elsewhere (sand resting on oil instead
                 * of sinking) - so gravity-drift treats a liquid target
                 * as blocked, same as a wall, and only ever falls into
                 * genuinely empty space or a non-liquid occupant. */
                const cell_t gtarget = sand_at(s, cx, cy);
                if (!CELL_IS_EMPTY(gtarget) &&
                    material_of(gtarget)->kind == KIND_LIQUID) {
                    continue;
                }
                if (!can_impulse_enter(gtarget)) {
                    continue;
                }
                const size_t gat  = entry.index;
                const size_t gnat = (size_t)cy * (size_t)w + (size_t)cx;
                const cell_t gdisplaced = s->cells[gnat];
                s->cells[gnat] = entry.cell;
                s->cells[gat]  = gdisplaced;
                latch_content_flags(s, entry.cell);
                if (!CELL_IS_EMPTY(gdisplaced)) {
                    latch_content_flags(s, gdisplaced);
                }
                mark_move(s, gx, gy, cx, cy);
                entry.index = (uint16_t)gnat;
                break;
            }
        }

        /* The roll happens before the move attempt, and it happens EVERY
         * turn - blocked or not. Two ways to write this, and the choice
         * matters:
         *
         *   roll every turn (this)   a wedged entry ages out on the same
         *                            schedule as one that has been moving
         *                            the whole time
         *   roll only on a move      a wedged entry pays nothing for
         *                            waiting, so it waits FOREVER if its
         *                            target never opens
         *
         * The second sounds more generous - "it never got to move, why
         * should it decay" - but it breaks the one guarantee the whole
         * design rests on: every entry's lifetime is bounded. A sealed
         * vessel or an undisturbed pile is exactly the case where a
         * blocked entry's target may never open at all, and that is
         * precisely when an unbounded wait would show up: entries surviving
         * indefinitely, silently occupying the list, for no visible reason
         * on the board. Rolling every turn keeps every entry - moving or
         * merely hoping to - on the same bounded clock everything else in
         * this file already trusts.
         *
         * The roll's own chance IS entry.speed - see
         * SAND_IMPULSE_SPEED_RAMP's own comment in sand.h for why one byte
         * carries both "chance this turn's move happens" and "how much
         * flight is left" instead of a separate step counter alongside a
         * fixed rate, and for why that is what turns the arc into an
         * actual curve instead of a bent line. */
        const bool rolled_move = rng_chance(&s->rng, entry.speed);

        /* Ramps down every turn, for exactly the same "no exceptions"
         * reason the roll above runs every turn - moved, blocked, or about
         * to be dropped, `speed` ages regardless. Saturating rather than
         * wrapping: once it reaches zero it stays there, so rng_chance()
         * with a zero numerator never succeeds again and the entry is
         * dropped, below, the very next time this runs - the ramp needs no
         * separate "done flying" check of its own.
         *
         * WATER AND ACID GET THEIR OWN, GEOMETRIC DECAY - reported as
         * needing to hit hard and then die out fast, rather than fading
         * gradually over a linear ramp's full ~128-step tail (see
         * SAND_IMPULSE_SPEED_RAMP's own comment for that arithmetic). A
         * right-shift instead of a subtraction is what makes the tail
         * actually SHORT instead of merely smaller - halving loses most of
         * the value in the first couple of steps and reaches zero from 255
         * in about 8, where the old linear ramp took ~128 - and it is the
         * same halving idiom the cascade's own hop-to-hop decay already
         * uses (SAND_CASCADE_SPEED_DIVISOR), so the two now share one
         * vocabulary for "loses energy" instead of two different shapes.
         * Scoped to water/acid, same as the cascade and the wall-bounce
         * just above splash_displace()'s own call site - every other
         * material keeps the original linear SAND_IMPULSE_SPEED_RAMP
         * unchanged, so this does not touch sand_explode()'s own
         * extensively-measured tuning (see SAND_IMPULSE_SPEED_RAMP's and
         * SAND_EXPLODE_CORE_DIVISOR's comments in sand.h for that history).
         *
         * DID INTERACT WITH THE CASCADE GATE - caught by
         * test_a_cascading_impulse_moves_more_than_one_cell (suite_sand.c)
         * failing the moment this landed: a relayed entry that takes more
         * than one step to roll a move now decays under the cascade's own
         * SAND_CASCADE_MIN_SPEED * SAND_CASCADE_SPEED_DIVISOR gate before
         * it gets the chance, since that per-step decay is this same
         * geometric shift now, not the old linear ramp. Fixed by dropping
         * SAND_CASCADE_MIN_SPEED to 1 (see its own comment) so the gate
         * stops being the thing that cuts a chain short - the roll's own
         * exhaustion is. */
        if (mat_id == MAT_WATER || mat_id == MAT_ACID) {
            entry.speed = (uint8_t)(entry.speed -
                                    (entry.speed >> SAND_SPLASH_SPEED_DECAY_SHIFT));
        } else {
            entry.speed = (entry.speed > SAND_IMPULSE_SPEED_RAMP)
                              ? (uint8_t)(entry.speed - SAND_IMPULSE_SPEED_RAMP)
                              : 0;
        }

        if (!rolled_move) {
            continue;   /* out of flight - settles exactly where it is */
        }

        const int x = (int)((unsigned)entry.index % (unsigned)w);
        const int y = (int)((unsigned)entry.index / (unsigned)w);
        const int *d = ring_dir(entry.dir);
        const int nx = x + d[0];
        const int ny = y + d[1];

        /* can_impulse_enter(), NOT a bare CELL_IS_EMPTY() check any more -
         * see that function's own comment for why a flying grain now
         * shoulders aside any non-static occupant instead of waiting on
         * only a genuinely empty cell. STATIC still blocks unconditionally,
         * so containment still falls out of this one check with no
         * raycast: a blast inside a sealed vessel throws its grains up to
         * the wall and they wait there, exactly as before - only the
         * packed interior on the way there stopped being a wall too.
         * sand_at() reading out-of-bounds as STONE (KIND_STATIC) folds the
         * grid edge into the same guarantee for free. */
        const cell_t target = sand_at(s, nx, ny);
        if (!can_impulse_enter(target)) {
            /* Blocked means WAIT: keep the entry exactly as it is - same
             * position, same direction, already-ramped speed and all - so
             * it gets another turn next step rather than being dropped for
             * something that may clear a moment later. Reached only for a
             * true wall now, an excluded target, or the grid edge - see
             * can_impulse_enter().
             *
             * WATER AND ACID BOUNCE INSTEAD OF JUST WAITING - a splash
             * hitting a wall mid-flight should kick back off it, not stall
             * against it for the rest of its (already-ramped) flight the
             * way a chunk of solid debris plausibly would. Reversing `dir`
             * to the opposite ring direction turns the remaining speed
             * into a rebound rather than a wasted wait; if the opposite
             * direction is ALSO blocked (a corner, a one-wide gap) this
             * simply flips again next step, which reads as the droplet
             * vibrating in place until its flight ages out - bounded by
             * SAND_IMPULSE_SPEED_RAMP the same as any other entry, not a
             * new failure mode. Every other material keeps the plain wait,
             * unchanged - see splash_displace()'s own comment in
             * sand_liquid.c for why this scope matches the splash feature
             * itself, water and acid only. */
            if (mat_id == MAT_WATER || mat_id == MAT_ACID) {
                entry.dir = (entry.dir + 4) & 7;
            }
            s->impulse_buf[kept++] = entry;
            continue;
        }

        const size_t at  = entry.index;
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;

        /* A SWAP, not an overwrite - move_to()'s own trick (sand_priv.h),
         * reused here for the same reason: whatever the mover is displacing
         * (empty, same as always, or now a real occupant) takes the cell
         * the mover is vacating, rather than that occupant's cell being
         * blanked. Conservation needs nothing extra to hold: two cells that
         * both already existed a moment ago simply trade places, so the
         * grand total is untouched whichever of the two cases this was. */
        const cell_t displaced = s->cells[nat];

        s->cells[nat] = entry.cell;
        s->cells[at]  = displaced;
        latch_content_flags(s, entry.cell);
        /* The displaced occupant, if any, is not a fresh cell - it already
         * existed on the board a moment ago, at `nat` - but every write
         * owes the same bookkeeping regardless of whether what landed there
         * is new, so this costs one more cheap latch rather than a special
         * case for "not empty". Skipped only for the everyday case where
         * there was nothing to displace at all. */
        if (!CELL_IS_EMPTY(displaced)) {
            latch_content_flags(s, displaced);
        }
        mark_move(s, x, y, nx, ny);

        /* CASCADE - see its own comment above this loop for why this only
         * COLLECTS a candidate rather than queuing one directly. WATER
         * and ACID only, matching splash_displace()'s own scope
         * (sand_liquid.c) - this exists to serve that feature, not as a
         * general property of every impulse.
         *
         * RELAYS BACKWARD, NOT FORWARD - checked one step BEHIND where
         * this entry started (x, y - the position it just vacated - minus
         * one more step in `d`, its own direction of travel), not one
         * step past where it landed. The cell AHEAD of a mover is close
         * to definitionally open (that is why the move just succeeded),
         * so checking there almost never finds anything to relay into -
         * tried first, and a straight test column pushed upward showed
         * exactly that: the lone entry moved once into open space and the
         * cascade never fired again, because there was never more of the
         * same material further along ITS OWN path to find. What should
         * relay is whatever water was FEEDING this move from behind: if
         * it is the same material, it can now advance into the gap this
         * entry just left, which is what actually turns one grain moving
         * into a connected chain advancing together - a piston, not a
         * single flying droplet. */
        if ((mat_id == MAT_WATER || mat_id == MAT_ACID) &&
            entry.speed >= SAND_CASCADE_MIN_SPEED * SAND_CASCADE_SPEED_DIVISOR &&
            cascade_count < SAND_CASCADE_MAX_PER_STEP) {
            const int rx = x - d[0];
            const int ry = y - d[1];
            if ((unsigned)rx < (unsigned)w && (unsigned)ry < (unsigned)h) {
                const cell_t relay_target =
                    s->cells[(size_t)ry * (size_t)w + (size_t)rx];
                if (!CELL_IS_EMPTY(relay_target) &&
                    CELL_MATERIAL(relay_target) == mat_id) {
                    impulse_t *c = &cascade[cascade_count++];
                    c->index = (uint16_t)((size_t)ry * (size_t)w + (size_t)rx);
                    c->cell  = relay_target;
                    c->dir   = entry.dir;
                    c->speed = (uint8_t)(entry.speed / SAND_CASCADE_SPEED_DIVISOR);
                }
            }
        }

        s->impulse_buf[kept].index = (uint16_t)nat;
        s->impulse_buf[kept].cell  = entry.cell;
        s->impulse_buf[kept].dir   = entry.dir;
        s->impulse_buf[kept].speed = entry.speed;
        kept++;
    }

    s->impulse_count = kept;

    /* Appended only now that `kept` (and so s->impulse_count, set just
     * above) is final - see this function's own top comment for why
     * mid-loop queuing was not safe here. Each entry is queued exactly
     * the way sand_impulse() itself would, just batched: this is not a
     * new primitive, only a deferred, bounded set of ordinary impulses. */
    for (int i = 0; i < cascade_count; i++) {
        sand_impulse(s, (int)((unsigned)cascade[i].index % (unsigned)w),
                    (int)((unsigned)cascade[i].index / (unsigned)w),
                    cascade[i].dir, cascade[i].speed);
    }
}

void sand_step(sand_t *s, int gx, int gy, int jostle)
{
    /* Emitters get their one attempt per step FIRST, before gravity is even
     * looked at - so that, from outside this file, a step with emitters on
     * the board looks exactly like sand_spawn_cell() having been called at
     * each emitter's point a moment before sand_step(), which is already
     * the shape every other kind of pour on this board takes. Placed here
     * rather than after the sweep, a freshly emitted grain also gets to
     * move in the very same step it appears, instead of sitting one whole
     * frame before its first move - and placing it consistently at one end
     * of the step or the other is what lets a test rely on which. Run
     * unconditionally, even in free fall (gx == gy == 0): "once per
     * sand_step()" as the design calls for, not "once per step gravity
     * happens to be nonzero". */
    emit_from_emitters(s);

    /* Dithered rather than nearest, so a tilt between two of the eight
     * directions flows at its true angle instead of snapping. Costs one random
     * number per STEP - not per grain - so it is free at this scale. */
    int dx, dy;
    sand_gravity_direction_dithered(s, gx, gy, &dx, &dy);

    /* Load is measured against the NEAREST direction, not the dithered one.
     *
     * How much weight is on a grain is a property of the pile; it cannot change
     * because of which way this particular step happened to round. Using the
     * dithered direction looks up-and-left on the diagonal steps, which reads
     * empty above a vertical column - so a buried grain was treated as a free
     * surface grain roughly one step in eight, which is more than enough to
     * walk the base of a pile sideways. */
    int load_dx, load_dy;
    sand_gravity_direction(gx, gy, &load_dx, &load_dy);

    if (dx == 0 && dy == 0) {
        return;   /* free fall: no down, so nothing settles */
    }

    const int i = ring_of(dx, dy);
    const int *slide_a = ring_dir(i + 7);
    const int *slide_b = ring_dir(i + 1);

    const uint8_t settled_bit = compute_settled_bit(s, jostle, dx, dy,
                                                    load_dx, load_dy);

    /* Remembered AFTER compute_settled_bit() has compared against it, and
     * OUTSIDE it, which is the point: it used to be set in there, past an
     * early return taken whenever block sleeping is off. That made it a
     * fact about the sleeping bookkeeping rather than about the board, and
     * anything else asking which way is down - growth, for one - read (0,0)
     * on any grid without block_state. It is the settled direction of the
     * step just taken, so it is written once the step has decided it. */
    s->last_load_dx = load_dx;
    s->last_load_dy = load_dy;
    s->last_step_dx = dx;
    s->last_step_dy = dy;

    bool driven[MATERIAL_MAX][2];
    compute_driven(driven, slide_a, slide_b, gx, gy);

    /* Sweep AGAINST the direction of travel, on both axes.
     *
     * This is the one thing that has to be right. A grain only ever moves to a
     * cell in the gravity-ward half of its neighbourhood, so visiting those
     * cells first guarantees a grain that moves is never visited again in the
     * same step. Sweep the other way and a falling grain gets picked up and
     * moved repeatedly, teleporting to the floor in a single frame. */
    const int y_from = (dy > 0) ? s->h - 1 : 0;
    const int y_to   = (dy > 0) ? -1       : s->h;
    const int y_step = (dy > 0) ? -1       : 1;

    const int x_step = sweep_x_order(s, dx);

    /* Which way a liquid spreads: PERPENDICULAR TO GRAVITY, not across the
     * screen - tilt the board and the surface tilts with it, so spreading
     * along a screen row spreads in the wrong direction once tilted.
     *
     * Both directions across the flow, not just the one the sweep already
     * passed - the main sweep can safely use neither, see equalise_liquids().
     *
     * Taken from the NEAREST direction, not the dithered one, for the same
     * reason load_dx/load_dy is above: the dithered direction changes almost
     * every step once off axis, and a resting pool judged against a
     * constantly-changing axis reads as unbalanced when it is not - see
     * test_a_settled_pool_does_not_flicker. */
    const int i_stable = ring_of(load_dx, load_dy);
    const int *const perp_a = ring_dir(i_stable + 2);
    const int *const perp_b = ring_dir(i_stable + 6);

    xflow_t flow;
    build_xflow(&flow, gx, gy);

    const int w = s->w;
    const uint16_t is_liquid = liquid_mask();

    for (int y = y_from; y != y_to; y += y_step) {
        step_one_row(s, y, w, dx, dy, slide_a, slide_b, x_step,
                    load_dx, load_dy, jostle, settled_bit, is_liquid, driven);
    }

    /* Everything about a liquid that is NOT gravity-ward: cross-flow. See
     * sand_step_liquids() in sand_liquid.c. Run before finalising which
     * blocks get to sleep below, since a cross-flow move can still touch a
     * block the main sweep left quiet - BLOCK_ACTIVE has to reflect the
     * WHOLE step, not just the sweep's share of it. */
    sand_step_liquids(s, &flow, dx, dy);

    /* Same reasoning, same slot, for gas: rising is not gravity-ward, so it
     * cannot join the main sweep either - see sand_step_gas()'s own
     * comment in sand_priv.h. Order relative to sand_step_liquids() above
     * does not matter; both must finish before finalize_settling() does.
     *
     * Checked here rather than left to sand_step_gas()'s own early
     * return, unlike sand_step_liquids() just above - a deliberate,
     * measured asymmetry: this call site is reached on every step of
     * every test in the suite (sand_step_liquids() always was too, and
     * its own equivalent check was never worth revisiting), and skipping
     * the call outright avoids marshalling all nine arguments for a
     * function that would immediately return anyway on every step no
     * gas has ever touched. Small - most of the small residual cost
     * measured after fixing the two real regressions above is flash
     * layout, not this call - but genuinely free to take. */
    if (s->may_have_gas) {
        sand_step_gas(s, gx, gy, dx, dy, slide_a, slide_b, perp_a, perp_b,
                     load_dx, load_dy, x_step, jostle);
    }

    /* Same slot again, for a burning cell's reactions: ignition/
     * extinguish/burn-out are neither gravity-ward nor movement at all,
     * so they cannot join the main sweep and must finish before
     * finalize_settling() too. Takes only `s`, unlike sand_step_gas()'s
     * nine arguments - it briefly took (gx, gy) as well, while boiling
     * walked against gravity to find a liquid's surface, but boiling
     * happens at the heat source now and the steam bubbles up by itself.
     * With nothing to marshal there is no cost to dodge by checking
     * may_have_burning out here as well, so the function's own internal
     * check (mirroring sand_step_liquids()'s pattern, not
     * sand_step_gas()'s) is enough. */
    sand_step_reactions(s);

    /* Last of all, and deliberately so - see step_impulses()'s own comment
     * and docs/Sand/Explosion-Plan.md's "Where the pass runs, and why it
     * must be LAST". Everything above this line has already had its one
     * chance to move or replace a cell this step; running flight after all
     * of it is what lets an entry trust its own stored position at the top
     * of its next turn, and it is what makes a thrown grain arc instead of
     * flying in a straight line - gravity already pulled in the sweep, this
     * only adds the outward half. */
    step_impulses(s, dx, dy);

    finalize_settling(s, settled_bit);
}
