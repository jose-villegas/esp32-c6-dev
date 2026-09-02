# Performance Tuning Attempts

Part of the falling-sand app's own documentation folder — see
[`README.md`](README.md) for the full set. Continues from
[`Simulation-Lessons.md`](Simulation-Lessons.md): that file is the
discovery narrative for how the simulation was built; this one is the
chronological record of every real-hardware performance attempt made
against it since, numbered in the order they happened.

**Condensed on 2026-08-31** from roughly 3,700 lines of full derivations
to this. This file is read by an AI model at the start of nearly every
sand performance session, and the per-attempt narrative — the false
starts, the device captures, the exact reasoning chains — was costing
more context than it was returning in decisions changed. Every commit
below still carries its own full write-up in its message; `git show
<sha>` (or `git log --all` if a sha shows "not on main" in the table)
recovers everything this file used to spell out in prose. The table,
the negative results, and the structural lessons below are what actually
changes a future attempt's plan; keep this file that short.
[`Tuning-At-a-Glance.md`](Tuning-At-a-Glance.md) has the scoreboard, the
pass-ownership map, and the mermaid diagrams for the three mechanisms
that won.

---

## The attempts

| # | What | Commits | Verdict |
|--:|---|---|---|
| 1 | Cast to unsigned for the wake path's block-index division (signed `int/16` isn't a shift) | `8397610`, `799c548` | shipped |
| 2 | Batch several neighbour-wake calls into one per block-row instead of one per grain move | reverted, no surviving commit | reverted |
| 3 | Thread block indices through the sweep instead of re-deriving them by division | `229ab87` (surviving host tests only) | reverted |
| 4 | Replace per-move push-based neighbour wake with a per-step pull-based block check | `cc48f9b`, `6c98bb5` | shipped |
| 5 | Stagger settled-block release across several steps, at three granularities | `ab8fbb9`, `be7ff07` (doc, main); code on unmerged branch `sand-block-row-stagger`: `e508f57`, `367fd07` | reverted (kept unmerged) |
| 6 | Sweep `SAND_BLOCK_W`/`SAND_BLOCK_H` on real hardware instead of shipping a guess | `55ed26e`, `647d39c`, `4ee0fe7`, `2823573` | shipped (32×64) |
| 7 | Share gas's movement primitives with sand's without losing sand's inlined hot path | `9f6b886` | mixed (shipped; planted attempt 8's landmine) |
| 8 | Force-inline, IRAM-place, and RNG-early-out a function the failing benchmark never calls | `8e4a0c4` (doc only) | no-op |
| 9 | Narrow, then delete outright, the `ROW_NO_LIQUID` skip cache | `758aa25`, `8f28fbc`, `0378703`, `d5b4e4c` | shipped (cache deleted) |
| 10 | Give the cross-flow pass a per-block liquid-presence skip (`BLOCK_HAS_LIQUID`/`NEAR`) | `60d03bd`, `90a1ddc` | shipped (failures 1→0) |
| 11 | Bisect a 75-commit feature wave; fix one mis-scheduled branch; recommend re-pegging two fire budgets | `4db3522`, `6493281`, `9accf71` | mixed |
| 12 | Add three cross-material reaction benchmarks; fix a convection gate that could never close | `4de2af4`, `3f2246f`, `82e2c00`, `1d592f3` | mixed |
| 13 | Add thermal-shock-lattice and boiler benchmarks for sustained (not transient) load | `47043ae`, `a1dc854`, `b1230e1`, `41507c0` | shipped (benchmarks only) |
| 14 | Trade cross-flow settle-angle precision cost for correctness: two rays dithered in space | `ef87042`, `de452e3`, `bb755ce` | mixed (deliberate cost) |
| 15 | Pin a stale capture to its commit; attribute two flash-layout regressions; turn off the diag watchdog | `1c4413f`, `741846e`, `e55f334`, `deb340f`, `a60b5e9` | mixed |
| 16 | Fix a wet-earth inlining cliff (4th occurrence); add a full-width dirty-region present path | `723fac6`, `3cf88c3`, `142b2f8`, `127037a`, `dc9e7d2` | shipped |
| 17 | Clear three named suspects; ship a sequential memo for the gas sight scan | `e07081f`, `468a53f`, `0cfc087` | shipped |
| 18 | Counter-driven decomposition of both hot passes; mask the reaction probes' pre-roll rejects; pin `sand_step` to the cache line | `8a20c86`, `66a1e9b` | shipped |
| 19 | The pair-matrix: a 16×16 classification table gating five probes, plus a one-load-per-neighbour cascade; a stage-list dispatcher tried and retired | `7b3273a`, `58b1f42` (shipped); `fcf329b`, `69e796e` (kept on `sand-pair-matrix`, unmerged) | mixed (stages 1–2 shipped) |

A sha marked "not on main" lives on a feature/exploration branch that was
never merged; `git show` still works once that branch is fetched, or
`git log --all` will find it.

---

## Never retry

- **Batched neighbour-wake ranges within one step** (2) — provably safe
  (a superset can't under-wake) but measured worse on every benchmark;
  the coarse wake radius re-examined more than the per-move guard chain
  it replaced ever cost.
- **Threading block indices through the sweep instead of dividing** (3)
  — strictly less arithmetic, measured worse twice, root cause never
  fully diagnosed. Two failures on one path is what motivated 4.
- **Staggered settled-block release, at three granularities** — row-based
  (12/step), block-based 4/step, block-based 8/step (5) — each won one
  axis and lost another; no variant beat plain no-staggering across the
  board. Kept on the unmerged `sand-block-row-stagger` branch.
- **Forcing `try_slide_impl` inline with `always_inline`** (8, exp. 1) —
  a wash overall, and the one benchmark that is 99% pure sweep moved the
  *wrong* way.
- **IRAM placement of the sweep's hot functions** (8, exp. 2) — worse
  across the board; the 32 KB instruction cache was never missing.
- **`DRAM_ATTR` on `materials[]`** (10, exp. 1) — an order of magnitude
  below this target's own flash-layout noise floor; indistinguishable
  from zero.
- **A settled-block skip in the cross-flow pass** (10, exp. 2) — fatal,
  not just slow: the settled bits key off gravity's *dithered* direction
  while cross-flow alternates the *perpendicular* every step. An
  imbalanced pool can freeze permanently, one step away from level,
  because the step that would fix it lands on a gravity dither the block
  already looks settled under.
- **A per-cell "can this material react at all" mask** (12, exp. 1) —
  19.3% of cells were provably skippable and skipping them measured
  −0.1%, because the cells were already cheap to fall through. It also
  went stale mid-pass against flags armed during the same pass.
- **Per-move liquid narrowing of `mark_rows()`** (9, exp. B) — counted,
  exactly, as strictly less work (6,160 fewer calls/step) and still
  measured 14% *slower*, purely from where the extra branch moved the
  hot function in flash.
- **`ROW_MAX_RUNS`/`LEAF_REFINE_MAX_RUNS` sweeps** (6, then again 16) —
  proven inert twice, and for structural reasons, not because the wrong
  values were tried: no scene has a row between "needs the full span"
  and "needs one run," and no cell in any benchmark is ever
  leaf-eligible in the first place.
- **`GATHER_MAX_PIXELS` increases** (6, then again 16) — declined twice,
  for different reasons each time. The second sweep found a send path
  that reaches the same pixel totals as raising the cap, for zero extra
  memory and zero copying.
- **Cold-hinting `move_liquid_grain`'s dead-for-water branches** (18) —
  counters proved the drag/foreign/splash machinery executes zero times
  in the water and mixed-flip windows, and hinting it to the tail did
  tighten the hot span 22 → 16 cache lines — and the host showed no win
  anywhere, baseline fastest in every trial. The function's layout was
  already close enough to its ceiling; water's gap is call volume, not
  code shape. Don't re-run the hints; the next water idea has to reduce
  the ~11k grains × 2 passes per step, not rearrange their bytes.
- **Interior bounds-check skip in `step_one_soaking_cell`** (18, cand. B)
  — provably removes redundant comparisons on ~98% of cells and measured
  consistently *slower* than the mask alone on the very scene it targets,
  across three capture sizes, with no inlining cliff to blame.
- **A loop-plus-switch dispatcher for the reactions stage ladder** (19) —
  fingerprint-identical, `sand_step_reactions` even shrank 44 B, and the
  device confirmed the host's warning almost exactly: fifteen rows
  regressed, smoke+steam +24.5%, boiler +17.3%, every-material +16.3%.
  Not the indirect-jump trap (the switch compiled to compares); best
  hypothesis is register pressure from every local staying live across
  the dispatch loop instead of the ladder's per-branch scopes. A future
  dispatcher needs a different shape; the stage commits and the sweepable
  order-vector infrastructure wait on the unmerged `sand-pair-matrix`
  branch.
- **`aligned(64)` on a flash-resident function** (18) — does not link:
  it raises `.flash.text`'s section alignment past the `0x...20` start
  ESP-IDF's script pairs with `.flash_rodata_dummy`, and ld refuses the
  overlap. 32 (this chip's actual i-cache line) links and works.
- **Raising a budget to make it pass.** Never done, even under pressure
  to close a gap — a budget moved to fit the code it guards has stopped
  guarding anything. When a feature genuinely earns a higher cost,
  re-peg from a fresh measurement instead.

---

## Recurring failure modes

- **The inlining cliff — found four times.** A feature commit adds a
  second call site to a `static inline` function, or reshapes an early
  `return` into an `if`/`else`, and `-finline-functions-called-once`'s
  unconditional guarantee evaporates, leaving only size heuristics that
  decline the body. Cost ranged 4–26% across the four occurrences
  (attempts 7→8, 10, 16). The only proof either way is `objdump -t`
  against the real device object — a symbol with its own
  `.text.<name>` section was not inlined, whatever the source says.
- **Large stack arrays in test fixtures.** A fixture sized off a tunable
  constant (`SAND_BLOCK_W * LOC_H`) overflowed the device's 3,584-byte
  main task stack and panic-looped with a `Stack protection fault`
  (attempt 6). A host suite cannot see this at all — the host stack is
  megabytes — so this class of bug only ever shows up on a device flash.
- **A fixture allocating more than the heap has.** A host-proved-fine
  mask fixture needed ~82 KB against the ~66–68 KB free once the
  framebuffer is carved out (attempt 13). `TEST_ASSERT_NOT_NULL` used as
  an assert-before-free skipped every earlier `free()` in that fixture on
  failure, leaking ~41 KB and starving every later test that boot — the
  whole capture came back with zero numbers, not a bad one.
- **Build-config fragments silently not reaching the build.** `idf.py`
  only applies `SDKCONFIG_DEFAULTS` when it *creates* the sdkconfig; an
  existing `build.diag/sdkconfig` left over from a previous build wins
  silently, so a fresh worktree can compile a "diag" image with
  `CONFIG_LAUNCHER_SELFTEST` unset and no self-test in it at all — a
  capture with zero measurements and no error. Hit and fixed twice on the
  one-click report scripts (`report_performance.sh`/`report_test_results.sh`,
  which live separately from `run_device_tests.sh` and drifted from it
  both times); the current scripts remove the stale sdkconfig and pass
  `-D SDKCONFIG_DEFAULTS=... -D SDKCONFIG=...` explicitly rather than
  relying on a bare `idf.py build`.
- **Host numbers mispredicting the device.** Reliable when a change
  alters *where code sits* (attempt 16: every host prediction landed on
  device, including a null result). Unreliable when a change alters *how
  much work runs* — the host and device weigh instruction count and code
  placement differently, so a 26% host win recovered only 20% of a much
  larger device cost by a different mechanism (attempt 11), and a
  measured +8% host cost arrived as roughly +16% on device (attempt 14).
  Flash a device capture before trusting a host percentage on anything
  that changes work quantity, not just code shape.

---

## Fixed facts about this target

- No data cache — SRAM is direct-access at every load/store. Only a
  32 KB **instruction** cache sits between the core and flash-resident
  code and `const` data. "Touch fewer bytes" is the only data-side lever;
  code *size* and *placement* are what keep deciding benchmarks whose
  semantics never changed.
- 80 MHz QSPI is unavailable — tried twice, produces real visual
  artifacts on this panel. A full-panel blit is ~17 ms and the frame is
  94% bus-bound; the present-path's own overhead is only ~1.1 ms of that.
- Main task stack is 3,584 bytes (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`); a
  host build's stack is megabytes and cannot reproduce an overflow here.
- ~66–68 KB of heap is free once the display framebuffer is carved out;
  a full sand grid alone is ~41 KB.
- The flash-layout lottery is quantised, but not two-state: attempt 18's
  bisect found the mechanism (a hot function's compiled bytes stay
  byte-identical while unrelated size changes earlier in its file move
  its address across cache-line boundaries), observed four distinct
  control value-pairs in one day, and saw two *different* binaries land
  the same pair to the microsecond. Reading *which pair* the controls
  landed in remains the sharp diagnostic. `sand_step` is pinned
  (`aligned(32)`, commit `66a1e9b`) so its own ticket is no longer drawn;
  other hot functions still play the lottery, and the pin's padding
  re-rolls everything downstream of it exactly once.
- The diagnostics image no longer runs a task watchdog. It used to charge
  its own periodic console dump to whatever benchmark's timed loop it
  landed inside — deterministically, up to 2.6× inflation, with nothing
  in the report to flag it.

---

## Related

- [`Tuning-At-a-Glance.md`](Tuning-At-a-Glance.md) — the scoreboard,
  the one-line-per-attempt table with verdicts, the pass-ownership map,
  and the mermaid diagrams for the three mechanisms that won.
- [`Perf-Round-Guide.md`](Perf-Round-Guide.md) — start here to run a new
  performance round.
- [`Simulation-Lessons.md`](Simulation-Lessons.md) — the discovery
  narrative this file continues from.
- [`Sand-Simulation.md`](Sand-Simulation.md) — how the simulation works
  today, including the gas material's own section.
- [`Adding-a-Material.md`](Adding-a-Material.md) — the practical
  checklist this campaign's gas-material findings fed directly into.
- [`Architecture.md`](Architecture.md) — the device-verification workflow,
  written as one exact command sequence.
- [Display-and-Rendering.md](../Notes/Display-and-Rendering.md) — the
  parallel `gfx_dirty.h`/`row_runs.h` cap sweeps.
- [Optimization-Playbook.md](../Notes/Optimization-Playbook.md) — several
  of the findings above generalised into board-agnostic techniques.
- `launcher/tools/sweeps/` — the sweep automation this campaign built
  and now keeps as repo tooling.
