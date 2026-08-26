# Tuning at a Glance

The visual map of [`Performance-Tuning-Attempts.md`](Performance-Tuning-Attempts.md) —
fifteen numbered attempts to make a 41,216-cell falling-sand simulation fit its frame
budgets on a 160 MHz single-core chip with no data cache. The tenth ended at **every
budget met, none ever raised**; a wave of new materials then put four back over, and
the eleventh sorted the accident from the feature. The twelfth found a gate that could
never close; the thirteenth shipped no optimisation at all, measuring the two shapes
of thermal load nothing had measured yet — a shock and a steady boil. The fourteenth
went the other way on purpose: it spent host time it did not have to, buying back a
settled surface's true angle from a fix (30335ae, predating this campaign) that had
quantised it to 0/45/90 degrees without anyone pricing the trade. The fifteenth
shipped no code at all — it was a fresh device capture chasing down two loose ends
the eleventh attempt and the 2026-08-26 re-base had left open, and it found both,
plus a watchdog that had been charging its own console output to the benchmarks
it was meant to be guarding. The prose file holds the full derivations and is the
authority when the two disagree; this page is for a first read, a refresher, or
finding which attempt taught the lesson you half-remember.

---

## The scoreboard

**The 2026-08-26 re-base: all thirteen budgets are now uniform reduction
targets — measured × 0.9, rounded — and all thirteen deliberately fail.**
The first full capture of the post-wave tree (`performance_20260826_150930`,
reproduced by a second capture to within 4 µs on every test) established a
fresh baseline for every scene, and every budget was re-set a tenth below
its own measurement. No bars this round: by construction every row sits at
~111% of its target, and the honest visualization is the gap itself. A row
turns to pass only when its tenth is genuinely won.

| Test | Measured (2026-08-26 clean) | Target | To close |
|---|---:|---:|---:|
| Settled screen, nothing moves | 262 µs | 235 | 27 µs |
| Full-size step, all falling | 6,451 µs | 5,800 | 651 µs |
| Gravity flip, settled pile | 6,546 µs | 5,900 | 646 µs |
| Mixed scene flip | 14,391 µs | 11,700 | 2,691 µs |
| Screen of water collapsing | 18,667 µs | 14,400 | 4,267 µs |
| Boiler, sustained boil | 32,667 µs | 28,500 | 4,167 µs |
| Every material at once | 78,617 µs | 67,500 | 11,117 µs |
| Thermal shock lattice | 98,738 µs | 89,000 | 9,738 µs |
| Four liquids reacting | 124,336 µs | 112,000 | 12,336 µs |
| Lava stress scene | 127,386 µs | 109,000 | 18,386 µs |
| Smoke + steam screen | 141,444 µs | 127,000 | 14,444 µs |
| Full screen of fire, steady | 297,220 µs | 266,000 | 31,220 µs |
| Fire cascade through gas | 412,718 µs | 456,000 | **passes**, +9.5% |

The measured column was refreshed from `performance_20260826_183646` — the
first watchdog-free capture (zero contamination possible, see attempt 15),
taken after the fourteenth attempt's surface-angle fix landed. Three
things it moved: the two contaminated rows were re-pegged from their first
clean numbers (four liquids 113,000 → 112,000; thermal shock 96,000 →
89,000 — its old number carried ~7,900 µs of watchdog console I/O); the
water and mixed rows now carry the fourteenth attempt's real device cost
(+16% and +11%, roughly double its host prediction — the fifteenth
attempt's host-vs-device lesson, again); and the fire cascade came in
94,000 µs *below* its baseline and now passes — evidence its 506,666
baseline was itself a contaminated row the survey missed, making its
456,000 target a re-peg candidate (≈371,500) awaiting a decision.

Two findings from that capture, unresolved at re-base time and first in
line for the next round: attempt 11's host-measured water/mixed recovery
**did not materialise on the device** (16,043 against a predicted
12–13.5k — the host→device ratio is scene-specific, as this file already
warns), and both liquid-free controls moved ~+10% against the older build
(5,867→6,434 and 5,959→6,529) — layout-lottery double hit or a genuine
global per-cell cost from the wave's tail, not yet attributed.

Fixed RNG seeds make identical builds reproduce these numbers to the microsecond.
What moves them *between* builds is flash layout, not chance — see
[the layout lottery](#the-layout-lottery) below.

**The fourteenth attempt has no device data behind it and spent host time rather
than saving it.** Host-relative, best of 5, interleaved, landed build against HEAD:
water at exactly (0, 1000) 74.7-75.1 → 80.8-81.8 µs/step (≈+8%, pure code-shape cost
— every scene in the table above runs at that exact gravity, where the fix is
behaviourally a no-op); water at 20 degrees 76.3-77.8 → 98.8-100.9 µs/step (≈+29%);
water at 26 degrees 83.0-85.2 → 113.6-116.4 µs/step (≈+37%) — neither tilt is
measured by any row here. Expect **"Screen of water collapsing" to move on the next
device capture, in the wrong direction**, by something in the neighbourhood of the
+8% figure: it is the one row in this table whose scene cross-flow touches at scale,
and this round's cost lands on every liquid cell whether or not the gravity it runs
under ever leaves the axis.

**That prediction still stands, untested.** The fifteenth attempt's device captures
are all pre-4bdcf77 — the attribution round it ran chased two older findings, not
this one, and none of its numbers include the fourteenth attempt's fix. The next
device capture taken after 4bdcf77 lands is the one that tests it.

---

## The campaign, one line per attempt

🟢 shipped · 🔴 reverted or dropped · 🟠 mixed / turning point. The reverted rows
are kept deliberately — half the value of the record is negative results that stop
the next person from re-running them.

| # | | Attempt | What happened |
|--:|---|---|---|
| 01 | 🟢 | Unsigned division on the hot path | Signed `int / 16` is a five-instruction dance, not a shift. One cast at the per-move call site: **≈3 ms** back. Same bug found twice more by grepping for the shape. |
| 02 | 🔴 | Batch the neighbour wakes | Provably safe — a superset can't under-wake. Worse everywhere anyway. **Proven safe ≠ proven fast.** |
| 03 | 🔴 | Thread block indices instead of dividing | Strictly less arithmetic, measured worse twice, never fully diagnosed. Two failures on one path became the signal for 04. |
| 04 | 🟢 | Stop pushing wakes — pull them | Deleted the per-move notify machinery; the existing end-of-step pass asks instead. O(moves) → O(blocks). Flip **−26%**, water **−15%**, six functions deleted. |
| 05 | 🔴 | Stagger the mass wake-up | Three granularities measured; each won one axis, lost another. Kept on a branch, unmerged. **Correct and bounded still isn't worth it.** |
| 06 | 🟢 | Sweep the block size | It had shipped as a guess. Six `(W,H)` pairs on real hardware: 16×64 → **32×64**, the only pair clearing the settled-screen budget. |
| 07 | 🟠 | A fourth material vs. the inliner | Sharing movement code with gas regressed twice (un-inlined hot path +26%; 3.9 KB duplicated into flash, 2×). The split that shipped quietly planted 08's landmine. |
| 08 | 🟠 | Three good ideas, zero effect | Force-inline, IRAM, RNG early-out: all correct, all reverted — **the failing test never calls that function.** Moving a grain costs ~3.2 ms/step more than failing to move one. |
| 09 | 🟢 | Delete the row cache | `ROW_NO_LIQUID` cost **33,426 writes/step** to save 104 cheap row scans. The whole `row_state` buffer went. Failures **3 → 1**. |
| 10 | 🟢 | Tell the cross-flow pass where water isn't | The sweep already reads every awake cell — a per-block liquid bit is free. One neighbour ring makes it sound; the pass skips **41%** of its cells. Failures **1 → 0**. |
| 11 | 🟢 | A feature wave, and one branch in the wrong place | Rebuilt and re-timed all 75 commits of the wave on the host: water's whole regression is **one commit, one branch**, and the branch never even says no. GCC had spliced its cold blocks into the hot path — hinting them cold recovers **26%**, simulation byte-identical. Fire's is diffuse and genuine; the reactions pass **doubled**, and the gas pass nobody suspected is 60% of it. |
| 12 | 🟠 | A mask that measured nothing, and a gate that never closed | Three new benchmarks put the reaction engine under cross-material load for the first time. A per-cell "can this react" mask measured **−0.1%** where the counters promised 19.3% of cells were skippable — too cheap to be worth skipping. The real find: a convection gate that could never close on smoke or steam, costing **41,215 neighbour scans a step** on a screen of gas — fixed with a flag that arms and is never cleared, **−7.1%**. Measure-by-deleting also priced the gas pass's sight scan at **15-18%** of the fire benchmarks — the next round's target, not yet built. |
| 13 | 🟢 | Two scenes for the shape of load nothing had measured | Everything benchmarked so far was a transient; nothing measured a heat source left running. A thermal shock lattice (480 glass-ringed compartments) and a boiler (a stone basin held at a sustained boil) shipped as a pair, each with a host guard whose assertions were checked by breaking the scene: **24 of 33** individually turned red. Ten steps was a measured decision, not a round one — the only window where the last third of new cullet still clears **15%** of the total instead of trailing off. An earlier boiler draft's exact-conservation count held by **one step**; a floor replaced it. No optimisation shipped — two device ceilings added, both provisional. |
| 14 | 🟠 | Two rays, dithered in space instead of time | Cross-flow's nearest-axis fix (30335ae) was correct but unpriced: a settled surface could only ever be perpendicular to one of eight directions, so it quantised to 0/45/90 — measured as two values across the whole tilt range, ≈0.00 below 22.5° and ≈0.94 above. The near-vertical octant couldn't tilt at all: its ray is horizontal, and a horizontal ray moves mass only within a row. The fix moves the dither from TIME into SPACE — a fixed per-column pattern choosing between two rays — and compares gravitational potential rather than raw mass. Cost: **+8%** on host water at the axis gravity every benchmark uses, **+29-37%** off-axis, which no budget measures. Slow alternation was rejected on a measurement: switching axis on a settled pool costs **1,582** units of churn against the flicker guard's own ceiling of 60. |
| 15 | 🟠 | Two loose ends closed, and a watchdog counting itself in | An attribution round — no code shipped. Pinned the stale 2026-08-25 capture to `4b5168c` by matching its 272 self-test names against `RUN_TEST()` declarations at each candidate, then validated the method by rebuilding that commit fresh a day later: four rows exact, one off by **1 µs**. The liquid-free controls' **+9.6%**: byte-identical simulation and unchanged instruction count end to end, then isolated by device bisect to one commit, `e03aabd` — a line that **never executes** in either control, costing **~5%** purely from how GCC rescheduled `sand_step` around it once it existed. Round five's 28% host win: real on device too, confirmed by `objdump` moving exactly the blocks it should — but only **20%** of the gate's cost there, because the device's real water regression was `move_liquid_grain` **nearly tripling** across four separate commits, not the gate. Underneath both: a task watchdog silently charging its own console dump to the benchmark loop it shares a UART with, **up to 2.6×**, deterministically — two of the current thirteen budgets were pegged from contaminated rows and are too loose by an unknown amount. |

---

## The machine, as measured

Three facts about this chip decided most attempts — none match desktop-CPU intuition.

### There is no data cache — only an instruction cache

```mermaid
flowchart LR
    CPU["RISC-V core<br/>single core, 160 MHz"]
    IC["i-cache<br/><b>32 KB</b>"]
    FLASH["Flash 16 MB<br/>.text — all code<br/>.rodata — const tables"]
    SRAM["SRAM ~424 KB<br/>322 KB framebuffer<br/>40 KB grid + free"]

    CPU -->|"fetch code + const"| IC
    IC -->|"miss → refill"| FLASH
    CPU ==>|"data: direct, every access,<br/><b>no cache tier at all</b>"| SRAM
```

**Why it matters:** "optimize for cache locality" has nothing to bite on for the
grid — SRAM already runs at cache speed, so the only data-side lever is *touching
fewer bytes* (attempts 04, 09, 10 are all this lever). The cache that does exist
serves **code**, which is why code size and code *placement* kept deciding
benchmarks whose semantics never changed. Both "move it closer" experiments
measured neutral-to-worse: IRAM for code (08) and DRAM for the material table (10)
— the 32 KB cache was never missing on either.

### The layout lottery

Same code, **3.2 ms vs 3.9 ms** — two builds that never touched the hot function,
20% apart, purely because unrelated code shifted where things landed in flash.
This is the noise floor under every number here: differences under a few percent
are layout until they reproduce, and budgets carry margin so a rebuild alone
cannot flip them. The antidote (found in attempt 10): **keep control benchmarks in
every capture.** Three of the seven tests never touch liquid — when only the
liquid numbers moved, the cause had to be in the liquid path. A global layout
shift can't leave three tests byte-identical.

### The inlining cliff

```mermaid
flowchart TB
    subgraph two["TWO call sites — only size heuristics remain, and they decline 1 KB bodies"]
        C["sand_step()<br/><i>per blocked grain</i>"] -->|"push 12 registers,<br/>96-byte frame, pop them all"| E["try_slide_impl<br/>out-of-line shared copy"]
        D["try_slide()<br/><i>gas wrapper</i>"] --> E
    end
    subgraph one["ONE call site — inlined unconditionally (-finline-functions-called-once)"]
        A["sand_step()"] --- B["try_slide_impl<br/><i>body folded in — free</i>"]
    end
    style B stroke:#2f7d4f,stroke-width:2px
    style E stroke:#b23c33,stroke-width:2px
```

`static inline` is a request. The *guarantee* exists only at exactly one call
site; a second caller — or even respelling an early `return` as an `if`/`else` —
hands the decision back to heuristics. This bit the campaign three times: attempt
07 created it, 08 found it, and 10 re-triggered it with a pure readability
tidy-up that cost **14% on water**. The only proof either way is `objdump -t`: a
symbol with its own `.text.<name>` section was not inlined. And forcing the inline
can cost more than the call — one level too far and the loop outgrows the 32 KB
i-cache and *everything* slows (attempts 07 and 08 both measured this).

---

## The three mechanisms that won

### 04 · Push → pull: relocate the question to where it's already cheap

```mermaid
flowchart LR
    subgraph push["PUSH — deleted · O(grain moves), ~10,000×/step"]
        M["every grain move"] -->|"guard chain, edge maths,<br/>'wake whoever might care'"| N["neighbour blocks"]
    end
    subgraph pull["PULL — shipped · O(blocks), 24×/step, fixed"]
        P["end-of-step settle pass<br/><i>already running anyway</i>"] -->|"'did any neighbour move?'<br/>one bit test × 8"| Q["each block"]
    end
    style push stroke:#b23c33
    style pull stroke:#2f7d4f
```

Two attempts to make the push cheaper (02, 03) both regressed; the third asked
whether the push needed to exist. Flip 13,053 → 9,638 µs, water 19,703 → 16,540 µs,
and the entire precision machinery the push needed came out with it. **When a hot
path resists optimizing in place twice, the path — not the implementation — is
usually the problem.**

### 09 · The maintenance ledger: who pays to keep a flag true?

A skip structure bills its **maintenance** to writers and pays its **dividend** to
readers. Nobody had ever put the two sides of `ROW_NO_LIQUID`'s ledger next to
each other:

| Structure | Who pays to keep it true | Who benefits | Verdict |
|---|---|---|---|
| `ROW_NO_LIQUID` ("row holds no water") | every mover, every step — **33,426 byte-writes/step** | 104 row scans skipped — scans that bail on *one bitmask test per cell* | **deleted**, with the whole `row_state` buffer |
| Block sleeping (`block_state`) | one fixed **O(24)** pass per step | entire block walks skipped, read by the whole sweep | **kept** — earns it easily |

Deleting the cache beat every attempt at making its upkeep cheaper — including a
narrowing that provably did *strictly less work* and still measured slower (code
placement, again). Water 17,860 → 13,130 µs; every benchmark improved.

### 10 · Skip cells the sweep already knows are dry

The cross-flow (liquid-levelling) pass read **every cell of the grid** while only
15% held anything it could act on. The sweep already reads every cell of every
awake block, so noting "this block held liquid" is a register OR and one store —
nobody is charged per move:

```
        ┌──────┬──────┬──────┬──────┬──────┬──────┐
        │ skip │ skip │ skip │ skip │ skip │ skip │
        ├──────┼──────┼──────┼──────┼──────┼──────┤
        │ skip │ skip │ NEAR │ NEAR │ NEAR │ NEAR │
        ├──────┼──────┼──────┼──────┼──────┼──────┤
        │ skip │ skip │ NEAR │ NEAR │ ≈HAS │ ≈HAS │   ≈ = water
        ├──────┼──────┼──────┼──────┼──────┼──────┤
        │ skip │ skip │ NEAR │ NEAR │ ≈HAS │ ≈HAS │
        └──────┴──────┴──────┴──────┴──────┴──────┘
          the cross-flow pass skips 17,024 cells/step — 41%
```

The bit cannot be read raw: the sweep runs bottom-up, so water falling across a
block boundary lands in a block already scanned and found dry. Without the `NEAR`
ring (HAS expanded to 8 neighbours in one 24-block pass), one falling cell makes
the pass conclude the grid holds no water and **switches cross-flow off
permanently**. All 194 existing tests missed that; a new one was written and
verified to fail first. Mixed scene 12,675 → **11,167 µs** — the last failing
budget, closed.

---

## The most expensive lesson: profile *whether*, not just *where*

Attempt 08's three experiments were each correct, verified in the binary — and
aimed at code the failing test never executes:

```mermaid
flowchart TB
    G["each grain, each step"] --> F{"try the<br/>gravity-ward fall"}
    F -->|"succeeds"| MV["move: swap + bookkeeping"]
    F -->|"blocked"| SL["try_slide_impl:<br/>RNG draw + 5-cell load walk"]

    MV --- FLIP["gravity flip (was failing):<br/>all 10,304 grains take this arm<br/><b>8,931 µs/step</b>"]
    SL --- FULL["full-size step (the control):<br/>all grains take this arm, nothing moves<br/><b>5,736 µs/step</b>"]

    style MV stroke:#b23c33,stroke-width:2px
    style SL stroke:#2f7d4f,stroke-width:2px
```

Same grid, same 10,304 grains, one difference — and the arm with the RNG and the
load walk is the *cheap* one. **Moving a grain ≈ 3.2 ms/step more expensive than
deciding not to.** The campaign had been optimizing the cheap arm. In the flip's
measured window, `try_slide_impl` is called **zero times**.

What settled it wasn't the profiler (which truthfully said "the sweep") or
`objdump` (which truthfully said "un-inlined"): it was a host-side **call
counter** — ten seconds, no flash cycle — asking not "how expensive is this
function" but **"does it run at all."** The tell was an impossible result: an
RNG-stream-shifting change measuring byte-identical to the microsecond.

---

## The playbook, distilled

The board-agnostic versions live in
[`../Notes/Optimization-Playbook.md`](../Notes/Optimization-Playbook.md); each row
names the attempt that paid for it.

| Rule | Taught by |
|---|---|
| **Measure by deleting, not by reasoning.** Stub the suspect to a no-op, re-measure on device, revert win or lose. | throughout |
| **Check the code runs before making it faster.** A host call-counter costs ten seconds; three device rounds went to a function called zero times. | 08 |
| **Proven safe ≠ proven fast ≠ less work.** A superset wake lost; a bounded stagger lost; a counted-strictly-less-work build lost to code placement. | 02 · 05 · 09 |
| **Ask who pays to keep the flag true, versus who reads it.** If payers outnumber readers 50:1, delete the structure. | 09 |
| **Keep a control in every capture.** Benchmarks the change cannot touch turn "layout moved" into "only the liquid path moved — look there." | 10 |
| **Readability refactors are performance changes here.** An if/else respelling cost 14% via the inlining cliff. Ship the version you measured. | 10 |
| **Inlining has a ceiling, and it moves.** Verify with `objdump -t`; never trust the attribute name. | 07 · 08 |
| **Know which memory you actually have.** No data cache: touching fewer bytes is the whole game; IRAM/DRAM placement measured neutral-to-worse. | 08 · 10 |
| **Signed division isn't a shift.** Cast to unsigned where the range proves it — then grep for every sibling call site. | 01 |
| **Don't filter the live capture.** A `grep` on the one crashing run discarded the only lines that explained it. Save everything, filter after. | 03 |
| **A resisting hot path is information.** Two well-reasoned failures on one mechanism mean the mechanism is the wrong shape. | 04 |
| **A new test must fail first.** A test never seen red is not yet a test. | 09 · 10 |
| **Layout is not only about inlining — it is about block order.** A branch that costs nothing to evaluate can still cost 26% by sitting between the entry and the work. `objdump` the device object; look at what is *between*, not just at what exists. | 11 |
| **Bisect on the host.** Rebuilding and re-timing a commit range costs twenty minutes and no flash cycles, and answers the question a regression actually poses — *when did this start* — with a byte-identical simulation either side. | 11 |
| **A capture measures a tree, not a project.** Check the flashed build against the code before diagnosing anything with it. This one was fifty commits stale and said so nowhere. | 11 |
| **Count what skipping *saves*, not just what is skippable.** 19.3% of cells were provably idle and skipping them measured −0.1%, because the cells were already cheap to fall through. | 12 |

---

## Related

- [`Performance-Tuning-Attempts.md`](Performance-Tuning-Attempts.md) — the full
  chronicle this page maps; the authority when they disagree.
- [`Simulation-Lessons.md`](Simulation-Lessons.md) — how the simulation got built.
- [`../Notes/Optimization-Playbook.md`](../Notes/Optimization-Playbook.md) — the
  lessons above, made board-agnostic.
- [`Architecture.md`](Architecture.md) — how to reproduce any number here on real
  hardware, as one exact command sequence.
