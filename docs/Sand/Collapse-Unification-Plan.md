# Plan: one collapse primitive behind water rain and acid rain

Status: proposed, not started. Pure refactor — no behaviour change is
intended, and the plan is written so that "no behaviour change" is a
testable claim rather than an aspiration.

## Why

Two functions in `sand_reactions.c` do the same thing in different words:

| | `step_one_condensing_cell()` | `step_one_acid_rain_cell()` |
|---|---|---|
| block | 2×2 | 4×4 |
| accepts | all four cells the caller's own material | every cell `MAT_STEAM` or `MAT_GAS`, ≥2 of each |
| chance | `r->condenses`, override `s->condenses` | `SAND_ACID_RAIN_CHANCE`, override `s->acid_rain` |
| product | `r->condenses_to` in the top-left **1** cell | `MAT_ACID` in the top-left **2×2** |
| rest | emptied | emptied |
| ratio | 4 → 1 | 16 → 4 |

Both are: bounds-check a square, test its composition, roll a chance,
then collapse it into its own top-left quarter and empty the remainder.
The ratio is 4:1 in both, and the Reaction-Table already describes acid
rain as "the same 4-cells-into-1 collapse ratio `condenses` uses, scaled
to a 16-cells-into-4 block" — the relationship is already documented, it
just is not expressed in the code.

The payoff is not tidiness. It is that **the block size and the
composition rule become parameters**, so the open question about acid
rain — whether it should qualify on a 2×2 rather than a 4×4 — turns into
changing an argument instead of rewriting a function. That question is
live (see "Open question" below) and this is what makes it cheap to
answer.

## What unifies

A single helper, roughly:

```c
static inline bool collapse_square(sand_t *s, int x, int y, int w, int h,
                                   int edge,            /* 2 or 4 */
                                   uint8_t mat_a, int min_a,
                                   uint8_t mat_b, int min_b,  /* MAT_EMPTY = unused */
                                   int chance, uint8_t product);
```

- **Bounds**: `x + edge - 1 < w`, same shape as both today.
- **Composition**: every cell must be `mat_a` or `mat_b`; counts must meet
  `min_a` / `min_b`. Condensation passes `{mat, edge*edge, MAT_EMPTY, 0}`;
  acid rain passes `{MAT_STEAM, 2, MAT_GAS, 2}`.
- **Collapse**: the top-left `(edge/2)²` cells take `product` via
  `place_reacted()`; every other cell is emptied via `place_cell()`. That
  reproduces both yields exactly — 2×2 gives 1, 4×4 gives 4 — and makes
  the 4:1 ratio structural rather than coincidental.

**No function pointers.** A predicate callback would be the obvious
generalisation and is the wrong one here: this runs per candidate cell in
the reacting-row scan, and an indirect call in that loop is exactly the
kind of cost this codebase measures and refuses. The two-material +
minimum-counts descriptor covers both callers with plain data.

## What does NOT unify, deliberately

- **Dispatch.** Condensation is driven per material off `r->condenses` in
  the reaction table; acid rain is a standalone check with its own global
  constant. Unifying *that* means giving acid rain a reaction-table row,
  which changes what `dump_reactions.c` emits and what the generated
  pairwise table says. Out of scope — this plan unifies the collapse, not
  how each caller is reached.
- **The chance and its override.** Each keeps its own constant and its own
  `sand_set_*()`. They are tuned independently and mean different things.
- **`condenses_to` staying a per-material field.** Acid rain's product is
  a constant; condensation's is data. The helper takes a `product`
  argument and neither caller changes.

## Correctness strategy

The whole value depends on this being invisible, so:

1. **Byte-exact pin first, before any refactor.** Add a test that runs a
   fixed scene through both paths and hashes the resulting grid — the
   suite already has `grid_fingerprint` tooling for exactly this. Record
   the hash on the current code, then require it unchanged after.
2. Existing coverage stays green untouched. If any existing test needs
   editing, that is a signal the refactor changed behaviour — stop and
   report rather than updating the test.
3. Watch the pin fail: temporarily perturb the helper (e.g. empty one
   extra cell) and confirm the fingerprint test goes red, so it is known
   to be load-bearing.

## Performance

`step_one_reacting_row()` calls these per candidate cell, so this is a hot
path. The refactor must not regress it.

- The composition scan is already O(edge²) in both callers; the helper
  does not change the work, only where it is written.
- The risk is the compiler failing to specialise the generic loop as well
  as it did the two hand-written ones. `edge` is a compile-time constant
  at both call sites and the helper is `static inline`, so it should
  specialise — but "should" is not evidence.
- **Measure with the repo's own sand performance harness before and
  after** and put the numbers in the commit. If the generic version is
  measurably slower, the honest outcome is to abandon this plan rather
  than to accept a real cost for an internal tidiness win.

## Open question this unlocks

Whether acid rain should qualify on a **2×2** rather than a 4×4. The
condition would then read the way it already sounds — "two gas and two
steam make acid" — and would genuinely share its shape with water rain.

Expect it to make acid rain **much more frequent**, not less: four clean
cells are enormously more common than sixteen, and the 4:1 ratio is
unchanged, so more qualifying events at the same ratio means more acid
produced. It also weakens contamination-style fixes, which work precisely
because sixteen cells all have to be clean.

So this is a behaviour change to measure on its own, after the refactor,
against the same loop-termination metric used for
`SAND_ACID_DILUTE_NO_BYPRODUCT_CHANCE`: live acid at step 400 and 800,
and surviving water. Not to be folded into the refactor commit.

## Recommendation

Worth doing **only as a precursor to changing the acid-rain condition**.
As pure cleanup it buys little: both functions are correct, tested, and
short. As the thing that turns "should acid rain be 2×2?" from a rewrite
into an argument, it pays for itself immediately — and the fingerprint pin
means the refactor can be verified free of behaviour change before any
tuning question is opened on top of it.

If the 2×2 question is not going to be asked, skip this.
