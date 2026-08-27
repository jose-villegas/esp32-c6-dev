#include "tilt.h"

#include "util/intmath.h"

#define Q 256   /* fixed-point scale for the stored vector */

/* Integer square root, by binary search over the bits. Needed because shaking
 * is measured as a DISTANCE from one g, and a distance cannot be compared in
 * squared space - unlike the trust gate, which only asks which side of a
 * boundary a value falls. Runs once per frame. */
static int32_t isqrt64(int64_t v)
{
    if (v <= 0) {
        return 0;
    }

    int64_t root = 0;
    int64_t bit  = 1ll << 46;      /* highest power of four below the range */

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= root + bit) {
            v    -= root + bit;
            root  = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (int32_t)root;
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
    t->shake_q8     = 0;
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

/* How far this sample departs from rest, as 0-255.
 *
 * At rest the magnitude is exactly one g whatever the orientation, so anything
 * left over is the device being moved rather than turned. That is what shaking
 * is, and it is why the gyroscope is the wrong instrument for it: a smooth
 * rotation keeps the magnitude at one g and reads as nothing. */
static int shake_from_sample(const tilt_t *t, int gx, int gy, int gz)
{
    const int64_t mag2 = (int64_t)gx * gx + (int64_t)gy * gy + (int64_t)gz * gz;
    const int32_t mag  = isqrt64(mag2);

    const int32_t departure = im_abs(mag - t->counts_per_g);

    const int32_t full = (t->counts_per_g * TILT_SHAKE_FULL_PCT) / 100;
    const int32_t level = (int32_t)(((int64_t)departure * 255) / full);

    return level > 255 ? 255 : (int)level;
}

void tilt_update(tilt_t *t, int gx, int gy, int gz, int rotation,
                 uint32_t dt_ms)
{
    /* Worked out before anything else returns early. A sample rejected by the
     * trust gate below is precisely a sample where the device is being thrown
     * around, which is exactly the sample that has the most to say about how
     * hard it is being shaken. */
    {
        const int32_t target = (int32_t)shake_from_sample(t, gx, gy, gz) * Q;
        const uint32_t step  = dt_ms > TILT_MAX_DT_MS ? TILT_MAX_DT_MS : dt_ms;

        /* Deliberately not conditioned on t->primed. A sample violent enough
         * to be shaking is exactly one the trust gate below rejects, so the
         * position filter may never prime at all while the board is being
         * shaken - tying the two together left shake unsmoothed and snapping
         * to zero the instant the board was set down. */
        if (step == 0) {
            /* no time passed, so nothing to integrate */
        } else {
            t->shake_q8 += (int32_t)(((int64_t)(target - t->shake_q8) * step) /
                                     (TILT_SHAKE_TAU_MS + (int)step));
        }
    }

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

    if (rotation < 0) {
        rotation = 0;
    } else if (rotation > 255) {
        rotation = 255;
    }

    /* Interpolate the time constant itself. Still hands get heavy smoothing, a
     * moving board gets a short one, and everything between is proportional
     * rather than a switch - a threshold here would just move the rigidity from
     * the sand to the filter.
     *
     * Safe only because the trust gate above has already thrown out the samples
     * where "moving" meant "being shoved" rather than "being turned". */
    const int tau = TILT_TAU_STILL_MS -
                    ((TILT_TAU_STILL_MS - TILT_TAU_MOVING_MS) * rotation) / 255;

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

    const int mag = im_len(t->gx_q8 / Q, t->gy_q8 / Q);
    const int scaled = (int)(((int64_t)mag * 256) / t->counts_per_g);

    return scaled > 256 ? 256 : scaled;
}

int tilt_shake(const tilt_t *t)
{
    const int level = t->shake_q8 / Q;

    return level < 0 ? 0 : (level > 255 ? 255 : level);
}

bool tilt_in_free_fall(const tilt_t *t) { return t->free_fall; }
