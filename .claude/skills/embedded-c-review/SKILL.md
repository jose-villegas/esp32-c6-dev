---
name: embedded-c-review
description: Review checklist for embedded/low-level C on resource-constrained hardware (no OS, no heap, fixed-point math, ISR-shared state, host-vs-device builds). Trigger on "review this C code for embedded correctness", "check for embedded/firmware bugs", "low-level C review".
---

# Embedded and low-level C review

A checklist for reviewing C meant to run on a microcontroller (or any
resource-constrained target) directly — not general application code, and not
a substitute for a project's own architecture docs (read those first; this
skill is the general expert knowledge underneath them, not a replacement for
knowing the specific codebase). Apply what's relevant to the code actually
under review — most files will only touch two or three of these categories.

## Memory and allocation

- **No unbounded or hidden allocation.** `malloc`/`new` inside a hot path,
  an ISR, or anything that runs every frame/tick is a defect on a target
  with no virtual memory to paper over fragmentation. Static buffers, pools,
  and arena allocators are the normal answer, not an optimization.
- **Every buffer has a known worst case.** A `char buf[N]` sized to "should
  be enough" rather than to a proven maximum is a stack-smash waiting for
  the one input nobody tested. Trace the actual bound, don't estimate it.
- **Stack depth is a real, finite resource.** Recursion with no proven
  bound, large stack-local arrays, and deep call chains from an ISR context
  all need the same question asked: what's the worst-case depth, and does
  it fit in the stack this runs on (often a few KB, not the OS-thread
  megabytes a reviewer's habits assume)?
- **Struct layout and padding are visible costs.** On a RAM-starved target,
  reordering fields to kill padding, or noting where a `packed` attribute
  changes alignment-trap behavior on the target ISA, is a legitimate review
  comment, not bikeshedding.

## Fixed-point and integer arithmetic

- **Every fixed-point value has an implicit scale (a "Q format") — verify
  it's consistent across an expression.** The classic bug is mixing a Q12
  value with a Q8 one without converting first; it compiles clean and
  produces a value that's off by exactly `2^4`, invisible without knowing
  the intended scale.
- **Multiplication of two fixed-point values needs a wider intermediate.**
  `int32_t` × `int32_t` in Q-anything overflows the moment both operands
  are non-trivial; the fix is casting one operand to `int64_t` (or a
  documented wider type) before the multiply, not after.
- **Division truncates toward zero; right-shift of a signed value rounds
  toward negative infinity.** These are NOT interchangeable for negative
  fixed-point values even though `x >> 1` and `x / 2` look equivalent for
  positive ones — flag `>>` used as a "fast divide" on a value that can be
  negative.
- **Signed integer overflow is undefined behavior in C, not wraparound.**
  A compiler is entitled to assume it never happens and optimize on that
  assumption; code relying on `INT_MAX + 1` wrapping to `INT_MIN` is a bug
  even if it "worked" in one build. Unsigned wraparound is well-defined and
  fine; signed is not.
- **Watch implicit promotion.** `uint8_t + uint8_t` promotes to `int`
  before the addition on most platforms — usually harmless, but a
  comparison or shift that assumed the narrower type can silently change
  meaning (especially sign) after promotion.

## Timing, state, and ISR/shared-data correctness

- **Anything read by one context and written by another (ISR vs. main
  loop, or two cooperating routines with no lock) needs `volatile` on the
  shared variable, at minimum** — and `volatile` alone is not a
  synchronization primitive; a multi-step update (read-modify-write, or a
  struct with several fields) still needs a real critical section or
  atomic swap to avoid a torn read.
- **Time is an input, not an ambient fact.** Code that calls a
  wall-clock/timer function internally instead of taking `now_ms` (or
  equivalent) as a parameter is both harder to test and a common source of
  subtle timing bugs when the same logic runs at different call
  frequencies. Flag it whether or not the project has an explicit
  "pass time in" convention documented — it's good practice generally.
- **Debounce, hysteresis, and "has N frames/ms elapsed" logic needs an
  explicit state variable, not an assumption about call frequency.** Code
  that assumes "this runs every frame" breaks the moment frame pacing
  varies (a slow frame, a paused loop, a host-test harness driving the
  same function at an arbitrary rate).
- **A watchdog-fed or ISR-driven system must not block.** A `while` loop
  waiting on a condition another interrupt is supposed to set, called from
  inside that same interrupt's own priority level or above, is a deadlock
  in waiting.

## Undefined and unspecified behavior

- **Strict aliasing.** Reinterpreting one pointer type as another
  (`float*` as `uint32_t*` for a bit-hack, a network/protocol buffer cast
  to a struct pointer) is UB unless done through a `union`, `memcpy`, or a
  compiler-specific exemption — flag raw pointer-cast reinterpretation.
- **Uninitialized reads.** A struct or array declared without an
  initializer and partially filled before use — especially one crossing a
  function boundary or written to flash/a file — can read as garbage in
  release builds even when a debug build happened to zero it.
- **Out-of-bounds by one.** Off-by-one in a loop bound, a `<=` where `<`
  was meant (or vice versa) against an array size, and forgetting a
  null/sentinel terminator's own slot in a size calculation are the
  single most common class of embedded memory-safety bug — check every
  loop bound and buffer-size arithmetic explicitly, don't skim it.
- **Returning a pointer/reference to a stack-local.** Easy to miss when
  the local is inside a nested block or a helper the caller doesn't see
  the body of.

## Portability across build targets

- **Code meant to build for both the real target and a host test runner
  (or two different MCU families) needs its hardware-specific assumptions
  made explicit** — endianness, `int` width, pointer size, whether
  floating point is hardware-accelerated or software-emulated (and
  therefore expensive) on the target. A `sizeof(int) == 4` or
  little-endian assumption baked in without a comment is a portability
  landmine for whoever ports this next.
- **Conditional compilation (`#ifdef`) that changes behavior, not just
  what's included, needs both branches reviewed** — it's easy to fix a bug
  in the branch currently being tested and never re-check the other one.

## What NOT to flag

- Style preferences with no correctness or performance implication —
  that's a different pass, not this one.
- A deviation from "how a desktop application would typically do this"
  that exists BECAUSE of a documented constraint (no heap, no OS, a fixed
  frame budget) — that's the correct call for the environment, not a code
  smell to push back on.
- Missing input validation for values that are compile-time constants or
  are already validated at a boundary earlier in the same call chain —
  trace the actual data flow before flagging a "missing" check.
