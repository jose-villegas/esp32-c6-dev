/*=============================================================================
 * tween - timeline ramps, easing and lerps, in one place.
 *
 * Pulled out of boot_anim.h, where every one of these was hand-rolled first:
 * a millisecond ramp to a 0..255 fraction, an ease-out curve applied to one,
 * and an "a plus (b minus a) times a fraction over 255" lerp repeated at
 * something like a dozen call sites. boot_anim.c is a one-shot five-second
 * sequence, but it will not stay the only thing in this tree animating a
 * value over time - an app's own intro or a transition is the same problem
 * again - so this is where that vocabulary lives now, not copied a second
 * time into the next place that needs it.
 *
 * `static inline`, like fixed.h next to it: nothing here is expensive enough
 * on its own to need a cross-file call, and some of these run once per point
 * drawn in an animation.
 *
 * MILLISECONDS IN, Q0 (0..255) OUT
 *
 * tween_ramp() takes plain uint32_t milliseconds rather than a fixed-point
 * time, because every caller so far has a wall-clock timestamp already
 * (esp_timer_get_time(), scaled to ms) and nothing here needs sub-millisecond
 * precision. The OUTPUT fraction is Q0 - a uint8_t, 0..255 standing for
 * 0.0..1.0 - deliberately coarser than the Q12/Q16.16 this tree uses
 * elsewhere: a choreography fraction only ever multiplies something and gets
 * divided by 255 again, so nothing is lost by keeping it in a single byte,
 * and a single byte is what threads cleanly through boot_anim.h's existing
 * arithmetic without a second fixed-point scale to keep straight there.
 *===========================================================================*/
#pragma once

#include <stdint.h>

/* 0 before `start_ms`, 255 from `start_ms + dur_ms` on, linear between.
 * `dur_ms` of 0 jumps straight to 255 the instant `now_ms` passes
 * `start_ms`, rather than dividing by zero. */
static inline uint8_t tween_ramp(uint32_t now_ms, uint32_t start_ms,
                                 uint32_t dur_ms)
{
    if (now_ms <= start_ms) {
        return 0;
    }
    const uint32_t elapsed = now_ms - start_ms;
    if (dur_ms == 0 || elapsed >= dur_ms) {
        return 255;
    }
    return (uint8_t)((elapsed * 255u) / dur_ms);
}

/* Fast off the mark, settling as it arrives. Motion that starts and stops at
 * the same speed reads as mechanical; one squared term is enough to stop it
 * looking like a progress bar. Endpoints are exact (0 stays 0, 255 stays
 * 255), so composing this with tween_ramp() never drifts a settled value
 * off its target. */
static inline uint8_t tween_ease_out(uint8_t linear)
{
    const uint32_t left = 255u - linear;
    return (uint8_t)(255u - (left * left) / 255u);
}

/* a, at u8 = 0; b, at u8 = 255; linear between. The one shape "interpolate
 * toward a target by this much of the way there" keeps taking in this tree -
 * a position lerping toward where a letter settles, an angle turning toward
 * where a camera ends up, a length growing from a short arm to a long one -
 * so it is worth naming once rather than re-deriving `a + (b - a) * u8 / 255`
 * at every call site, where it also has to keep guarding against widening
 * one more time.
 *
 * int64_t on the way through the same widening fixed.h's fx_mul_floor()
 * insists on: (b - a) can be tens of thousands (a pixel span, a Q12 angle,
 * a Q8 height) and u8 up to 255, and a naive int32_t product of those is not
 * actually going to overflow at today's call sites, but nothing about the
 * TYPE says so, and the cost of being wrong about that later is a silent
 * wraparound in an animation, not a compiler error. */
static inline int32_t tween_lerp_i32(int32_t a, int32_t b, uint8_t u8)
{
    return a + (int32_t)(((int64_t)(b - a) * u8) / 255);
}
