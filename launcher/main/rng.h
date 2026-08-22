/*=============================================================================
 * rng - a small deterministic pseudo-random generator, for anything that wants
 * one.
 *
 * xorshift32. Three shifts and three exclusive-ors, no multiply, no division,
 * no state beyond a single word - which matters on a chip with no hardware
 * divider and 424 KiB of RAM. Its statistical quality is nowhere near a
 * cryptographic generator's and it is not meant to be: this is for scattering
 * sand grains and picking shades.
 *
 * NOT for anything security-related. It is trivially predictable from a couple
 * of outputs.
 *
 * DETERMINISM IS THE POINT. Every generator is explicitly seeded and carries
 * its own state, so nothing shares a hidden global. That is what lets a test
 * say "shaking flattens this pile" and get the same answer every run, and what
 * makes a bug reproducible from a seed rather than only sometimes.
 *
 * Header-only and inline on purpose. The sand simulation calls this tens of
 * thousands of times a second from its innermost loop, and a function call per
 * draw across a translation-unit boundary would be a real cost - removing that
 * generator from the common path was once worth a factor of two.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t state;
} rng_t;

/* Any seed will do except zero, which is a fixed point of the algorithm - the
 * state would stay zero and every draw would return zero for ever. Substituted
 * silently rather than rejected, because a caller seeding from a timer that
 * happened to read zero deserves a working generator, not a subtle one. */
static inline void rng_seed(rng_t *r, uint32_t seed)
{
    r->state = seed != 0 ? seed : 0x9E3779B9u;
}

static inline uint32_t rng_next(rng_t *r)
{
    uint32_t x = r->state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    r->state = x;
    return x;
}

/* A number in 0 .. bound-1, or 0 if `bound` is not positive.
 *
 * Uses a modulo, so the lowest values are very slightly more likely for bounds
 * that do not divide 2^32. For picking one of six shades that bias is around
 * one part in seven hundred million and nobody is going to see it. */
static inline int rng_below(rng_t *r, int bound)
{
    if (bound <= 0) {
        return 0;
    }
    return (int)(rng_next(r) % (uint32_t)bound);
}

/* True with probability `chance` in 256.
 *
 * The boundaries are handled without consuming a draw, and that is deliberate
 * rather than an optimisation: callers express "always" as 256 and "never" as
 * 0, and those have to mean exactly that. A bare `(rng_next(r) & 0xFF) < chance`
 * can never be true 256 times out of 256, so "always" would quietly become
 * "almost always" - the kind of thing that shows up as one grain in a thousand
 * behaving oddly. */
static inline bool rng_chance(rng_t *r, int chance)
{
    if (chance <= 0) {
        return false;
    }
    if (chance >= 256) {
        return true;
    }
    return (int)(rng_next(r) & 0xFF) < chance;
}
