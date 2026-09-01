# Optimization Playbook

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 — see
[`README.md`](README.md) for the full set.

Everything else in this folder is specific to this board. This file is not —
it is the general-purpose techniques that came out of optimizing on it,
written so they travel to a different chip, project, or person. Each one is
grounded in a real measurement, mostly from
[`../Sand/Performance-Tuning-Attempts.md`](../Sand/Performance-Tuning-Attempts.md)
and [Display-and-Rendering.md](Display-and-Rendering.md), but the lesson
itself is not about falling sand or this particular display.

The one rule everything below serves: **a plausible-sounding explanation for
where the time goes is not the same as a measured one.**

---

## Measure by deleting — and know what you deleted

Stub the suspect to the cheapest correctness-breaking no-op that still runs,
leave a `TEMPORARY EXPERIMENT` comment, build, flash, measure, then revert —
win or lose. Isolate one variable per round. Don't filter a diagnostic
capture before reading it once: a crash piped through `grep -E
"FAILED|FAIL:"` discarded the one line that explained it; the unfiltered
rerun showed no crash at all.

The blind spot: deleting code removes its *work* and its *presence* — its
effect on code shape — at once, and attributes everything to the work. A
branch suspected of costing what it did: deleting it outright recovered
9.5%. Fixing the code *shape* it had caused (it had pushed its function past
the inlining threshold) while keeping every line of the branch recovered
12.1%. The branch's work was never the cost. **When a deletion recovers less
than a structural fix does, the cost was structural.**

---

## Know what kind of memory you actually have

"Optimize for cache locality" assumes a data cache. This chip has none —
SRAM is direct-access, so the only data-side lever is touching fewer bytes.
It does have a real 32 KB **instruction** cache for flash-resident code and
`const` data: two builds that never touched the hot function measured 3.2 ms
and 3.9 ms on the same benchmark, purely from unrelated code shifting flash
layout. Treat differences under ~20% as noise unless they reproduce. Check
which memory a chip actually has before applying "typical CPU" wisdom — a
datasheet-advertised secondary region here turned out to be a *slower* one
meant for deep-sleep wake stubs, not a performance tier.

---

## Inlining and call boundaries: verify, don't trust the attribute

`static inline` is a *request*; the compiler may decline and won't say so.
The only proof is `objdump -t file.c.obj | grep fn` — a function with its
own `.text.fn` section was not folded in. `__attribute__((always_inline))`
(with `static inline`, not instead of it) is the forcing mechanism; confirm
it worked by re-running the same check, not by trusting the name.

A "free" call can still cost a full register-spill: a helper called twice,
with eight `int` locals live across both calls, compiled to a 96-byte
prologue with ten registers pushed and popped every time — even when the
helper did nothing that call. Forcing it inline collapsed the frame to 32
bytes and moved the real number by ~2 ms on a ~16 ms budget. Disassemble the
caller's prologue before assuming a disproportionately expensive function's
cost is the logic and not the register traffic.

Fixing one un-inlined boundary can relocate the problem: the now-larger
caller may itself stop being inlined at *its* own call sites. This does not
compound forever for free — eventually a function gets folded into every one
of its call sites and the hot loop stops fitting the 32 KB cache, and the
technique that had been winning at every prior level makes *everything*
worse. Measure past the point a technique keeps winning, not just up to the
first win.

A readability refactor is an inlining change too — an early return respelled
as `if`/`else`, semantics-identical and checksum-identical, cost 14% of a
benchmark by changing what got folded. Re-measure a hot-path refactor on
real hardware like any other change, and ship the version measured (the fix
here was to remove the second call site, both faster and simpler than
either alternative).

The same discipline covers any silent automatic decision, not just
inlining: a memo is only a win if what arms it was actually expensive to
produce. Arming a carried run from *every* non-moving cell, cheap ones
included, cost 4.4% on a workload where the memo could never fire. Ask what
armed it, not just who reads it.

---

## Skip structures: shape, upkeep, and prove the ceiling first

A dirty/settled/active-tracking structure is only as good as its unit. Too
coarse re-examines unrelated work sharing a unit with something that
changed; too fine reintroduces the per-item cost it existed to avoid. Match
the unit's shape to the actual motion, not to convenience — a block made
taller in the dominant-motion direction reduced how often a boundary was
crossed at all, beating a faster boundary check. Prefer widening a cheap
*skip* check over narrowing an expensive *action*.

That is the cost of the wrong granularity. There is a second cost nobody
bills by default: **keeping the structure true**, paid by whoever changes
anything, not by whoever reads it. A per-row "no liquid here" flag correctly
skipped ~104 of 224 rows/step — and cost 33,426 bytes written per step to
stay honest, to save 104 already-cheap bitmask tests. Deleting the flag
outright beat every attempt at cheapening its invalidation, including one
that *proved*, by counter, it did strictly less work and still measured
slower from a compiler layout shift. Ask not just "is this the right unit"
but "who pays to keep it true, how often, compared to who reads it" — a
fixed-rate pass is almost always fine; per-event upkeep outnumbering the
units it skips by an order of magnitude is suspect on arithmetic alone,
cheap to check with host counters before writing code.

Before tuning an approximation, measure what a *perfect* version would
achieve — the unreachable exact answer, every cap removed, usually easy to
write because it's allowed to be slow. A three-cap approximation scheduled
for a re-sweep: the oracle matched the shipped output on two of three
workloads and beat it by 3% on the third — not nearly dead, nearly maxed
out. A cap that measures inert across its whole range is telling you about
the workload, not the cap.

---

## Provably safe, correct, or less-work isn't provably fast

A coarse, already-trusted check reused somewhere hotter, reasoned as a
strict superset of the precise version, is a valid correctness argument and
settles nothing about speed. It made every real-hardware measurement worse,
including a benchmark that had been comfortably passing — the
over-approximation cost more in needless re-examination than the precise
overhead it replaced. Always measure the performance question separately,
even when the safety argument is airtight; the rigor is what makes it
tempting to stop one step early.

When two independently-reasoned attempts to cheapen the *same* hot path
both regress, that's information: the path, not the implementation, is
probably the wrong shape. A periodic fixed-cost pass already existed (there
to decide what could sleep); letting it *pull* the same information
("anything change near me?") instead of an event *pushing* it turned
O(events) into O(fixed units, once per pass) and let the per-event machinery
come out entirely. Before a third attempt at the same mechanism, ask whether
a different one can answer the question from somewhere the cost is already
amortised.

The trap in that move: a pull-based check only observes state something
already recorded. A case with no equivalent bookkeeping of its own — an
external touch with no periodic pass covering it — needs an explicit poke,
or it silently breaks while every common-path test keeps passing.
Re-derive the design against the sharpest existing test for the mechanism
being replaced *before* writing code.

---

## Keep something in the benchmark set the change cannot touch

Benchmarks overlap by design — every one exercises the hot path — which
makes a whole class of regression undiagnosable: when everything moves at
once, "my change got slower" and "the binary landed differently in flash"
look identical. Keep at least one benchmark that provably cannot reach the
changed code, and read it first. A change confined to the liquid code
regressed everything 6–14%, indistinguishable at a glance from flash-layout
noise — three of seven tests placed no liquid at all and came back
byte-identical to the microsecond, turning "something's slower" into "the
liquid path" in one capture.

---

## Division by a power of two is not automatically a shift

`x / 16` compiles to a shift only if the compiler can prove `x`
non-negative; a plain signed `int` usually can't, and falls back to a
five-or-six-instruction round-toward-zero sequence. Cast to `unsigned`
wherever the value is genuinely always non-negative. Not a one-off fix:
grep for the same shape at every call site, not just the one profiled
first — this bug recurred twice more in the same codebase, once on a much
hotter path.

---

## Test and debug code shares your production memory budget

A `static` array in test-only code that compiles into the shipped self-test
image is not free — a buffer sized off a tunable constant taxes the RAM
available to *everything else in that boot*, and the failure looks like
hardware flakiness (a malloc failure somewhere unrelated) that survives a
clean power cycle unchanged. That survival is the tell: a deterministic
code cause survives a clean boot; a hardware fault does not.

A dynamically-allocated fixture that exceeds the budget is worse: a null
check written as assert-*before*-free skips a multi-malloc fixture's
earlier frees on failure, leaking everything already allocated and starving
every later test that boot — invisible on a host, where the failure path
never runs at all. This recurred across four unrelated files (two
selftest-only static buffers, two separate `--dev`-only save buffers) —
DIRAM backs `.text`/`.bss`/`.data` *and* heap together, so any static
anywhere taxes the same pool, and neither a clean compile nor a clean host
run says whether the largest contiguous block a device-only allocation
needs still exists after the addition. Diff `.bss`/`.data` size for
**every** build variant, not just release; trust
`heap_caps_get_largest_free_block()` over "total free heap."

---

## A host-validated win is a hypothesis until the target measures it

A host bisect measures the *host's* cost function, not necessarily the
target's. A branch hint moved a benchmark 25% on the host and 0.43% on the
embedded target — both real, confirmed by disassembly. The host's number
was dominated by whether cold code sat between the hot path's entry and its
work (a linker question); the target's by how many instructions the hot
path executes per call (a codegen question). The hint fixed the first
completely and barely touched the second. A commit a host bisect clears is
not necessarily innocent on the target, which may be paying for a different
line in the same window — confirm the attributed commit against the target
before trusting it.

---

## A benchmark sharing a console with a watchdog is measuring the console

A benchmark loop that never yields, plus a watchdog that prints to the same
console: if the timer brackets the whole loop with one start/stop pair and
a watchdog dump lands inside it, the dump's console I/O gets charged to the
benchmark. Measured inflation up to 2.6×, with nothing in the report to
flag it. It does not average out and repeating the capture does not reveal
it — with a fixed image and RNG seed the collision is deterministic, so a
repeat reproduces the exact same wrong number. Fix it in reporting, not
inside the measured code: flag/discard a collided row, or remove the
watchdog from the measuring image as its own change against its own fresh
baseline. Never feed the watchdog from inside the code being timed.

---

## Ask what a benchmark contains, not what it was built from

A counter answering "does this even run" has one failure mode: pointing it
at the benchmark's *constructor* instead of the benchmark. For a system
that evolves while it runs, the setup code is not a complete description —
it can create inputs the setup code proves are absent. A scene built from
an explicit material list seemed to rule out one expensive material; it was
there, three hundred cells of it, made by a *reaction* from two materials
that were on the list. Dump actual state at the moment of measurement, not
at the moment the fixture is built, and diff the two.

---

## Compile the system under test; do not copy it into the harness

Copying workload definitions into a bisect harness for convenience lets the
copy drift silently, in the one place drift is fatal — attributing a
regression against a workload that no longer matches the real one. Compile
the project's actual test source, unmodified, against shim headers that
stub the platform (no-op assertions, a host-backed clock, a stderr logger).
Diff the workload definitions between the two ends of the bisect range
first; where the harness models a real decision, validate it against
counters the real system already reports.

---

## Before optimising a query, check what the iteration already knows

A per-item query that looks like it needs an index sometimes doesn't,
because the loop asking it is already walking the structure the index would
describe. If an outer loop advances by exactly one step opposite a per-item
scan's own direction, each item's scan is the previous one shifted by one —
the answer is the previous answer plus one element, and a length-*k* walk
collapses to an increment: no allocation, no invalidation invariant. The
instinct is to reach for a spatial index, which answers "where are things,"
not "what has the walk already covered." The geometric prerequisite must be
checked, not assumed — here it held only when one direction component was
exactly zero, so the shortcut was switched off, not approximated, off that
axis.

---

## Related

- [`../Sand/Performance-Tuning-Attempts.md`](../Sand/Performance-Tuning-Attempts.md)
  — the campaign several of the techniques above were extracted from.
- [`../Sand/Tuning-At-a-Glance.md`](../Sand/Tuning-At-a-Glance.md) — the
  visual map of that campaign.
- [Display-and-Rendering.md](Display-and-Rendering.md) — the dirty-region
  tracking system, another case of the skip-structure lesson above.
- [Board-and-Memory.md](Board-and-Memory.md) — the memory budget these
  techniques operate inside of.
- [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md) — where the
  `objdump`/`nm` tools above come from.
