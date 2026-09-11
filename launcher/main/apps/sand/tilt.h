/*
 * tilt - turning raw accelerometer counts into a direction worth steering
 * sand with.
 *
 * Pure logic, no sensor and no clock: samples and elapsed time are passed in,
 * so the whole thing is testable on a host.
 *
 * An accelerometer measures gravity PLUS whatever else is accelerating the
 * board, and one reading cannot tell the two apart. Most of what this header
 * declares exists to work around that: a trust gate on magnitude, a strength
 * scalar so a flat board settles rather than stops dead, and a shake level
 * read off the accelerometer rather than the gyroscope. See
 * docs/notes/Input-and-Sensors.md for the reasoning and the measurements
 * behind each.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Time constants, in milliseconds: how long the filter takes to cover about
 * two thirds of the distance to a new reading.
 *
 * STILL is long enough to bury sensor noise. MOVING is short enough that a
 * deliberate tilt arrives without perceptible lag - below roughly 50 ms the eye
 * stops registering it as delay. */
#define TILT_TAU_STILL_MS  260
#define TILT_TAU_MOVING_MS  40

/* A frame longer than this is treated as this long. Guards against a huge step
 * after a pause - a stall should not teleport the filter to the newest reading,
 * and unbounded dt would overflow the fixed-point arithmetic. */
#define TILT_MAX_DT_MS 100

/* Magnitude bounds, as percentages of one g. Between LO and HI a sample
 * is close enough to rest to be treated as gravity. Outside, something
 * is pushing the device and the reading is mostly that. Below
 * FREE_FALL nothing is supporting it at all. Wide on purpose: the cost
 * of rejecting a good sample is a few milliseconds of staleness, and
 * the cost of accepting a bad one is sand thrown across the screen. */
#define TILT_TRUST_LO_PCT     70
#define TILT_TRUST_HI_PCT    130
#define TILT_FREE_FALL_PCT    30

/* How much linear acceleration counts as fully shaken, as a percentage of one
 * g. Half a g of departure from rest is a brisk shake and not something a hand
 * produces by accident. */
#define TILT_SHAKE_FULL_PCT   50

/* Shaking is smoothed too, over about this long, so a shake carries for a
 * moment rather than flickering off between strokes. */
#define TILT_SHAKE_TAU_MS    120

typedef struct {
    /* Smoothed in-plane gravity, in input units scaled by 256. The extra bits
     * matter: without them a slow tilt loses its fractional part every frame
     * and the filter creeps in visible steps. */
    int32_t gx_q8, gy_q8;

    /* Smoothed shake level, 0-255 scaled by 256. */
    int32_t shake_q8;

    int     counts_per_g;
    bool    primed;
    bool    free_fall;
} tilt_t;

/* `counts_per_g` is one g in whatever units the samples use. It is what makes
 * "is this reading actually gravity?" answerable. */
void tilt_reset(tilt_t *t, int counts_per_g);

/* Feed one sample. (gx, gy) is gravity in SCREEN axes and `gz` is the
 * component through the screen - needed only for the magnitude, but
 * needed: without it a flat device looks identical to free fall.
 * `rotation` is 0-255 from the GYROSCOPE, setting only how quickly the
 * filter tracks; deliberately not what shaking is read from. The first
 * sample after a reset is adopted exactly, so the sand does not
 * visibly swing into place when the app opens. */
void tilt_update(tilt_t *t, int gx, int gy, int gz, int rotation,
                 uint32_t dt_ms);

/* The direction sand should flow, in input units. */
int tilt_x(const tilt_t *t);
int tilt_y(const tilt_t *t);

/* How much of a g lies in the screen plane, as 0-256 - which is sin of
 * the tilt away from flat, and therefore how hard the sand is being
 * driven. The caller should scale its simulation rate by this. Full
 * upright, sand runs at full speed; laid flat it coasts to a stop
 * instead of freezing mid-frame. Zero in free fall, where nothing is
 * driving anything. */
int tilt_strength(const tilt_t *t);

/* How hard the device is being shaken, 0-255, from linear acceleration rather
 * than rotation. Turning the board smoothly reads as nothing at all. */
int tilt_shake(const tilt_t *t);

/* True when nothing is supporting the device. The caller should stop the
 * simulation: in free fall sand does not settle, it hangs. */
bool tilt_in_free_fall(const tilt_t *t);
