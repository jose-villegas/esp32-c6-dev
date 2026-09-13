# Perf Instruments

What each instrument measures, and — the half that matters — what it cannot
say. The process that uses them is [`Perf-Round-Guide.md`](Perf-Round-Guide.md);
this is the toolbox.

| instrument | answers | cannot answer |
|---|---|---|
| **ceiling probe** (host counters) | how OFTEN work finds nothing | how long it takes |
| **cache counters** (EXTMEM L1) | whether a scene stalls on fetch | which instructions it ran |
| **instruction model** (`instruction_model.py`) | where a step's instructions go | what a candidate will save |
| **pass gate** (`volatile bool`) | what one phase costs, within one image | whether the phase was worth running |
| **single step** from a rebuilt board | a phase's cost without compounding | anything about steady state |
| **control pass** | which part of a loop is yours | the part you did not isolate |
| **fingerprint** (`report_fingerprint.sh`) | did behaviour change | whether it *reached* your change |
| **`check_static_ram`** | does the instrument still fit | — |

Two rules cut across all of them:

- **Only within-capture comparisons are trustworthy.** The layout spread
  between separately linked images is ±6.7%, wider than most effects.
- **A green result proves nothing until you show the instrument reaches the
  code.** Mutate deliberately and watch it go red first.

---

## Turning the gates on

Gates are **scaffolding**. They go in to answer one question, the answer goes
in the commit message, and they come out before the round ships — see
"Retiring the instrumentation" below. What is permanent is this section.

### And skip the restore, which is most of the wait

`capture_ref.sh` rebuilds and reflashes the release image when it finishes.
That work produces nothing you read, and it is the bulk of the cycle:

| phase | measured |
|---|---:|
| build + flash + run, report written | ~5 min |
| release reflash afterwards | ~13 min |

Pass `--no-restore` for every capture of a round and restore once at the end:

```sh
sh scripts/capture_ref.sh <ref> --perf-scope --no-restore COM3
```

**The report is complete before the restore starts.** If a capture is already
running without the flag, read
`main/apps/sand/tools/results/capture_ref_<ref>_*.md` as soon as it appears
rather than waiting for the command to return — the numbers are final at that
point and the remaining minutes are only reflashing.

Scoped and unrestored together take an iteration from roughly 25 minutes to
about 5, which is the difference between measuring a hunch and not bothering.

**It leaves the board on a diagnostics build.** That is the point during a
round, but say so when handing the device back, and restore before treating
it as a normal board again.

### Scope the build first, or there is no room for the instrument

A full diagnostics image compiles all 48 suites, and their static RAM had
left the "one grid fits after POST" gate 64 bytes of headroom — one added
decomposition row costs 272 bytes and fails the build. Take
every capture of a round perf-scoped and the instrumentation has somewhere to
live:

```sh
sh scripts/capture_ref.sh <ref> --perf-scope
bash launcher/main/apps/sand/tools/report_performance.sh --perf-scope
```

| | full | perf scope |
|---|---|---|
| suites compiled | 48 | 3 — `suite_sand_perf` + the scenes and fixtures it calls |
| device run | 952 timed tests, 7m 23s | 39 timed tests, 2m 19s |
| behaviour coverage | complete | none, deliberately — not a gate |

It is also a *better* instrument, not merely a roomier one: a smaller image
sits closer to release layout, so its numbers carry less of the ±6.7% link
spread. It is **not** because the sim is fetch-bound — that claim stood here
until the cache counters below measured 89 instruction misses in a 2.3
million cycle water step.

**Scope every capture of one round the same way.** A scoped and an unscoped
image are different layouts, so the within-capture rule below does not merely
apply — it forbids the comparison outright. What each scope contains, and how
selection works, is in [`../Testing-Guide.md`](../Testing-Guide.md).

**How much that ban is actually worth, measured once (2026-09-11,
`c2c672e`):** both scopes of the same tree, captured back to back, put the
two controls at **5,585 / 5,684** and **5,584 / 5,684** — the same layout
ticket, to the microsecond — and then disagreed by **at most 0.90%** on
every one of the twenty-four budget rows. So the ban is a ban on an unknown,
not on a known large offset: when the scopes happen to land the same
layout, scope itself is worth less than the ±1% band. Keep scoping a round
uniformly, because nothing makes them land together on purpose — but a
cross-scope comparison whose *controls* match is worth reading rather than
discarding.

### The instrument

A `volatile bool` per thing you want to price:

```c
/* sand_priv.h */   extern volatile bool sand_step_gate_<name>;
/* sand.c */        volatile bool sand_step_gate_<name> = true;
```

wrapped at the call site with one of two macros (both in `sand_priv.h`):

```c
SAND_STEP_GATE(name)          /* wraps a bare call or block */
SAND_STEP_GATED(name, cond)   /* ANDs into a condition that already exists */
```

Both compile to **nothing** when `CONFIG_LAUNCHER_SAND_PASS_GATES` is off,
which is the default and every release build. `SAND_STEP_GATE(x)` becomes
empty and `SAND_STEP_GATED(x, c)` becomes `(c)`, so the gated source and the
ungated source are the same program. Confirm it rather than assume:

```sh
riscv32-esp-elf-nm launcher/build/launcher.elf | grep -c sand_step_gate_   # 0
```

Turn them on for a round by appending `CONFIG_LAUNCHER_SAND_PASS_GATES=y` to
`launcher/sdkconfig.defaults.diag` in a commit marked TEMP, and strip that
commit before the PR.

## Cache counters: is this scene stalling, or just executing?

The C6 counts L1 hits and misses in hardware, one pair per bus, at
`EXTMEM_L1_{I,D}BUS_ACS_{HIT,MISS}_CNT_REG`. `suite_sand_perf.c` reads them
either side of a step, with `esp_cpu_get_cycle_count()`, and logs both scenes
in every perf-scoped capture. They cost nothing in the loop, so what they
price is the shipped code.

Per step, capture `performance_20260911_141810`:

| scene | cycles | ibus hit | ibus miss | dbus hit | dbus miss |
|---|---:|---:|---:|---:|---:|
| water | 2,298,429 | 2,047,557 | **89** | 1,009 | **0** |
| falling sand | 893,427 | 825,440 | **57** | 10,808 | **0** |

**Read three things off that table.**

*The sim does not stall on memory.* One instruction fetch in 23,000 misses,
and no data fetch misses at all. Even at a 200-cycle flash penalty that is
0.8% of the step. "Cycles per grid load" has been read as a memory signature
three separate times in this campaign; it never was one. The grid is heap
SRAM and does not reach this cache, and the flash-resident const tables stay
resident.

*The core is executing, not waiting.* 2,047,557 fetches against 2,298,429
cycles is 0.89 accesses per cycle — the fetch unit is busy nearly every
cycle. There is no stall reservoir to reclaim, so the only lever on a pass is
how many instructions it runs.

*Cycles convert exactly.* 2,298,429 / 14,365 µs = 160.0, so the µs figures
every other instrument reports are cycles / 160 with no correction.

The counters also attribute a candidate where wall-clock cannot. Handing
cross-flow's probes their rows moved water by −581
µs; the counters on the same pair of images read −93,120 cycles, which is
−582 µs, and −85,680 fetches. Cycles fell 1.09 per fetch removed. A layout
accident cannot produce that agreement.

## Where the instructions go: the line model

If a pass costs what it executes, the question about any hot loop is how many
instructions it runs — and that needs no device.
`tools/perf_probe/instruction_model.py` joins the two halves:

| half | from |
|---|---|
| riscv instructions per source line | `objdump -dl` on `build.diag`'s own `.obj` |
| executions per source line | `gcov`, running the same scene on a host |

**Divide by the number of code sites a line owns, or the answer is 2× too
big.** One source line lands at several places in the object — a loop
versioned for its two directions, a tail GCC duplicated, a `static inline`
expanded at fifteen call sites — and gcov reports the *sum* of their
executions. The naive product squares that multiplicity: 3,812,840
instructions per water step against a measured 1,961,884.

Against the device's fetch counter, water scene:

| ref | model | device ibus | error |
|---|---:|---:|---:|
| 4d9efa7 | 1,918,761 | 1,961,884 | −2.2% |
| 01d12db | 2,122,082 | 2,047,557 | +3.6% |

So the **shape** of a step is trustworthy. It put cross-flow at 48% of the
water step against the gated decomposition's 51%, and ranked the two grid
walks first and second — which is what choosing a target needs.

### It cannot price a candidate, and the trap is that it looks like it can

**Differencing two runs of the model is not a prediction, and it is wrong in
both directions.** Across the pair above it reads −203,321 instructions where
the device measured −85,673 fetches, 2.4× over. Across the two skips of the
round that built this tool it reads −156,929 against a measured −250,289, 1.6×
*under*. The ±2–4% absolute errors are independent per build, and their
difference is several percent of a two-million-instruction step — wider than
most effects worth chasing. A change also relabels work: the first pair reads
−171,276 on `equalise_one_row_cell` and +56,013 on `equalise_one_block`, which
is one function's instructions being counted as another's.

**Price a candidate with counters times a hand-counted path instead.** Work
counters (`counters_scene_main.c`) for how often the work happens; the
disassembly for what one occurrence costs, weighting each exit by how often it
is taken and *including whatever the change pushes back into its caller*.
Leaving that out is most of why PR #174's own static model said 9–10% and the
device gave 4.0%: it counted the 7 and 11 instructions its two probes lost and
not the row selection and address arithmetic the caller picked up. Counting a
function's whole static size rather than the path a call actually walks is the
other half — that alone over-weights by 1.8×.

Done that way, PR #175's ray skip was predicted at −69,000 instructions a step
against a measured −76,547 fetches — 10% out, the best any instrument in this
campaign has managed before a build. Its empty-span skip was predicted at
−107,000 against −173,742, 38% under, and the disassembly says why: the price
of a *skipped cell* is not the reject loop's nine instructions alone but about
twelve, once the block prologue and the two lines of the mask test it also
avoids are counted. **A skip's saving is the whole region it stops entering,
not the loop body you traced.**

## Which PAIR of materials is expensive: the arena

`main/apps/sand/tools/arena/` answers a question the frame-budget scenes
cannot: of every material against every other, which *combination* costs, and
why. It pours material A to a share of a 184x224 board, settles it, then
pours B over it with a dragged brush at the app's own radius
(`POUR_RADIUS_PX`, 10 px at 2 px per cell), sweeping left to right and back,
and times every step of the pour.

```sh
./build_arena.sh
build/arena_probe --list              # every material the grid can hold
build/arena_probe Sand 65 Water 10    # settled sand, water poured over it
python round_robin.py                 # all 380 ordered pairings
python interactions.py                # rank them
```

Four things about it are load-bearing, each of which produced a wrong answer
first:

**The pour is a dragged brush, not a scatter.** Scattering single cells across
the width is rain; it touches far more of the settled mass at once and
inflates whichever material has the most contact-driven mechanics. Ice
measured 576 us scattered and 222 us poured.

**Not everything can be poured.** A `KIND_STATIC` material never falls, so
pouring stone left 176 cells of 16,486 on the board and it measured 0.3 us -
absent, not cheap. Statics are built from the floor up; a gas enters at the
FLOOR and rises, since one released at the top is already at its destination.
The brush also searches inward and sideways for space, because a player pours
where there is room.

**Rank the interaction, not the pairing.** A raw ranking puts one expensive
material at the top of every row it appears in. `interactions.py` decomposes
cost two-way - grand mean plus a row effect for the settled material plus a
column effect for the poured one - and ranks the residual, which is the excess
that exists only because those two met. Water is the cheapest material in the
tree and `Root <- Water` is near the top.

**Worst step is a third ranking again.** `Oil <- Acid` has an unremarkable
mean and the largest excursion measured, 5.8x. It appears in neither of the
other two tables, and a mean-based suite never sees it.

**AN ARENA BOARD HOLDS EXACTLY TWO MATERIALS, AND THAT IS A BLIND SPOT, NOT
JUST A LIMIT.** A mechanism whose precondition is a THIRD material reads as
100% rejection here, and reads that way convincingly - with counters, over
millions of reaches.

It has already produced one wrong answer. `Root 40 + Water 10` showed the
plant ladder's root body entering 4,533,650 times and changing nothing across
300 steps, which looked like a large behaviour-neutral skip waiting to be
taken. It was not: every plant stage ultimately needs moist SOIL, and an
arena board pairing roots with water contains no soil at all. On the real
scenes the local predicates are true 99.5-100% of the time and nothing
rejects; the skip was closed negative after the round was already briefed on
those numbers.

So: before believing an arena rejection count, ask what the mechanism's
preconditions are and whether a two-material board can even satisfy them.
Confirm any rejection rate against a real scene's own measured window before
anyone builds on it.

Host timing: good for shape, ranking and screening, silent on absolute cost.
Anything that has to be priced goes to the board.

## Before you build anything

The cheapest instruments need no device, no build and no capture. Reach for
these first; twice this campaign they turned a banked design into a single
flag before a device was touched.

### Ask how often the work finds nothing

A gate prices work that runs. It cannot tell you the work was **pointless**,
and that is the cheaper question — it needs no device, no build, no capture.

Two host counters in the walk you suspect:

```c
walks++;                       /* every time the walk is entered   */
found |= <the thing it looks for>;
...
if (!found) { barren++; }      /* it looked, and there was nothing */
```

**It says WHETHER, never HOW MUCH.** Acid rain's quad test was entered 204,247
times in a run and found nothing every single time; removing all of it
measured **1.6%** on device. The counter was right that the work was
removable and silent on what it was worth. Third time measured — the
cross-flow alternating-sweep removal, where counters halved and the
device gave 2.5%. Use it to
choose *what* to build, never to claim a number.

Run a scene on the host and read the ratio.

| walk | entered per step | found nothing | what it was worth |
|---|---:|---:|---|
| burning cell's pair walk | 41,216 | **100%** | −17.7% of the fire scene |
| `conduct_heat` | 41,216 | **100%** | 18% of that scene, same shape |
| pair walk, campfire | 36 | 91% | — |

**100% barren is a decision, not a hint.** Both of those became a single
board-wide flag and a `goto`. Neither needed the design that was banked for
it. The counters cost minutes; the design they replaced would have cost a day.

Counters say how OFTEN, never how long — see "A counter delta is a prediction
about time, never a result". Pair the ratio with a phase split before
believing a size.

### The trap that makes a probe measure nothing at all

Build a scene by writing `s->cells` directly and `latch_content_flags()` never
runs, so `may_have_burning` stays false and `sand_step_reactions()` early-outs.
The probe then reports a clean, confident **zero** — of a pass that never ran.

```
    cells[i] = CELL_MAKE(MAT_FIRE, ...)   ->  calls = 0      WRONG
    sand_set(&g, x, y, ...)               ->  calls = 41,216 right
```

Build fixtures with `sand_set()`. This is also why the content flags that gate
a SKIP start `true`: a board filled by a raw write latches nothing, and a false
negative there loses a reaction rather than merely wasting work.

### A settled board is below the host clock and reads as zero

`settled_screen` and `plant_idle` come back as **0 us** from the host probe —
every candidate, every run. Their device figures are 52 and 54 us, the mean is
an integer division over 50 and 200 steps, and on a host each step costs a
fraction of a microsecond. `run_probe.py --compare` then prints `0 0 0 0.00%`,
which reads as "no change" and is really "not measured".

That matters more than it sounds. The settled-screen row is the one the
block-size sweep has always been decided on: a smaller block skips a busy board
more finely and scans more blocks on a still one, and only the still board
carries the second half. So the half of the trade that argues AGAINST a small
block is exactly the half the host cannot see, and a shape ranked on the host
alone will always come back "smaller is better".

Any row whose device figure is in the tens of microseconds belongs on the
device, not in a host ranking.

### When the measurement contains work you did not add

Timing a loop that touches every cell measures the loop, not your change. The
grove shading walk read 8,930 us — and 4,162 of that was the hash and the
material tests the renderer pays regardless.

| | us |
|---|---:|
| walk with the change | 8,930 |
| **control** — identical walk, change removed | 4,162 |
| attributable | **4,768** |

Run the control in the same test, on the same board, in the same capture.
Without it the headline figure overstates by nearly half — the same
attribution error as comparing two builds, one loop further in.

## Measuring on the device

### The single-step technique

A gate that is off changes the board, so timing twenty steps with a pass
disabled measures a *different simulation*. Instead:

1. rebuild the scene from its builder
2. warm up N steps with **every** gate on
3. flip one gate off
4. time **exactly one** step
5. restore, and take the **min** over repeats, never the mean

Every configuration then sees a byte-identical board at the moment it is
timed. This is valid because the scene builders and the RNG are deterministic
and a `volatile` read consumes no RNG.

Twenty steps with a gate off is only safe when the disabled work cannot feed
the passes that follow it in the same step — check `sand_step()`'s pass order
before relying on it.

### The harness that does the five steps

`suite_sand_split.h` — permanent, unlike the gates it drives, because
`strip-pass-gates.py` deliberately does not list it. A round writes its gates
and one row; the harness is already there.

```c
static const split_scene_t wet = {
    .name = "wet earth", .setup = split_enable_soak,
    .build = build_wet_earth_scene, .seed = 53u,
    .sgx = 0, .sgy = 1000, .gx = 0, .gy = 1000, .settle = 60,
};
static const char *const names[] = { "soak_dry" };
static volatile bool *const gates[] = { &sand_step_gate_soak_dry };
split_report(&wet, names, gates, 1, 3);
```

**A scene is its setup plus its builder plus its mid-settle pours.** Leave any
of the three out and the number is of a different scene. The first use of this
harness measured a soak stage at **1 µs** because it called
`build_wet_earth_scene()` but not `sand_set_soak()`, which that row does
*before* the builder — so nothing soaked and the gate had nothing to skip. With
the setup restored the same gate read **31,268 µs, 32%**.

**Check the whole-step figure against the row it mirrors before reading any
phase.** The broken fixture came in 47% under its budget row; the corrected one
lands within 3%. That one comparison catches the entire class.

**A negative phase means the gates do not partition.** Switching work off made
the step cost *more*, so something else absorbed it — gating the fall inside
`move_liquid_grain` left its mass to the slides and read −18%. Gates partition
between passes, not inside a function sharing a budget across its branches.

**A phase can fail to partition with no shared budget at all, because the
compiler duplicated the code between two gates.** Ten gates on the water scene
(2026-09-11): the four pass-level ones summed to 15,735 µs against 15,733 for
the same four flipped together, 0.01% apart. Three gates *inside*
`move_liquid_grain` summed to 2,689 µs against 2,034 together — 24% apart, and
repeatable to 1 µs, so not noise. The objdump said why: two gates in one inlined
region made GCC tail-duplicate the remainder of that region, so flipping one
gate does not skip its work, it moves execution onto a **second copy** of the
code with the other gate's check arranged differently. Two phases measured in
two different programs cannot be shares of one.

**Count a gate's load sites before reading its number**, which is the cheap
check that separates the two cases:

```sh
riscv32-esp-elf-objdump -d build.diag/launcher.elf | grep sand_step_gate_
```

One site per source call site means the gate isolates what it names. *More*
sites than the source has call sites means the compiler restructured around it
— in the round above, `move_splash` had three for one call, one of them in a
path where the guarded work can never run, loading the gate only to discard it
(`volatile`, so it could not be removed). That gate was pricing the
restructuring as well as the work; the single-site gates in the same image
partitioned perfectly. The fix is one gate per configuration, or a gate at a
real call boundary.

**Gate overhead is not a constant, and where it lands decides which figures are
clean.** The five-percent figure earlier rounds recorded was for a handful of
gates at pass boundaries. Ten gates, most of them per-cell in the liquid path,
cost **25%** on the water row (18,116 µs gated against 14,481 stripped) while
the two liquid-free controls moved 0.3% — the overhead follows the instrumented
code, not the image. It cancels wherever a gate's own load is paid in both
configurations, which is every gate measured alone; it does **not** cancel for a
region with gates nested inside it, since turning that region off stops paying
their loads too. So an outer figure (a whole pass, a whole call) is an upper
bound carrying its inner instrument, and the innermost figures are the clean
ones. Prefer few gates, and read an outer number as a ceiling.

### Four rules, each learned by getting it wrong

**Only within-capture comparisons are trustworthy.** Two builds of *identical*
simulation code measured 143,165 and 133,574 us on the same scene — a 6.7%
flash-layout spread, wider than most effects worth chasing. Comparing two
captures produced a phantom −5.5% win and a phantom +1.4% regression on the
same change, and a change was written to fix the regression that did not
exist. A gate flipped on one board in one image does not have this problem:
untouched phases hold to 1–2 us across it. Two differently-**scoped** builds
are the same trap with a wider mouth: never diff a perf-scoped capture
against an unscoped one.

*But read the controls before applying that rule.* The spread is not a
constant — it scales with how much the layout was disturbed. Two images
differing by a single skip returned **byte-identical** control rows (settled
sand 241, half-screen gas 55,642, mixed flip 11,157), and against controls
that flat a 0.5% delta is real. The controls tell you the resolution of the
comparison: check them before dismissing a small number as noise, and before
trusting one.

**Only size-neutral changes attribute cleanly.** A change that grows the hot
function relocates everything after it and moves every phase together. When
the controls move by ~1,000 us instead of ~2, the per-phase split is no longer
readable and only the whole-step number means anything.

**Check `nm` for a new out-of-line symbol before believing an inline-shaped
change.** A per-direction helper marked `static inline` was out-lined by GCC
and cost −14.5%; the hot function got *smaller*, and that shrinkage was the
symptom, not evidence of a win. A plain call costs ~27 cycles here.

Nor is smaller faster. `step_one_reacting_row` went from 9,340 to 8,410 bytes
— 10% smaller, 292 cache lines down to 263 — and the scenes that run it
hardest measured 0.5–1.4% **slower**, on boards holding none of the materials
the change touched. Instruction count, register pressure and gross layout were
all excluded; what moved was the function's offset within a 32-byte cache
line. Size is not the variable.

**An identical fingerprint proves nothing until the oracle reaches the new
path.** Mutate the grid *inside* the new branch and confirm `--check` moves. It
did not for a `conduct_heat` guard, nor for the tilted row skip — in the second
case because `grid_fingerprint.c` pins gravity to `(0, 1000)` for all six of
its scenes, so no fingerprint scene can reach `py != 0` at all.

## Retiring the instrumentation

At the end of a round, strip **all** of it — not some. Partial removal leaves
two idioms side by side and a reader cannot tell "never gated" from "gate
removed once answered".

The strip is behaviour-neutral by construction, and that is mechanically
checkable rather than a matter of care: gated source built with gates **off**
and stripped source must produce a **byte-identical release object file**. A
strip that changes an object file changed the program, whatever the diff looks
like. Some gates restructure control flow to exist (`burn_decay` turned an
`if/else` into a `bool` plus a block), so unwrapping them by hand is a rewrite,
not a deletion — which is exactly why the check is on the object file and not
on the diff.

