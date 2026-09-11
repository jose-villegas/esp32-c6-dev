/*
 * fixed - shift-based fixed-point arithmetic, in one place.
 *
 * `static inline` because some of these run in innermost simulation loops
 * where a cross-file call is not free - the same reasoning as intmath.h.
 *
 * `shift` is a parameter rather than a constant because the tree works in
 * more than one fixed-point scale, and one vocabulary per scale is how this
 * arithmetic ended up hand-rolled five times over.
 *
 * The `(int64_t)` in `((int64_t)a * b) >> shift` is load-bearing: two 32-bit
 * fixed-point numbers multiplied in 32 bits overflow silently, well before
 * the shift can bring the result back into range. It lives here, once, so no
 * caller can forget it.
 *
 * fx_mul_floor() and fx_mul_round() are NOT interchangeable, and picking the
 * one that sounds more correct is the mistake this exists to head off: on a
 * signed, routinely-negative accumulator, rounding nudges every negative step
 * upward relative to the arithmetic shift and moves the output over many
 * steps. Match whatever an accumulator already used; rounding is for one-shot
 * geometry, where round-trip accuracy matters and nothing compounds.
 */
#pragma once

#include <stdint.h>

/* Rounds `v`, ties away from zero (see header top for floor vs round).
 * Splitting on sign matters: a plain arithmetic shift on a negative value
 * rounds toward -infinity, pushing a value half a unit below zero further
 * from zero than one half above it - unlike a truncating cast. Exposed
 * separately from fx_mul_round() because ui_transform.h's ui_fp_round()
 * sums several Q16.16 products (see ui_transform_point()), not a single
 * product, so it needs this directly. */
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
