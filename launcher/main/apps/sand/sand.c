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

#include "intmath.h"


/* The eight directions, in ring order, so that the two neighbours of any
 * direction are simply the entries either side of it. That is what lets the
 * movement rule work at any gravity angle without eight special cases. */
static const int ring[8][2] = {
    {  0,  1 },   /* 0  down            */
    {  1,  1 },   /* 1  down-right      */
    {  1,  0 },   /* 2  right           */
    {  1, -1 },   /* 3  up-right        */
    {  0, -1 },   /* 4  up              */
    { -1, -1 },   /* 5  up-left         */
    { -1,  0 },   /* 6  left            */
    { -1,  1 },   /* 7  down-left       */
};

/* tan(22.5 deg) is the boundary between "straight down" and "diagonal"; its
 * reciprocal, 2.4142, is approximated as 29/12 to keep this in integers.
 * The largest operand is a raw accelerometer reading, so 32767 * 29 stays well
 * inside 32 bits. */
#define AXIS_NUM 29
#define AXIS_DEN 12

static cell_t random_cell(sand_t *s, material_id_t material)
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
    /* A material that dries has MOISTURE in its variant, and a fresh cell
     * of it is bone dry. Fourth meaning the variant can carry, and the
     * fourth reason a random shade would be wrong - it would hand the
     * player soil that arrives already watered. */
    if (reactions[material].dries != 0) {
        return CELL_MAKE(material, 0);
    }
    return CELL_MAKE(material, rng_below(&s->rng, MATERIAL_VARIANTS));
}

static int ring_index(int dx, int dy)
{
    for (int i = 0; i < 8; i++) {
        if (ring[i][0] == dx && ring[i][1] == dy) {
            return i;
        }
    }
    return 0;   /* unreachable for a unit direction */
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
    /* Computed here, unconditionally, rather than only when sleeping is
     * enabled: the main sweep always walks block-columns (see
     * step_one_row()), whether or not block_state exists, so block_cols/
     * block_rows must be real grid-derived values from the start - never
     * zero, which would make that walk cover nothing. */
    s->block_cols  = (w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    s->block_rows  = (h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;
    s->last_load_dx = 0;
    s->last_load_dy = 0;
    s->scatter      = 0;
    s->decay        = 0;
    s->soak         = 0;    /* nothing soaks unless asked - see
                             * sand_set_soak() */
    s->mobility     = 255;  /* full speed by default - see sand_set_mobility() */
    s->flammability = SAND_FLAMMABILITY_PER_MATERIAL;  /* see sand_set_flammability() */
    s->conduction   = SAND_CONDUCTION_PER_MATERIAL;    /* see sand_set_conduction() */
    s->mom_x_q8 = 0;
    s->mom_y_q8 = 0;
    s->dir_x_q8 = 0;
    s->dir_y_q8 = 0;
    s->mom_primed = false;
    s->flick = 0;
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
static bool try_spawn_one(sand_t *s, int x, int y, cell_t spec)
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
                        : random_cell(s, (material_id_t)CELL_MATERIAL(spec));
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

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            if (try_spawn_one(s, cx + dx, cy + dy, spec)) {
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
    return removed;
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

/* can_enter()/cell_open()/move_to() moved to sand_priv.h (still
 * static inline) - see that header's own comment for why the whole
 * grain-movement primitive stack lives there now. pour_into()/room_in()
 * are the liquid-specific siblings and stayed in sand_liquid.c; sand.c's
 * own movement never splits a grain, so nothing here needed them once
 * the liquid branch did. */

/* How hard gravity's direction turned since last step, decayed so a single
 * flick fades over a few frames rather than lingering or accumulating
 * without bound under sustained shaking. Based on the RAW direction, not
 * the dithered or nearest one - both are quantised for the grid's benefit
 * and neither is a fair measure of how fast the input itself is actually
 * moving. Called once a step, not once a cell, so this carries none of the
 * inlining concerns the per-grain helpers below do. */
static void update_momentum(sand_t *s, int gx, int gy)
{
    s->mom_x_q8 = (int32_t)(((int64_t)s->mom_x_q8 * SAND_MOMENTUM_DECAY) >> 8);
    s->mom_y_q8 = (int32_t)(((int64_t)s->mom_y_q8 * SAND_MOMENTUM_DECAY) >> 8);

    const int len = im_len(gx, gy);
    if (len > 0) {
        const int32_t ux = (int32_t)(((int64_t)gx * 256) / len);
        const int32_t uy = (int32_t)(((int64_t)gy * 256) / len);

        if (s->mom_primed && s->flick > 0) {
            /* Which way it turned, from the smoothed direction - a step or
             * two late, but the right way eventually. Renormalised to a
             * unit vector so a turn too small for the smoothing to have
             * caught up on yet still points somewhere definite, rather than
             * contributing almost nothing just because the filter has not
             * finished moving.
             *
             * HOW FAR comes from the gyroscope, not from how big this
             * renormalised step is - see the comment above
             * SAND_REBOUND_GAIN for why the delta itself is the wrong thing
             * to scale by. */
            const int32_t tx = ux - s->dir_x_q8;
            const int32_t ty = uy - s->dir_y_q8;
            const int tlen = im_len((int)tx, (int)ty);

            if (tlen > 0) {
                s->mom_x_q8 += (int32_t)((((int64_t)tx * 256 / tlen) *
                                          s->flick) >> 8);
                s->mom_y_q8 += (int32_t)((((int64_t)ty * 256 / tlen) *
                                          s->flick) >> 8);
            }
        }
        s->mom_primed = true;
        s->dir_x_q8 = ux;
        s->dir_y_q8 = uy;
    }

    /* Capped so a long spell of shaking cannot ratchet this past what any
     * single flick could ever produce. */
    if (s->mom_x_q8 >  1024) { s->mom_x_q8 =  1024; }
    if (s->mom_x_q8 < -1024) { s->mom_x_q8 = -1024; }
    if (s->mom_y_q8 >  1024) { s->mom_y_q8 =  1024; }
    if (s->mom_y_q8 < -1024) { s->mom_y_q8 = -1024; }
}

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
    s->last_load_dx = load_dx;
    s->last_load_dy = load_dy;

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

void sand_step(sand_t *s, int gx, int gy, int jostle)
{
    update_momentum(s, gx, gy);

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

    const int i = ring_index(dx, dy);
    const int *slide_a = ring[(i + 7) & 7];
    const int *slide_b = ring[(i + 1) & 7];

    const uint8_t settled_bit = compute_settled_bit(s, jostle, dx, dy,
                                                    load_dx, load_dy);

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
    const int i_stable = ring_index(load_dx, load_dy);
    const int *const perp_a = ring[(i_stable + 2) & 7];
    const int *const perp_b = ring[(i_stable + 6) & 7];

    const int w = s->w;
    const uint16_t is_liquid = liquid_mask();

    for (int y = y_from; y != y_to; y += y_step) {
        step_one_row(s, y, w, dx, dy, slide_a, slide_b, x_step,
                    load_dx, load_dy, jostle, settled_bit, is_liquid, driven);
    }

    /* Everything about a liquid that is NOT gravity-ward: cross-flow, and
     * the wall-rebound splash on top of it. See sand_step_liquids() in
     * sand_liquid.c. Run before finalising which blocks get to sleep below,
     * since a cross-flow or rebound move can still touch a block the main
     * sweep left quiet - BLOCK_ACTIVE has to reflect the WHOLE step, not
     * just the sweep's share of it. */
    sand_step_liquids(s, perp_a, perp_b, dx, dy);

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

    finalize_settling(s, settled_bit);
}
