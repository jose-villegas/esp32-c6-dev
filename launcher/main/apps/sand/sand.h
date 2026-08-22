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

/* A cell is one byte: empty, or a grain carrying a shade.
 *
 * The shade travels with the grain rather than being derived from position,
 * which matters more than it sounds - position-derived colour makes a moving
 * pile shimmer, because each grain changes colour as it falls. */
#define SAND_EMPTY        0
#define SAND_SHADE_COUNT  6
#define SAND_FIRST_SHADE  1
#define SAND_LAST_SHADE   (SAND_FIRST_SHADE + SAND_SHADE_COUNT - 1)

typedef struct {
    uint8_t *cells;      /* w * h, row-major, caller-owned */
    int      w, h;
    uint32_t rng;        /* xorshift32 state - deterministic, so tests repeat */
    bool     sweep_flip; /* alternates the sweep direction between steps */
} sand_t;

/* `cells` must have room for w * h bytes and is cleared. */
void sand_init(sand_t *s, uint8_t *cells, int w, int h, uint32_t seed);

void sand_clear(sand_t *s);

/* Out-of-bounds reads return a grain, not empty.
 *
 * That is deliberate: it makes the walls solid for free, so the movement code
 * never needs a bounds check before asking what is in the next cell. */
uint8_t sand_at(const sand_t *s, int x, int y);

/* Ignores out-of-bounds writes. */
void sand_set(sand_t *s, int x, int y, uint8_t cell);

int sand_count(const sand_t *s);

/* Fill a disc with grains of random shade. Returns how many cells it filled,
 * which is less than the disc's area when it overlaps the edge or existing
 * grains. */
int sand_spawn(sand_t *s, int cx, int cy, int radius);

/* Advance one frame.
 *
 * (gx, gy) is a gravity vector in any units - only its direction matters, and
 * it is quantised to one of eight. A zero vector means free fall, where
 * nothing settles and so nothing moves.
 *
 * `jostle` is 0-255, how hard the device is being shaken. It makes grains
 * prefer sliding sideways over falling straight down, which flattens a pile
 * the way shaking a real one does. */
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
