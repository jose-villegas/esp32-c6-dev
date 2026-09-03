---
name: realtime-engine-review
description: Review checklist for real-time simulation/rendering code (frame loops, custom software rasterizers, particle/cellular-automaton systems, animation timelines) - frame budgets, cache locality, interpolation correctness, determinism. Trigger on "review this for game/engine performance", "check this simulation/rendering code", "frame budget review".
---

# Real-time simulation and rendering review

A checklist for code that runs every frame (or every tick of a simulation)
under a time budget — a custom renderer, a particle/cellular system, an
animation/timeline player. This is about *how it behaves as a system running
continuously*, not line-by-line C correctness (see the `embedded-c-review`
skill for that half, when both apply).

## Frame budget and hot-path cost

- **Distinguish per-frame cost from one-time cost.** Something allocated,
  computed, or logged once at startup is free to be a little wasteful;
  the same operation inside the function called every frame is not. Read
  every finding through "does this run once, or N times per second,
  forever?" before deciding it matters.
- **A loop over "everything" (every particle, every grid cell, every
  vertex) is the single most sensitive place in a frame** — an extra
  branch, an extra function call that doesn't inline, or an extra memory
  read with poor locality here is multiplied by the count and paid every
  frame. A cost that's negligible called once is not negligible called
  10,000 times a frame.
- **Early-out before doing expensive work, not after.** Compute a cheap
  bounding check (visibility, distance, "is this even active") before the
  costly path, not as a discard applied to its result — this includes
  things like "don't bother projecting or lighting a point that's already
  known to be off-screen or fully transparent."
- **Watch for accidental O(n²).** A per-object loop that itself iterates
  every other object (naive collision, naive neighbor search) is fine at
  small counts and silently becomes the whole frame budget as the count
  grows — flag it even if today's count makes it invisible.

## Memory layout and cache behavior

- **Struct-of-arrays vs. array-of-structs matters when a hot loop only
  touches one or two fields of many.** Walking an array of large structs
  to read one field each drags the whole struct through cache for nothing;
  a parallel array of just that field is often the fix, when the loop
  count and struct size make it worth the complexity.
- **Sequential access beats "logically tidy" access.** A loop that walks
  memory in allocation order is fast; the same logic expressed as a
  recursive tree walk or a pointer-chasing linked structure, doing the
  identical work, can be many times slower purely from cache misses -
  worth flagging when the data structure choice, not the algorithm, is
  what's expensive.
- **Reallocation/resize inside a loop that runs every frame is a
  budget-breaker even when the total data is small** — a vector that grows
  one element at a time inside the per-frame loop reallocates repeatedly
  instead of being sized once outside it.

## Interpolation, easing, and timeline correctness

- **Every eased/interpolated value needs its endpoints checked explicitly:
  does it read as exactly the start value at t=0 and exactly the end value
  at t=1** (or the fixed-point equivalent), with no residual offset from an
  approximation. A curve that's "close enough" at the ends reads as a
  visible snap or a value that never quite settles.
- **A value driven by "elapsed time since an event" needs that origin
  point defined unambiguously** — measured from when, reset on what
  condition, and what happens if the same frame both starts and would
  otherwise end the same timed event (a zero-duration window).
- **Looping/wrapping values (an angle, a phase, a hue) need the wrap
  handled at every place the value is used, not just where it's produced**
  — a value correctly wrapped to `[0, 1)` that later gets a delta added
  and compared without re-wrapping is a common source of a once-per-loop
  glitch.
- **Two things meant to move together (paired animation channels, a value
  and its own rate-of-change) need one shared clock, not two independently
  advanced timers** that can drift apart under a variable frame rate.

## Determinism and testability of simulation code

- **A simulation/rendering function that reads wall-clock time or a random
  seed internally, rather than taking them as parameters, cannot be
  unit-tested for a specific instant or replayed deterministically.** This
  matters more here than in general application code — a simulation bug
  that only reproduces "sometimes, near frame 400" is far more expensive
  to debug than one a test can pin to an exact input.
- **Floating-point (or fixed-point) accumulation across many frames drifts
  — flag state that's updated as `state += delta` every frame indefinitely,
  with no periodic re-derivation from a stable source,** as a candidate for
  slow numerical drift over a long-running session.
- **A change to shared visual/simulation constants needs the actual
  before/after checked, not just "the code compiles."** A tuning constant
  is often trusted at face value; if the review has access to render or
  simulate the change, a plausible-looking value that produces a visibly
  wrong result is the most valuable class of finding this skill exists to
  catch.

## What NOT to flag

- Micro-optimizations in code that provably doesn't run in a hot path
  (setup, one-time initialization, error/rare branches) — correctness and
  readability win there, not cycles.
- A straightforward, unoptimized implementation in a part of the system
  with a generous budget and a small working set — premature optimization
  against a budget that isn't actually tight is its own kind of defect
  (added complexity for no measured benefit).
- Disagreement with a tuned-by-eye constant's specific value, absent a
  concrete argument for why the current value produces a wrong or broken
  result rather than just a different one — visual/gameplay tuning is a
  legitimate creative choice, not a bug, unless it demonstrably breaks
  something (an invariant, a test, an on-screen artifact).
