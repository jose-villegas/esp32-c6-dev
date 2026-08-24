# Optimization Playbook

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 — see
[`README.md`](README.md) for the full set.

Everything else in this folder is specific to this board. This file is not —
it is the general-purpose techniques that came out of optimizing on it,
written so they travel to a different chip, a different project, or a
different person. Each one is grounded in a real measurement from this repo
(the specific numbers live in [`../Sand/Simulation-Lessons.md`](../Sand/Simulation-Lessons.md)
and [Display-and-Rendering.md](Display-and-Rendering.md)), but the lesson
itself is not about falling sand or this particular display.

The one rule everything below serves: **a plausible-sounding explanation for
where the time goes is not the same as a measured one.** Every technique here
exists because a first, second, or third guess about the bottleneck turned
out to be wrong, and the thing that actually found it was cheaper and more
direct than the guess had been.

---

## Measure by deleting, not by reasoning

The fastest way to find out whether some piece of code is expensive is not to
read it and estimate — it is to make it do nothing, keep everything else
identical, and re-measure on the real target. If the number does not move,
that code was never the cost, no matter how expensive it looks on paper. If
it does move, you now have an exact figure instead of a guess.

Concretely: comment out the suspect logic, replace it with the cheapest
correctness-breaking stub that still lets the program run (`return false;`,
`return 0;`, a bare pointer write with no bookkeeping), leave a `TEMPORARY
EXPERIMENT` comment explaining what it disproves and that it must be
reverted, build, flash, measure, then actually revert it — win or lose. Treat
the revert as part of the experiment, not a follow-up task: an experiment
that stays half-applied is how a codebase ends up with silently broken
correctness "for speed."

This sounds slow compared to reasoning about it, but it is not, because
reasoning about it is what generates the wrong theories in the first place.
One investigation ruled out three separate plausible-sounding causes this
way — a load-computation loop, an entire friction/slide code path, and an
over-eager wake condition — each bypassed in isolation and measured
unchanged, before the real cause (below) turned up. Every one of those three
would have been a reasonable thing to spend an afternoon "optimizing" based
on how it read.

**Corollary: isolate one variable per experiment.** Bypassing two suspects at
once and seeing an improvement tells you *one of them* mattered, not which.
It is tempting to combine fixes to save round-trips; do the round-trips
instead, or you will ship a fix for the wrong thing.

**Corollary: don't filter a diagnostic capture before you've read it
once.** A device crash was first captured through a command piped to
`grep -E "FAILED|FAIL:"`, to keep the terminal output short - which
silently discarded every line that did not match, including the actual
panic diagnostics (a watchdog-trigger warning, in that case) that would
have explained what happened. The filtered capture left nothing to go on
but a mangled line and a fresh boot banner, which read like a mysterious
crash; the full, unfiltered capture on a retry showed a normal
completion with no crash at all, settling in seconds a question the
filtered version could not answer no matter how long it was stared at.
Grep the *saved* output after the fact, as many times as needed - never
the live stream on the one run that might explain a failure, since
there is no way to know in advance which line that will be.

---

## Know what kind of memory you actually have

"Optimize for cache locality" is received wisdom from a machine with a data
cache in front of RAM. Not every chip has one. This one does not: SRAM
(DIRAM) is accessed directly, at the speed a cache would give on a bigger
CPU, with no tier above it. Laying out a data structure for better cache
hits has nothing to bite on here — the only way to move less data is to
touch fewer bytes, full stop, not to touch the same bytes in a friendlier
order.

What this chip *does* have is a 32 KB **instruction** cache, for code and
`const` data running from flash. That one is real, and it explains a
specific, otherwise-baffling symptom: two builds that never touched the hot
function measured 3.2 ms and 3.9 ms on the exact same benchmark, because
unrelated code elsewhere shifted where things landed in flash and changed
the cache hit rate. Treat performance differences under about 20% as noise
unless they reproduce, and give performance regression tests enough margin
that a rebuild alone cannot flip them.

The general lesson: **before applying a technique because it is good
practice on "a typical CPU," check which memory the chip in front of you
actually has, and for what.** A generic secondary memory region existing
does not mean it is faster — one datasheet claim checked directly here
turned out to describe a *slower* region meant for deep-sleep wake stubs,
not a performance tier. Moving hot data there on the assumption that "more
memory tiers = faster" would have been a regression, caught only by reading
the actual spec rather than assuming the usual hierarchy applied.

---

## `static inline` is a request, not a guarantee — verify with objdump

A function marked `static inline` in a header is *offered* to the compiler
for inlining at every call site. At real optimization levels the compiler is
still free to decline, based on its own size/complexity heuristics, and it
will not tell you it did. The only way to know for certain is to look at
what actually got built:

```sh
riscv32-esp-elf-objdump.exe -t path/to/file.c.obj | grep function_name
```

If the function shows up with its own `.text.function_name` section, it was
compiled as a real, standalone function, not folded into its callers. From
there, disassembling that section directly:

```sh
riscv32-esp-elf-objdump.exe -d --section=.text.function_name path/to/file.c.obj
```

answers the next question directly instead of by inference — how large is
the stack frame, how many registers does the prologue save, are there real
`call`/`jal`/`jalr` instructions to other functions. This is not
toolchain-specific to this project; every GCC/Clang cross-compiler ships an
`objdump` (or the `llvm-objdump` equivalent) and the technique applies
anywhere a `.obj`/`.o` file exists to inspect.

`__attribute__((always_inline))` (combined with `static inline`, not instead
of it) is the actual forcing mechanism when the plain hint is being
declined. Confirm it worked the same way you found the problem — re-run the
same `objdump -t` and check the standalone symbol is gone — rather than
trusting the attribute name and moving on. In one case here, adding the
attribute to a function that was *already* wrapped in another experiment
made no measured difference; only checking the disassembly revealed that the
surrounding experiment had made the function's body trivial enough that
being inlined or not no longer mattered, which is a different finding than
"the attribute doesn't work."

---

## A "free" function call can still cost a full register-spill

A function that does almost nothing can still be expensive to *call*, if
enough live values have to survive the call boundary. On a standard
register-window-free calling convention (this chip's RISC-V ABI included),
any value that needs to exist both before and after a call must live in a
callee-saved register, and callee-saved registers must be pushed in the
callee's prologue and popped in its epilogue — paid on every single call,
regardless of what the call itself computes.

The concrete shape of this, found by disassembly rather than guessed: a
small helper function, called twice, with eight `int` locals that had to
survive both calls to combine their results afterward. The caller's compiled
prologue: a 96-byte stack frame, ten callee-saved registers pushed and
popped, on every invocation — for a helper whose actual job, most of the
time, was to notice nothing needed to happen and return. Forcing that helper
inline (see above) collapsed the frame to 32 bytes and two registers, and
the real-hardware measurement moved by nearly 2 ms on a ~16 ms budget from
that alone.

**The signature to watch for:** a function is disproportionately expensive
relative to what its logic obviously does, and it either calls, or is called
by way of, another small function across a real (non-inlined) boundary,
with several values needing to stay alive across that boundary. Disassemble
the caller's prologue before assuming the cost is "just" the logic —
sometimes the logic is nearly free and the register traffic isn't.

---

## Inlining does not compound for free — there is a ceiling, and it moves

Fixing one un-inlined call boundary can just relocate the problem one level
up the call chain, not remove it: the function that used to call the
now-inlined helper is itself now larger, and the compiler may stop inlining
*that* one at its own call sites in turn. Chasing this is a legitimate
technique — each fix is cheap to try and easy to verify with the same
`objdump -t` check — but it does not go on forever for free.

Eventually, forcing another level of the chain inline makes a large function
get inlined at every one of *its* call sites, and if there are enough of
them, the function containing all of it grows past what fits in the
instruction cache described above. When that happened here, the fix that had
been winning at every previous level made *everything* measurably worse —
including code paths that barely touched the function in question — because
the whole hot loop no longer fit, and cache misses on ordinary execution
started costing more than the saved calls had ever been worth.

There is no way to calculate in advance where this ceiling sits; the cache
size is known, but how much of it a given inlining decision consumes is not
something to compute by hand from source. **Measure past the point where a
technique keeps winning, not just up to it** — the same fix, applied one
level too far, is not a smaller version of the same win, it can be a net
loss with the opposite sign.

---

## Division by a power of two is not automatically a shift

`x / 16` for a compile-time-constant power-of-two divisor *can* compile to a
single shift instruction — but only if the compiler can prove `x` is
non-negative. For a plain signed `int` parameter with no visible range
information at the call site, it often cannot, and falls back to the
full "round toward zero" sequence: extract the sign, conditionally adjust,
then shift — five or six instructions where one would do. If the value's
range is genuinely known to be non-negative (grid coordinates, array
indices, anything that can never be negative in practice), say so
explicitly — cast to `unsigned` and shift directly, or let the compiler see
enough range information to prove it itself. Confirmed worth doing here, but
modest on its own (roughly 1.6 ms out of a much larger gap) — worth
applying, but not a substitute for finding the actual dominant cost.

**This is not a one-off — check every call site with the same shape, not
just the one that got measured first.** The same bug, on the same kind of
compile-time-constant power-of-two divisor, turned up twice more in this
codebase after the finding above: once in a function on a much hotter
path (called once per grain move rather than once per frame), where it
cost nearly double — ~2.7-3.1 ms on its own — and once in a much colder
one (once per row), where it was still worth the one-line fix but barely
measurable. A function being "already optimized" for one cost does not
mean a sibling call site sharing the same divisor and the same
plain-`int` parameter got the same treatment — grep for the pattern
across the file, not just the function that was being profiled.

---

## A coarse skip-structure needs a shape that matches the actual work

A dirty/settled/active-tracking structure (skip re-examining something that
hasn't changed) is only as good as the *unit* it tracks at. Too coarse a
unit (a whole row, a whole large region) forces real, unrelated work to be
re-examined just because it shares that unit with something that changed.
Too fine a unit (checking every single element's neighbors individually)
can reintroduce the exact per-item cost the structure was meant to avoid, if
checking "does this need to wake" costs almost as much as doing the work
would have.

Concretely, moving from row-granularity to a 2D block grid fixed a real
problem (row-shaped tracking forced provably-idle work to be re-examined
whenever anything else in its row was busy) — and the *shape* of the block
mattered too, not just its existence: since the motion being tracked was
overwhelmingly one direction (gravity, vertical), making the block taller
in the direction of dominant motion reduced how often a boundary had to be
crossed at all, which is cheaper than making the boundary-crossing check
itself faster. **Match the tracking unit's shape to the shape of the actual
motion or change pattern, not to convenience or symmetry.**

The other lesson from the same change: prefer widening a *skip* check over
narrowing an *action*. Checking "is there anything at all near this
boundary that could react" cheaply, and only doing the expensive real work
if that first check says yes, was a bigger win than making the expensive
work itself faster — because the cheap check is what runs the overwhelming
majority of the time.

---

## A change proven safe is not the same as a change proven fast

A tempting shortcut once a coarse, conservative version of some check
already exists elsewhere in the codebase: reuse it somewhere hotter,
reasoning that "it can only ever do *more* than the precise version, so
it cannot introduce a correctness bug" — and stopping there, treating
that reasoning as if it settled the performance question too. It does
not. A change that is a provable superset of the correct behaviour (wakes
at least everything the precise version would have, checks at least
everything that needed checking, re-examines at least everything that
changed) can only fail in the *safe* direction — but "safe direction" and
"cheap direction" are different axes, and proving one proves nothing
about the other.

Concretely: a coarse, always-expand-by-one-unit wake call already existed
and was already trusted (used once per batch of changes elsewhere in the
same codebase). Reusing it to replace many precise, individually-checked
calls with one coarse call per batch was reasoned through carefully
first — the coarse version is a strict superset of the precise one, so
nothing could be left incorrectly unwoken. That reasoning was entirely
correct, and the change still made every real-hardware measurement worse,
not better, including regressing a benchmark that had been comfortably
passing. The coarse call's unconditional over-approximation cost more
(in needless re-examination of things that did not actually need it)
than the precise per-call overhead it removed.

**The rule this leaves behind: "provably cannot be wrong" is a
correctness argument, not a performance one — always measure the
performance question separately, on the real target, even when the
safety argument is airtight.** It is tempting to skip the measurement
step precisely *because* the safety reasoning feels rigorous; that
rigor is exactly what makes it easy to stop one step too early.

---

## If a hot path resists optimizing in place, stop optimizing it in place

Two separate, independently-reasonable attempts to cheapen the same
per-event notification (a mover telling a neighbour something changed,
once per event) both regressed on real hardware — one for an understood
reason, one for a reason that was never fully pinned down. After the
second failure, the useful move was not a third, cleverer variant of the
same fix. It was asking whether the notification needed to be a *push*
at all.

It did not. The system already had a periodic, fixed-cost pass running
regardless of activity (there to decide what could go back to sleep).
That pass could just as well *pull* the same information — "did
anything change near me since I last checked?" — instead of relying on
the event that caused the change to *push* it there. Once the check
moved from "once per event, cost scales with event count" to "once per
fixed unit per pass, cost is constant regardless of event count," the
whole class of problem the two failed attempts were fighting (how to
make an event-scaled cost cheap) stopped applying: there was no longer
an event-scaled cost to make cheap. The per-event mechanism, along with
all the precision machinery it had needed to stay affordable, could be
deleted outright rather than tuned further.

**The signal to watch for: repeated, independently-motivated failures to
cheapen the same hot path are information, not bad luck.** Each
individual attempt can be perfectly well-reasoned and still lose,
because the path itself — not any specific implementation of it — may be
the wrong shape for the cost it is trying to pay cheaply. Before a third
attempt at the same mechanism, ask whether a *different* mechanism could
answer the same question from somewhere the cost is already bounded
(a pass that already runs at a fixed rate, a structure that already
visits every unit once) rather than trying to bound a per-event cost
directly. Push-to-pull is one concrete shape this takes — an event
telling every interested party immediately, versus interested parties
periodically asking whether anything relevant happened — but the general
move is the same regardless of shape: relocate the check to somewhere
its cost is already amortised, rather than continuing to shave a cost
that scales with the thing you cannot control (how often the event
happens).

**The correctness trap to watch for when making this move:** a pull-based
check only observes state that something already recorded. If the thing
that needs to notify a neighbour has no equivalent of the event-driven
mechanism's own bookkeeping — an external touch with no periodic pass of
its own already covering it, say — that specific case still needs an
explicit, if cheap, poke, or it silently stops working for exactly that
case while every test built around the *common* path keeps passing. Find
this by re-deriving the design against the sharpest existing test for
the mechanism being replaced (a corner-case test, not the average case)
before writing code, not by hoping the test suite catches it after.

---

## Test and debug code shares your production memory budget

On a target with one shared address space and no virtual memory, a `static`
array sized off a tunable constant in test-only code is not free just
because the test itself never runs in production. If that source file
compiles into the same binary the product ships (common when test suites are
built into a self-test firmware image, as they are here), a static buffer
that grows when a constant is tuned reduces the RAM available to *everything
else in that boot*, including code paths with no relation to what was being
tuned. On a device where the display framebuffer alone claims the large
majority of RAM, the remaining margin can be small enough that this bites
immediately and non-obviously: a malloc failure in an unrelated, previously
reliable allocation, survivable across a full power cycle, that looks like a
hardware fault and is actually a sizing bug three files away.

**When a symptom looks like flaky hardware but survives a clean power
cycle unchanged, suspect a deterministic code cause before a nondeterministic
hardware one** — a clean boot resets everything hardware-side; if the
failure is still there, something in the built image caused it. And when
tuning a constant that feeds a test fixture's array size, check what that
constant costs in the worst case you intend to try, not just the case you
are currently running.

---

## Related

- [`../Sand/Simulation-Lessons.md`](../Sand/Simulation-Lessons.md) — the falling-sand app's
  full discovery narrative, including the specific investigation several of
  the techniques above were extracted from.
- [Display-and-Rendering.md](Display-and-Rendering.md) — the dirty-region
  tracking system, another concrete case of the coarse-skip-structure
  lesson above.
- [Board-and-Memory.md](Board-and-Memory.md) — the memory budget these
  techniques operate inside of.
- [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md) — where the
  toolchain used for `objdump` above comes from.
