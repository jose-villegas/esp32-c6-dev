/*
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
 */

#include "sand_priv.h"

#include <string.h>

#include "sand_liquid_move.h"
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

/* Grid access */

void sand_init(sand_t *s, uint8_t *cells, int w, int h, uint32_t seed)
{
    s->cells      = cells;
    s->w          = w;
    s->h          = h;
    rng_seed(&s->rng, seed);
    s->pour_phase = 0;
    s->step_phase = 0;
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
    s->soak_convert = SAND_SOAK_CONVERT_PER_MATERIAL;
    s->soak         = 0;    /* nothing soaks unless asked - see
                             * sand_set_soak() */
    s->mobility     = 255;  /* full speed by default - see sand_set_mobility() */
    s->gas_walk     = true;   /* random walk, 23% cheaper - see sand_set_gas_walk()
                                * for the deterministic exhaustive mover */
    s->flammability = SAND_FLAMMABILITY_PER_MATERIAL;  /* see sand_set_flammability() */
    s->conduction   = SAND_CONDUCTION_PER_MATERIAL;    /* see sand_set_conduction() */
    s->boils        = SAND_BOILS_PER_MATERIAL;         /* see sand_set_boils() */
    s->condenses    = SAND_CONDENSES_PER_MATERIAL;     /* see sand_set_condenses() */
    s->lava_cooloff = SAND_LAVA_COOLOFF_DEFAULT; /* see sand_set_lava_cooloff() */
    s->lava_burst   = SAND_LAVA_BURST_DEFAULT;   /* see sand_set_lava_burst() */
    s->crust        = -1;                        /* see sand_set_crust() */
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

/* sand_enable_impulses() moved to sand_impulse.c - it belongs with its own
 * subsystem, not the grid-access group above it. */

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

/* The outward-impulse seeding chain (isqrt_floor() through sand_explode())
 * moved to sand_impulse.c - see that file's own banner for why the whole
 * outward-flight subsystem is one file, separate from this one's
 * gravity-ward sweep. */

/*
 * Emitters - see the `emitters` field of sand_t and the EMITTERS section of
 * sand.h for the design. What is here is just list management; the actual
 * per-step write lives in emit_from_emitters() below, next to sand_step().
 */

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

/* Movement */

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

void sand_set_soak_convert(sand_t *s, int period)
{
    s->soak_convert = period > 0 ? period : SAND_SOAK_CONVERT_PER_MATERIAL;
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

void sand_set_gas_walk(sand_t *s, bool on)
{
    s->gas_walk = on;
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

void sand_set_crust(sand_t *s, int chance)
{
    s->crust = (chance < 0) ? -1 : (chance > 65535 ? 65535 : chance);
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

/* One bit per materials[] row for the two questions the sweep asks of every
 * cell on the grid, so each reads as a shift out of a word in SRAM instead of
 * dereferencing a struct in flash. The same trade gas_kind_mask makes in
 * sand_gas.c, on a hotter loop: this one runs per cell of every awake block,
 * and there is no data cache, so that dereference is a real flash read.
 *
 * Eight bytes rather than two 32-entry tables, because MATERIAL_ROWS is 32 and
 * a row index therefore fits a uint32_t exactly. */
static uint32_t sweep_skip_mask;    /* KIND_STATIC and KIND_GAS - not ours */
static uint32_t sweep_liquid_mask;  /* KIND_LIQUID - takes the liquid path  */
static bool     sweep_tables_ready;

static void build_sweep_tables(void)
{
    if (sweep_tables_ready) {
        return;
    }
    for (int r = 0; r < MATERIAL_ROWS; r++) {
        if (materials[r].kind == KIND_STATIC || materials[r].kind == KIND_GAS) {
            sweep_skip_mask |= 1u << r;
        }
        if (materials[r].kind == KIND_LIQUID) {
            sweep_liquid_mask |= 1u << r;
        }
    }
    sweep_tables_ready = true;
}

static bool step_one_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                           uint8_t *arow, uint8_t *brow, int x, int y, int w,
                           int dx, int dy, const int *slide_a,
                           const int *slide_b, int load_dx, int load_dy,
                           int jostle, bool driven[MATERIAL_ROWS][2])
{
    const cell_t grain = row[x];

    /* Two shifts answer what two flash reads used to. Gas is skipped because
     * it moves AGAINST the sweep's direction, into cells not yet visited, and
     * would teleport - sand_step_gas() has its own reversed pass for it.
     *
     * Only a powder reaches materials[] now, and only for its density and
     * scatter, which no mask can carry. */
    const unsigned mrow = (unsigned)grain >> 3;
    if (((sweep_skip_mask >> mrow) & 1u) != 0) {
        return false;
    }
    if (((sweep_liquid_mask >> mrow) & 1u) != 0) {
        return move_liquid_grain(s, row, prow, x, y, dx, dy, slide_a,
                                 slide_b, grain, CELL_MATERIAL(grain));
    }

    const material_t *mat = material_of(grain);

    const uint8_t density = mat->density;

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
        if (step_one_grain(ctx->s, ctx->row, ctx->prow,
                               ctx->arow, ctx->brow, x, ctx->y, ctx->w,
                               ctx->dx, ctx->dy, ctx->slide_a, ctx->slide_b,
                               ctx->load_dx, ctx->load_dy, ctx->jostle,
                               ctx->driven)) {
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

/* can_impulse_enter() through step_impulses() itself all moved to
 * sand_impulse.c, alongside the seeding half of this subsystem - see that
 * file's own banner. step_impulses() is declared extern in sand_priv.h and
 * called from sand_step() below, the same shape sand_step_liquids()/
 * sand_step_gas() already use. */


/* PINNED at 16, not left to the compiler: an unpinned attribute can bind to
 * whatever definition follows it rather than to this function, letting an
 * unrelated change silently shift sand_step()'s alignment and regress
 * performance. 32 costs about 4.5% on liquid-free controls for nothing; 16 is
 * near-free and still starts within a 32-byte cache block. */

/* CHECK WITH objdump, NOT the diff: .text.sand_step should read 2**4. Binding
 * to the wrong symbol still compiles and passes everything. */
__attribute__((aligned(16)))
void sand_step(sand_t *s, int gx, int gy, int jostle)
{
    /* Emitters act first per step, before gravity, mimicking
     * sand_spawn_cell() calls. This allows new grains to move immediately.
     * Runs unconditionally, even in free fall, ensuring "once per
     * sand_step()". */
    build_sweep_tables();

    s->step_phase++;

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

    /* Written AFTER compute_settled_bit() returns, and OUTSIDE it: this is
     * a fact about the board's settled direction, not about sleeping
     * bookkeeping, so it must not depend on block sleeping being on -
     * anything else asking which way is down (growth, for one) needs it
     * on any grid, with or without block_state. */
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

    /* Asked once per BLOCK row, not once per row. step_one_row() already
     * skips a settled block, but only after building a seventeen-field
     * context for the row - and the answer is the same for all
     * SAND_BLOCK_H rows sharing that block row, so on a settled board that
     * context is built 64 times over to find nothing to do. Nothing in the
     * skipped row has a side effect (dest_row() is pure), so this is the
     * same program with the dead contexts removed. */
    int scanned_by = -1;
    bool block_row_settled = false;

    for (int y = y_from; y != y_to; y += y_step) {
        if (settled_bit != 0) {
            const int by = y / SAND_BLOCK_H;
            if (by != scanned_by) {
                scanned_by = by;
                block_row_settled = true;
                const uint8_t *const brow =
                    &s->block_state[(size_t)by * (size_t)s->block_cols];
                for (int bx = 0; bx < s->block_cols; bx++) {
                    if ((brow[bx] & settled_bit) == 0) {
                        block_row_settled = false;
                        break;
                    }
                }
            }
            if (block_row_settled) {
                continue;
            }
        }
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
