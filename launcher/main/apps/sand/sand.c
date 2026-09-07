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

/* Mirrors random_cell() above but keyed off reaction_of()'s tones, not
 * MATERIAL_SHADE_SPAN() - GUNPOWDER's variant space is narrower and
 * comes from GUNPOWDER_REACTION, not the material table. */
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
    /* Resets every content flag, so a reused sand_t cannot carry a stale one
     * into a fresh board and wake reactions it shouldn't. */
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

/* `r2` enforces the true circular disc though the caller loop walks square
 * Chebyshev shells (shell order picks the SEQUENCE, this decides
 * MEMBERSHIP). `disc_count`/`keep`/`*accum` self-limit via a DDA, not a
 * modulo stride (aliases with the ring's own edge lengths), degrading
 * DENSITY evenly instead of truncating the SHAPE. */
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

    /* DELIBERATELY - a distance-scaled falloff worsened the blast when
     * tried: cells that escape the footprint are disproportionately near
     * the outer radius, with least distance left to travel, and falloff
     * there guts that evidence. Flat speed stays until a falloff is found
     * that doesn't trade the outer annulus for a hotter core. */

    /* true HERE, AND ONLY HERE - the one caller that turns a wall's
     * KIND_STATIC refusal from a certainty into a density-scaled chance
     * (see queue_flying_grain()). Everything else about the dislodged
     * cell's flight is identical to any other entry. */
    queue_flying_grain(s, cx + dx, cy + dy, ring_of(qdx, qdy),
                       SAND_EXPLODE_INITIAL_SPEED, true, mat_filter, false,
                       SAND_IMPULSE_SPEED_RAMP);
}

/* SHARED IMPLEMENTATION behind sand_impulse() and sand_explode()'s own
 * annulus seeding, so the bounds/empty/buffer-full checks stay in one
 * place. `allow_dislodge_static` and `ramp` (speed decay) differ per
 * caller. */
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

    /* WALL CANNOT BE THROWN BY DEFAULT, any more than one can be entered -
     * can_impulse_enter() gates the DESTINATION, this gates the SOURCE.
     * `allow_dislodge_static` uses `255 - density` for chance: LOWER
     * density means HIGHER chance. */
    if (material_of(cell)->kind == KIND_STATIC) {
        if (!allow_dislodge_static) {
            return;
        }
        /* `guaranteed_dislodge` bypasses the roll above, for
         * sand_impulse_dislodge()'s caller, which wants a KIND_STATIC
         * target moved unconditionally rather than toughness-scaled. */
        if (!guaranteed_dislodge) {
            const int chance = 255 - (int)material_of(cell)->density;
            if ((int)(rng_next(&s->rng) & 0xFF) >= chance) {
                return;   /* the roll failed - the wall holds, same as always */
            }
        }
    }

    /* REFUSE A SECOND ENTRY FOR A CELL ALREADY TRACKED - two entries both
     * claiming to be the one grain at `at` is a bookkeeping error. Matched
     * on `cell` too, not index alone: queuing happens during reactions,
     * before step_impulses()'s own re-acquisition runs, so a stored index
     * can be stale here. */
    for (int existing = 0; existing < s->impulse_count; existing++) {
        if (s->impulse_buf[existing].index == (uint16_t)at &&
            s->impulse_buf[existing].cell == cell) {
            return;
        }
    }

    /* A pane knocked loose is a pane broken: it flies on as cullet,
     * carrying the push that dislodged it rather than sailing off as an
     * intact sheet. Written to the grid as well as the entry - flight
     * matches `cell` against what is actually there before moving it. */
    cell_t flying = cell;
    if (CELL_MATERIAL(cell) == MAT_GLASS) {
        flying = cullet_cell(s);
        s->cells[at] = flying;
        latch_content_flags(s, flying);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
    }

    impulse_t *entry = &s->impulse_buf[s->impulse_count++];
    entry->index = (uint16_t)at;
    entry->cell  = flying;
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

    /* RING ORDER (Chebyshev distance), NOT ROW ORDER: an undersized buffer's
     * cap would otherwise land entirely in the disc's top rows before the
     * scan reaches the core or lower half. The r2 check still enforces the
     * true circular disc regardless of ring - ring order only changes the
     * SEQUENCE cells are offered in, never which cells qualify. */
    const int r2 = radius * radius;

    /* `disc_count` is exact_disc_count()'s own EXACT count, not a safe
     * over-estimate - fine for sizing the buffer itself, but an overshoot
     * would compute `keep` too low for small discs. `keep` is sized
     * against `room`, not `s->impulse_max` - DO NOT simplify: a second
     * displacement mid-arc would otherwise size its density as if the
     * whole buffer were free - see
     * test_two_overlapping_blasts_share_the_buffer_evenly. */
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

    /* FILLS a cavity with fire before queuing any flight entries - see
     * SAND_EXPLODE_CORE_DIVISOR's own comment in sand.h. Every cell in the
     * core is written unconditionally, occupied or empty alike. Written by
     * hand rather than through place_cell() (sand_reactions.c), which is
     * static to that file. */
    const int core_radius_raw = radius / SAND_EXPLODE_CORE_DIVISOR;
    /* Clamped to a minimum core_radius of 1 for radius >= 2 - see
     * SAND_EXPLODE_CORE_DIVISOR's own comment in sand.h. radius 1 is
     * excluded: it stays at core_radius 0, the single centre cell every
     * test already assumes. */
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
            /* Life is floored at 1, variant 0 indicates an expired fire. */
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

/* Via sand_spawn_cell(), never a raw write: `emitters[i].cell` is a
 * placeholder (CELL_MAKE(material, 0)), and for a KIND_LIQUID variant 0
 * is zero mass - a raw write once placed a lava emitter with nothing in
 * it, never rendering or flowing. Radius 0, not a disc, so the source
 * cannot bury itself; sand_spawn_cell() also wakes the block. */
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

    /* See `move_liquid_grain()` in `sand_liquid.c` and `sand_step_liquids()`.
     * `mat_id` is computed here, not above with `density`. Calculating
     * `mat_id` for every grain would waste resources on unused
     * `CELL_MATERIAL()` calls. */
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
 * gravitational potential. See xflow_t. */
static void build_xflow(xflow_t *f, int gx, int gy)
{
    const int ax = im_abs(gx), ay = im_abs(gy);
    const int sx = im_sign(gx), sy = im_sign(gy);

    /* Whichever of gx/gy has the larger magnitude picks the major ray:
     * `ax` runs perpendicular to that axis, `dg` built from the same two
     * signs (not looked up), so it is always the diagonal beside the
     * lean, never the far one. `q_q8` is the raw ratio of the smaller
     * gravity component to the larger (0-256) - the TANGENT of the tilt,
     * deliberately not diagonal_weight()'s angle correction (that answers
     * where the true angle sits between octants for dithering gravity,
     * not the slope wanted here). */
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

    /* im_len()'s ~4% approximation (intmath.h) is fine: nothing reads a
     * bias to that precision, only sign and rough size. A bias of exactly
     * zero when a ray is exactly level still holds - it comes from the
     * dot product cancelling exactly, independent of length. */
    const int len = im_len(gx, gy);
    if (len == 0) {
        f->bias_ax_q8 = 0;
        f->bias_dg_q8 = 0;
        return;
    }
    /* Both biases share one pair of per-axis constants, `bx`/`by`, rather
     * than each worked out separately - that is what keeps the level
     * field consistent: find_shallowest() compares cells reachable by
     * either ray, and a shared (bx, by) guarantees those comparisons
     * agree regardless of which ray got each cell there. */
    const int bx = (MASS_MAX * 256 * gx) / len;
    const int by = (MASS_MAX * 256 * gy) / len;
    f->bias_ax_q8 = bx * f->ax[0] + by * f->ax[1];
    f->bias_dg_q8 = bx * f->dg[0] + by * f->dg[1];
}

/* Whether a FLYING grain may swap into `target` - unlike can_enter(), which
 * requires the mover be DENSER (useless here, a buried grain sits in
 * identical-density material). Density does not gate this. KIND_STATIC IS
 * STILL A WALL, keeping a blast inside a sealed vessel. A FLYING LIQUID
 * mover is refused against anything but another liquid, so a splash cannot
 * tunnel through a powder bed. */
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

/* Both step_impulses() call sites (KIND_STATIC, KIND_POWDER) used to write
 * out the same three-candidate scan by hand, which let them diverge -
 * shared here instead. `cand_out` is always filled; KIND_STATIC reads
 * `cand_out[0]` post-false to tell wall from support, KIND_POWDER ignores
 * it. */
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

/* ONE RAMP CHARGE, for `cells` cells of it - shared by step_impulses()'s
 * two charge sites instead of each re-stating the water/acid-vs-rest
 * branch. MAT_WATER/MAT_ACID decay geometrically; everything else,
 * linearly via entry->ramp. Every entry pays the same total either way a
 * step goes. */
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

/* Shared by push and gravity-drift, charged identically: a free drift
 * swap once let a mostly-spent chunk tunnel through with no drag.
 * `impact_speed` is captured before drag touches `entry->speed` -
 * transfer derives from what was LOST, not what is left. `dir_for_transfer`
 * is not always `entry->dir`: the drift can displace along a heading it
 * never moved through. */
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

/* The flight pass: every entry in s->impulse_buf either moves one cell
 * along its queued direction, waits another turn, or is finally dropped.
 * Called from sand_step(), immediately before finalize_settling() - see
 * docs/Sand/Explosion-Plan.md's "Where the pass runs, and why it must be
 * LAST": running after every pass that can move a cell keeps an entry's
 * position honest, and turns a plain outward push into a ballistic arc
 * for free, since gravity has already pulled by the time this runs. */

/* BLOCKED MEANS WAIT, NOT STOP. A cell in the way used to drop the entry
 * on the spot - fine for open air, wrong for anything packed: an
 * explosion into a bed of sand or water starts with every queued cell
 * surrounded by more of the same material, so that rule dropped nearly
 * everything on its first turn. */

/* Only the annulus already touching open space (or the fire-filled core
 * sand_explode() now writes - see SAND_EXPLODE_CORE_DIVISOR in sand.h -
 * which a denser neighbour can swap straight through) ever went
 * anywhere. */

/* Keeping a blocked entry instead lets it try again next step, once
 * whatever was ahead of it has had a chance to move out of the way -
 * which is what lets the disturbance the core's fire starts unpack
 * outward over several steps instead of being a single frozen ring. */

/* WAIT STILL HAPPENS, BUT ONLY AGAINST A TRUE WALL NOW - see
 * can_impulse_enter()'s own comment just above this function for why a
 * flying grain shoulders aside any non-static occupant it meets instead of
 * only ever moving into a genuinely empty cell. This narrows "blocked" to
 * KIND_STATIC and the grid edge, but the branch is still needed:
 * wait-then-retry keeps an entry pinned rather than dropped the instant
 * it arrives. */

/* `dx`/`dy` is this step's own dithered gravity direction, the same one
 * sand_step()'s main sweep just used - needed for RE-ACQUISITION, below,
 * which is what makes a thrown grain arc at all rather than flying dead
 * straight for exactly one cell. Without it, a stale stored index fails
 * the identity check and the entry is dropped after one hop - lateral
 * scatter out of a crater, not an arc. */

/* The sweep runs BEFORE this pass, every step, on every ordinary cell
 * including ones this list still has an eye on: an airborne grain
 * sitting in open air is not special to the sweep, so gravity moves it
 * down one cell before this pass ever gets a turn on it that step. */

/* A single `if` with nothing queued, which is every step on a board with
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

    /* DEFERRED follow-up entries (CASCADE relays and TRANSFER hits alike),
     * appended only after the main loop finishes: queuing mid-loop would
     * grow s->impulse_count while the loop's own bound still reads it,
     * letting a freshly-queued entry be revisited the same pass. TWO
     * COUNTERS, ONE ARRAY: transfer writes from the front (capped at
     * SAND_CASCADE_TRANSFER_MAX_PER_STEP), cascade from the back (capped
     * at whatever transfer left unused) - neither can starve the other. */
    impulse_t deferred[SAND_CASCADE_MAX_PER_STEP];
    int deferred_transfer_count = 0;
    int deferred_cascade_count = 0;

    for (int i = 0; i < s->impulse_count; i++) {
        impulse_t entry = s->impulse_buf[i];

        /* Verify before moving: nothing marks a cell "spoken for", so
         * gravity or a reaction may have touched it since. RE-ACQUIRE by
         * checking the three cells step_one_grain() could have moved this
         * grain to, for a byte-for-byte match - same material and variant
         * is the same GRAIN anywhere else in this file. Never adopt a
         * DIFFERENT byte. */
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

            /* LIQUIDS GET A SECOND CHANCE, MATCHED ON MATERIAL ONLY:
             * cross-flow moves MASS, not grains, so a cell's variant can
             * change without moving it to one of the three candidates
             * above - measured 44.5% of queued water entries lost to this
             * gap otherwise. Scoped to water/acid; checked at the original
             * cell, then its 8 neighbours. */
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

        /* AIRBORNE SOLIDS FALL TOO - KIND_STATIC never moves in the
         * ordinary sweep, so a thrown chunk never arced like sand or
         * water, which get gravity for free every step besides this
         * loop's own push. While tracked, it also gets one UNCONDITIONAL
         * gravity-ward attempt every step - not rolled, since gating it
         * on `speed` would tie "still falling" to "still has push left",
         * which is backwards. */
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
                const cell_t gtarget = sand_at(s, cx, cy);
                /* can_impulse_enter_gravity_ward() is the same predicate
                 * the settled check below uses. No separate "but not
                 * liquid" exclusion: this move is a SWAP, not an
                 * overwrite, so a lava cell a thrown chunk enters just
                 * relocates - conservation and "never smothered" both
                 * hold. An ENERGETIC chunk sinks into a liquid like a
                 * dense powder already does; below
                 * SAND_IMPULSE_SINK_MIN_SPEED a SPENT one gets none of
                 * this. */
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

        /* The roll happens before the move attempt, EVERY turn, blocked or
         * not: rolling only on a move would let a wedged entry wait
         * FOREVER if its target never opens (a sealed vessel, an
         * undisturbed pile), breaking the one guarantee this design rests
         * on - every entry's lifetime is bounded. The roll's own chance IS
         * entry.speed - see SAND_IMPULSE_SPEED_RAMP for why one byte
         * carries both "chance this turn's move happens" and "how much
         * flight is left". */
        const bool rolled_move = rng_chance(&s->rng, entry.speed);

        /* First cell's ramp charges every turn. `speed` ages with roll.
         * `push_count` from distance before `impulse_decay()`. Further cells
         * charge once inside hop loop. Saturating `speed` prevents
         * `rng_chance()` success, dropping entry next turn. No separate "done
         * flying" check needed. */
        impulse_decay(&entry, mat_id, 1);

        /* WATER AND ACID GET THEIR OWN GEOMETRIC DECAY
         * (SAND_SPLASH_SPEED_DECAY_SHIFT) instead of the linear ramp
         * everything else uses - see that constant's own comment in
         * sand.h. Interacts with the cascade's own gate; see
         * SAND_CASCADE_MIN_SPEED's comment for why that constant is 1. */

        /* Uses entry.ramp, not a hardcoded SAND_IMPULSE_SPEED_RAMP - see
         * impulse_t's own comment for why the decay rate is per-entry;
         * sand_impulse_dislodge() is the one caller that queues a
         * different figure. */

        /* Dropping from tracking (not re-added to `kept` below) the instant
         * a roll fails is harmless for every kind except KIND_STATIC: a
         * liquid or powder entry keeps falling under the ordinary sweep
         * whether or not this loop still tracks it. */

        if (!rolled_move) {
            /* KIND_STATIC has no other fallback, so SUPPORTED, NOT MERELY
             * ROLLED, decides settled for it - the same predicate the
             * drift above uses, so the two can never disagree about an
             * opening (a cheaper CELL_IS_EMPTY()-only check once
             * regressed that way). A SUPPORT ITSELF IN FLIGHT MEANS WAIT,
             * NOT SETTLE: the blocker may be another tracked entry that
             * hasn't drifted yet this step - settling on it would freeze
             * two flying chunks forever. impulse_index_still_tracked()
             * catches that rare case. */
            if (material_of(entry.cell)->kind == KIND_STATIC) {
                const int rx = (int)((unsigned)entry.index % (unsigned)w);
                const int ry = (int)((unsigned)entry.index / (unsigned)w);
                int rcand[3][2];
                if (impulse_has_opening(s, rx, ry, dx, dy, entry.cell,
                                        entry.speed, rcand)) {
                    s->impulse_buf[kept++] = entry;
                    continue;   /* still airborne - keep falling */
                }

                /* Off-grid is excluded first to prevent synthetic edge cells. */
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
            /* A THROWN GRAIN GETS THE SAME "STILL AIRBORNE" TREATMENT A
             * THROWN CHUNK GETS ABOVE - without it, a powder entry dropped
             * the instant one push-roll failed, so TRANSFER never fired on
             * impact. No "support in flight" wait here: the sweep already
             * falls a powder grain regardless. FLOORED AT
             * SAND_IMPULSE_BOUNCE_MIN_SPEED: below it a grain can neither
             * clear the transfer floor nor bounce, so tracking further is
             * pure bookkeeping cost. */
            } else if (material_of(entry.cell)->kind == KIND_POWDER &&
                       entry.speed >= SAND_IMPULSE_BOUNCE_MIN_SPEED) {
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
                /* Blocked means WAIT: keeps position, direction and speed
                 * for another try next step, rather than dropping.
                 * WATER/ACID BOUNCE INSTEAD: reversing `dir` turns speed
                 * into rebound, scoped to water/acid, matching
                 * splash_displace(). THROWN CHUNK OR GRAIN REFLECTS TOO,
                 * off surface's normal, floored at
                 * SAND_IMPULSE_BOUNCE_MIN_SPEED and charges restitution. */
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

            /* A SWAP, not an overwrite - move_to()'s trick, so
             * conservation needs nothing extra. Drag and transfer are
             * charged by impulse_charge_displacement(), the same body
             * gravity-drift uses, at the move site so open air costs
             * nothing. `impact_speed` is captured fresh per hop, so a
             * multi-cell move pays as if it took one step per cell. */

            /* ONLY AN *EXTRA* CELL IS CHARGED HERE - hop 0's ramp was
             * already paid before push_count was computed, so charging it
             * again here would double it. */
            if (hop > 0) {
                impulse_decay(&entry, mat_id, 1);
            }

            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            impulse_charge_displacement(s, &entry, nat, entry.dir,
                                        deferred, &deferred_transfer_count);
            moved++;

            /* THE ENERGY EXIT - push_count is a DISTANCE budget only,
             * fixed from speed before this loop's drag charged, so a
             * chunk that paid nearly all its speed to drag on hop 0 would
             * otherwise still take every hop the budget allowed, since
             * packed medium is not a wall. DISTANCE alone still stays a
             * hard cap too: an energy-only exit would let open air tunnel
             * a full-speed entry far past push_count's cells. Both are
             * required. */
            if (entry.speed < SAND_IMPULSE_CELLS_PER_STEP_DIVISOR) {
                break;
            }
        }

        /* RELAYS BACKWARD, NOT FORWARD - the cell ahead is open, so relay
         * material feeding this move from behind. */
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

#if CONFIG_LAUNCHER_SAND_PASS_GATES
/* Declared extern in sand_priv.h, next to the SAND_STEP_GATE()/
 * SAND_STEP_GATED() macros sand_step() below uses them through - see that
 * declaration's own comment for what these buy and why they are safe to
 * leave enabled. Not `static`: those macros expand at sand_step()'s own
 * call sites, in this file, but a probe outside it needs to flip them. */
volatile bool sand_step_gate_main_sweep = true;
volatile bool sand_step_gate_cross_flow = true;
volatile bool sand_step_gate_gas        = true;
volatile bool sand_step_gate_reactions  = true;
#endif

/* Pinned to a cache-line boundary so this function's placement is not a
 * coin flip of whatever unrelated code sits before it: a host bisect found
 * sand_step()'s compiled bytes IDENTICAL across commits that never touched
 * this body, yet performance still swung between two bands in lock-step
 * with the function's own address alignment. 32, not 64: 32 bytes is this
 * chip's actual i-cache line, and 64 does not link here - ld refuses the
 * section overlap it causes with ESP-IDF's linker script. */
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

    SAND_STEP_GATE(main_sweep) {
        for (int y = y_from; y != y_to; y += y_step) {
            step_one_row(s, y, w, dx, dy, slide_a, slide_b, x_step,
                        load_dx, load_dy, jostle, settled_bit, is_liquid, driven);
        }
    }

    /* Cross-flow for liquids, excluding gravity. See sand_step_liquids() in
     * sand_liquid.c. Runs before finalising block sleep states to ensure
     * BLOCK_ACTIVE reflects entire step. */
    SAND_STEP_GATE(cross_flow) {
        sand_step_liquids(s, &flow, dx, dy);
    }

    /* Rising gas doesn't join main sweep. Order of sand_step_liquids()
     * doesn't matter; both must finish before finalize_settling(). Checked
     * here, not via sand_step_gas()'s early return. Called every step,
     * skipping avoids marshalling nine arguments if no gas. Flash layout
     * cost. */
    if (SAND_STEP_GATED(gas, s->may_have_gas)) {
        sand_step_gas(s, gx, gy, dx, dy, slide_a, slide_b, perp_a, perp_b,
                     load_dx, load_dy, x_step, jostle);
    }

    /* Same slot for burning cell reactions; ignition/extinguish/burn-out are
     * not gravity-ward or movement. Must finish before finalize_settling().
     * Takes `s` argument, unlike sand_step_gas(). Boiling now happens at heat
     * source. No cost to dodge by checking may_have_burning, internal check
     * suffices. */
    SAND_STEP_GATE(reactions) {
        sand_step_reactions(s);
    }

    /* Final step after others to ensure correct position and arc for thrown
     * grains, adding outward half after gravity. */
    step_impulses(s, dx, dy);

    finalize_settling(s, settled_bit);
}
