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
 *===========================================================================*/

#include "sand.h"

#include <string.h>


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
    s->dirty_rows = NULL;
    s->row_state  = NULL;
    s->last_load_dx = 0;
    s->last_load_dy = 0;
    s->scatter      = 0;
    sand_clear(s);
}

/* Row sleeping state: one bit per gravity direction the row has been examined
 * under without anything moving.
 *
 * Two bits rather than one because the direction is DITHERED - it alternates
 * between the two that bracket the true angle - and those two do not offer the
 * same moves. Straight down allows the slides (-1,+1) and (+1,+1); down-right
 * allows (0,+1) and (+1,0), and that last one is purely sideways. A row that
 * settled under one direction may well have somewhere to go under the other,
 * so a single "settled" bit lets grains freeze on a slope.
 *
 * That is not a hypothetical: it is what the slope test caught. */
#define ROW_SETTLED_NEAREST 0x1
#define ROW_SETTLED_OTHER   0x2

/* Something changed across rows y0..y1, so none of them - nor the rows touching
 * them - can still be considered settled under any direction. Clearing the
 * neighbours is what makes a grain notice that its support has moved.
 *
 * Written as one clamped range rather than two separate neighbourhoods: a move
 * spans at most two adjacent rows, so the two overlap almost entirely, and this
 * is on the hot path. */
static inline void wake_span(sand_t *s, int y0, int y1)
{
    int lo = (y0 < y1 ? y0 : y1) - 1;
    int hi = (y0 > y1 ? y0 : y1) + 1;

    if (lo < 0) {
        lo = 0;
    }
    if (hi >= s->h) {
        hi = s->h - 1;
    }
    for (int y = lo; y <= hi; y++) {
        s->row_state[y] = 0;
    }
}

/* Marking is a pair because every move touches two rows: the one a grain left
 * and the one it arrived in. Both are guaranteed in range at every call site -
 * a move only happens once its destination row has been found to exist. */
static inline void mark_rows(sand_t *s, int y0, int y1)
{
    if (s->dirty_rows != NULL) {
        s->dirty_rows[y0] = 1;
        s->dirty_rows[y1] = 1;
    }
    if (s->row_state != NULL) {
        wake_span(s, y0, y1);
    }
}

void sand_enable_sleeping(sand_t *s, uint8_t *rows)
{
    s->row_state = rows;
    if (rows != NULL) {
        /* Nothing is known about the grid yet, so nothing may be assumed
         * settled. */
        memset(rows, 0, (size_t)s->h);
    }
    s->last_load_dx = 0;
    s->last_load_dy = 0;
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
    mark_rows(s, y, y);
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

int sand_spawn(sand_t *s, int cx, int cy, int radius, material_id_t material)
{
    int filled = 0;
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
            if (s->cells[y * s->w + x] != SAND_EMPTY) {
                continue;   /* never overwrite, so the count cannot drift */
            }
            s->cells[y * s->w + x] = random_cell(s, material);
            mark_rows(s, y, y);
            filled++;
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
            mark_rows(s, y, y);
            removed++;
        }
    }
    return removed;
}

/*---------------------------------------------------------------------------
 * Movement
 *-------------------------------------------------------------------------*/

void sand_gravity_direction(int gx, int gy, int *dx, int *dy)
{
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ax == 0 && ay == 0) {
        *dx = 0;
        *dy = 0;
        return;
    }

    const int sx = gx > 0 ? 1 : (gx < 0 ? -1 : 0);
    const int sy = gy > 0 ? 1 : (gy < 0 ? -1 : 0);

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

/* The row a move would land in, or NULL if that is off the grid.
 *
 * Worked out once per row rather than once per grain: every grain in a row
 * shares the same three destination rows, so the vertical bounds check is done
 * 224 times per step instead of 41,216 times. */
static inline uint8_t *dest_row(const sand_t *s, int y)
{
    if (y < 0 || y >= s->h) {
        return NULL;
    }
    return s->cells + (size_t)y * (size_t)s->w;
}

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
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ax == 0 && ay == 0) {
        *dx = 0;
        *dy = 0;
        return;
    }

    const int sx = gx > 0 ? 1 : (gx < 0 ? -1 : 0);
    const int sy = gy > 0 ? 1 : (gy < 0 ? -1 : 0);

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

void sand_step(sand_t *s, int gx, int gy, int jostle)
{
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

    /* Sleeping. Everything wakes when the gravity direction changes or the grid
     * is being shaken, because either can free a grain that had nothing to do
     * with what its neighbours were doing. The NEAREST direction is what is
     * compared, not the dithered one - that changes almost every step by
     * design, and comparing it would mean nothing ever slept. */
    uint8_t settled_bit = 0;   /* zero means sleeping is off entirely */
    if (s->row_state != NULL) {
        if (jostle > 0 ||
            load_dx != s->last_load_dx || load_dy != s->last_load_dy) {
            /* Either can free a grain that had nothing to do with what its
             * neighbours were doing, so no row's settled state survives. */
            memset(s->row_state, 0, (size_t)s->h);
        }
        s->last_load_dx = load_dx;
        s->last_load_dy = load_dy;

        /* Which of the two dithered directions this step is using. */
        settled_bit = (dx == load_dx && dy == load_dy)
                    ? ROW_SETTLED_NEAREST : ROW_SETTLED_OTHER;
    }

    /* Whether each slide is driven at this tilt, for each material.
     *
     * It depends only on the direction and the material's angle of repose, so
     * it is worked out once per step for all of them - sixteen booleans -
     * rather than recomputed for every one of 41,000 cells. */
    bool driven[MATERIAL_MAX][2];
    for (int m = 0; m < MATERIAL_MAX; m++) {
        const int repose = materials[m].repose;
        driven[m][0] = driven_by_gravity(slide_a[0], slide_a[1], gx, gy, repose);
        driven[m][1] = driven_by_gravity(slide_b[0], slide_b[1], gx, gy, repose);
    }

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

    int x_from, x_to, x_step;
    if (dx > 0) {
        x_from = s->w - 1; x_to = -1;   x_step = -1;
    } else if (dx < 0) {
        x_from = 0;        x_to = s->w; x_step = 1;
    } else {
        /* Gravity has no horizontal component, so either order is correct.
         * Alternating between steps keeps piles from leaning consistently one
         * way, which a fixed order makes surprisingly obvious. */
        if (s->sweep_flip) {
            x_from = s->w - 1; x_to = -1;   x_step = -1;
        } else {
            x_from = 0;        x_to = s->w; x_step = 1;
        }
    }
    s->sweep_flip = !s->sweep_flip;

    const int w = s->w;

    for (int y = y_from; y != y_to; y += y_step) {
        /* Already examined under this exact direction with nothing to do, and
         * nothing has moved next to it since. It cannot have anywhere to go. */
        if (settled_bit != 0 && (s->row_state[y] & settled_bit)) {
            continue;
        }
        bool moved_here = false;

        uint8_t *row = s->cells + (size_t)y * (size_t)w;

        /* The three rows a grain in this row can reach. */
        uint8_t *prow = dest_row(s, y + dy);
        uint8_t *arow = dest_row(s, y + slide_a[1]);
        uint8_t *brow = dest_row(s, y + slide_b[1]);

        for (int x = x_from; x != x_to; x += x_step) {
            const cell_t grain = row[x];
            if (CELL_IS_EMPTY(grain)) {
                continue;
            }

            const material_t *mat = material_of(grain);
            if (mat->kind == KIND_STATIC || mat->kind == KIND_GAS) {
                /* Static never moves, so it costs a single comparison - which
                 * is what makes a wall of stone free to have on screen.
                 *
                 * Gas is skipped for now rather than mishandled. Rising means
                 * moving AGAINST the sweep, into cells not yet visited, which
                 * would let it move several times in one step and teleport to
                 * the ceiling. Doing it properly needs a second, downward sweep
                 * for rising materials; there is nothing that rises yet. */
                continue;
            }

            const uint8_t mat_id  = CELL_MATERIAL(grain);
            const uint8_t density = mat->density;
            const int scatter = (s->scatter >= 0) ? s->scatter : mat->scatter;

            /* The common case by a wide margin - on a screen of falling sand
             * almost every grain simply moves the way gravity points. Taking it
             * before drawing a random number matters: the generator was the
             * single most expensive thing in this loop, and most grains never
             * needed it.
             *
             * Scatter only applies to a grain that could fall, and is only
             * asked about once that is established, so a blocked grain still
             * costs nothing extra here. */
            if (jostle == 0) {
                if (scatter != 0 && cell_open(prow, x + dx, w, density)) {
                    const uint32_t r = rng_next(&s->rng);

                    if ((int)(r & 0xFF) < scatter) {
                        /* Drift: sideways as well as down, if that way is
                         * open. Spreads the stream horizontally. */
                        if ((r & 0x100) == 0) {
                            const bool pick_a = (r & 0x200) != 0;
                            uint8_t  *drow = pick_a ? arow : brow;
                            const int ddx  = pick_a ? slide_a[0] : slide_b[0];
                            const int ddy  = pick_a ? slide_a[1] : slide_b[1];

                            if (move_to(row, drow, x, x + ddx, w, grain, density)) {
                                mark_rows(s, y, y + ddy);
                                moved_here = true;
                                continue;
                            }
                        }

                        /* Lag: nothing at all this step, so the grains around
                         * it pull ahead and the stream spreads vertically.
                         *
                         * The row is still counted as busy. A grain that CHOSE
                         * not to move is not a settled grain, and letting the
                         * row sleep here would strand it in mid-air. */
                        moved_here = true;
                        continue;
                    }
                }

                if (move_to(row, prow, x, x + dx, w, grain, density)) {
                    mark_rows(s, y, y + dy);
                    moved_here = true;
                    continue;
                }
            }

            /* Blocked, or being shaken. Now the order of the two slides has to
             * be decided, and without randomising it the sand develops a
             * visible grain with everything leaning the same way. */
            const uint32_t r = rng_next(&s->rng);

            uint8_t *first_row,  *second_row;
            int      first_dx,    second_dx;
            int      first_dy,    second_dy;
            bool     first_driven, second_driven;
            if (r & 1) {
                first_row  = arow; first_dx  = slide_a[0]; first_dy  = slide_a[1];
                first_driven = driven[mat_id][0];
                second_row = brow; second_dx = slide_b[0]; second_dy = slide_b[1];
                second_driven = driven[mat_id][1];
            } else {
                first_row  = brow; first_dx  = slide_b[0]; first_dy  = slide_b[1];
                first_driven = driven[mat_id][1];
                second_row = arow; second_dx = slide_a[0]; second_dy = slide_a[1];
                second_driven = driven[mat_id][0];
            }

            /* Shaking reorders the attempts rather than adding a new move: a
             * shaken grain prefers to spread sideways before it drops. Every
             * destination stays inside the already-swept half, so the
             * no-double-move guarantee above still holds. */
            const bool shaken = jostle > 0 && (int)((r >> 8) & 0xFF) < jostle;

            if (!shaken && jostle > 0 &&
                move_to(row, prow, x, x + dx, w, grain, density)) {
                mark_rows(s, y, y + dy);
                moved_here = true;
                continue;
            }

            /* Friction, and only on the slides. The grain could not fall, so
             * whether it may SHUFFLE depends on what is sitting on it. Reached
             * only once the gravity-ward move has already failed, so a grain in
             * open air never pays for this. */
            const int load = sand_load_above(s, x, y, load_dx, load_dy);
            const int allowance = slide_chance(mat, load, jostle);
            if (allowance >= 256 || (int)((r >> 16) & 0xFF) < allowance) {
                if (first_driven &&
                    move_to(row, first_row, x, x + first_dx, w, grain, density)) {
                    mark_rows(s, y, y + first_dy);
                    moved_here = true;
                    continue;
                }
                if (second_driven &&
                    move_to(row, second_row, x, x + second_dx, w, grain, density)) {
                    mark_rows(s, y, y + second_dy);
                    moved_here = true;
                    continue;
                }
            }

            if (shaken && move_to(row, prow, x, x + dx, w, grain, density)) {
                mark_rows(s, y, y + dy);
                moved_here = true;
                continue;
            }

            /* A liquid that can go neither down nor diagonally still has to
             * find its own level, so it tries straight across.
             *
             * ONLY UNDER PRESSURE - something has to be resting on it.
             *
             * That is hydrostatic pressure, and leaving it out is what made
             * settled water behave like slime: a drop lying on a full pool has
             * nowhere to fall, finds air beside it, and slides sideways every
             * single step. Since the sweep direction alternates, it slid left,
             * then right, then left, wandering across the surface for ever
             * instead of coming to rest.
             *
             * With the test, a drop with nothing on top of it has no reason to
             * move and simply becomes part of the surface, while water with
             * weight above it still spreads - which is the behaviour that
             * makes a liquid fill a flat-bottomed container rather than
             * standing in a column.
             *
             * The move is also only ever made AGAINST the sweep, into the
             * column just visited. A sideways move stays in the same row, so
             * going the other way would land in a cell not yet processed and
             * let one drop travel several cells in a single step, draining a
             * pool sideways in one frame. The sweep alternates, so both
             * directions remain available, just not at once. */
            if (mat->kind == KIND_LIQUID) {
                const int back = -x_step;

                /* Under pressure it spreads regardless: that is what makes a
                 * column collapse and run along a flat floor. */
                bool go = load > 0;

                /* Otherwise it moves only if there is somewhere LOWER within
                 * reach along the surface.
                 *
                 * Both halves are needed. Without the pressure test, a drop
                 * lying on a full pool slides sideways every step and - since
                 * the sweep direction alternates - wanders left and right for
                 * ever instead of coming to rest. Without this scan, water
                 * poured into a basin heaps up in the corner it landed in and
                 * never levels, because the cells on top of the heap have
                 * nothing pressing on them and so never move.
                 *
                 * The scan stops at anything solid, so water cannot see past a
                 * wall, and it only ever moves ONE cell - it is looking for a
                 * reason to go, not travelling the whole way at once. */
                if (!go) {
                    for (int k = 1; k <= SAND_LIQUID_REACH; k++) {
                        const int sx = x + back * k;
                        if ((unsigned)sx >= (unsigned)w ||
                            !CELL_IS_EMPTY(row[sx])) {
                            break;
                        }
                        if (cell_open(prow, sx + dx, w, density)) {
                            go = true;
                            break;
                        }
                    }
                }

                if (go && move_to(row, row, x, x + back, w, grain, density)) {
                    mark_rows(s, y, y);
                    moved_here = true;
                }
            }
        }

        /* Examined, and nothing here could move. Remember that against THIS
         * direction only - the other one offers different moves. A later row
         * moving next to this one clears it again, via wake_around(). */
        if (settled_bit != 0 && !moved_here) {
            s->row_state[y] |= settled_bit;
        }
    }

}
