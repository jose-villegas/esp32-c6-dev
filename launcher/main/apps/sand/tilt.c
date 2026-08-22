#include "tilt.h"

#define Q 256   /* fixed-point scale for the stored vector */

/* |v| without a square root, to about 4%: the larger component plus two fifths
 * of the smaller. Ample - it decides how much to lean on screen-down, and a few
 * percent either way is not a visible difference. */
static int magnitude_2d(int x, int y)
{
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    const int hi = ax > ay ? ax : ay;
    const int lo = ax > ay ? ay : ax;

    return hi + (lo * 2) / 5;
}

/* Compared as squares, so no root is needed and the percentages stay exact.
 * int64 because three squared sensor readings overflow 32 bits, and this runs
 * once per frame where the width costs nothing. */
static bool magnitude_within(int x, int y, int z, int counts_per_g,
                             int lo_pct, int hi_pct)
{
    const int64_t mag2 = (int64_t)x * x + (int64_t)y * y + (int64_t)z * z;
    const int64_t g2   = (int64_t)counts_per_g * counts_per_g;

    return mag2 * 10000 >= g2 * lo_pct * lo_pct &&
           mag2 * 10000 <= g2 * hi_pct * hi_pct;
}

static bool magnitude_below(int x, int y, int z, int counts_per_g, int pct)
{
    const int64_t mag2 = (int64_t)x * x + (int64_t)y * y + (int64_t)z * z;
    const int64_t g2   = (int64_t)counts_per_g * counts_per_g;

    return mag2 * 10000 < g2 * pct * pct;
}

void tilt_reset(tilt_t *t, int counts_per_g)
{
    t->gx_q8        = 0;
    t->gy_q8        = 0;
    t->counts_per_g = counts_per_g > 0 ? counts_per_g : 1;
    t->primed       = false;
    t->free_fall    = false;
}

/* One axis of an exponential moving average.
 *
 * The continuous form is  alpha = 1 - exp(-dt / tau).  Evaluating that needs a
 * transcendental; the standard discrete approximation  alpha = dt / (tau + dt)
 * agrees closely while dt stays well below tau, which TILT_MAX_DT_MS enforces,
 * and costs one divide.
 *
 * The multiply is widened to 64 bits deliberately. The difference can reach
 * +/-32768 * 256, and multiplying that by dt overflows a signed 32-bit value
 * for perfectly ordinary inputs. */
static int32_t approach(int32_t current_q8, int target, int tau_ms, uint32_t dt_ms)
{
    const int32_t target_q8 = (int32_t)target * Q;
    const int64_t delta     = (int64_t)(target_q8 - current_q8) * (int64_t)dt_ms;

    return current_q8 + (int32_t)(delta / (int64_t)(tau_ms + (int)dt_ms));
}

void tilt_update(tilt_t *t, int gx, int gy, int gz, int shake, uint32_t dt_ms)
{
    /* Free fall first: it is the one case where the right answer is to stop
     * rather than to estimate. Checked before the trust gate, which would
     * otherwise reject it as just another untrustworthy reading. */
    t->free_fall = magnitude_below(gx, gy, gz, t->counts_per_g,
                                   TILT_FREE_FALL_PCT);
    if (t->free_fall) {
        return;
    }

    /* Is this reading gravity, or is it mostly whatever is shoving the device?
     * A magnitude far from 1 g proves the latter. Hold the previous estimate
     * rather than follow a number that is not describing orientation. */
    if (!magnitude_within(gx, gy, gz, t->counts_per_g,
                          TILT_TRUST_LO_PCT, TILT_TRUST_HI_PCT)) {
        return;
    }

    if (!t->primed) {
        t->gx_q8  = (int32_t)gx * Q;
        t->gy_q8  = (int32_t)gy * Q;
        t->primed = true;
        return;
    }

    if (dt_ms == 0) {
        return;            /* no time passed, so nothing to integrate */
    }
    if (dt_ms > TILT_MAX_DT_MS) {
        dt_ms = TILT_MAX_DT_MS;
    }

    if (shake < 0) {
        shake = 0;
    } else if (shake > 255) {
        shake = 255;
    }

    /* Interpolate the time constant itself. Still hands get heavy smoothing, a
     * moving board gets a short one, and everything between is proportional
     * rather than a switch - a threshold here would just move the rigidity from
     * the sand to the filter.
     *
     * Safe only because the trust gate above has already thrown out the samples
     * where "moving" meant "being shoved" rather than "being turned". */
    const int tau = TILT_TAU_STILL_MS -
                    ((TILT_TAU_STILL_MS - TILT_TAU_MOVING_MS) * shake) / 255;

    t->gx_q8 = approach(t->gx_q8, gx, tau, dt_ms);
    t->gy_q8 = approach(t->gy_q8, gy, tau, dt_ms);
}

int tilt_x(const tilt_t *t) { return t->gx_q8 / Q; }
int tilt_y(const tilt_t *t) { return t->gy_q8 / Q; }

int tilt_strength(const tilt_t *t)
{
    if (t->free_fall) {
        /* The estimate is deliberately stale here - free fall is not a reading
         * to follow - so it cannot be trusted to be small. Say so explicitly. */
        return 0;
    }

    const int mag = magnitude_2d(t->gx_q8 / Q, t->gy_q8 / Q);
    const int scaled = (int)(((int64_t)mag * 256) / t->counts_per_g);

    return scaled > 256 ? 256 : scaled;
}

bool tilt_in_free_fall(const tilt_t *t) { return t->free_fall; }
