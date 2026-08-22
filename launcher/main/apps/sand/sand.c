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

/* xorshift32. Deterministic given a seed, which is what makes a test like
 * "shaking flattens a pile" reproducible rather than flaky. */
static uint32_t next_random(sand_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

static uint8_t random_shade(sand_t *s)
{
    return (uint8_t)(SAND_FIRST_SHADE + (next_random(s) % SAND_SHADE_COUNT));
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
    s->rng        = seed ? seed : 1u;   /* xorshift is stuck at zero */
    s->sweep_flip = false;
    sand_clear(s);
}

void sand_clear(sand_t *s)
{
    memset(s->cells, SAND_EMPTY, (size_t)s->w * (size_t)s->h);
}

uint8_t sand_at(const sand_t *s, int x, int y)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        /* Outside the grid reads as occupied, which makes the four walls solid
         * without a single bounds check in the movement code. */
        return SAND_LAST_SHADE;
    }
    return s->cells[y * s->w + x];
}

void sand_set(sand_t *s, int x, int y, uint8_t cell)
{
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return;
    }
    s->cells[y * s->w + x] = cell;
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

int sand_spawn(sand_t *s, int cx, int cy, int radius)
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
            s->cells[y * s->w + x] = random_shade(s);
            filled++;
        }
    }
    return filled;
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

/* Move a grain into `to_row` at column `nx`, if that cell exists and is free.
 *
 * A NULL row or an out-of-range column is a wall, so both simply fail. The
 * single unsigned comparison catches nx < 0 as well, by wrapping it to a huge
 * value - the usual trick, worth it in a loop this hot. */
static inline bool move_to(uint8_t *from_row, uint8_t *to_row,
                           int x, int nx, int w, uint8_t grain)
{
    if (to_row == NULL || (unsigned)nx >= (unsigned)w ||
        to_row[nx] != SAND_EMPTY) {
        return false;
    }
    to_row[nx]  = grain;
    from_row[x] = SAND_EMPTY;
    return true;
}

void sand_step(sand_t *s, int gx, int gy, int jostle)
{
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);

    if (dx == 0 && dy == 0) {
        return;   /* free fall: no down, so nothing settles */
    }

    const int i = ring_index(dx, dy);
    const int *slide_a = ring[(i + 7) & 7];
    const int *slide_b = ring[(i + 1) & 7];

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
        uint8_t *row = s->cells + (size_t)y * (size_t)w;

        /* The three rows a grain in this row can reach. */
        uint8_t *prow = dest_row(s, y + dy);
        uint8_t *arow = dest_row(s, y + slide_a[1]);
        uint8_t *brow = dest_row(s, y + slide_b[1]);

        for (int x = x_from; x != x_to; x += x_step) {
            const uint8_t grain = row[x];
            if (grain == SAND_EMPTY) {
                continue;
            }

            /* The common case by a wide margin - on a screen of falling sand
             * almost every grain simply moves the way gravity points. Taking it
             * before drawing a random number matters: the generator was the
             * single most expensive thing in this loop, and most grains never
             * needed it. */
            if (jostle == 0 && move_to(row, prow, x, x + dx, w, grain)) {
                continue;
            }

            /* Blocked, or being shaken. Now the order of the two slides has to
             * be decided, and without randomising it the sand develops a
             * visible grain with everything leaning the same way. */
            const uint32_t r = next_random(s);

            uint8_t *first_row,  *second_row;
            int      first_dx,    second_dx;
            if (r & 1) {
                first_row  = arow; first_dx  = slide_a[0];
                second_row = brow; second_dx = slide_b[0];
            } else {
                first_row  = brow; first_dx  = slide_b[0];
                second_row = arow; second_dx = slide_a[0];
            }

            /* Shaking reorders the attempts rather than adding a new move: a
             * shaken grain prefers to spread sideways before it drops. Every
             * destination stays inside the already-swept half, so the
             * no-double-move guarantee above still holds. */
            const bool shaken = jostle > 0 && (int)((r >> 8) & 0xFF) < jostle;

            if (!shaken && jostle > 0 &&
                move_to(row, prow, x, x + dx, w, grain)) {
                continue;
            }
            if (move_to(row, first_row, x, x + first_dx, w, grain)) {
                continue;
            }
            if (move_to(row, second_row, x, x + second_dx, w, grain)) {
                continue;
            }
            if (shaken) {
                move_to(row, prow, x, x + dx, w, grain);
            }
        }
    }
}
