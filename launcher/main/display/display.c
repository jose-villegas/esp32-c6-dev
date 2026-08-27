/*=============================================================================
 * display - see display.h for the module's job and the hysteresis math.
 *
 * A real translation unit rather than static inline in the header (compare
 * ui_style.h/ui_transform.h, which are header-only because their geometry is
 * a handful of one-shot computations with no state of their own to carry
 * between calls). This module is a small state machine - the decision on
 * one call depends on where display_update() left `quarter` last time - so
 * it gets the same treatment as gesture.c and tilt.c, the two modules this
 * one is explicitly modelled on.
 *===========================================================================*/

#include "display/display.h"

#include <stdint.h>

void display_init(display_t *d)
{
    d->quarter = 0;
}

/* Splits (gx, gy) into the component along quarter `q`'s own "down"
 * direction and the component perpendicular to it - see display.h's top
 * comment. Mirrors the branch structure app_sand.c's old
 * gravity_quarter_turn() used to pick a quarter from scratch: quarters 0/2
 * read gy as the deciding axis and gx as the offender; 1/3 the other way
 * round. `*aligned` is positive when the board is still roughly where
 * quarter `q` expects. */
static void split_gravity(int q, int gx, int gy, int *aligned, int *perp)
{
    switch (q) {
    case 0: *aligned =  gy; *perp = gx; break;   /* down is down */
    case 2: *aligned = -gy; *perp = gx; break;   /* board upside down */
    case 3: *aligned =  gx; *perp = gy; break;   /* down is to the right */
    default: /* 1 */                             /* down is to the left */
             *aligned = -gx; *perp = gy; break;
    }
}

/* Which quarter is reached by leaving `q` when `perp` (the perpendicular
 * component split_gravity() just computed for `q`) is the one driving the
 * switch. Same sign-to-quarter mapping the old gravity_quarter_turn() used,
 * just entered from whichever quarter is already current instead of
 * recomputed from nothing every call. */
static int neighbor_quarter(int q, int perp)
{
    if (q == 0 || q == 2) {
        return (perp >= 0) ? 3 : 1;
    }
    return (perp >= 0) ? 0 : 2;
}

bool display_update(display_t *d, int gx, int gy)
{
    int aligned, perp;
    split_gravity(d->quarter, gx, gy, &aligned, &perp);

    const int perp_abs = (perp < 0) ? -perp : perp;

    /* Cross-multiplied instead of divided: exact integer arithmetic, no
     * rounding, and no need to guard a zero denominator. See display.h's top
     * comment for the ratio, the angle it stands in for, and why a negative
     * `aligned` (tilt past 90 degrees from the current quarter) falls
     * straight through to a switch instead of getting stuck: the right side
     * goes negative while perp_abs stays non-negative, so the "no switch"
     * branch below can never be taken. */
    if ((int64_t)perp_abs * DISPLAY_HYST_DEN <=
        (int64_t)aligned * DISPLAY_HYST_NUM) {
        return false;
    }

    /* neighbor_quarter() always returns something other than d->quarter by
     * construction - each of the four cases above maps to one of the other
     * three - so reaching here always is a real change. */
    d->quarter = neighbor_quarter(d->quarter, perp);
    return true;
}

int display_quarter(const display_t *d)
{
    return d->quarter;
}
