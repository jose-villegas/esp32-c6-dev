# Plan: a short description for every brush, in the game

**Status**: the generator is built. `dump_reactions.c`, the splice
mechanism that lets hand-written mechanics coexist with generated rows
(see `report_reactions.sh`'s own top comment), cause clauses via
`REACTION_DOC`, the rate ladder and contrast-safe colour all ship, and
`docs/sand/Reaction-Table.md` is regenerated from them.

What remains is the thing the generator was built for: **a short
description for each brush, shown in the game**. The record of what every
material does already exists as data in `reactions[]` and
`extended_reactions[]`; this turns it into player-facing text.

Everything below is the unbuilt part. For how the generator works, read
`dump_reactions.c` and `report_reactions.sh` - they are the live record,
and this plan is no longer it.

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
generated `docs/sand/Reaction-Table.md`. Plus `report_reactions.sh` in the
same folder, and a `--check` mode wired into
`.github/workflows/host-tests.yml` (no new CI dependency: gcc and a diff).

**Additive only — phase 1 touches no simulation source.** New files
(`dump_reactions.c`, `report_reactions.sh`, the generated
`Reaction-Table.md`) plus one line in the CI workflow, and nothing else. It
reads `material.c`'s tables by linking them; it does not edit them.

That is worth protecting rather than treating as a happy accident. The
simulation is under active change — weathering landed and was reverted
within a day — so anything that edits `material.h` or `sand_reactions.c`
buys merge conflicts for no benefit. It also means phase 1 **can land
independently**: it cannot break the sim, so it need not wait on it.

Two items that DO touch source are therefore *not* phase 1, and get their
own small change: the `material.h` comment-order fix below, and the
table-integrity test for the sand test suite (`suite_sand_*.c`). Phase 2's `REACTION_DOC` edits
`sand_reactions.c` and is likewise its own step.

Deliberately ships with **no hand-written text at all**: every adverb takes
its computed bucket, and every cause renders as a visible `[TODO: trigger]`
placeholder. The point is to have real output to look at as early as
possible — the adverb calibration, the groupings and the clause wording are
all far easier to judge against a generated table than in the abstract, and
the placeholders show exactly how many clauses phase 2 owes. Tune after
seeing it, not before.

**Phase 2 (done)** — `REACTION_DOC` registration in `sand_reactions.c`, cause
clauses at every read site (`shatters_to`'s two thresholds, `spoils_to`,
`hardens_to`'s still-attached exception), and the by-feel adverb pass (the
3-bucket ladder with a silent middle and 0/255 as absolutes, plus one
measured exception - sand's `heat_chance` computes fast and plays slow).

The EMBER chart in `Adding-a-Material.md` was not retired as originally
planned here. A separate line of work kept that hand-drawn diagram instead
and updated it as the simulation grew (dirt, metal, and more since) - a
live, actively-maintained chart, not the stale one this plan was written
against. Only the "two independent axes" diagram's own stale ember mention,
in a different section, still needed fixing, and has been.

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

The sand test suite (`suite_sand_*.c`) asserts individual reaction values
but never sweeps the tables. Add one test that walks both and asserts
every `_to` target names
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

## Resolved: the palette did not survive being used as text

Material names are coloured with the device's exact palette values. Measured
against a 3:1 contrast floor on both GitHub themes, 14 of 18-plus materials
failed somewhere - roughly half unreadable on a dark background, the rest on
a light one.

The cause was structural, not a bad palette: **it was designed to fill
cells, not to draw glyphs.** Oil against black works as a solid region of
pixels and vanishes as thin letterforms. Area and text are different
legibility problems, and one set of values cannot serve both.

Fixed by lifting or darkening only the colours that actually fail, hue and
saturation held, moved the least distance that clears 3:1 on both
backgrounds - not the alternative of clamping every colour into one narrow
band, which was considered and rejected: it would have collapsed several
pale, hue-distinguished materials (snow, steam, ice, gas) into
near-identical mid-tones. The adjusted values live in `LEGIBILITY_OVERRIDES`
in `dump_reactions.c`, each carrying the raw value it was computed against;
a startup check recomputes that raw value live and refuses to build if the
device palette has since moved out from under it, so a stale override
cannot ship silently.

Measure rather than eyeball. The generator's own colour list follows this
same WCAG relative-luminance formula, not plain HSL lightness - see
`LEGIBILITY_OVERRIDES`'s own comment for the exact method if this ever needs
recomputing.

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
