# Optimization Playbook

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 — see
[`README.md`](README.md) for the full set.

Everything else in this folder is specific to this board. This file is not —
it is the general-purpose techniques that came out of optimizing on it,
written so they travel to a different chip, a different project, or a
different person. Each one is grounded in a real measurement from this repo
(the specific numbers live in [Simulation-Lessons.md](Simulation-Lessons.md)
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

- [Simulation-Lessons.md](Simulation-Lessons.md) — the falling-sand app's
  full discovery narrative, including the specific investigation several of
  the techniques above were extracted from.
- [Display-and-Rendering.md](Display-and-Rendering.md) — the dirty-region
  tracking system, another concrete case of the coarse-skip-structure
  lesson above.
- [Board-and-Memory.md](Board-and-Memory.md) — the memory budget these
  techniques operate inside of.
- [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md) — where the
  toolchain used for `objdump` above comes from.
