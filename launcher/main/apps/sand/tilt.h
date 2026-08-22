/*=============================================================================
 * tilt - smoothing a raw gravity vector into one worth steering sand with.
 *
 * Pure logic, no sensor and no clock: samples and elapsed time are passed in,
 * so the whole filter is testable on a host.
 *
 * Raw accelerometer output is not usable directly. It carries noise of a few
 * hundred counts even on a board sitting still, and every hand tremor with it.
 * Feeding that straight into a simulation makes the result feel twitchy and
 * rigid at the same time - jittering while still, then lurching between states
 * when moved.
 *
 * The fix is an exponential moving average - a lerp toward the new reading
 * rather than a jump to it - with two refinements that matter more than the
 * lerp itself:
 *
 *   FRAMERATE INDEPENDENCE.  A fixed "move 10% of the way each frame" changes
 *   meaning the moment the framerate does, and this project's framerate has
 *   already moved from 25 to 70 fps. The smoothing is defined by a TIME
 *   CONSTANT instead, and the per-frame fraction is derived from the elapsed
 *   milliseconds, so it behaves identically at any framerate.
 *
 *   ADAPTIVE RESPONSE.  Heavy smoothing feels laggy when the device is
 *   genuinely being moved, and light smoothing feels noisy when it is not.
 *   The gyroscope resolves this: it reports rotation RATE, which is the one
 *   thing the accelerometer cannot tell you, and it is near zero whenever the
 *   board is being held still no matter how it is tilted. So it says exactly
 *   when to stop smoothing and start tracking.
 *
 * That last point is what makes this temporal rather than just filtered: the
 * gyro describes what is happening *now*, and the filter changes its own
 * behaviour in response.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Time constants, in milliseconds: how long the filter takes to cover about
 * two thirds of the distance to a new reading.
 *
 * STILL is long enough to bury sensor noise. MOVING is short enough that a
 * deliberate tilt arrives without perceptible lag - below roughly 50 ms the
 * eye stops registering it as delay. */
#define TILT_TAU_STILL_MS  260
#define TILT_TAU_MOVING_MS  40

/* A frame longer than this is treated as this long. Guards against a huge
 * step after a pause - a stall should not teleport the filter to the newest
 * reading, and unbounded dt would overflow the fixed-point arithmetic. */
#define TILT_MAX_DT_MS 100

typedef struct {
    /* Smoothed gravity, in input units scaled by 256. The extra bits matter:
     * without them a slow tilt loses its fractional part every frame and the
     * filter creeps in visible steps. */
    int32_t gx_q8, gy_q8;
    bool    primed;
} tilt_t;

void tilt_reset(tilt_t *t);

/* Feed one sample.
 *
 * (gx, gy) is raw gravity in screen axes; `shake` is 0-255 from the gyroscope,
 * where 255 means the board is being moved hard; `dt_ms` is the time since the
 * previous call.
 *
 * The first sample after a reset is adopted exactly, so the sand does not
 * visibly swing into place when the app opens. */
void tilt_update(tilt_t *t, int gx, int gy, int shake, uint32_t dt_ms);

/* The smoothed vector, back in input units. */
int tilt_x(const tilt_t *t);
int tilt_y(const tilt_t *t);
