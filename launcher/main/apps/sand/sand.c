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
    s->may_have_liquid = false;
    s->dirty_rows = NULL;
    s->row_state  = NULL;
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
    s->stagger_rows_remaining = 0;
    s->scatter      = 0;
    s->mom_x_q8 = 0;
    s->mom_y_q8 = 0;
    s->dir_x_q8 = 0;
    s->dir_y_q8 = 0;
    s->mom_primed = false;
    s->flick = 0;
    sand_clear(s);
}

/* wake_span()/wake_blocks_point()/wake_blocks_range() and mark_rows()/
 * mark_move() are shared with
 * sand_liquid.c and live in sand_priv.h now - see the comment there for
 * why they are `static inline` in a header rather than ordinary functions
 * declared extern. BLOCK_SETTLED_NEAREST/OTHER/ACTIVE (block_state) and
 * ROW_NO_LIQUID (row_state, sand_liquid.c's own) live there too, next to
 * the code that reads them. */

void sand_enable_sleeping(sand_t *s, uint8_t *blocks, uint8_t *rows)
{
    s->block_state = blocks;
    if (blocks != NULL) {
        /* Nothing is known about the grid yet, so nothing may be assumed
         * settled. */
        memset(blocks, 0, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    s->row_state = rows;
    if (rows != NULL) {
        memset(rows, 0, (size_t)s->h);
    }
    s->last_load_dx = 0;
    s->last_load_dy = 0;
    s->stagger_rows_remaining = 0;
}

bool sand_block_settled(const sand_t *s, int bx, int by)
{
    if (s->block_state == NULL) {
        return false;
    }
    return (s->block_state[by * s->block_cols + bx] &
           (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) != 0;
}

bool sand_block_staggered(const sand_t *s, int bx, int by)
{
    if (s->block_state == NULL) {
        return false;
    }
    return (s->block_state[by * s->block_cols + bx] & BLOCK_STAGGER_HOLD) != 0;
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
    if (s->row_state != NULL) {
        memset(s->row_state, 0, (size_t)s->h);
    }
    if (s->block_state != NULL) {
        memset(s->block_state, 0, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    s->stagger_rows_remaining = 0;
}

cell_t sand_at(const sand_t *s, int x, int y)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        /* Outside the grid reads as STONE, which makes the four walls solid
         * without a single bounds check in the movement code - and, being the
         * densest thing there is, too solid for anything heavy to displace its
         * way through the floor. */
        return CELL_MAKE(MAT_STONE, 0);
    }
    return s->cells[y * s->w + x];
}

void sand_set(sand_t *s, int x, int y, cell_t cell)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return;
    }
    s->cells[y * s->w + x] = cell;
    if (!CELL_IS_EMPTY(cell) &&
        materials[CELL_MATERIAL(cell)].kind == KIND_LIQUID) {
        s->may_have_liquid = true;
    }
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
static bool try_spawn_one(sand_t *s, int x, int y, material_id_t material)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return false;
    }
    if (s->cells[y * s->w + x] != SAND_EMPTY) {
        return false;   /* never overwrite, so the count cannot drift */
    }
    if (materials[material].kind == KIND_LIQUID) {
        s->may_have_liquid = true;
    }
    s->cells[y * s->w + x] = random_cell(s, material);
    mark_move(s, x, y, x, y);
    return true;
}

int sand_spawn(sand_t *s, int cx, int cy, int radius, material_id_t material)
{
    int filled = 0;
    const int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            if (try_spawn_one(s, cx + dx, cy + dy, material)) {
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

/* Chance in 256 that a loaded grain may still slide sideways.
 *
 * A surface grain is free. Each grain above halves the chance, and past the cap
 * it is nil. Shaking overrides the lot - a shaken pile flows regardless of how
 * deeply buried its grains are, which is the whole reason shaking a jar of
 * sand levels it. */
static int slide_chance(const material_t *m, int load, int jostle)
{
    /* A material whose slip is 255 is never held by load at all. That is most
     * of what separates a liquid from a powder: water at the bottom of a deep
     * pool carries just as much weight as sand at the bottom of a dune, and
     * flows anyway. */
    if (load == 0 || m->slip >= 255) {
        return 256;
    }

    const int chance = (load >= SAND_LOAD_CAP) ? 0 : (m->slip >> (load - 1));

    return chance > jostle ? chance : jostle;
}

/* Whether a grain may slide in direction (mx, my) at all, given gravity.
 *
 * This is the OTHER half of friction, and the half that burial cannot supply.
 * A single layer of sand on a floor has nothing on top of it, so load-based
 * friction leaves it free - and it would still skate sideways on the faintest
 * tilt, which is exactly the complaint.
 *
 * The physical rule is the one for a block on an incline: it stays put until
 * the slope exceeds the friction angle, tan(theta) > mu. Written in terms of a
 * candidate move, the move descends by (m . g) and travels across the slope by
 * |m x g|, so it is only driven if
 *
 *     descent > mu * lateral
 *
 * At zero tilt that permits a diagonal topple (descent and lateral both one,
 * and mu is under one) while forbidding a purely sideways shuffle (descent
 * zero). Sideways only becomes possible once the board is tilted past the
 * friction angle - which is what makes tilting the device feel like tipping
 * sand rather than dragging it.
 *
 * Depends only on the direction, not the grain, so sand_step works it out once
 * per step for each of the two slides. */
static bool driven_by_gravity(int mx, int my, int gx, int gy, int repose)
{
    const int descent = mx * gx + my * gy;
    if (descent <= 0) {
        return false;              /* uphill, or across a level slope */
    }
    if (repose == 0) {
        return true;               /* no friction angle at all - a liquid */
    }

    int lateral = mx * gy - my * gx;
    if (lateral < 0) {
        lateral = -lateral;
    }

    /* `repose` is mu times ten, so 7 is the ~35 degrees of dry sand. Kept as
     * a ratio so this stays in integers. */
    return (int64_t)descent * 10 > (int64_t)lateral * repose;
}

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

/* Whether `mover` may occupy the cell currently holding `target`.
 *
 * Empty space always yields. Anything else yields only to something denser,
 * which is how sand sinks through water while water cannot push its way back
 * up through sand. Static materials never yield whatever the arithmetic says,
 * so a wall stays a wall. */
static inline bool can_enter(uint8_t mover_density, cell_t target)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }

    const material_t *t = material_of(target);

    return t->kind != KIND_STATIC && mover_density > t->density;
}

/* Whether a cell exists and can be entered, without moving anything into it.
 *
 * Needed so scatter can be decided ONLY for cells that could actually fall.
 * Drawing a random number for every cell regardless would undo the single
 * biggest saving in this loop - see the note on the common path below. */
static inline bool cell_open(const uint8_t *row, int nx, int w, uint8_t density)
{
    return row != NULL && (unsigned)nx < (unsigned)w &&
           can_enter(density, row[nx]);
}

/* pour_into() and room_in() moved to sand_liquid.c - sand.c's own movement
 * never splits a grain, so nothing here needed them once the liquid branch
 * did. */

/* Move a grain into `to_row` at column `nx`, if that cell exists and is free.
 *
 * A NULL row or an out-of-range column is a wall, so both simply fail. The
 * single unsigned comparison catches nx < 0 as well, by wrapping it to a huge
 * value - the usual trick, worth it in a loop this hot. */
static inline bool move_to(uint8_t *from_row, uint8_t *to_row,
                           int x, int nx, int w, cell_t mover, uint8_t density)
{
    if (to_row == NULL || (unsigned)nx >= (unsigned)w ||
        !can_enter(density, to_row[nx])) {
        return false;
    }

    /* A SWAP, not an overwrite. Where the target was empty this is exactly the
     * old behaviour; where it held something lighter, that lighter thing takes
     * the vacated cell and is displaced upward.
     *
     * Safe with respect to sweep order: the displaced cell lands in the very
     * cell being processed, which the sweep has just finished with, so it
     * cannot move a second time this step. */
    const cell_t displaced = to_row[nx];

    to_row[nx]  = mover;
    from_row[x] = displaced;
    return true;
}

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

/* One grain's turn: try to fall, then the two slides either side of it, with
 * friction and shaking deciding whether the slides are allowed at all.
 * Returns whether it did anything - moved, or (for a scattering grain)
 * deliberately did not, which the caller must still count as activity so
 * the row does not sleep on a grain mid-air.
 *
 * Marked `static`, not `inline`: this is too large for that to be a useful
 * hint at this build's optimisation level, so the call it costs per grain
 * is a real cost - confirmed against the frame-budget tests on device
 * rather than assumed away. */
/* The common case by a wide margin - on a screen of falling sand almost every
 * grain simply moves the way gravity points. Taking it before drawing a
 * random number matters: the generator was the single most expensive thing
 * in this loop, and most grains never needed it.
 *
 * Only called when jostle == 0: scatter only applies to a grain that could
 * fall, and a shaken grain has to reach the slide logic in try_slide()
 * regardless of whether the plain fall is open. */
/* Whether a scattering grain drifted, lagged, or the scatter roll simply did
 * not apply - in every one of those cases the grain is spoken for, so the
 * caller must not also attempt a plain fall on it. */
static bool try_scatter(sand_t *s, uint8_t *row, uint8_t *prow, uint8_t *arow,
                        uint8_t *brow, int x, int y, int w, int dx,
                        const int *slide_a, const int *slide_b, cell_t grain,
                        uint8_t density, int scatter)
{
    if (scatter == 0 || !cell_open(prow, x + dx, w, density)) {
        return false;
    }

    const uint32_t r = rng_next(&s->rng);
    if ((int)(r & 0xFF) >= scatter) {
        return false;
    }

    /* Drift: sideways as well as down, if that way is open. Spreads the
     * stream horizontally. Blocked or not chosen, it lags instead: nothing
     * at all this step, so the grains around it pull ahead and the stream
     * spreads vertically.
     *
     * Either way still counts as activity. A grain that CHOSE not to move is
     * not a settled grain, and letting the row sleep here would strand it in
     * mid-air. */
    if ((r & 0x100) == 0) {
        const bool pick_a = (r & 0x200) != 0;
        uint8_t  *drow = pick_a ? arow : brow;
        const int ddx  = pick_a ? slide_a[0] : slide_b[0];
        const int ddy  = pick_a ? slide_a[1] : slide_b[1];

        if (move_to(row, drow, x, x + ddx, w, grain, density)) {
            mark_rows(s, y, y + ddy);
        }
    }
    return true;
}

static bool try_fall_or_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                                uint8_t *arow, uint8_t *brow, int x, int y,
                                int w, int dx, int dy, const int *slide_a,
                                const int *slide_b, cell_t grain,
                                uint8_t density, int scatter)
{
    if (try_scatter(s, row, prow, arow, brow, x, y, w, dx, slide_a, slide_b,
                    grain, density, scatter)) {
        return true;
    }

    if (move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }
    return false;
}

/* Which of the two slides to try first, and which second - without
 * randomising it the sand develops a visible grain with everything leaning
 * the same way. */
static void pick_slide_order(uint32_t r, uint8_t *arow, uint8_t *brow,
                             const int *slide_a, const int *slide_b,
                             uint8_t mat_id, bool driven[MATERIAL_MAX][2],
                             uint8_t **first_row, int *first_dx,
                             int *first_dy, bool *first_driven,
                             uint8_t **second_row, int *second_dx,
                             int *second_dy, bool *second_driven)
{
    if (r & 1) {
        *first_row  = arow; *first_dx  = slide_a[0]; *first_dy  = slide_a[1];
        *first_driven = driven[mat_id][0];
        *second_row = brow; *second_dx = slide_b[0]; *second_dy = slide_b[1];
        *second_driven = driven[mat_id][1];
    } else {
        *first_row  = brow; *first_dx  = slide_b[0]; *first_dy  = slide_b[1];
        *first_driven = driven[mat_id][1];
        *second_row = arow; *second_dx = slide_a[0]; *second_dy = slide_a[1];
        *second_driven = driven[mat_id][0];
    }
}

/* Friction, and only on the slides. The grain could not fall, so whether it
 * may SHUFFLE depends on what is sitting on it. Reached only once the
 * gravity-ward move has already failed, so a grain in open air never pays
 * for this. */
static bool try_slide_pair(sand_t *s, uint8_t *row, int x, int y, int w,
                           cell_t grain, uint8_t density,
                           const material_t *mat, int load_dx, int load_dy,
                           int jostle, uint32_t r, uint8_t *first_row,
                           int first_dx, int first_dy, bool first_driven,
                           uint8_t *second_row, int second_dx, int second_dy,
                           bool second_driven)
{
    const int load = sand_load_above(s, x, y, load_dx, load_dy);
    const int allowance = slide_chance(mat, load, jostle);
    if (allowance < 256 && (int)((r >> 16) & 0xFF) >= allowance) {
        return false;
    }

    if (first_driven &&
        move_to(row, first_row, x, x + first_dx, w, grain, density)) {
        mark_rows(s, y, y + first_dy);
        return true;
    }
    if (second_driven &&
        move_to(row, second_row, x, x + second_dx, w, grain, density)) {
        mark_rows(s, y, y + second_dy);
        return true;
    }
    return false;
}

/* Blocked, or being shaken. */
static bool try_slide(sand_t *s, uint8_t *row, uint8_t *prow, uint8_t *arow,
                      uint8_t *brow, int x, int y, int w, int dx, int dy,
                      const int *slide_a, const int *slide_b, int load_dx,
                      int load_dy, int jostle, cell_t grain, uint8_t mat_id,
                      uint8_t density, const material_t *mat,
                      bool driven[MATERIAL_MAX][2])
{
    const uint32_t r = rng_next(&s->rng);

    uint8_t *first_row,  *second_row;
    int      first_dx,    second_dx;
    int      first_dy,    second_dy;
    bool     first_driven, second_driven;
    pick_slide_order(r, arow, brow, slide_a, slide_b, mat_id, driven,
                     &first_row, &first_dx, &first_dy, &first_driven,
                     &second_row, &second_dx, &second_dy, &second_driven);

    /* Shaking reorders the attempts rather than adding a new move: a shaken
     * grain prefers to spread sideways before it drops. Every destination
     * stays inside the already-swept half, so the no-double-move guarantee
     * above still holds. */
    const bool shaken = jostle > 0 && (int)((r >> 8) & 0xFF) < jostle;

    if (!shaken && jostle > 0 &&
        move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    if (try_slide_pair(s, row, x, y, w, grain, density, mat, load_dx,
                       load_dy, jostle, r, first_row, first_dx, first_dy,
                       first_driven, second_row, second_dx, second_dy,
                       second_driven)) {
        return true;
    }

    if (shaken && move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    return false;
}

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
         * Gas is skipped for now rather than mishandled. Rising means
         * moving AGAINST the sweep, into cells not yet visited, which would
         * let it move several times in one step and teleport to the
         * ceiling. Doing it properly needs a second, downward sweep for
         * rising materials; there is nothing that rises yet. */
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
        if (try_fall_or_scatter(s, row, prow, arow, brow, x, y, w, dx, dy,
                                slide_a, slide_b, grain, density, scatter)) {
            return true;
        }
    }

    return try_slide(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a,
                     slide_b, load_dx, load_dy, jostle, grain, mat_id,
                     density, mat, driven);
}

/* Sets BLOCK_STAGGER_HOLD on every block outside row 0, right after a
 * mass wake's full reset - split out of compute_settled_bit() purely to
 * keep its own complexity down. Row 0 stays released immediately (always,
 * regardless of gravity's actual sign - simplest to implement and
 * correctness-independent, since a held block is only ever delayed, never
 * incorrectly skipped forever; see BLOCK_STAGGER_HOLD's own comment).
 * s->stagger_rows_remaining then tracks how many more rows still need
 * releasing, one per subsequent step - see release_next_stagger_row(). */
static void hold_non_leading_rows(sand_t *s)
{
    for (int by = 1; by < s->block_rows; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            s->block_state[by * s->block_cols + bx] |= BLOCK_STAGGER_HOLD;
        }
    }
    s->stagger_rows_remaining = (uint8_t)(s->block_rows - 1);
}

/* Releases the next held block-row, one per call - compute_settled_bit()
 * calls this once every non-reset step until stagger_rows_remaining
 * reaches 0, so every row is fully released within block_rows - 1 steps
 * of the triggering mass wake, unconditionally, regardless of activity
 * elsewhere. That bound is what makes staggering safe: a held block's
 * grains simply do not move on steps where they are held, not stuck
 * forever - the same "eventual, guaranteed within N steps" shape as
 * any_neighbor_active()'s own one-step bound, just wider. */
static void release_next_stagger_row(sand_t *s)
{
    if (s->stagger_rows_remaining == 0) {
        return;
    }
    const int by = s->block_rows - s->stagger_rows_remaining;
    for (int bx = 0; bx < s->block_cols; bx++) {
        s->block_state[by * s->block_cols + bx] &=
            (uint8_t)~BLOCK_STAGGER_HOLD;
    }
    s->stagger_rows_remaining--;
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
 * see the finalisation pass there.
 *
 * A full reset is also a mass wake - every block becomes eligible for a
 * full walk on the exact same step, which is the single most expensive
 * step the simulation ever pays (confirmed on real hardware: this is what
 * dominates the flip/water frame-budget benchmarks). hold_non_leading_rows()
 * spreads that release across a few steps instead - see its own comment,
 * and release_next_stagger_row()'s, for why this cannot strand a grain.
 * Measured trade-off, not a blanket win - see BLOCK_STAGGER_HOLD's own
 * comment in sand_priv.h for the numbers and why this stays on its own
 * branch rather than merged. */
static uint8_t compute_settled_bit(sand_t *s, int jostle, int dx, int dy,
                                   int load_dx, int load_dy)
{
    if (s->block_state == NULL) {
        return 0;
    }

    const int n = s->block_cols * s->block_rows;
    if (jostle > 0 ||
        load_dx != s->last_load_dx || load_dy != s->last_load_dy) {
        memset(s->block_state, 0, (size_t)n);
        hold_non_leading_rows(s);
    } else {
        for (int i = 0; i < n; i++) {
            s->block_state[i] &= (uint8_t)~BLOCK_ACTIVE;
        }
        release_next_stagger_row(s);
    }
    s->last_load_dx = load_dx;
    s->last_load_dy = load_dy;

    return (dx == load_dx && dy == load_dy)
         ? BLOCK_SETTLED_NEAREST : BLOCK_SETTLED_OTHER;
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
    for (int x = cx_from; x != cx_to; x += ctx->x_step) {
        if (CELL_IS_EMPTY(ctx->row[x])) {
            continue;
        }
        if (step_one_grain(ctx->s, ctx->row, ctx->prow, ctx->arow, ctx->brow,
                           x, ctx->y, ctx->w, ctx->dx, ctx->dy, ctx->slide_a,
                           ctx->slide_b, ctx->load_dx, ctx->load_dy,
                           ctx->jostle, ctx->driven)) {
            moved_here = true;
        }
    }

    if (moved_here && ctx->s->block_state != NULL) {
        ctx->s->block_state[ctx->by * ctx->s->block_cols + bx] |= BLOCK_ACTIVE;
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
                         uint8_t settled_bit, bool driven[MATERIAL_MAX][2])
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
        .driven = driven,
    };

    int bx_from, bx_to, bx_step;
    block_x_order(s->block_cols, x_step, &bx_from, &bx_to, &bx_step);

    for (int bx = bx_from; bx != bx_to; bx += bx_step) {
        if (settled_bit != 0) {
            const uint8_t block_bits =
                s->block_state[ctx.by * s->block_cols + bx];
            /* Settled under this step's direction, or held back for its
             * block-row's turn after a mass wake (BLOCK_STAGGER_HOLD -
             * see compute_settled_bit()) - either way, nothing here gets
             * walked this step. */
            if ((block_bits & settled_bit) || (block_bits & BLOCK_STAGGER_HOLD)) {
                continue;
            }
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
 * once across the entire sweep.
 *
 * A block still held back by BLOCK_STAGGER_HOLD (compute_settled_bit())
 * is never eligible to earn the settled bit here, whatever
 * any_neighbor_active() says - its cells have not been examined at all
 * yet this wake event, so settling it now would leave it skipped even
 * after its hold releases, silently undoing the stagger's own bound. */
static void finalize_settling(sand_t *s, uint8_t settled_bit)
{
    if (s->block_state == NULL) {
        return;
    }
    for (int by = 0; by < s->block_rows; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            const int i = by * s->block_cols + bx;
            if (s->block_state[i] & (BLOCK_ACTIVE | BLOCK_STAGGER_HOLD)) {
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

    for (int y = y_from; y != y_to; y += y_step) {
        step_one_row(s, y, w, dx, dy, slide_a, slide_b, x_step,
                    load_dx, load_dy, jostle, settled_bit, driven);
    }

    /* Everything about a liquid that is NOT gravity-ward: cross-flow, and
     * the wall-rebound splash on top of it. See sand_step_liquids() in
     * sand_liquid.c. Run before finalising which blocks get to sleep below,
     * since a cross-flow or rebound move can still touch a block the main
     * sweep left quiet - BLOCK_ACTIVE has to reflect the WHOLE step, not
     * just the sweep's share of it. */
    sand_step_liquids(s, perp_a, perp_b, dx, dy);

    finalize_settling(s, settled_bit);
}
