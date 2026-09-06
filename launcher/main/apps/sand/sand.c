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

/* Jumps shades instead of stepping one at a time. Walking one shade apart
 * results in a gap of about twenty luminance points. Five is coprime with 12,
 * 16, 8, and 3, so all shades are visited but not in order. Stride must be
 * coprime with span to visit all shades. */
#define POUR_BAND_STRIDE 5u

/* `band` centers this pour's shade, computed once by the caller for all
 * cells. Modulo uses real division due to MATERIAL_SHADE_SPAN's runtime
 * ternary. Computed per cell, it costs about 11% of the spawn path; hoisting
 * reduces this to one calculation per brushful. */
static cell_t random_cell(sand_t *s, material_id_t material, int band)
{
    /* A liquid's variant is an amount, not a shade, so a fresh cell is a full
     * one. Giving it a random level would be pouring random quantities. */
    if (material_by_id(material)->kind == KIND_LIQUID) {
        return CELL_MAKE(material, MASS_MAX);
    }
    /* A transient material's variant is life remaining, not a shade either -
     * see material.h's top comment and the `decay` field it documents.
     * Fresh gas starts at full life so it fades from vivid to gone, rather
     * than spawning already partway decayed. */
    if (material_by_id(material)->decay != 0) {
        return CELL_MAKE(material, MATERIAL_VARIANTS - 1);
    }
    /* A heat-ramping material's variant is a TEMPERATURE, not a shade. A
     * fresh cell is at ROOM temperature, not the bottom of its range
     * (frosted). Random would give a pane already half melted, and
     * MATERIAL_VARIANTS - 1 would melt on the next step. */
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
    /* Dry cells have no moisture, picking a SOIL_DRY_TONES shade instead.
     * Fresh soil is always dry to avoid giving players fertile ground. `band`
     * is pre-folded into the dry range, using +/-1 jitter for varied tones. */
    if (reactions[material].dries != 0) {
        const reaction_t *r = &reactions[material];
        int tone = band + (int)rng_below(&s->rng, 3) - 1;
        if (tone < 0) {
            tone = 0;
        } else if (tone >= r->tones) {
            tone = r->tones - 1;
        }
        /* soil_cell(), the table-driven form of CELL_SOIL() - see
         * material.h. Byte-identical for dirt (r->tones ==
         * SOIL_DRY_TONES), and the form that keeps working if a second
         * material ever sets `dries != 0`. */
        return soil_cell(CELL_MAKE(material, 0), (uint8_t)tone, 0, r);
    }
    /* Sand retains top four shades; painted dune avoids window grains.
     * Centred on drifted band. Brushfuls vary slightly, showing layers.
     * Jitter (+/-1 shade) preserves grain texture, not flatness. Single draw
     * remains. */
    const int span = MATERIAL_SHADE_SPAN(material);
    int shade = band + (int)rng_below(&s->rng, 3) - 1;
    if (shade < 0) {
        shade = 0;
    } else if (shade >= span) {
        shade = span - 1;
    }
    return CELL_MAKE(material, (uint8_t)shade);
}

/* GUNPOWDER's `random_cell()` rejects `material_id_t`. GUNPOWDER's ID uses
 * top 5 bits. Its variant is a dry tone, like dirt's branch, with a band +/-
 * 1, clamped. GUNPOWDER's three bits have a narrower span defined in
 * `GUNPOWDER_REACTION.tones`. Constructed via `GUNPOWDER_CELL()`, where
 * `band` is folded into this span using `material_shade_span_cell()`, similar
 * to `sand_spawn_cell()`. */
static cell_t random_gunpowder(sand_t *s, int band)
{
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    int tone = band + (int)rng_below(&s->rng, 3) - 1;
    if (tone < 0) {
        tone = 0;
    } else if (tone >= r->tones) {
        tone = r->tones - 1;
    }
    return GUNPOWDER_CELL((uint8_t)tone);
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
    s->fuse_blast_wait = 0;
    s->fuse_cooldown   = -1;   /* see sand_set_fuse_cooldown() */
    /* THIRD copy; four of five flags reset, may_have_temperature not. Sand_t
     * reused across tests carried flag, hiding brush latching bug. On fresh
     * board, flag starts false, preventing painted snow from waking
     * reactions. */
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
    /* Computed unconditionally: main sweep always walks block-columns (see
     * step_one_row()), requiring real grid-derived block_cols/block_rows,
     * never zero. */
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
    s->boils        = SAND_BOILS_PER_MATERIAL;         /* see sand_set_boils() */
    s->condenses    = SAND_CONDENSES_PER_MATERIAL;     /* see sand_set_condenses() */
    s->lava_cooloff = SAND_LAVA_COOLOFF_DEFAULT; /* see sand_set_lava_cooloff() */
    s->lava_burst   = SAND_LAVA_BURST_DEFAULT;   /* see sand_set_lava_burst() */
    s->acid_rain    = SAND_ACID_RAIN_DEFAULT;    /* see sand_set_acid_rain() */
    s->acid_dilute_mass_bias = SAND_ACID_DILUTE_MASS_BIAS_DEFAULT; /* see
                                          * sand_set_acid_dilute_mass_bias() */
    /* The array itself need not be touched - every reader below goes
     * through emitter_count, so an entry past it is simply never looked
     * at, the same way sand_spawn_cell()'s clipped cells are never looked
     * at rather than being separately zeroed. */
    s->emitter_count = 0;
    sand_clear(s);
}

/* wake_blocks_range()/wake_block_and_neighbors() and mark_rows()/mark_move()
 * are shared with sand_liquid.c and live in sand_priv.h.
 * BLOCK_SETTLED_NEAREST/OTHER/ACTIVE (block_state) also live there. */

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
    /* Any in-flight entry names a cell just wiped, so its stored `cell` byte
     * no longer matches. Dropping them here avoids paying for it later: no
     * stale index survives. */
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
    /* Latched from the finished cell due to variant-dependent flags using
     * sand_set(). Statics written as given, with low nibble as identity.
     * Gunpowder uses random_gunpowder() due to its identity-adjacent bits. */
    const cell_t cell = cell_is_extended(spec)  ? spec
                        : cell_is_gunpowder(spec) ? random_gunpowder(s, band)
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
    /* Once for the whole brushful - see random_cell(). material_shade_span_
     * cell(), not the plain id-only macro, because `spec` may be gunpowder
     * (whose span is 3, read off its own reaction row) rather than a
     * material_id_t CELL_MATERIAL() could safely extract a span for. */
    const int span = material_shade_span_cell(spec);
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

/* Integer floor(sqrt(v)) for exact_disc_count() - Newton's method, converges
 * quickly (v ≤ grid dimension squared, a few hundred thousand). Not the same
 * as tilt.c's isqrt64(), which is static and for int64_t accelerometer
 * readings. A smaller, local version is more efficient. */
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

/* Exact lattice cell count inside a disc radius r; sum of 2*floor(sqrt(r*r -
 * dy*dy)) + 1 for each row dy from -r to r, in O(r) integer square roots. */
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
                               bool guaranteed_dislodge, int ramp);

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

    /* (dx, dy) is the vector from the centre to this cell. Using the same
     * quantiser as gravity gives "away from the centre" in one of eight
     * directions. The centre cell (dx, dy) = (0, 0) is reported as no
     * direction, leaving it unchanged. It spends one unit of `keep` due to a
     * fixed, one-time rounding cost. */
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
     * in suite_sand_dune_blast.c (test_the_sand_dune_scene_throws_grains_
     * beyond_its_own_footprint). Both made the blast markedly WORSE by the
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
                       SAND_EXPLODE_INITIAL_SPEED, true, mat_filter, false,
                       SAND_IMPULSE_SPEED_RAMP);
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
 * just because this function grew the capability somewhere inside it.
 *
 * `ramp` is impulse_t's own per-entry speed decay (see that struct's own
 * comment) - threaded straight through rather than hardcoded so a caller
 * wanting a throw to travel farther (or less far) than SAND_IMPULSE_
 * SPEED_RAMP's own, already-measured figure can ask for that WITHOUT
 * retuning every other caller of this same shared mechanism. sand_
 * impulse() and queue_outward_impulse() (sand_explode()'s own seeding,
 * above) both pass SAND_IMPULSE_SPEED_RAMP unchanged; sand_impulse_
 * dislodge() is the one caller whose own `ramp` parameter can pass
 * something else instead - see its own comment in sand.h for why. */
static void queue_flying_grain(sand_t *s, int x, int y, int dir, int speed,
                               bool allow_dislodge_static, int mat_filter,
                               bool guaranteed_dislodge, int ramp)
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

    /* mat_filter < 0 means "any material", typical for sand_impulse() or
     * unmasked sand_displace(). sand_displace_material() is an exception,
     * preventing a liquid's splash from flinging nearby objects. */
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
        /* `guaranteed_dislodge` bypasses the roll in
         * `sand_impulse_dislodge()`. The density-scaled toughness roll
         * distinguishes stone from sand in explosions.
         * `sand_impulse_dislodge()`'s caller aims to unconditionally move a
         * KIND_STATIC target. See `sand.h` for details (initially due to lava
         * pressure). */
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
     * them are still in flight - the removed lava-vent mechanism
     * (reaction_t.vent_chance, material.h, at a high roll rate - bd
     * esp32c6-0f2) was exactly that: its own covered-cell gate stayed
     * true for as long as any covering cell remained, so its throw could
     * fire again on literally the next step, and without this check every
     * such re-fire would add a FRESH duplicate for every cell still
     * blocked from a PRIOR firing - measured filling the impulse buffer
     * to within a few percent of impulse_max on a 20-pod vent-cap test
     * before this existed, with real entries then silently refused once
     * it filled. A linear scan of the buffer, not a per-cell
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
    entry->ramp  = (uint8_t)ramp;
}

void sand_impulse(sand_t *s, int x, int y, int dir, int speed)
{
    queue_flying_grain(s, x, y, dir, speed, false, -1, false,
                       SAND_IMPULSE_SPEED_RAMP);
}

/* Static-wall version of sand_impulse() with `allow_dislodge_static` true and
 * `guaranteed_dislodge` set. Skips density-scaled toughness roll. `ramp`
 * passed to impulse_t for custom decay. */
void sand_impulse_dislodge(sand_t *s, int x, int y, int dir, int speed,
                           int ramp)
{
    queue_flying_grain(s, x, y, dir, speed, true, -1, true, ramp);
}

/* Shared logic in `sand_displace()` and `sand_displace_material()`. Annulus
 * math, buffer-sharing, and ring-order scan are independent of the material
 * filter. `mat_filter` and `guaranteed_dislodge` are passed to
 * `queue_outward_impulse()` and `queue_flying_grain()`. See comments for
 * details. */
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
     * test_two_overlapping_blasts_share_the_buffer_evenly
     * (suite_sand_impulse.c) for a test that fails immediately if someone
     * does. */
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

/* Like sand_displace(), queues only cells with material `mat_id`; ignores
 * others. For a liquid's splash: water lands hard, throws water, not
 * underlying dirt. */
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
            /* FIREBALL fades with distance, showing a 16-step gradient from
             * dark ember (0x400A00) to bright yellow (0xFFE060) in
             * material.c. Scaled by squared distance, it highlights outer
             * cells for a hot core and rapid cooling edges. The rim fades
             * first, causing inward collapse. Life is floored at 1, with
             * variant 0 indicating an expired fire. */
            int life = MATERIAL_VARIANTS - 1;
            if (core_r2 > 0) {
                life -= (fdx * fdx + fdy * fdy) * SAND_EXPLODE_CORE_FADE / core_r2;
                if (life < 1) {
                    life = 1;
                }
            }
            const cell_t fire = CELL_MAKE(MAT_FIRE, (uint8_t)life);
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

/* Counts grains above, capped. Does NOT use sand_at() as it reports
 * out-of-bounds as occupied, making walls solid. Here, off-grid is open sky,
 * not occupied. */
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

/* driven_by_gravity() - checks if a grain slides in direction (mx, my) given
 * gravity and material's angle of repose, moved to sand_priv.h for use in
 * sand_gas.c. See its comment there for details. */

/* True angle's diagonal lean (0-256). `r` is ratio of smaller to larger
 * component (0-256), 0 on axis, 256 at 45 degrees. Angle position: Rajan's
 * approximation, atan(r/256) / 45deg, 0.3477 * 256 = 89. Accurate within a
 * degree. */
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

void sand_set_boils(sand_t *s, int chance)
{
    if (chance < 0) {
        s->boils = SAND_BOILS_PER_MATERIAL;
    } else {
        s->boils = chance > 255 ? 255 : chance;
    }
}

void sand_set_condenses(sand_t *s, int chance)
{
    if (chance < 0) {
        s->condenses = SAND_CONDENSES_PER_MATERIAL;
    } else {
        s->condenses = chance > 255 ? 255 : chance;
    }
}

void sand_set_lava_cooloff(sand_t *s, int chance)
{
    if (chance < 0) {
        s->lava_cooloff = SAND_LAVA_COOLOFF_DEFAULT;
    } else {
        s->lava_cooloff = chance > 255 ? 255 : chance;
    }
}

void sand_set_fuse_cooldown(sand_t *s, int steps)
{
    s->fuse_cooldown = (steps < 0) ? -1 : (steps > 255 ? 255 : steps);
}

void sand_set_lava_burst(sand_t *s, int chance)
{
    if (chance < 0) {
        s->lava_burst = SAND_LAVA_BURST_DEFAULT;
    } else {
        s->lava_burst = chance > 255 ? 255 : chance;
    }
}

void sand_set_acid_rain(sand_t *s, int chance)
{
    if (chance < 0) {
        s->acid_rain = SAND_ACID_RAIN_DEFAULT;
    } else {
        s->acid_rain = chance > 255 ? 255 : chance;
    }
}

void sand_set_acid_dilute_mass_bias(sand_t *s, int bias)
{
    s->acid_dilute_mass_bias = (bias < 0) ? SAND_ACID_DILUTE_MASS_BIAS_DEFAULT : bias;
}


/* can_enter()/cell_open()/move_to() moved to sand_priv.h (still static
 * inline) - see header comment for grain-movement stack location.
 * pour_into()/room_in() remain in sand_liquid.c; sand.c movement never splits
 * a grain. */


/* Each slide's tilt for hot table rows depends on direction and angle of
 * repose, computed once per step for all 32 rows (MATERIAL_ROWS) using cell
 * >> 3. Reads directly from `materials[]` instead of material_by_id() to
 * include gunpowder's row. TWIN_ROW writes identical repose for ORDINARY
 * materials. */
static void compute_driven(bool driven[MATERIAL_ROWS][2], const int *slide_a,
                           const int *slide_b, int gx, int gy)
{
    for (int m = 0; m < MATERIAL_ROWS; m++) {
        const int repose = materials[m].repose;
        driven[m][0] = driven_by_gravity(slide_a[0], slide_a[1], gx, gy, repose);
        driven[m][1] = driven_by_gravity(slide_b[0], slide_b[1], gx, gy, repose);
    }
}

/* Sweep column order against travel direction for sand_step(). Alternates
 * when gravity is vertical. Outputs step direction, not range. Uses
 * block_x_order() for block-column sweeping. */
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

/* try_scatter()/pick_slide_order()/try_slide_pair() and _impl forms of
 * try_fall_or_scatter()/try_slide() - grain's turn: fall, then slides with
 * friction and shaking. Moved to sand_priv.h (static inline). See header
 * comment for reason and why non-inline calls are defined below. */

static bool step_one_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                           uint8_t *arow, uint8_t *brow, int x, int y, int w,
                           int dx, int dy, const int *slide_a,
                           const int *slide_b, int load_dx, int load_dy,
                           int jostle, bool driven[MATERIAL_ROWS][2])
{
    const cell_t grain = row[x];
    const material_t *mat = material_of(grain);
    if (mat->kind == KIND_STATIC || mat->kind == KIND_GAS) {
        /* Static costs a single comparison. Gas skipped as it moves against
         * the sweep's direction, into unvisited cells, risking teleportation.
         * Handled in sand_gas.c via sand_step_gas() called after this sweep. */
        return false;
    }

    const uint8_t density = mat->density;

    /* Liquids move in amounts, not whole grains. See `move_liquid_grain()` in
     * `sand_liquid.c` and `sand_step_liquids()`. `mat_id` is computed here,
     * not above with `density`. The powder path reads `driven_row`, not
     * `mat_id`. Calculating `mat_id` for every grain would waste resources on
     * unused `CELL_MATERIAL()` calls. */
    if (mat->kind == KIND_LIQUID) {
        const uint8_t mat_id = CELL_MATERIAL(grain);
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

    /* The selected ROW isn't `mat_id`. `try_slide_impl()` uses `driven_row`
     * to index `driven[]` in `pick_slide_order()`, not as `mat_id`. This
     * change affects only MAT_EXTENDED, distinguishing it from gunpowder, due
     * to TWIN_ROW for ORDINARY materials. */
    const uint8_t driven_row = (uint8_t)(grain >> 3);
    return try_slide_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a,
                          slide_b, load_dx, load_dy, jostle, grain, driven_row,
                          density, mat, driven);
}

/* The non-inline forms exist alongside the inline versions (see sand_priv.h
 * comment). sand_gas.c calls these, not the _impl versions, to avoid a second
 * inlined copy. */
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
               int load_dy, int jostle, cell_t grain, uint8_t driven_row,
               uint8_t density, const material_t *mat,
               bool driven[][2])
{
    return try_slide_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a,
                          slide_b, load_dx, load_dy, jostle, grain, driven_row,
                          density, mat, driven);
}

/* Sleeping off when block_state missing. Wakes blocks if grid shaken or
 * settle direction changes. Returns dithered direction. Compares NEAREST
 * direction for sleeping. Clears BLOCK_ACTIVE each step for finalisation. */
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

/* Mirrors sweep_x_order()'s x_from/x_to/x_step based on x_step's sign to
 * align block order with cell order for clarity. */
static void block_x_order(int block_cols, int x_step, int *bx_from,
                          int *bx_to, int *bx_step)
{
    if (x_step > 0) {
        *bx_from = 0;            *bx_to = block_cols; *bx_step = 1;
    } else {
        *bx_from = block_cols - 1; *bx_to = -1;        *bx_step = -1;
    }
}

/* step_one_block() parameters bundled into a struct for efficiency. Reduced
 * call arguments to two (this and bx) to avoid register overflow. Flat list
 * caused performance regression on RISC-V hardware with 8 registers. */
typedef struct {
    sand_t     *s;
    uint8_t    *row, *prow, *arow, *brow;
    int         y, w, dx, dy, x_step;
    const int  *slide_a, *slide_b;
    int         load_dx, load_dy, jostle;
    int         by;
    /* Materials are liquid as a bitmask over the nibble, similar to
     * sand_liquid.c's liquid_mask(): the sweep checks if a cell is liquid to
     * maintain BLOCK_HAS_LIQUID, using a shift-and-mask on a register for
     * efficiency. */
    uint16_t    is_liquid;
    bool      (*driven)[2];
} sweep_ctx_t;

/* Marks BLOCK_ACTIVE if anything moves in a block's x-span within a row, for
 * compute_settled_bit()'s later finalisation pass; does nothing if
 * block_state is disabled. */
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
        /* Accumulated in a register and stored once per block, same shape as
         * moved_here. BLOCK_HAS_LIQUID keeps it true at O(blocks) per step
         * instead of O(moves). Docs/Sand/Performance-Tuning-Attempts.md ninth
         * attempt advises questioning skip structures before implementation. */
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

/* Gravity sweep row, block-column: skip settled blocks. Skipped if
 * settled_bit set, no work needed. When sleeping disabled (block_state NULL),
 * settled_bit 0, no skips, same as cell-by-cell walk. */
static void step_one_row(sand_t *s, int y, int w, int dx, int dy,
                         const int *slide_a, const int *slide_b, int x_step,
                         int load_dx, int load_dy, int jostle,
                         uint8_t settled_bit, uint16_t is_liquid,
                         bool driven[MATERIAL_ROWS][2])
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

/* Finalise a step's settling: a block earns the settled bit if no
 * BLOCK_ACTIVE marks exist in the step or its neighbours. This is deferred
 * per block, not row, as blocks span SAND_BLOCK_H rows and require full
 * sweeping to check movement. */
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
 * can_enter()'s own callers.
 *
 * A FLYING LIQUID IS THE ONE EXCEPTION TO "ANY OCCUPIED, NON-STATIC CELL" -
 * added after measurement showed the powder-free case above was not the
 * whole story. Reported bug: pouring water over a settled dirt bed made the
 * submerged dirt "move a lot". The obvious suspect - splash_displace()'s
 * ring kick (sand_liquid.c) - was measured directly (a 40-row dirt bed under
 * 0/2/8 rows of water, poured on for 600 steps, seed 0xC0FFEE) and hit a
 * dirt cell 60, 2 and 0 times out of ~7,500 firings: not the cause. The
 * actual cause was this function - every water grain the splash throws
 * flies through can_impulse_enter()'s blanket "any non-static occupant"
 * rule above and swaps with whatever dirt sits in its path, which is not
 * settling, it is impulse tunnelling through a powder bed one dirt-water
 * pair at a time. Same scene: impulse-driven dirt-cell changes measured
 * 1207 / 704 / 281 across the three pool depths. Refusing the swap when
 * `mover` is KIND_LIQUID and `target` is occupied by anything other than
 * another KIND_LIQUID dropped that to 0 / 0 / 0 - every last bit of the
 * churn, because ordinary movement already refuses this exact swap
 * (can_enter(), sand_priv.h: water's density 30 is well under dirt's 62).
 * Liquid-into-liquid is still open, so water splashing into oil, acid or
 * lava keeps working exactly as it does today.
 *
 * SCOPED TO KIND_LIQUID MOVERS, NOT A GENERAL DENSITY GATE, and that is a
 * deliberate narrowing, not laziness. The principled generalisation is to
 * make this whole function defer to can_enter()'s own density comparison
 * for every kind, and it measures identically in the scene above - but it
 * would also reach into how an explosion behaves inside a powder bank,
 * where sand (60) and dirt (62) sit almost tied, and that mechanic already
 * carries its own extensively swept tuning (SAND_IMPULSE_SPEED_RAMP,
 * SAND_EXPLODE_CORE_DIVISOR in sand.h). Changing what a thrown powder grain
 * can shoulder aside deserves its own round with device evidence, not a
 * rider on a liquid-only fix. Considered, and deliberately deferred.
 *
 * A KIND_STATIC mover (a thrown chunk) is untouched by any of this - the
 * check below only ever fires for a KIND_LIQUID `mover`, so a flying chunk
 * of stone or glass keeps displacing exactly what it always could. */
static inline bool can_impulse_enter(cell_t target, cell_t mover)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }
    const material_t *t = material_of(target);
    if (t->kind == KIND_STATIC) {
        return false;
    }
    return material_of(mover)->kind != KIND_LIQUID || t->kind == KIND_LIQUID;
}

/* SHARED PREDICATE in `step_impulses()` uses speed gating in
 * `can_impulse_enter()`. Fixes gravity-drift and settled check.
 * `impulse_gravity_candidates()` applies. At or above
 * `SAND_IMPULSE_SINK_MIN_SPEED`, drag stops sideways but not downward. Below,
 * SPENT only enters empty cells. */
static inline bool can_impulse_enter_gravity_ward(cell_t target, cell_t mover,
                                                  uint8_t speed)
{
    if (speed < SAND_IMPULSE_SINK_MIN_SPEED) {
        /* Packed grain holds chunks near the rim; fluids part around solids.
         * Exhausted chunks check the medium; only powder and walls stop them. */
        return CELL_IS_EMPTY(target) ||
               material_of(target)->kind == KIND_LIQUID;
    }
    return can_impulse_enter(target, mover);
}

/* Fills `cand` with three cells gravity-ward of (x, y) in order: straight
 * down, then diagonals. Uses same geometry and order as `step_one_grain()`.
 * Shared by gravity-drift and settled check in `step_impulses()` to avoid
 * divergent candidate lists. */
static void impulse_gravity_candidates(int x, int y, int dx, int dy,
                                       int cand[3][2])
{
    const int i_dir = ring_of(dx, dy);
    const int *slide_a = ring_dir(i_dir + 7);
    const int *slide_b = ring_dir(i_dir + 1);
    cand[0][0] = x + dx;          cand[0][1] = y + dy;
    cand[1][0] = x + slide_a[0];  cand[1][1] = y + slide_a[1];
    cand[2][0] = x + slide_b[0];  cand[2][1] = y + slide_b[1];
}

/* `index` occupied by TRACKED impulse entry during step_impulses() loop; rare
 * case in three-way gate check. O(entries) against buffer max 2048
 * (APP_IMPULSE_MAX). Skipped range [kept, self_i) is scratch from emptied
 * entries. */
static bool impulse_index_still_tracked(const sand_t *s, int kept, int self_i,
                                        uint16_t index)
{
    for (int j = 0; j < kept; j++) {
        if (s->impulse_buf[j].index == index) {
            return true;
        }
    }
    for (int j = self_i + 1; j < s->impulse_count; j++) {
        if (s->impulse_buf[j].index == index) {
            return true;
        }
    }
    return false;
}

/* Checks gravity on (x, y). `step_impulses()` calls twice for KIND_STATIC and
 * KIND_POWDER to check airborne status. Both calls wrote the same
 * three-candidate scan, causing divergence. `cand_out` is always filled.
 * KIND_STATIC uses `cand_out[0]` post-false to differentiate wall from
 * support; KIND_POWDER ignores it. */
static bool impulse_has_opening(const sand_t *s, int x, int y, int dx, int dy,
                                cell_t mover, uint8_t speed, int cand_out[3][2])
{
    impulse_gravity_candidates(x, y, dx, dy, cand_out);
    for (int c = 0; c < 3; c++) {
        if (can_impulse_enter_gravity_ward(
                sand_at(s, cand_out[c][0], cand_out[c][1]), mover, speed)) {
            return true;
        }
    }
    return false;
}

/* ONE RAMP CHARGE, FOR `cells` CELLS OF IT - the single mechanism behind
 * what used to be two separately-written branches in step_impulses() (an
 * adversarial architecture review, bd esp32c6-w2h, named this as the same
 * duplication the decay logic had already grown once): a "per-step" charge,
 * unconditional and paid before any hop was even attempted, and a "per-
 * extra-cell" charge, paid once per cell beyond the first AFTER the whole
 * hop loop finished. Both were the exact same schedule - MAT_WATER/MAT_ACID
 * geometric (SAND_SPLASH_SPEED_DECAY_SHIFT), everything else linear
 * (entry->ramp) - written out twice because one was charged before the loop
 * and the other after it, not because the RULE itself ever differed.
 *
 * THIS FUNCTION IS THE ONE RULE; THE TWO CALL SITES ARE UNCHANGED IN
 * NUMBER, ONLY IN BODY. step_impulses() still charges the first cell's
 * ramp unconditionally, before push_count is even computed - that call
 * site stays where it always was because push_count's own formula has
 * always been measured against the speed left AFTER that one charge (see
 * its own comment), and moving it would quietly hand push_count a
 * different number to work from, a second consequence nobody asked this
 * fix to have. It still charges every cell BEYOND the first inside the hop
 * loop, next to drag, one call per extra cell - the part that actually
 * moved from a batched pass after the whole loop to a per-cell one inside
 * it. Every entry pays exactly the same TOTAL either way a step could have
 * gone: `cells` lets both call sites share one body instead of each
 * re-stating the water/acid-vs-everything-else branch. */
static void impulse_decay(impulse_t *entry, uint8_t mat_id, int cells)
{
    if (mat_id == MAT_WATER || mat_id == MAT_ACID) {
        for (int i = 0; i < cells; i++) {
            entry->speed = (uint8_t)(entry->speed -
                                     (entry->speed >> SAND_SPLASH_SPEED_DECAY_SHIFT));
        }
        return;
    }
    const unsigned total = (unsigned)entry->ramp * (unsigned)cells;
    entry->speed = (entry->speed > total) ? (uint8_t)(entry->speed - total) : 0;
}

/* DISPLACE THE MOVER INTO (the cell at) `new_index` - charge what that
 * costs, then actually do it: swap, latch, mark, and update `entry->index`.
 * Re-cut this way (bd esp32c6-w2h, an adversarial architecture review) from
 * an earlier version that only charged drag and transfer and left an
 * identical 8-line swap/latch/mark/index block duplicated at both of this
 * function's two call sites below - a caller-visible ordering rule
 * (charge, THEN swap, in that order) that this function can simply enforce
 * itself instead of trusting two call sites to keep restating it the same
 * way. Also derives what it can from `entry` and `s` rather than taking it
 * as a parameter: `mat_id` is CELL_MATERIAL(entry->cell) and `impact_speed`
 * is `entry->speed` at the moment this function is entered, both always
 * true at every call site there ever was, so a parameter for either only
 * existed so a caller could hand back a fact this function already has
 * access to.
 *
 * CHARGED THE SAME WAY REGARDLESS OF WHICH OF step_impulses()'s TWO
 * DISPLACING MOVES ACTUALLY MOVED THE MOVER THIS STEP - the push move at
 * the bottom of that loop, or the "AIRBORNE SOLIDS FALL TOO" gravity-drift
 * a few dozen lines above it, which is what keeps a KIND_STATIC entry
 * falling once its own push has spent itself. Only the push move used to
 * charge either. The drift's own swap ran for free: no drag to slow it, no
 * transfer to show for it - which is exactly the asymmetry a slow-arriving
 * chunk exploited. A chunk struck near the blast is still fast enough for
 * its push-roll (rolled_move, that loop's own comment) to succeed most
 * turns, so it mostly takes the push path and pays for what it hits; a
 * chunk that has flown a long way arrives with `speed` mostly ramped away,
 * its push-roll mostly failing, so nearly every displacement IT makes is
 * the drift's own free swap - it tunnels through a bank in complete
 * silence, burying itself no matter how the drag constants are tuned, and
 * never throwing a single transfer entry however far it travelled to get
 * there. A displacement is a displacement either way - the medium being
 * displaced does not know or care which of the loop's two rules is what
 * moved the mover into it, so neither should the bookkeeping.
 *
 * ONE BODY, NOT TWO CALL SITES ANSWERING "WHAT DOES DISPLACING A CELL
 * COST" TWO DIFFERENT WAYS - this file already shipped one bug from
 * exactly that shape once, for a different question ("is this entry still
 * airborne" - see impulse_gravity_candidates()'s and
 * can_impulse_enter_gravity_ward()'s own comments for that history); the
 * lesson is the same one. A future retune of drag or the transfer floor
 * changes this function once, not the drift site and the push site
 * separately, free to drift apart again exactly as they already did.
 *
 * `impact_speed` is the speed `entry` ARRIVED at this displacement WITH -
 * the "what it LOST, not what it has LEFT" rule the push site's own
 * history already settled (momentum handed to the medium is what the
 * mover lost, so deriving the transfer from what is left after paying drag
 * gets the relationship backwards - measured, at powder's present cost, as
 * a full-speed impactor keeping 5 of 253, which would starve the transfer
 * to nothing were it read after drag instead of before). Capturing it as
 * the very first thing this function does, before drag touches
 * `entry->speed`, is what makes that true regardless of caller.
 *
 * `dir_for_transfer` is the direction THIS displacement actually happened
 * in - the push site's own `entry.dir` unchanged, or, at the drift site,
 * whichever of the three gravity-ward candidates the fall actually took.
 * NOT always entry.dir: a chunk moved by the drift can still be labelled
 * with a `dir` it never displaced anything along this step, and the
 * transfer's backward cone (dir_for_transfer + 3/4/5, below - see the
 * ordinary TRANSFER history for why backward: pushing struck material
 * further along the direction it was just hit from only ever shoves it
 * deeper into whatever bank it is already part of) has to spray out of the
 * surface the chunk actually broke through, not out of whatever heading it
 * happens to still carry. Still a parameter, unlike `mat_id` and
 * `impact_speed` above: unlike those two, it is NOT always derivable from
 * `entry` alone, so a caller still has to say it.
 *
 * Gated twice, same as the ordinary push-site transfer always was: the
 * floor (SAND_IMPULSE_TRANSFER_MIN_SPEED) keeps a nearly-spent mover from
 * queuing a transfer too faint to ever move, and `deferred_transfer_count`'s
 * own cap (SAND_CASCADE_TRANSFER_MAX_PER_STEP - see SAND_CASCADE_MAX_
 * PER_STEP's own comment in sand.h for why TRANSFER now has its own share
 * of that budget rather than a counter shared with the cascade relay) is
 * what actually protects impulse_buf from a long plow queuing one transfer
 * per cell touched.
 *
 * The swap, latch, mark and `entry->index` update at the bottom run
 * UNCONDITIONALLY, even when nothing was displaced (an empty `new_index`)
 * or the mover is a kind drag/transfer never charges (KIND_LIQUID) - only
 * the charging above is conditional. That is deliberate, not incidental: a
 * KIND_STATIC chunk should come to rest within the first few layers of a
 * packed bank whether it arrived falling or pushing, the same "close to
 * the rim, not buried in it" shape the push site's own drag already
 * produced - see SAND_IMPULSE_DRAG_POWDER_SHIFT's own comment in sand.h
 * for the measured figure that shape rests on. */
static void impulse_charge_displacement(sand_t *s, impulse_t *entry,
                                        size_t new_index, int dir_for_transfer,
                                        impulse_t *deferred,
                                        int *deferred_transfer_count)
{
    const int w = s->w;
    const size_t old_index = entry->index;
    const cell_t displaced = s->cells[new_index];
    const uint8_t impact_speed = entry->speed;

    if (!CELL_IS_EMPTY(displaced) &&
        (material_of(entry->cell)->kind == KIND_STATIC ||
         material_of(entry->cell)->kind == KIND_POWDER)) {
        const uint8_t drag = impulse_drag_of(displaced);
        entry->speed = (entry->speed > drag) ? (uint8_t)(entry->speed - drag)
                                              : 0;

        if (impact_speed >= SAND_IMPULSE_TRANSFER_MIN_SPEED &&
            *deferred_transfer_count < SAND_CASCADE_TRANSFER_MAX_PER_STEP) {
            /* Throw only cells with open air; surface cells dominate the
             * budget. Random scan avoids sheet-like ejection. sand_at()
             * ensures no out-of-world cells. */
            const int ex = (int)((unsigned)old_index % (unsigned)w);
            const int ey = (int)((unsigned)old_index / (unsigned)w);
            const int base = dir_for_transfer + 3;
            const unsigned first = rng_below(&s->rng, 3);
            int chosen = -1;

            for (unsigned k = 0; k < 3u && chosen < 0; k++) {
                const int cand = (base + (int)((first + k) % 3u)) & 7;
                const int *cd = ring_dir(cand);
                if (CELL_IS_EMPTY(sand_at(s, ex + cd[0], ey + cd[1]))) {
                    chosen = cand;
                }
            }
            if (chosen >= 0) {
                impulse_t *t = &deferred[(*deferred_transfer_count)++];
                t->index = (uint16_t)old_index;
                t->cell  = displaced;
                t->dir   = (uint8_t)chosen;
                t->speed = (uint8_t)(((unsigned)impact_speed *
                                      SAND_IMPULSE_TRANSFER_KEEP) >> 8);
            }
            /* chosen < 0 means buried - no surface to leave by - so nothing
             * is queued, but the displacement below still happens: burial
             * is a fact about the EJECTA, not about whether the mover
             * itself still moves into the cell it just paid drag for. */
        }
    }

    s->cells[new_index] = entry->cell;
    s->cells[old_index] = displaced;
    latch_content_flags(s, entry->cell);
    /* Displaced occupant, if any, existed at `new_index`. Bookkeeping costs
     * one more latch regardless of occupancy, skipped only when nothing to
     * displace. */
    if (!CELL_IS_EMPTY(displaced)) {
        latch_content_flags(s, displaced);
    }
    mark_move(s, (int)((unsigned)old_index % (unsigned)w),
             (int)((unsigned)old_index / (unsigned)w),
             (int)((unsigned)new_index % (unsigned)w),
             (int)((unsigned)new_index / (unsigned)w));
    entry->index = (uint16_t)new_index;
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

    /* DEFERRED follow-up entries, collected here and appended only AFTER
     * the main loop below finishes - see SAND_CASCADE_SPEED_DIVISOR's own
     * comment in sand.h for why. Queuing into s->impulse_buf mid-loop would
     * grow s->impulse_count while this same loop's own bound (`i <
     * s->impulse_count`) is still reading it, so a freshly-queued entry
     * could be revisited in this SAME pass - an unbounded same-step chain,
     * not the one-hop-per-step ripple every caller below is meant to be.
     * Collecting here and queuing once the loop's own compaction (`kept`)
     * has already finished keeps every one of them exactly one step behind
     * the hop that caused it.
     *
     * NAMED FOR WHAT IT IS, NOT FOR ITS FIRST CALLER - this used to be
     * `cascade`, back when a water/acid relay (below) was the only thing
     * that ever landed in it; the same overloading trap impulse_t's own
     * comment describes for the old `blast_t` name. It now also carries
     * TRANSFER entries (a struck cell picking up impulse of its own from
     * whatever just displaced it, KIND_STATIC/KIND_POWDER movers only,
     * inside impulse_charge_displacement() above) - a second, unrelated
     * reason to defer a follow-up queue by exactly the same one step,
     * sharing this one array (SAND_CASCADE_MAX_PER_STEP cells - left named
     * for the mechanism that motivated the array's own size, not renamed
     * just because a second caller now shares it) rather than growing a
     * twin of this whole mechanism for a second kind of follow-up.
     *
     * TWO COUNTERS, ONE ARRAY - see SAND_CASCADE_MAX_PER_STEP's own comment
     * in sand.h for why a single shared count let one mechanism starve the
     * other by queue order alone. `deferred_transfer_count` is gated
     * against SAND_CASCADE_TRANSFER_MAX_PER_STEP and writes from the front
     * of `deferred`; `deferred_cascade_count`, just below the main loop, is
     * gated against whatever transfer has left unused, and writes from the
     * back - two disjoint partitions of the one array, each mechanism's
     * own cap enforced independently of how much of its half the other one
     * happened to use this same step. */
    impulse_t deferred[SAND_CASCADE_MAX_PER_STEP];
    int deferred_transfer_count = 0;
    int deferred_cascade_count = 0;

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

            /* HEAT-RAMPING MATERIALS FIRST, CHECK POSITION ONLY. VARIANT
             * NIBBLE DRIFTS NEAR HEAT. BLOCKED ENTRIES DON'T MOVE, CAUSING
             * MISMATCH. TRYING MOVEMENT FIRST MAY CAUSE FALSE POSITIVES,
             * ESPECIALLY WITH COVERED LAVA (bd esp32c6-mqt,
             * sand_reactions.c). */
            const uint8_t lost_mat = CELL_MATERIAL(entry.cell);
            /* reaction_of(entry.cell), not reactions[lost_mat] - lost_mat is
             * high nibble shared by statics and gunpowder in MAT_EXTENDED
             * range; reaction_of() handles this, while reactions[lost_mat]
             * incorrectly reads reactions[MAT_EXTENDED]. */
            if (reaction_of(entry.cell)->heat_ramp != 0) {
                const cell_t here = s->cells[entry.index];
                if (!CELL_IS_EMPTY(here) && CELL_MATERIAL(here) == lost_mat) {
                    entry.cell = here;
                    reacquired = true;
                }
            }

            const int ox = (int)((unsigned)entry.index % (unsigned)w);
            const int oy = (int)((unsigned)entry.index / (unsigned)w);
            /* THE SAME THREE CANDIDATES impulse_gravity_candidates() (above)
             * already builds for the gravity-drift move and the settled
             * check further down this loop - hand-rolled here before an
             * adversarial review (bd esp32c6-w2h) pointed out this was the
             * exact duplication that helper exists to prevent. */
            int cand[3][2];
            impulse_gravity_candidates(ox, oy, dx, dy, cand);

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
        if (material_of(entry.cell)->kind == KIND_STATIC) {
            const int gx = (int)((unsigned)entry.index % (unsigned)w);
            const int gy = (int)((unsigned)entry.index / (unsigned)w);
            int gcand[3][2];
            impulse_gravity_candidates(gx, gy, dx, dy, gcand);
            /* Direction each candidate is: straight down, then two diagonals.
             * Matches order in impulse_gravity_candidates(). gcand_dir[c] is
             * needed for impulse_charge_displacement()'s transfer cone, not
             * just entry.dir. */
            const int i_dir = ring_of(dx, dy);
            const int gcand_dir[3] = { i_dir, (i_dir + 7) & 7, (i_dir + 1) & 7 };
            for (int c = 0; c < 3; c++) {
                const int cx = gcand[c][0];
                const int cy = gcand[c][1];
                if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                    continue;
                }
                /* can_impulse_enter_gravity_ward(), THE SAME PREDICATE THE
                 * SETTLED CHECK BELOW USES (see that function's own comment)
                 * - no more separate "but not liquid" exclusion here. That
                 * exclusion used to read as protecting lava from being
                 * overwritten ("a burning LIQUID is never smothered",
                 * step_one_burning_cell()'s own invariant), but this move is
                 * a SWAP (four lines down: gdisplaced is read out of the
                 * target cell, then written back into the entry's OLD
                 * cell), not an overwrite - the same move_to() swap trick
                 * can_impulse_enter()'s own comment already relies on for
                 * every other kind. A lava cell a thrown chunk swaps into
                 * does not stop existing, it changes which cell it occupies
                 * - conservation holds exactly, and so does the "never
                 * smothered" invariant, since lava is never replaced with
                 * something else, only relocated by one cell. See
                 * test_a_thrown_static_chunk_conserves_lava_mass_on_sink
                 * (suite_sand_impulse.c), which asserts total lava mass
                 * unchanged and no lava cell deleted across a chunk sinking
                 * through a pool. Dropping the exclusion is a deliberate
                 * design choice, not an oversight: a thrown, ENERGETIC chunk sinks
                 * into a liquid (including lava) the same way a dense
                 * powder already does under ordinary movement (lava's
                 * density is 45 against sand's 60 and dirt's 62) - and
                 * "ENERGETIC" is no longer just descriptive, it is exactly
                 * what can_impulse_enter_gravity_ward() asks: below
                 * SAND_IMPULSE_SINK_MIN_SPEED a SPENT chunk gets none of
                 * this, only a genuinely empty cell will do (that constant's
                 * own comment in sand.h has the device feedback and the
                 * known liquid trade-off this narrowing accepts). An
                 * ordinary, non-impulse KIND_STATIC cell earns none of
                 * this either way: it never reaches this loop at all unless
                 * it is being tracked as a flying entry, and the main sweep
                 * skips KIND_STATIC outright, so a static cell with no
                 * impulse behind it still just sits exactly where it was
                 * placed - it is IMPULSE that earns the sinking, not being
                 * a solid (see
                 * test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder). */
                const cell_t gtarget = sand_at(s, cx, cy);
                if (!can_impulse_enter_gravity_ward(gtarget, entry.cell,
                                                    entry.speed)) {
                    continue;
                }
                /* A DISPLACEMENT IS A DISPLACEMENT -
                 * impulse_charge_displacement() handles silent tunnelling:
                 * slow chunks paid nothing, threw nothing. It manages
                 * swap/latch/mark/index-update, not just charge. */
                const size_t gnat = (size_t)cy * (size_t)w + (size_t)cx;
                impulse_charge_displacement(s, &entry, gnat, gcand_dir[c],
                                            deferred, &deferred_transfer_count);
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
         * actual curve instead of a bent line.
         *
         * A DETERMINISTIC, NEVER-ROLLED VARIANT OF THIS WAS TRIED AND
         * REVERTED, for the now-removed lava-vent mechanism's own throw
         * (a fast, deliberately-far-travelling entry.ramp - bd
         * esp32c6-0f2 removed the mechanism itself, this history stays
         * because the reasoning still shapes this ordinary roll): several
         * entries queued in the same firing, already sharing a throw
         * angle, still drifted out of step if each ALSO independently
         * rolled whether this turn is the one it moves - sound reasoning,
         * but a first attempt keying "should this move" off `speed > 0`
         * alone measured a real regression: an entry occupies this loop
         * for its full ~255-step budget even after landing, since
         * nothing ever asks it to check. A second attempt kept the
         * roll gated on genuine support (excluding liquid, since a
         * covering cell starts out resting on the very lava it seals,
         * which is the problem state to escape, not a landing) fixed
         * that regression, but introduced a DIFFERENT one: pieces that
         * refuse to settle while merely near a liquid can end up
         * bouncing in place - thrown up, pulled back down by gravity-
         * drift, refusing to call that a landing, thrown again - for
         * long enough that fewer of them cleared within the same step
         * budget than the plain probabilistic roll already reliably
         * cleared. Left as the ordinary roll for every caller - at speed
         * 255 that is a ~99.6% per-turn chance, close enough to
         * deterministic in practice that a shared angle alone keeps
         * several identically-queued entries moving together far more
         * often than not, without the settle-timing regressions a fully
         * deterministic version measured. */
        const bool rolled_move = rng_chance(&s->rng, entry.speed);

        /* First cell's ramp charges every turn. `speed` ages with roll.
         * `push_count` from distance before `impulse_decay()`. Further cells
         * charge once inside hop loop. Saturating `speed` prevents
         * `rng_chance()` success, dropping entry next turn. No separate "done
         * flying" check needed. */
        impulse_decay(&entry, mat_id, 1);

        /*
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
         * material keeps the original linear ramp unchanged, so this does
         * not touch sand_explode()'s own extensively-measured tuning (see
         * SAND_IMPULSE_SPEED_RAMP's and SAND_EXPLODE_CORE_DIVISOR's
         * comments in sand.h for that history).
         *
         * DID INTERACT WITH THE CASCADE GATE - caught by
         * test_a_cascading_impulse_moves_more_than_one_cell
         * (suite_sand_materials.c)
         * failing the moment this landed: a relayed entry that takes more
         * than one step to roll a move now decays under the cascade's own
         * SAND_CASCADE_MIN_SPEED * SAND_CASCADE_SPEED_DIVISOR gate before
         * it gets the chance, since that per-step decay is this same
         * geometric shift now, not the old linear ramp. Fixed by dropping
         * SAND_CASCADE_MIN_SPEED to 1 (see its own comment) so the gate
         * stops being the thing that cuts a chain short - the roll's own
         * exhaustion is.
         *
         * entry.ramp, NOT A HARDCODED SAND_IMPULSE_SPEED_RAMP - see
         * impulse_t's own comment for why this is now per-entry rather
         * than one constant every caller shares. sand_impulse() and
         * queue_outward_impulse() both queue with entry.ramp ==
         * SAND_IMPULSE_SPEED_RAMP, so this line does exactly what it
         * always did for them; sand_impulse_dislodge() (sand.h) is the
         * one primitive whose caller can queue a different figure
         * instead. */

        /* "OUT OF FLIGHT" USED TO MEAN "SETTLES EXACTLY WHERE IT IS" -
         * dropped from tracking (not re-added to kept, below) the instant
         * a single per-turn roll failed, regardless of whether the entry
         * had actually landed on anything. Harmless for every kind except
         * KIND_STATIC: a liquid or powder entry keeps falling under the
         * ordinary sweep whether or not this loop is still tracking it,
         * so ending the outward push the moment the roll fails costs it
         * nothing it was not about to lose anyway. KIND_STATIC has no
         * such fallback - the "AIRBORNE SOLIDS FALL TOO" gravity-drift
         * just above is the ONLY thing that ever moves it once thrown,
         * and it only runs "while an entry is tracked here at all" (that
         * block's own comment). A roll can fail at essentially any point
         * in the flight - even early, with plenty of `speed` left, this
         * is a per-turn coin flip, not a threshold - so a dislodged wall
         * chunk still hanging over open space, nowhere near anything to
         * rest on, could lose tracking on an ordinary unlucky roll and be
         * left floating there permanently: nothing else in this engine
         * ever revisits a KIND_STATIC cell to ask whether it should
         * still be falling.
         *
         * SUPPORTED, NOT MERELY ROLLED, IS WHAT DECIDES "SETTLED" NOW,
         * for KIND_STATIC specifically - the same gravity-relative
         * "is there something under it" question try_flare()'s own
         * falling check already asks elsewhere in this feature, applied
         * here to the entry's OWN cell rather than a falling grain. Still
         * airborne keeps the entry tracked - re-added to kept exactly as
         * the blocked branch below already does - so next step gets
         * another unconditional gravity-drift attempt AND another chance
         * at the outward-push roll, however small `speed` has decayed to;
         * the roll failing no longer ends the entry's only route to
         * eventually landing. Genuinely supported (or, for every OTHER
         * kind, any roll failure at all) still drops it exactly as before
         * - this only closes the gap for a KIND_STATIC entry that has not
         * actually come to rest. Bounded the same way an ordinary fall
         * already is: the grid has a finite height, and sand_at()'s own
         * off-grid-is-STONE convention means even a chunk thrown off the
         * visible edge reads as supported once it would otherwise fall
         * forever.
         *
         * "SUPPORTED" MUST MEAN THE SAME THING THE DRIFT ABOVE MEANS BY
         * IT, not a cheaper approximation of it - this used to check only
         * CELL_IS_EMPTY() on the single straight-down cell, which
         * disagreed with the gravity-drift block a few dozen lines up the
         * moment that block's own can_impulse_enter() accepted a
         * DIAGONAL opening: the drift would happily slide the chunk
         * sideways-and-down onto open ground, while this check, asked
         * about the very same step, saw the (occupied) straight-down cell
         * and declared the chunk settled anyway - dropping it from
         * tracking one loop before it ever got to take the move the drift
         * had already found for it. impulse_gravity_candidates() plus
         * can_impulse_enter_gravity_ward() is now the ONE predicate both
         * places ask (see that function's own comment), so the two cannot
         * disagree again - see test_an_energetic_static_chunk_over_a_
         * powder_bank_still_sinks_to_the_bottom (suite_sand_impulse.c), which is
         * exactly the diagonal-opening case that regressed.
         *
         * SPENT IS SETTLED TOO, NOW - can_impulse_enter_gravity_ward()
         * folds SAND_IMPULSE_SINK_MIN_SPEED into the very same predicate,
         * so an entry with nothing left only counts a genuinely
         * CELL_IS_EMPTY() candidate as an opening, same as the drift above
         * - see test_a_spent_static_chunk_rests_on_a_powder_bank_instead_
         * of_sinking_forever (suite_sand_impulse.c) for the case this reopens on
         * purpose: a spent chunk over a packed bank now correctly finds no
         * opening on its very first check and settles right there, rather
         * than the drift and this check disagreeing about it a second way.
         *
         * A SUPPORT THAT IS ITSELF IN FLIGHT MEANS WAIT, NOT SETTLE. Even
         * with the disagreement above fixed, "no opening" can still be
         * true for the wrong reason: the straight-down cell may be
         * occupied by ANOTHER tracked KIND_STATIC entry that merely
         * hasn't taken ITS OWN gravity-drift turn yet this step (this
         * loop processes entries in s->impulse_buf order, not bottom-to-
         * top, so a lower chunk can easily still be sitting exactly where
         * it was when this entry's turn comes around). Settling on top of
         * it right now would freeze two flying chunks mid-air, stacked,
         * neither one ever falling again once both are (wrongly) dropped
         * from tracking. So when the drift found no opening AND the
         * blocking cell is KIND_STATIC, one more check asks whether that
         * specific cell is itself still a tracked entry
         * (impulse_index_still_tracked(), just above this function) -
         * O(entries) in the worst case, hence gated behind both of the
         * above (push roll failed is the surrounding `if`, no drift
         * opening and a KIND_STATIC blocker are the two checks right
         * below) so it only ever runs for the rare entry that is both
         * genuinely blocked AND blocked by exactly the one kind of
         * neighbour that can still move out of the way next step. See
         * test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_of_
         * settling_and_both_eventually_land (suite_sand_impulse.c). */
        if (!rolled_move) {
            if (material_of(entry.cell)->kind == KIND_STATIC) {
                const int rx = (int)((unsigned)entry.index % (unsigned)w);
                const int ry = (int)((unsigned)entry.index / (unsigned)w);
                int rcand[3][2];
                if (impulse_has_opening(s, rx, ry, dx, dy, entry.cell,
                                        entry.speed, rcand)) {
                    s->impulse_buf[kept++] = entry;
                    continue;   /* still airborne - keep falling */
                }

                /* No opening on candidates. `entry` is KIND_STATIC. ENERGETIC
                 * entries reject KIND_STATIC movers for KIND_STATIC targets
                 * via `can_impulse_enter()`. SPENT entries use
                 * `can_impulse_enter_gravity_ward()` to reject non-static
                 * rcand[0], ensuring rcand[0] is KIND_STATIC and avoiding
                 * O(entries) scans. Off-grid is excluded first to prevent
                 * synthetic edge cells. */
                const int bx = rcand[0][0];
                const int by = rcand[0][1];
                if ((unsigned)bx < (unsigned)w && (unsigned)by < (unsigned)h) {
                    const cell_t blocker =
                        s->cells[(size_t)by * (size_t)w + (size_t)bx];
                    if (!CELL_IS_EMPTY(blocker) &&
                        material_of(blocker)->kind == KIND_STATIC) {
                        const uint16_t block_index =
                            (uint16_t)((size_t)by * (size_t)w + (size_t)bx);
                        if (impulse_index_still_tracked(s, kept, i,
                                                        block_index)) {
                            s->impulse_buf[kept++] = entry;
                            continue;   /* support is itself still in
                                         * flight - wait, don't settle */
                        }
                    }
                }
            } else if (material_of(entry.cell)->kind == KIND_POWDER &&
                       entry.speed >= SAND_IMPULSE_BOUNCE_MIN_SPEED) {
                /* A THROWN GRAIN GETS THE SAME "STILL AIRBORNE, DON'T
                 * SETTLE" TREATMENT A THROWN CHUNK ALREADY GETS, ABOVE -
                 * this branch used to not exist at all, which meant only a
                 * KIND_STATIC entry survived a failed push-roll; every
                 * other kind (KIND_POWDER very much included) fell straight
                 * through to the plain `continue` below and was DROPPED,
                 * regardless of whether it was still genuinely falling.
                 * The roll's own chance IS entry.speed (this loop's own
                 * comment above), and the ramp erodes it every step, so a
                 * thrown grain's odds of still being tracked collapsed with
                 * `speed` long before its actual energy did - measured (one
                 * ramp, SAND_IMPULSE_SPEED_RAMP): ~62% still tracked at 10
                 * steps, ~16% at 20, ~2% at 30, ~0.1% at 40, while `speed`
                 * at step 30 is still 193 - nearly three times
                 * SAND_IMPULSE_TRANSFER_MIN_SPEED. The grain kept flying
                 * regardless (the ordinary sweep falls it every step
                 * whether or not this loop still has it tracked - see the
                 * "AIRBORNE SOLIDS FALL TOO" comment above for why that
                 * fallback does NOT double as a reason to extend the drift
                 * block itself to KIND_POWDER), but it arrived with no
                 * impulse entry attached, so the TRANSFER block a few dozen
                 * lines below never ran on impact and nothing was ever
                 * ejected - harmless before TRANSFER existed, which is
                 * exactly why only KIND_STATIC was ever checked here, and
                 * not any more.
                 *
                 * SAME PREDICATE, impulse_gravity_candidates() PLUS
                 * can_impulse_enter_gravity_ward() - NOT A SECOND,
                 * HAND-ROLLED "IS IT STILL FALLING" TEST. This file already
                 * shipped one bug from two call sites asking that exact
                 * question two different ways (see
                 * impulse_gravity_candidates()'s own comment for the
                 * history); "has an opening" means the same thing here it
                 * means for a KIND_STATIC chunk just above - gravity has
                 * not yet found this grain anywhere new to go, so it has
                 * not actually landed yet.
                 *
                 * NO "SUPPORT THAT IS ITSELF IN FLIGHT" WAIT, UNLIKE
                 * KIND_STATIC just above - that extra check exists solely
                 * because a thrown chunk's own gravity-drift IS the only
                 * thing in this engine that ever falls it; settling it
                 * early over a support that is about to move out from
                 * under it would freeze it there forever. A powder grain
                 * has no such dependency - the ordinary sweep falls it
                 * every step regardless of this loop, so a false "settled"
                 * here costs nothing but this one entry's own remaining
                 * impulse; the grain itself keeps moving exactly as it
                 * always would have.
                 *
                 * FLOORED AT SAND_IMPULSE_BOUNCE_MIN_SPEED (the `if`
                 * guarding this whole branch, above) - below it a grain can
                 * neither clear the TRANSFER floor
                 * (SAND_IMPULSE_TRANSFER_MIN_SPEED, well above this one)
                 * nor bounce off a wall, so tracking it any further is pure
                 * bookkeeping cost for an entry that can no longer do
                 * anything on impact. Dropping it here, rather than riding
                 * the ramp all the way to zero the way this used to (never)
                 * do, is what keeps this fix from growing impulse_buf's own
                 * average occupancy much beyond what that speed floor
                 * already bounds - see this file's own measurements for the
                 * actual figures. */
                const int rx = (int)((unsigned)entry.index % (unsigned)w);
                const int ry = (int)((unsigned)entry.index / (unsigned)w);
                int rcand[3][2];
                if (impulse_has_opening(s, rx, ry, dx, dy, entry.cell,
                                        entry.speed, rcand)) {
                    s->impulse_buf[kept++] = entry;
                    continue;   /* still airborne - keep tracked; the
                                 * ordinary sweep does the actual falling */
                }
            }
            continue;   /* settled - out of flight for good */
        }

        /* DISTANCE BUDGET - MAX CELLS STEP PUSH CAN COVER. See
         * SAND_IMPULSE_CELLS_PER_STEP_DIVISOR in sand.h. Computed from
         * post-ramp speed. Under divisor, exactly 1 cell. NOT ONLY BUDGET -
         * hop loop also has ENERGY exit. This is hard upper bound, preventing
         * mover from exceeding divisor. */
        const int push_count =
            1 + (int)entry.speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;

        /* Position and direction before this step's cells move - CASCADE
         * block measures against "one step behind where this entry started
         * ITS OWN MOVE THIS STEP", not where multi-cell push ends up. See
         * block's comment for why backward, not forward. */
        const int x0 = (int)((unsigned)entry.index % (unsigned)w);
        const int y0 = (int)((unsigned)entry.index / (unsigned)w);
        const int *d0 = ring_dir(entry.dir);

        /* Counts `push_count` cells moved before budget exhaustion or
         * obstruction, gating CASCADE check like single-cell moves: triggered
         * only if entry moved at least once, not for those stuck immediately. */
        int moved = 0;

        for (int hop = 0; hop < push_count; hop++) {
            const int x = (int)((unsigned)entry.index % (unsigned)w);
            const int y = (int)((unsigned)entry.index / (unsigned)w);
            const int *d = ring_dir(entry.dir);
            const int nx = x + d[0];
            const int ny = y + d[1];

            /* can_impulse_enter() checks if a flying grain can enter a cell,
             * displacing non-static occupants except liquids against
             * non-liquids. STATIC blocks unconditionally. sand_at() handles
             * grid edges as STATIC. */
            const cell_t target = sand_at(s, nx, ny);
            if (!can_impulse_enter(target, entry.cell)) {
                /* Blocked means WAIT, exactly as a single-cell move always
                 * did: STOP MOVING FOR THIS STEP rather than skip ahead to
                 * try a later cell of this step's own budget - the entry
                 * keeps whatever position it already reached this step
                 * (possibly its starting one, if this is the very first
                 * hop), same direction, already-ramped speed and all, so it
                 * gets another turn next step rather than being dropped for
                 * something that may clear a moment later. Reached only for
                 * a true wall now, an excluded target, or the grid edge -
                 * see can_impulse_enter().
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
                 * itself, water and acid only.
                 *
                 * A THROWN CHUNK OR GRAIN REFLECTS TOO, off the blocking
                 * surface's approximate normal rather than a flat 180 - a wall
                 * chunk glancing off a floor should skid onward, not bounce
                 * straight back the way a droplet's flat flip above does.
                 * KIND_POWDER is in for the same reason it is in for drag: a
                 * blast against a wall puts far more sand in the air than it
                 * ever dislodges stone, and every one of those grains used to
                 * stall against the wall until its flight aged out.
                 * blocker_normal() (this file) reads the three-cell arc ahead of
                 * the mover for what it hit, reflect_off_normal() turns that
                 * into a new ring direction, and either returning -1 (no static
                 * neighbour found, or the reflection degenerates) leaves `entry`
                 * untouched below - the plain wait, same as ever.
                 *
                 * FLOORED AT SAND_IMPULSE_BOUNCE_MIN_SPEED and CHARGED
                 * RESTITUTION every time it fires - both mandatory, not polish;
                 * see that constant's own comment in sand.h for the
                 * bounce-in-place pathology an unfloored, undamped version of
                 * this reopens (step_impulses()'s own roll comment above
                 * records the history). Half the speed on a HEAD-ON reflection
                 * (the new direction is exactly opposite the old one - a wall
                 * met square-on), a quarter otherwise (a glancing hit, which
                 * should read as skidding onward, not braking hard). Costing a
                 * head-on bounce more is the physically right shape and the one
                 * that damps the worst case - repeated head-on bounces in a
                 * narrow gap - hardest, while leaving a chunk skidding along a
                 * wall mostly intact. */
                if (mat_id == MAT_WATER || mat_id == MAT_ACID) {
                    entry.dir = (entry.dir + 4) & 7;
                } else if ((material_of(entry.cell)->kind == KIND_STATIC ||
                            material_of(entry.cell)->kind == KIND_POWDER) &&
                           entry.speed >= SAND_IMPULSE_BOUNCE_MIN_SPEED) {
                    const int normal = blocker_normal(s, x, y, entry.dir);
                    const int reflected = (normal < 0)
                                              ? -1
                                              : reflect_off_normal(entry.dir, normal);
                    if (reflected >= 0) {
                        const bool head_on =
                            (reflected == ((entry.dir + 4) & 7));
                        entry.dir = (uint8_t)reflected;
                        entry.speed = head_on
                            ? (uint8_t)(entry.speed >> 1)
                            : (uint8_t)(entry.speed - (entry.speed >> 2));
                    }
                }
                break;
            }

            /* A SWAP, not an overwrite - move_to()'s own trick (sand_priv.h),
             * reused here for the same reason: whatever the mover is displacing
             * (empty, same as always, or now a real occupant) takes the cell
             * the mover is vacating, rather than that occupant's cell being
             * blanked. Conservation needs nothing extra to hold: two cells that
             * both already existed a moment ago simply trade places, so the
             * grand total is untouched whichever of the two cases this was.
             *
             * MEDIUM DRAG AND TRANSFER - EVERY MOVER BUT A LIQUID, both now
             * charged by impulse_charge_displacement() (this file, just above
             * step_impulses()), the one place this loop's OTHER displacing move
             * - the gravity-drift a few dozen lines up - charges the exact same
             * two things for the exact same reason, and which now performs the
             * swap above too - see that function's own comment for the full
             * history (why one shared body, not two sites quietly answering
             * "what does displacing a cell cost" two different ways) and for
             * SAND_IMPULSE_DRAG_POWDER_SHIFT's own comment in sand.h for what
             * the drag number itself means. Charged here, at the move site,
             * rather than folded into impulse_decay()'s own per-cell ramp:
             * open air (nothing displaced) must cost exactly nothing, so a
             * flight through empty space stays byte-for-byte what it always
             * was. Liquids pay no drag either way: they carry their own
             * geometric decay (SAND_SPLASH_SPEED_DECAY_SHIFT) and the
             * splash/cascade feature is tuned around it.
             *
             * CHARGED ONCE PER CELL OF THIS STEP'S TRAVEL, NOT ONCE PER STEP -
             * `impact_speed` is captured fresh inside that call on every hop,
             * after whatever the PREVIOUS cell's own drag already took, so a
             * mover crossing several cells in one step pays for each one
             * exactly as if it had taken one step per cell, the same total
             * cost a multi-step crossing always charged. Momentum handed to
             * the medium is what the mover LOST, so deriving the throw from
             * what it has LEFT gets the relationship exactly backwards: the
             * harder a medium resists, the less it would fling. Every round
             * that strengthened drag was quietly strangling the ejecta, and at
             * powder's present cost a full-speed impactor would keep 5 of 253
             * - under the transfer gate, so a bank would have thrown nothing
             * at all, ever.
             *
             * `entry.dir` IS THE DIRECTION THIS DISPLACEMENT HAPPENED IN, at
             * this site specifically - the push move only ever moves along the
             * mover's own heading, unlike the drift's own swap, which is why
             * the drift site has to work out its own `dir_for_transfer`
             * instead of reusing `entry.dir` (see that function's own comment
             * on the parameter). */
            /* ONLY AN *EXTRA* CELL IS CHARGED HERE, NEXT TO DRAG - hop 0's
             * ramp was already paid before push_count (above) was even
             * computed, the same single charge a blocked-on-hop-0 or never-
             * rolled entry also pays and no more, so charging it again here
             * would double it. Every hop AFTER the first is exactly the
             * "extra cell" this file used to charge in one lump, after the
             * whole loop finished - impulse_decay()'s own comment has the
             * full account of why that lump is now paid per cell, here,
             * instead. */
            if (hop > 0) {
                impulse_decay(&entry, mat_id, 1);
            }

            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            impulse_charge_displacement(s, &entry, nat, entry.dir,
                                        deferred, &deferred_transfer_count);
            moved++;

            /* THE ENERGY EXIT (finding 1, bd esp32c6-w2h) - push_count
             * above is a DISTANCE budget only, fixed for the whole step
             * from the speed the mover carried BEFORE this loop's own drag
             * ever charged. Without this, a chunk that paid nearly all of
             * `speed` to drag on hop 0 still took every hop that budget
             * allowed - the loop's only other exit was the wall-block
             * `break` a few dozen lines up, and packed medium is not a
             * wall. Measured before this existed: a chunk thrown into a
             * dirt bank averaged 3.0 cells of penetration (32 seeds,
             * plow_total_distance(), suite_sand_impulse.c) against the constant's
             * own stated intent of stopping at the rim, roughly one - see
             * test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank for the
             * pin this closes.
             *
             * THE THRESHOLD IS THE SAME DIVISOR THAT SIZED THE BUDGET -
             * SAND_IMPULSE_CELLS_PER_STEP_DIVISOR is, by its own formula
             * (push_count above), exactly "how much speed buys one more
             * cell of reach". Once drag has left less than that, the
             * mover is no longer entitled to another cell by the same
             * arithmetic that granted it this one, so reusing the divisor
             * here needs no second, independently-tuned constant.
             *
             * DISTANCE ALONE STAYS A HARD CAP - an energy-only exit
             * (`while (speed >= DIVISOR)`, no push_count at all) was
             * considered and rejected: in OPEN AIR, nothing is ever
             * displaced, so impulse_charge_displacement() never charges
             * drag and only the plain ramp (2) erodes `speed` each hop - a
             * full-speed (255) entry would then take (255-104)/2 ~= 75
             * hops before this exit ever fired, tunnelling three orders of
             * magnitude further than push_count's own 1-3 cells ever
             * allowed. Both budgets are required: DISTANCE bounds the
             * common case (nothing displaced, or a light medium), ENERGY
             * cuts the loop short the moment a heavy one already has. */
            if (entry.speed < SAND_IMPULSE_CELLS_PER_STEP_DIVISOR) {
                break;
            }
        }

        /* CASCADE - see this array's own top comment for why this only
         * COLLECTS a candidate rather than queuing one directly. WATER
         * and ACID only, matching splash_displace()'s own scope
         * (sand_liquid.c) - this exists to serve that feature, not as a
         * general property of every impulse. Gated on `moved` rather than
         * on whether the loop above ran out of budget or was cut short by a
         * wall: what matters is whether this entry displaced anything at
         * all this step, exactly the question a single-cell move always
         * answered by reaching this point at all.
         *
         * RELAYS BACKWARD, NOT FORWARD - checked one step BEHIND where this
         * entry started THIS STEP's OWN MOVE (x0, y0 - the position it
         * occupied before its first hop above - minus one more step in
         * `d0`, its own direction of travel), not one step past wherever a
         * multi-cell push ended up. The cell AHEAD of a mover is close
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
        /* THE RAMP IS A COST PER CELL OF TRAVEL, NOT PER TICK OF THE CLOCK -
         * the first cell was already charged before push_count was even
         * computed, and every cell beyond it was charged inside the hop
         * loop above, next to drag, one impulse_decay() call per extra cell
         * rather than a second pass in this exact spot. See that function's
         * own comment for why one mechanism now covers both call sites. */

        if (moved > 0 &&
            (mat_id == MAT_WATER || mat_id == MAT_ACID) &&
            entry.speed >= SAND_CASCADE_MIN_SPEED * SAND_CASCADE_SPEED_DIVISOR &&
            deferred_cascade_count <
                SAND_CASCADE_MAX_PER_STEP - deferred_transfer_count) {
            const int rx = x0 - d0[0];
            const int ry = y0 - d0[1];
            if ((unsigned)rx < (unsigned)w && (unsigned)ry < (unsigned)h) {
                const cell_t relay_target =
                    s->cells[(size_t)ry * (size_t)w + (size_t)rx];
                if (!CELL_IS_EMPTY(relay_target) &&
                    CELL_MATERIAL(relay_target) == mat_id) {
                    /* Writes from the BACK of `deferred` - see the array's
                     * own top comment for why this and TRANSFER (which
                     * writes from the front, inside
                     * impulse_charge_displacement()) never collide: each
                     * partition is sized to its own cap, and the two caps
                     * sum to exactly this array's capacity. */
                    const int relay_slot = SAND_CASCADE_MAX_PER_STEP - 1 -
                                           deferred_cascade_count;
                    deferred_cascade_count++;
                    impulse_t *c = &deferred[relay_slot];
                    c->index = (uint16_t)((size_t)ry * (size_t)w + (size_t)rx);
                    c->cell  = relay_target;
                    c->dir   = entry.dir;
                    c->speed = (uint8_t)(entry.speed / SAND_CASCADE_SPEED_DIVISOR);
                }
            }
        }

        s->impulse_buf[kept].index = entry.index;
        s->impulse_buf[kept].cell  = entry.cell;
        s->impulse_buf[kept].dir   = entry.dir;
        s->impulse_buf[kept].speed = entry.speed;
        s->impulse_buf[kept].ramp  = entry.ramp;
        kept++;
    }

    s->impulse_count = kept;

    /* Queued `kept` impulses safely; TRANSFERs in [0,
     * deferred_transfer_count); CASCADEs in last deferred_transfer_count
     * cells. */
    for (int i = 0; i < deferred_transfer_count; i++) {
        sand_impulse(s, (int)((unsigned)deferred[i].index % (unsigned)w),
                    (int)((unsigned)deferred[i].index / (unsigned)w),
                    deferred[i].dir, deferred[i].speed);
    }
    for (int i = 0; i < deferred_cascade_count; i++) {
        const impulse_t *c = &deferred[SAND_CASCADE_MAX_PER_STEP - 1 - i];
        sand_impulse(s, (int)((unsigned)c->index % (unsigned)w),
                    (int)((unsigned)c->index / (unsigned)w),
                    c->dir, c->speed);
    }
}

/* Pinned to a cache-line boundary so this function's own placement stops
 * being a coin flip of whatever unrelated code happens to sit before it in
 * this translation unit. Host bisect evidence (docs/Sand/Perf-Round-Guide.md
 * "Open items", attempt 15's finding A): across six materials-wave commits
 * that each changed sand.c/material.c/sand_reactions.c but never this
 * function's own body, sand_step()'s compiled bytes were IDENTICAL every time
 * - only its absolute address moved, by whatever an unrelated function
 * earlier in the file grew or shrank by - and the two liquid-free control
 * benchmarks (which call only this) swung between two timing bands in
 * lock-step with that address's alignment. Forcing the alignment removes the
 * coin flip: unrelated code elsewhere in this file, or a future insertion
 * before this point, can no longer silently move this function across a
 * cache-line boundary and change its own performance for free. 32, not 64: 32
 * bytes is this chip's own i-cache line, and 64 does not link - it raises
 * .flash.text's section alignment past the 0x...20 start ESP-IDF's linker
 * script pairs with .flash_rodata_dummy, and ld refuses the overlap. The host
 * bisect that motivated this used 64 because that is what collapsed the x86
 * buckets; on this target the line is half that. */
__attribute__((aligned(32)))
void sand_step(sand_t *s, int gx, int gy, int jostle)
{
    /* Emitters act first per step, before gravity, mimicking
     * sand_spawn_cell() calls. This allows new grains to move immediately.
     * Runs unconditionally, even in free fall, ensuring "once per
     * sand_step()". */
    emit_from_emitters(s);

    /* Dithered rather than nearest, so a tilt between two of the eight
     * directions flows at its true angle instead of snapping. Costs one random
     * number per STEP - not per grain - so it is free at this scale. */
    int dx, dy;
    sand_gravity_direction_dithered(s, gx, gy, &dx, &dy);

    /* Load measures directionally, ignoring dithering. Grain weight, a pile
     * property, stays constant despite step rounding. Dithering makes
     * diagonal steps appear empty above vertical columns, treating buried
     * grains as free surface grains roughly one in eight, shifting pile bases
     * sideways. */
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
     * anything else asking which way is down - growth, for one - read
     * (0,0) on any grid without block_state. It is the settled direction
     * of the step just taken, so it is written once the step has decided
     * it. */
    s->last_load_dx = load_dx;
    s->last_load_dy = load_dy;
    s->last_step_dx = dx;
    s->last_step_dy = dy;

    bool driven[MATERIAL_ROWS][2];
    compute_driven(driven, slide_a, slide_b, gx, gy);

    /* Sweep against travel on both axes. Grains move to gravity-ward cells
     * first, ensuring no grain is revisited. Sweeping the other way causes
     * grains to be moved repeatedly, teleporting to the floor in one frame. */
    const int y_from = (dy > 0) ? s->h - 1 : 0;
    const int y_to   = (dy > 0) ? -1       : s->h;
    const int y_step = (dy > 0) ? -1       : 1;

    const int x_step = sweep_x_order(s, dx);

    /* Liquid spreads PERPENDICULAR TO GRAVITY, not across screen. Tilt
     * affects direction. Use nearest, not dithered, for stability. See
     * equalise_liquids() and test_a_settled_pool_does_not_flicker. */
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

    /* Cross-flow for liquids, excluding gravity. See sand_step_liquids() in
     * sand_liquid.c. Runs before finalising block sleep states to ensure
     * BLOCK_ACTIVE reflects entire step. */
    sand_step_liquids(s, &flow, dx, dy);

    /* Rising gas doesn't join main sweep. Order of sand_step_liquids()
     * doesn't matter; both must finish before finalize_settling(). Checked
     * here, not via sand_step_gas()'s early return. Called every step,
     * skipping avoids marshalling nine arguments if no gas. Flash layout
     * cost. */
    if (s->may_have_gas) {
        sand_step_gas(s, gx, gy, dx, dy, slide_a, slide_b, perp_a, perp_b,
                     load_dx, load_dy, x_step, jostle);
    }

    /* Same slot for burning cell reactions; ignition/extinguish/burn-out are
     * not gravity-ward or movement. Must finish before finalize_settling().
     * Takes `s` argument, unlike sand_step_gas(). Boiling now happens at heat
     * source. No cost to dodge by checking may_have_burning, internal check
     * suffices. */
    sand_step_reactions(s);

    /* Final step after others to ensure correct position and arc for thrown
     * grains, adding outward half after gravity. */
    step_impulses(s, dx, dy);

    finalize_settling(s, settled_bit);
}
