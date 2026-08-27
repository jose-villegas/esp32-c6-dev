/*=============================================================================
 * intmath - small integer helpers used from more than one file.
 *
 * `static inline`: some of these run in the sand simulation's innermost
 * loop, tens of thousands of times a second, where a cross-file function
 * call is not free.
 *===========================================================================*/
#pragma once

/* Not named `abs`/`sign` - those collide with <stdlib.h>. */

static inline int im_abs(int v)
{
    return v < 0 ? -v : v;
}

/* -1, 0 or 1. */
static inline int im_sign(int v)
{
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

static inline int im_min(int a, int b)
{
    return a < b ? a : b;
}

static inline int im_max(int a, int b)
{
    return a < b ? b : a;
}

/* |(x, y)| without a square root, to about 4%: the larger component plus two
 * fifths of the smaller. Nowhere here needs an exact length - only "how hard
 * is this being shaken" or "how far did this turn" - and this is far cheaper
 * than a real hypot() on a chip with no hardware divider, let alone a square
 * root. */
static inline int im_len(int x, int y)
{
    const int ax = im_abs(x);
    const int ay = im_abs(y);
    const int hi = im_max(ax, ay);
    const int lo = im_min(ax, ay);

    return hi + (lo * 2) / 5;
}
