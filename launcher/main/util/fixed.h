/*=============================================================================
 * fixed - shift-based fixed-point arithmetic, in one place.
 *
 * `static inline`: some of these run in the sand simulation's innermost
 * loops, tens of thousands of times a second, where a cross-file function
 * call is not free - same reasoning as intmath.h.
 *
 * THE SHIFT IS A PARAMETER, NOT A CONSTANT
 *
 * The tree uses more than one fixed-point scale - Q8 in sand and tilt, Q16.16
 * in the UI transform - and both need the same underlying operation. Rather
 * than one vocabulary per scale (which is exactly how this arithmetic ended
 * up hand-rolled five times over), `shift` says which fixed-point format a
 * caller is working in and the same three functions serve all of them.
 *
 * THE WIDENING CAST IS THE WHOLE POINT
 *
 * Every one of these is, at heart, `((int64_t)a * b) >> shift`. The
 * `(int64_t)` is load-bearing: two 32-bit fixed-point numbers multiplied in
 * 32 bits overflow silently, long before the shift ever gets a chance to
 * bring the result back into range. Putting the cast here, once, means a
 * caller cannot forget it.
 *
 * FLOOR VS ROUND - READ THIS BEFORE PICKING ONE
 *
 * `>> shift` on a signed value is an ARITHMETIC shift: for a negative value
 * it floors toward negative infinity, it does NOT truncate toward zero.
 * -1 >> 1 is -1, not 0. fx_mul_floor() is exactly that shift, done on a
 * 64-bit product so it cannot overflow; fx_mul_round() instead rounds the
 * product to the nearest representable value (ties away from zero) before
 * bringing it down.
 *
 * These are NOT interchangeable, and picking the one that "sounds more
 * correct" is exactly the mistake this comment exists to head off:
 *
 *   - sand.c used to decay a Q8, signed, routinely-negative momentum
 *     accumulator (mom_x_q8/mom_y_q8, built from a shaken tilt direction)
 *     with fx_mul_floor(), matching the `>> 8` it was hand-rolled as before
 *     this header existed - swapping in fx_mul_round() would have nudged
 *     every negative decay step upward by up to one part in 256 and changed
 *     the simulation's output. Removed 2026-08-30 along with the rest of
 *     the wall-rebound splash mechanism (see git history), but kept here as
 *     a worked example of exactly the mistake this comment warns against.
 *   - boot_anim.c's phase-span interpolation uses fx_mul_floor() on an
 *     operand already guarded non-negative by its own caller - floor and
 *     round agree there, so the choice is only for consistency with the
 *     rest of that file's math, not because it matters numerically.
 *   - ui_transform.h's ui_fp_mul()/ui_fp_div() use rounding (via
 *     fx_mul_round()/fx_div_round()), because a UI transform is a geometry
 *     computation where round-trip accuracy - a rect mapped and mapped back
 *     lands where it started - matters more than which way a fraction
 *     happens to break, and there is no accumulating simulation state for a
 *     rounding bias to compound in.
 *
 * When in doubt: if the value is a running accumulator that gets multiplied
 * against itself step after step (a decay, a momentum, anything simulated),
 * match whatever operation it already used - do not switch it to "round"
 * because that sounds more accurate. If the value is a one-shot geometric
 * computation, round is usually what you want.
 *===========================================================================*/
#pragma once

#include <stdint.h>

/* The rounding primitive underneath fx_mul_round(): shift an ALREADY-COMBINED
 * accumulator `v` down by `shift`, rounding to the nearest representable
 * value with ties broken away from zero (not toward it, and not floored -
 * see this header's top comment on the difference). Splitting on sign rather
 * than just adding half and shifting matters here: a plain arithmetic shift
 * of a negative number rounds toward -infinity, which would push a value
 * exactly half a unit below zero further from zero than one that is half a
 * unit above it - a bias a truncating cast does not have either.
 *
 * Exposed separately from fx_mul_round() because not every caller's
 * accumulator is a single product of two operands - ui_transform.h's
 * ui_fp_round() rounds a SUM of several Q16.16 products (see
 * ui_transform_point()), so it has nothing to hand fx_mul_round() and needs
 * this lower primitive directly instead. */
static inline int64_t fx_round_shift(int64_t v, int shift)
{
    const int64_t half = (int64_t)1 << (shift - 1);
    return v >= 0 ? (v + half) >> shift : -(((-v) + half) >> shift);
}

/* Multiply two fixed-point numbers in Q(*.shift) and shift the product back
 * down by `shift`, flooring toward negative infinity (see this header's top
 * comment on why that is not the same as truncating toward zero). */
static inline int32_t fx_mul_floor(int32_t a, int32_t b, int shift)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> shift);
}

/* Multiply two fixed-point numbers in Q(*.shift) and shift the product back
 * down by `shift`, rounding to the nearest representable value with ties
 * broken away from zero. */
static inline int32_t fx_mul_round(int32_t a, int32_t b, int shift)
{
    return (int32_t)fx_round_shift((int64_t)a * (int64_t)b, shift);
}

/* Divide two fixed-point numbers in Q(*.shift), rounding the Q(*.shift)
 * result to the nearest representable value, ties away from zero. `den` must
 * be nonzero - same contract ui_fp_div() has always had, just relocated. */
static inline int32_t fx_div_round(int32_t num, int32_t den, int shift)
{
    const int neg   = (num < 0) != (den < 0);
    const int64_t n = ((int64_t)(num < 0 ? -num : num)) << shift;
    const int64_t d = den < 0 ? -den : den;
    const int64_t q = (n + d / 2) / d;
    return (int32_t)(neg ? -q : q);
}
