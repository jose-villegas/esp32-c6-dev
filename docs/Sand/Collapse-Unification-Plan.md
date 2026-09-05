# Plan: one collapse primitive behind water rain and acid rain

Status: proposed, not started, and **largely overtaken** — read the
recommendation before the rest.

## Why

Two functions in `sand_reactions.c` do the same thing in different words:

| | `step_one_condensing_cell()` | `step_one_acid_rain_cell()` |
|---|---|---|
| block | 2×2 | 2×2 |
| accepts | all four cells the caller's own material | every cell `MAT_STEAM` or `MAT_GAS`, exactly two steam |
| chance | `r->condenses`, override `s->condenses` | `SAND_ACID_RAIN_CHANCE`, override `s->acid_rain` |
| product | `r->condenses_to`, top-left cell | `MAT_ACID` or `MAT_WATER` 50/50, top-left cell |
| rest | emptied | emptied |
| ratio | 4 → 1 | 4 → 1 |

Both bounds-check a square, test its composition, roll a chance, then
collapse it into its top-left cell and empty the rest.

This plan was written when acid rain used a 4×4 window collapsing to a
2×2, and its main argument was that unifying would turn "should acid rain
be 2×2?" from a rewrite into an argument. **Acid rain was then redesigned
to 2×2 directly on main**, which answered that question and delivered most
of the premise. What is left is a genuine duplication, but a much smaller
prize than when this was drafted.

## What would unify

One helper taking the composition rule as data:

```c
static inline bool collapse_square(sand_t *s, int x, int y, int w, int h,
                                   uint8_t mat_a, int min_a,
                                   uint8_t mat_b, int min_b,  /* MAT_EMPTY = unused */
                                   int chance, uint8_t product);
```

Condensation passes `{mat, 4, MAT_EMPTY, 0}`; acid rain passes
`{MAT_STEAM, 2, MAT_GAS, 2}`. Both then share the bounds check, the scan,
the roll, and the collapse.

**No function pointers.** A predicate callback is the obvious
generalisation and the wrong one: this runs per candidate cell in the
reacting-row scan, and an indirect call there is exactly the cost this
codebase measures and refuses.

Two things the helper does not absorb, and they are the reason this is
less tidy than it looks:

- Acid rain's product is **not fixed** — it resolves 50/50 between acid
  and water per firing, so `product` cannot simply be a `uint8_t` argument
  without either a second parameter or the caller doing its own roll and
  passing the winner in.
- Dispatch differs. Condensation is driven per material off `r->condenses`
  in the reaction table; acid rain is a standalone check with a global
  constant. Unifying that means giving acid rain a reaction-table row,
  which changes what `dump_reactions.c` emits. Out of scope.

## Correctness strategy, if it is done

1. **Byte-exact pin first.** Hash a fixed scene's grid through both paths
   with `grid_fingerprint`, record it on current code, require it
   unchanged after.
2. Existing coverage green untouched. A test needing an edit is a signal
   the refactor changed behaviour — stop and report.
3. Verify the pin is load-bearing by perturbing the helper and watching it
   go red.
4. Measure the hot path before and after on the repo's own sand harness.
   If the generic loop specialises worse than the two hand-written ones,
   abandon rather than accept a real cost for an internal tidiness win.

## Recommendation

**Probably skip.** The question this existed to unlock has been answered
elsewhere; what remains is deduplicating two short, correct, tested
functions whose products differ in kind. The 50/50 residue means the
shared helper needs an awkward extra seam precisely where the two callers
diverge, which is the usual sign that two things are similar rather than
the same.

Worth revisiting only if a third collapse-shaped reaction appears — at
that point the duplication stops being a pair and starts being a pattern.
