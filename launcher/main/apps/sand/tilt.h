/*=============================================================================
 * tilt - turning raw accelerometer counts into a direction worth steering sand
 * with.
 *
 * Pure logic, no sensor and no clock: samples and elapsed time are passed in,
 * so the whole thing is testable on a host.
 *
 * There are three separate problems here, and each needs its own answer.
 *
 * 1. NOISE.  Raw output carries a few hundred counts of jitter even on a board
 *    sitting still, and a real tilt arrives as a step change. Answered by an
 *    exponential moving average - a lerp toward the reading rather than a jump
 *    to it - defined by a TIME CONSTANT rather than a per-frame fraction, so it
 *    behaves the same at 25 fps and at 1000.
 *
 * 2. AN ACCELEROMETER DOES NOT MEASURE GRAVITY.  It measures gravity PLUS
 *    whatever else is accelerating it, and the two are indistinguishable in a
 *    single reading. Pick the device up and the reading is mostly the lift.
 *    Feeding that straight in is what makes sand lurch sideways whenever the
 *    device is handled.
 *
 *    The usable half-answer: at rest the magnitude is exactly 1 g, so a sample
 *    whose magnitude is far from 1 g is known to be contaminated even though a
 *    clean one cannot be proven honest. Those are ignored and the previous
 *    estimate held. Cheap, and it removes most of the lurch.
 *
 *    This is also why smoothing HARDER while moving would be wrong. The
 *    gyroscope tells us the board is genuinely rotating - and rotation alone
 *    keeps the magnitude at 1 g, so those samples stay trusted. Being handled
 *    fails the magnitude test instead. The two cases are separated, so tracking
 *    faster during real rotation is safe.
 *
 * 3. A FLAT DEVICE HAS NO IN-PLANE GRAVITY.  Lying on a table, gravity points
 *    into the screen and (x, y) really is zero. Sand SHOULD stop - on a level
 *    tray it does - but stopping dead in one frame looks like a crash rather
 *    than like settling.
 *
 *    The cause is not the sensor. A grain moves one cell per simulation step
 *    however strong gravity is, so the simulation has only two speeds: full and
 *    stopped. tilt_strength() supplies the missing dimension - how much of a g
 *    is actually in the plane - and the caller runs the simulation that much
 *    slower. A grain on a tray accelerates at g*sin(theta), so this is the
 *    physics rather than a fudge, and it means tipping the device flat brings
 *    the sand smoothly to rest.
 *
 *    It also disposes of the ill-conditioning quietly. Near flat, the direction
 *    is the ratio of two noise values and is meaningless - but it is also
 *    barely used, because the flow rate approaching zero is what makes it
 *    meaningless in the first place.
 *
 * 4. ROTATING IS NOT SHAKING.  These want opposite responses - a turn should
 *    be followed, a shake should fluidise the pile - so telling them apart
 *    matters, and the obvious sensor is the wrong one.
 *
 *    Reading "shaken" off the gyroscope means every deliberate turn of the
 *    device reads as a hard shake, which unlocks friction and throws the sand
 *    at the walls. Measured while a board was merely being held and tilted, a
 *    gyro-derived shake level sat between 160 and 255 out of 255.
 *
 *    Shaking means ACCELERATING the device back and forth, and that is the
 *    accelerometer's business: rotating smoothly keeps the magnitude at 1 g,
 *    while shaking swings it far away. So tilt_shake() is derived from how far
 *    the magnitude departs from 1 g - the same quantity the trust gate above
 *    already computes, read for its size rather than its plausibility.
 *
 *    The gyroscope keeps its honest job of saying when the board is genuinely
 *    turning, which is what the filter's time constant responds to.
 *
 * Genuine free fall is different from all of the above and is reported
 * separately: there the TOTAL magnitude collapses, nothing is holding the sand
 * up, and it should hang.
 *===========================================================================*/
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

/* Magnitude bounds, as percentages of one g.
 *
 * Between LO and HI a sample is close enough to rest to be treated as gravity.
 * Outside, something is pushing the device and the reading is mostly that.
 * Below FREE_FALL nothing is supporting it at all. Wide on purpose: the cost of
 * rejecting a good sample is a few milliseconds of staleness, and the cost of
 * accepting a bad one is sand thrown across the screen. */
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

/* Feed one sample.
 *
 * (gx, gy) is gravity in SCREEN axes and `gz` is the component through the
 * screen - needed only for the magnitude, but needed: without it a flat device
 * looks identical to free fall.
 *
 * `rotation` is 0-255 from the GYROSCOPE - how fast the board is turning. It
 * only sets how quickly the filter tracks; it is deliberately not what shaking
 * is read from. `dt_ms` is the time since the previous call.
 *
 * The first sample after a reset is adopted exactly, so the sand does not
 * visibly swing into place when the app opens. */
void tilt_update(tilt_t *t, int gx, int gy, int gz, int rotation,
                 uint32_t dt_ms);

/* The direction sand should flow, in input units. */
int tilt_x(const tilt_t *t);
int tilt_y(const tilt_t *t);

/* How much of a g lies in the screen plane, as 0-256 - which is sin of the
 * tilt away from flat, and therefore how hard the sand is being driven.
 *
 * The caller should scale its simulation rate by this. Full upright, sand runs
 * at full speed; laid flat it coasts to a stop instead of freezing mid-frame.
 * Zero in free fall, where nothing is driving anything. */
int tilt_strength(const tilt_t *t);

/* How hard the device is being shaken, 0-255, from linear acceleration rather
 * than rotation. Turning the board smoothly reads as nothing at all. */
int tilt_shake(const tilt_t *t);

/* True when nothing is supporting the device. The caller should stop the
 * simulation: in free fall sand does not settle, it hangs. */
bool tilt_in_free_fall(const tilt_t *t);
