# Plan: a generated description for every material

**Status**: planned, not built. Written 2026-08-27.

The end goal is **a short description for each brush**, shown in the game.
Getting there needs a prose record of what every material actually does —
and that record already exists as data, in `reactions[MATERIAL_MAX]` and
`extended_reactions[]`. 41 fields per row, not one branch in code.

So this is not a documentation chore that might later be reused. The
markdown table is the *by-product*; the deliverable is a sentence per
material, generated from the table, checked against it, and eventually
compiled into the firmware.

---

## Why generate rather than write

The hand copies have already drifted:

- the mermaid reaction chain in `Adding-a-Material.md` (~line 360) still has
  **EMBER** as a node. `grep EMBER material.h` returns nothing.
- that chart predates `MATX_ICE`, `MATX_PLANT`, `MATX_LEAF` and every field
  added since — soaking, growing, budding, drinking, hardening, sheltering.
- `Sand-Simulation.md` (~line 294) hand-maintains a byproduct table.

`main/apps/sand/tools/report_performance.sh` already made this argument for
its own table: generated fresh "so it can never go stale the way a
hand-transcribed copy can". Same problem, one table over — except here the
stale copy would be shown to the player.

---

## Where it lives

`launcher/main/apps/sand/tools/dump_reactions.c`, app-owned. The tools
reorganisation that landed first put `apps/<name>/tools/` in place for
exactly this, and two mechanisms now make the location safe:

- `main/CMakeLists.txt` filters `/apps/[^/]*/tools/` out of the recursive
  firmware glob, so a `main()` here is never force-linked into the image by
  `WHOLE_ARCHIVE`. **This generator is the first thing to actually exercise
  that guard — it has never been verified by a device build.**
- `test/run_tests.sh` globs `apps/*/*.c`, one level deep, so a `tools/`
  subfolder is invisible to the host test binary without special-casing.

**Compile the tables, do not parse them.** `MATX(MATX_LEAF)` and
`SAND_SHOCK_HEAT` (itself `SAND_AMBIENT_HEAT + 2`) are constant
expressions; resolving them by regex means reimplementing the preprocessor.

Linking: `material.c` alone is enough for the tables, but the cause clauses
below live in `sand_reactions.c`, so the tool links that too. It builds on
a host today — the test runner already compiles both.

---

## The grammar

A reaction sentence has five slots:

| slot | e.g. | source |
|---|---|---|
| subject | Glass | row index -> `material_name()`. **Generated.** |
| verb | shatters | field name, de-snake-cased. **Generated**, ~4 irregulars |
| object | Sand | field value, decoded by kind. **Generated.** |
| rate | readily | value -> adverb ladder. **Generated**, overridable |
| cause | if chilled while hot | **not in the table.** Declared at the read site. |

Plus a sixth notion, **what changes**, defaulting to the subject — because
`drinks` does not change the subject at all (see the wetting family below).

### Group before emitting

The failure mode to avoid is one sentence per field. Fields carry a group
tag and each group emits **one** clause. `MAT_GLASS`'s real row is five
fields and three ideas:

```
temperature  {heat_ramp 64, cools 5, conducts 220}  -> "Holds heat readily and passes it on."
transform    {heats_to MAT_LAVA + heat_ramp}         -> "Melts to lava under long heat."
unmake       {shatters_to MAT_SAND}                  -> "Shatters back to sand if chilled while hot."
```

against the spec-sheet version the same data produces without grouping —
*"Glass conducts at 220. Glass has heat ramp 64. Glass cools at 5."*
Roughly 8 groups over the 41 fields.

Order clauses by what a player does — how you make it, what it does, how it
is unmade — not by struct order. One sort key per group.

### The rate ladder

`SIM_HZ` is 60, so one step is 16.7 ms and expected time against a single
adjacent partner is `256/value` steps. Run over the real table:

| | value | steps | seconds |
|---|---|---|---|
| gas `flammability` | 255 | 1 | 0.02 |
| glass `conducts` | 220 | 1.2 | 0.02 |
| snow `heat_chance` | 120 | 2.1 | 0.04 |
| acid `dissolves` | 60 | 4.3 | 0.07 |
| oil `flammability` | 50 | 5.1 | 0.09 |
| sand `heat_chance` | 16 | 16 | 0.27 |
| wood `drinks` | 12 | 21 | 0.36 |
| sand `soaks` | 8 | 32 | 0.53 |
| wood `flammability` | 6 | 43 | 0.71 |
| snow `thaws` | 4 | 64 | 1.07 |

**The whole table resolves inside about a second.** That is the thing to
design against: raw 1–255 looks like a wide range and is not one.

| seconds | value | word |
|---|---|---|
| <=0.05 | >=85 | instantly |
| <=0.2 | >=21 | fast |
| <=1.0 | >=5 | readily |
| <=3 | >=2 | steadily |
| >3 | 1 | slowly |

Oil reads **fast**, wood **readily** — which is how they actually play. An
earlier draft of this ladder bucketed raw chance instead of time and called
them "slowly" and "barely". Both were wrong, and the correction came from
playing the game, not from the table.

### The ladder lies in two directions, so the word is overridable

**Partner count.** The roll is per adjacent partner per step, so a flame
front on three faces catches roughly three times as fast as the
single-partner arithmetic says.

**Persistence.** Sand->glass computes to 0.27 s, but `heat_chance`'s own
comment measured 18 steps to half a bed and 137 to all — about 2.3 s — and
says it should feel like "something you set up and wait for". Fire rises
away, so sand rarely *has* a sustained neighbour. The model assumes one
steady partner; that reaction never gets one.

So `adverb` is an optional declared field, defaulting to the computed
bucket. Most fields take the default. Flagged for a by-feel pass before
phase 2 ships: **sand->glass** (computes fast, plays slow), the **ignition
family** (partner count), and — raised and not yet settled — **acid
`dissolves`** and **snow `thaws`**.

### Kind cannot be inferred

```c
#define CELL_MATERIAL(c)    ((uint8_t)((c) >> 4))
```

Material is the HIGH nibble, so a plain material id decoded as a cell spec
reads as material 0, variant n — a confidently wrong name, silently.
`canopy_to`, `sprouts_to` and `shatters_to` are **cell specs**; every other
`_to` is a **material id**. Same suffix, different decoding, so `_to` fields
carry an explicit kind.

---

## The cause clause belongs at the read site

`shatters_to` is read at two places in `sand_reactions.c`, under two
different conditions:

```c
r->shatters_to != 0 && CELL_VARIANT(n) <= SAND_SHOCK_COLD   /* :426  chiller side */
temp >= SAND_SHOCK_HEAT && nr->shatters_to != 0             /* :2131 hot side */
```

Neither threshold appears in `reactions[]`. "Hot enough, and touched by
something cold" is a property of the code that READS the field, so no
script over `material.c` recovers it.

Putting that clause in a table inside `tools/` would be a hand-written copy
of a condition living in another file — the drift problem, relocated. This
repo already has the idiom for the alternative, used twice
(`SUITE_REGISTER` in `test/suites.h`, `APP_REGISTER` in `main/app.h`):
**things register themselves, so there is no list to keep in step.**

```c
/* SHOCK, which does not wait for the chilling roll... */
REACTION_DOC(shatters_to, "if chilled while hot");
if (temp >= SAND_SHOCK_HEAT && nr->shatters_to != 0) {
```

A constructor pushes it into a registry the dumper walks. The clause sits
against the `if` it describes, so a reviewer editing the condition sees the
stale text in the same hunk. Two read sites give two registrations, which a
single `cause` per field could not express at all.

Guard the strings out of device builds the way suites are (`DEVICE_BUILD`),
so they cost the firmware nothing.

Roughly 15 of 41 fields need a clause. The rest are self-describing "per
adjacent X per step" rolls where the partner IS the trigger.

---

## The pairwise join is phase 1, not phase 2

The wetting family is why. Current values:

| owner | fields | value |
|---|---|---|
| `MAT_WATER` | `wets` | 1 — a boolean gate, no rate |
| `MAT_SAND` | `soaks` / `soaks_to` | 8 / `MAT_DIRT` |
| `MAT_DIRT` | `soaks` / `dries` | 60 / 5, **no `soaks_to`** |
| `MAT_WOOD` | `drinks` | 12 |
| `MATX_PLANT`, `MATX_LEAF` | `drinks` | 40 |

Four distinct reactions:

```
| A     | B    | becomes                  | rate   | note                    |
| Water | Sand | Dirt, at moisture 1      | 8/256  | water pays a unit       |
| Water | Dirt | Dirt, +1 moisture        | 60/256 | no material change      |
| Dirt  | -    | Dirt, -1 moisture        | 5/256  | self-driven, no partner |
| Water | Wood | the SOIL AT ITS ROOT, +1 | 12/256 | wood itself unchanged   |
```

Three lessons, each of which breaks a simpler design:

1. **The gate and the rate are on opposite sides.** `wets = 1` says
   *whether*, never how fast; 8 and 60 live on the absorber. "Water + sand
   -> dirt @ 8/256" exists in neither row. A per-material section alone
   would print "Water: wets" and "Sand: soaks 8 -> Dirt" and leave the
   reader to do the join.
2. **`soaks_to == 0` is a real reaction with no target.** Water + dirt ->
   dirt, one level wetter. A generator iterating `*_to` fields emits
   **nothing** for it — a table that looks complete with a hole in it, the
   worst available failure here. Join rules key on `soaks` and branch on
   whether `soaks_to` is zero. Same for `dries`, self-driven with no
   partner at all.
3. **`drinks` changes a third cell.** Subject, partner and the thing that
   changes are three different cells. Hence the sixth slot above.

`wets` exists *because* of a bug — sand under oil, and under lava, turning
to saturated soil. A generated row reading "liquids that wet: Water" is the
documentation that would have caught it.

About seven join rules cover the table: `dissolves` x `dissolvable`,
`flammability`/`ignites_to` x `burns`, `quench_to` x water, `heats_to` x
`burns`, `chills` x `heat_ramp`/`shatters_to`, `wets` x `soaks`, `thaws` x
any liquid.

---

## The gate

Every field in `reaction_t` is `uint8_t`, so there is no padding and the
struct's size IS its field count:

```c
_Static_assert(ARRAY_LEN(field_docs) == sizeof(reaction_t),
               "every reaction_t field needs a row in field_docs[]");
```

41 today. Add a field and the generator stops compiling until someone
supplies its group, its kind if ambiguous, and a cause clause if the
trigger is a code condition — the things a script provably cannot derive,
and nothing else.

The count alone does not catch a duplicated or skipped `offsetof`, so a
startup check walks `field_docs[]` and asserts the offsets are exactly
`0 .. sizeof(reaction_t)-1` with no repeats.

---

## The blurb table

Not a field on `material_t`: its own comment defends keeping that row
inside a cache line, and it is read several times per cell per step. A
blurb pointer there costs the hot path for something only the brush picker
reads. That is the same argument `reactions[]` already won.

A third cold table, indexed like `reactions[]`, same extended-range split.
~14 strings x ~90 chars is on the order of 1.3 KB of `.rodata`.

All materials get a record, not just brushes. `brushes[]` in `app_sand.c`
holds 14 cells today; `MAT_STEAM`, `MAT_SMOKE` and `MATX_LEAF` are things a
player *creates* and will want to understand, so the table covers them and
`brushes[]` stays the UI's subset.

**Audit mode** is what keeps the blurbs honest: flag any reaction a
material participates in that its blurb never mentions, and any blurb
claiming something the table no longer supports. When weathering was
reverted mid-session, this would have said "sand's blurb describes a crust;
no field supports it."

Showing a blurb is UI work — where it fits on the panel, whether it wraps
or times out like the mode label at `app_sand.c:812` — and is deliberately
out of scope here.

---

## Phasing

**Phase 1 — get a table on screen.** `dump_reactions.c`: field spec with
group/kind, the static assert, the rate ladder, the pairwise join, and a
generated `docs/Sand/Reaction-Table.md`. Plus `report_reactions.sh` in the
same folder, and a `--check` mode wired into
`.github/workflows/host-tests.yml` (no new CI dependency: gcc and a diff).

**Additive only — phase 1 touches no simulation source.** New files
(`dump_reactions.c`, `report_reactions.sh`, the generated
`Reaction-Table.md`) plus one line in the CI workflow, and nothing else. It
reads `material.c`'s tables by linking them; it does not edit them.

That is worth protecting rather than treating as a happy accident. The
simulation is under active change — weathering landed and was reverted
inside a single session — so anything that edits `material.h` or
`sand_reactions.c` buys merge conflicts for no benefit. It also means phase
1 belongs in **its own worktree**: it cannot break the sim, so it need not
wait on it.

Two items that DO touch source are therefore *not* phase 1, and get their
own small change: the `material.h` comment-order fix below, and the
table-integrity test for `suite_sand.c`. Phase 2's `REACTION_DOC` edits
`sand_reactions.c` and is likewise its own step.

Deliberately ships with **no hand-written text at all**: every adverb takes
its computed bucket, and every cause renders as a visible `[TODO: trigger]`
placeholder. The point is to have real output to look at as early as
possible — the adverb calibration, the groupings and the clause wording are
all far easier to judge against a generated table than in the abstract, and
the placeholders show exactly how many clauses phase 2 owes. Tune after
seeing it, not before.

**Phase 2** — `REACTION_DOC` registration in `sand_reactions.c`, cause
clauses at every read site, the by-feel adverb pass, and the retirement of
the EMBER chart from `Adding-a-Material.md`.

**Phase 3** — the blurb table, audit mode, and the sentence-per-material
output the whole thing is for.

**Optional** — the mermaid chain, and node colours from
`material_colours()` (`gfx_color_t` is RGB565 byte-swapped; getting back to
`#rrggbb` means un-swapping and expanding 5/6/5). Demoted from the earlier
draft: a graph cannot express a variant-only reaction — `Dirt -> Dirt` is a
self-loop that reads as noise — so the diagram needs different emission
rules from the table, not a shared walk.

---

## A test worth adding, as its own change

`suite_sand.c` asserts individual reaction values but never sweeps the
tables. Add one test that walks both and asserts every `_to` target names
something that exists — a material id below `MAT_COUNT`, or a cell spec
whose extended nibble has a name in `extended_names[]`. A row pointing at a
dead slot is a live bug nothing would currently catch, and the generator
reads those same fields.

---

## Found while planning (fix separately — see phase 1's additive-only rule)

**`material.h` comment order.** The `SOAKING UP A LIQUID` block sits above
the `WETTING` block, and `soaks` / `soaks_to` are then declared bare two
lines *below* `wets`. Everywhere else the comment is immediately above its
field. Fix it in the same change.

**This plan's own drift.** An earlier draft said 41 fields were 43 and
reported a second loose comment block around `withers`. Both came from
reading `material.h` before `5ea0be0 Revert weathering: it cannot be had
for free` landed mid-session. Recorded because it is the same failure the
whole plan is about: a hand-written copy of the table, stale within the
hour.

---

## Deferred: the palette does not survive being used as text

Material names are coloured with the device's exact palette values. Measured
against a 3:1 contrast floor on both GitHub themes, **14 of 18 fail
somewhere**:

    unreadable on DARK    Oil #101008, Water, Wood, Lava, Acid, Dirt, Plant
    unreadable on LIGHT   Leaf, Sand, Ice, Gas, Fire, Snow, Steam
    fine on both          Glass, Stone, Smoke, Metal

The cause is structural, not a bad palette: **it was designed to fill cells,
not to draw glyphs.** Oil against black works as a solid region of pixels and
vanishes as thin letterforms. Area and text are different legibility
problems, and one set of values cannot serve both.

Two ways out, when this is picked up:

- **Lift only the seven that fail on dark.** Keeps every pale colour
  byte-exact, assumes a dark reader. Minimal fidelity loss, and it fixes the
  case actually complained about (oil).
- **Clamp everything into a both-themes band.** The contrast maths puts that
  band at relative luminance 0.117-0.30, narrow enough that hue becomes the
  only separator - and Snow, Steam, Ice and Gas are pale blue-whites
  distinguished BY lightness today. They would collapse into near-identical
  mid-blues. Probably worse than the problem it solves.

Deliberately left for later (2026-08-27): the table is legible enough to tune
prose against, which is what it is for at this stage.

Measure rather than eyeball - the script that produced the table above lives
in this session's scratchpad as `contrast.py` and takes the generated file as
its argument.

---

## Decisions taken

**Generate the sentences; do not hand-write them.** An intermediate draft
recommended hand-authoring ~14 blurbs for voice and using the tool only to
audit. Overruled, correctly: the objection was to naive
one-clause-per-field emission, not to generation. Grouping, an adverb
ladder and a declared cause clause produce natural subject-verb-object
sentences, and generation is what makes it cover every material rather than
only the 14 brushes — and what keeps it from drifting.

**Gate the un-derivable, script the rest.** 33 of 41 fields have a usable
first sentence in their own comment and the other 8 inherit from a sibling
by naming rule, so a scraped notes column is available if wanted. What no
script can produce is the trigger, the material-id/cell-spec distinction,
and an adverb where partner persistence breaks the arithmetic. Those three
are what the static assert demands.

**One generated doc, not edits into existing ones.**
`Adding-a-Material.md` and `Sand-Simulation.md` keep their prose and link
to the generated table. Generating *into* a hand-written document needs a
marker-block splicer and fails much worse.
