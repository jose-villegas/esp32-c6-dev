# Acid bubble investigation — delegation plan

## Goal

`acid_bubble()` (sand_reactions.c) is supposed to make acid look carbonated:
a rare, ambient chance for an exposed acid cell to throw a grain of itself
one step up-and-out via `sand_impulse()`. The mechanism has been rebuilt
once already this session (moved from `move_liquid_grain()` into the
reactions pass specifically because the main sweep skips sleeping blocks,
and a calm puddle sleeps within a few steps — see the function's own
comment in sand_reactions.c for the full history) and retuned twice
(CHANCE/SPEED constants in sand.h). Despite that, it has not been
confirmed visible on a real device. Host tests pass
(`test_acid_bubbles_do_not_favour_one_wall`,
`test_acid_bubbles_still_fire_once_the_block_is_asleep`, both in
suite_sand.c) — so whatever is wrong is either device-only, or the effect
is real but not perceptible.

**This is the same shape of bug as the original one**: a host test that
never enabled sleeping missed a mechanism that only broke once sleeping
was real. Do not trust "tests pass" as proof here either — go back to the
device with instrumentation, the same way the sleeping bug and the crash
investigation earlier this session were actually resolved.

## What's already confirmed (don't re-derive these)

- `sand_step_reactions()` is NOT gated by block-sleeping. Its only early
  exit is a whole-board flag check (`may_have_burning`,
  `may_have_dissolver`, etc.) — see line ~3083. `may_have_dissolver`
  should be true any time acid exists anywhere on the grid.
- `step_one_reacting_row()` has no per-cell or per-row skip beyond "is
  this cell empty" — it visits every non-empty cell in every row, every
  step this pass runs. No dirty-row filtering, no sleep check.
- `acid_bubble()` is reached from the exact same `r->dissolves` branch
  `step_one_dissolver_cell()` already uses (acid is the only material
  with `.dissolves` set), gated further to `MAT_ACID` specifically.
- The impulse buffer (`APP_IMPULSE_MAX` = 2048 entries, app_sand.c) is
  sized for detonate's blast annulus (needs up to 1961 entries for a
  50px-radius blast) — comfortably more headroom than ambient bubbling
  should ever need on its own.
- `step_impulses()` (sand.c) has real re-acquisition logic specifically
  for water/acid entries — a documented fix for a measured 44.5% loss
  rate of queued water/acid entries to ordinary gravity movement between
  queue time and processing time. This machinery is mature, not naive.
- **This exact device has a documented history of `impulse_buf`'s
  malloc silently failing** (see `APP_IMPULSE_MAX`'s own comment,
  app_sand.c ~line 249-270) — a live serial capture once found detonate
  becoming a total, silent no-op because of it. `sand_impulse()` and
  `sand_explode()` both open with `if (s->impulse_buf == NULL) return;`
  — silent by design. This is the single cheapest thing to rule out
  first.

## Hypotheses, ranked by cheapness-to-check first

### H1 — impulse_buf failed to allocate on this device/build (check FIRST)

Costs nothing: boot the current build under `monitor.sh` and look for
`ESP_LOGE(TAG, "Could not allocate the %d-entry blast buffer...")` in the
boot log (app_sand.c, right after the `impulse_buf = malloc(...)` call).
If this fires, EVERY impulse-based mechanic (detonate, splash, bubble) is
silently dead — not an acid-specific bug at all, and the fix is a heap-
budget one (see `SAND_IMPULSE_BUDGET_BYTES`'s own comment for how that
was diagnosed before), not anything in acid_bubble() itself.

If detonate visibly works on the device you're testing on, this is
already ruled out (detonate hits the same buffer). Confirm that first —
it's one tap.

### H2 — the roll's precondition rarely or never passes on a real puddle

`acid_bubble()` requires the cell directly AGAINST gravity (`s->last_step_dx/dy`)
to be empty. On a real device, gravity direction is constantly dithered by
tilt (per the user: "the device isn't flat so gravity is pointing to
landscape position generally but I also tilt the device all time"). Two
concrete ways this could suppress the effect far below what host tests
(which use fixed, simple gravity) would predict:

- If the puddle is continuously stirred by tilt, its OWN surface may
  rarely be the top-most exposed layer for more than a frame or two —
  acid cells might get buried/uncovered by other material or by the
  puddle's own churn before the roll has a chance to land.
- `s->last_step_dx/dy` is written once per step in sand.c before
  reactions runs — confirm it is never `(0, 0)` under real dithering
  (a zero vector would make `ux == x, uy == y`, so `sand_at(s, ux, uy)`
  reads the cell ITSELF, which is never empty since acid is sitting
  there — silently failing the exposure check every time). Check the
  dithering code in sand.c that produces `dx, dy` for a real device
  scene to confirm it is never `(0,0)` for a resting-but-tilted device
  (as opposed to a genuinely flat one, which the user has ruled out as
  the common case, but "rarely flat" is not "never flat" — worth a
  direct check).

**Diagnostic**: add a temporary counter (or printf, same technique used
for the mark_rows/wake_block crash diagnosis earlier this session) inside
`acid_bubble()` that increments once when the exposure check passes and
once more when the chance roll also passes, and print/log the totals
every N seconds. Compare against how many acid cells exist on the board
that step (`may_have_dissolver`-gated cell count, or eyeball it). If the
exposure check almost never passes on a real poured pool, that is the
actual bug, and CHANCE/SPEED are irrelevant.

### H3 — the impulse is queued but never becomes visible

Even if `sand_impulse()` successfully queues an entry, `step_impulses()`
could still make it invisible in practice:

- SPEED=120 with a single-row hop might resolve to "moves up one cell,
  falls right back next step" — visually indistinguishable from ordinary
  acid churn at 3px/cell scale, especially amid a moving pool.
- Confirm what units `speed` is actually processed in inside
  `step_impulses()` (look at how `entry.speed` is consumed further down
  in the function, past what this session already read through line
  ~1885) — if it decays fast or the resulting displacement is under a
  cell, the "bubble" would be a no-op in practice even with a queued,
  processed entry.

**Diagnostic**: same session/build, log every time `queue_flying_grain()`
actually appends an entry for an acid-bubble call specifically (a second
counter, distinct from H2's), and separately log how many pixels/cells
that entry visibly displaced after `step_impulses()` finishes processing
it. If entries are queued but the net displacement rounds to ~0, that is
the bug, and it lives in the speed/distance math, not the roll.

### H4 — it works, but it's not perceptible

368x448 screen at 3px/cell means a single popped grain is a 3px dot for
one or two frames before gravity pulls it back. If H1–H3 all check out
clean (buffer fine, roll fires, displacement is real and multi-cell),
the mechanism may simply be working and just too subtle to read as
"bubbling" against a busy, already-moving pool. This is a design/tuning
question, not a bug — the fix would be either a bigger SPEED/spread, or
giving popped grains a one-or-two-frame visual tell (a brighter variant,
say) so a "working but subtle" case doesn't get mistaken for "broken."

## How to instrument (reuse this session's proven method)

This exact technique already found and confirmed the crash-diagnosis
printf pattern earlier this session — reuse it rather than inventing a
new one:

1. Add temporary counters/printf directly in `acid_bubble()` (exposure
   check pass/fail, chance roll pass/fail) and in `queue_flying_grain()`
   (successful enqueue vs each rejection reason — buffer full, empty
   source, wall-without-dislodge, mat_filter miss).
2. Rate-limit the printf (e.g., dump counters once every N seconds, not
   every call) so `monitor.sh`'s event-rate throttling doesn't kill the
   stream — this session's crash monitor had to be filtered for exactly
   this reason (`grep -v "fps|down is"`).
3. `idf.py -B build.dev build && idf.py -B build.dev -p COM3 flash`,
   then attach `monitor.sh`, pour/pool acid on a real device for a
   sustained period, and read the counters back. This turns "we can't
   reproduce it" into actual numbers instead of another guess.
4. Remember `check-format.sh` should NOT be run on sand/ files without
   asking first — see the standing project memory on this (a formatter
   mistake already happened once this session).

## Time-box and fallback

This has already consumed real effort across two sessions without a
confirmed root cause. If a fresh session works through H1–H4 above and
still can't get a confirmed-visible, confirmed-working bubble on device
within a bounded effort (the user's own words: "if it doesn't work we
should first try to make it work, if we spend too much time, then it's
better to drop it and remove the expensive calls to an empty
functionality") — stop and remove it cleanly rather than let it linger
as dead weight. Removal touches exactly these spots:

- `acid_bubble()` itself, its call site in `step_one_reacting_row()`
  (`if (CELL_MATERIAL(c) == MAT_ACID) { acid_bubble(s, x, y); }`), and
  its long comment block — all in sand_reactions.c.
- `SAND_ACID_BUBBLE_CHANCE` / `SAND_ACID_BUBBLE_SPEED` and their
  comments in sand.h.
- `test_acid_bubbles_do_not_favour_one_wall` and
  `test_acid_bubbles_still_fire_once_the_block_is_asleep` in
  suite_sand.c, plus their dedicated fixture/globals
  (`sleepy_bubble_sim`, `sleepy_bubble_cells`, `sleepy_bubble_blocks`,
  `sleepy_bubble_buf`, `BUBBLE_W`/`BUBBLE_H`/`SLEEPY_BLOCK_COLS`/
  `SLEEPY_BLOCK_ROWS` if nothing else uses them) and their `RUN_TEST`
  registrations.
- Re-run `bash test/run_tests.sh` and `bash test/check_app_sources.sh`
  after removal to confirm nothing else referenced any of the above.
