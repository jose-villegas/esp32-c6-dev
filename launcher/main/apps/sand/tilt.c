#include "tilt.h"

#define Q 256   /* fixed-point scale for the stored vector */

void tilt_reset(tilt_t *t)
{
    t->gx_q8  = 0;
    t->gy_q8  = 0;
    t->primed = false;
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
 * for perfectly ordinary inputs. This runs once per frame, not once per grain,
 * so the wider arithmetic costs nothing worth measuring. */
static int32_t approach(int32_t current_q8, int target, int tau_ms, uint32_t dt_ms)
{
    const int32_t target_q8 = (int32_t)target * Q;
    const int64_t delta     = (int64_t)(target_q8 - current_q8) * (int64_t)dt_ms;

    return current_q8 + (int32_t)(delta / (int64_t)(tau_ms + (int)dt_ms));
}

void tilt_update(tilt_t *t, int gx, int gy, int shake, uint32_t dt_ms)
{
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

    /* Interpolate the time constant itself. Still hands get heavy smoothing,
     * a moving board gets a short one, and everything between is proportional
     * rather than a switch - a threshold here would just move the rigidity
     * from the sand to the filter. */
    const int tau = TILT_TAU_STILL_MS -
                    ((TILT_TAU_STILL_MS - TILT_TAU_MOVING_MS) * shake) / 255;

    t->gx_q8 = approach(t->gx_q8, gx, tau, dt_ms);
    t->gy_q8 = approach(t->gy_q8, gy, tau, dt_ms);
}

int tilt_x(const tilt_t *t) { return t->gx_q8 / Q; }
int tilt_y(const tilt_t *t) { return t->gy_q8 / Q; }
