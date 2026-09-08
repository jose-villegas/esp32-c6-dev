# Sand App Docs

The falling-sand app's own documentation folder, referenced by name from
[`../notes/README.md`](../notes/README.md) since this folder split out of
the platform notes once there was enough sand-specific material to justify
its own set. Ten files, three jobs:

**How it works today:**

- **[Sand-Simulation.md](Sand-Simulation.md)** — the app in depth:
  materials, the liquid model, momentum, chemistry, the performance
  budget every design choice answers to.
- **[Architecture.md](Architecture.md)** — a single-page map of
  `main/apps/sand/`'s shape (the grid byte, the material table, the file
  split) rather than the reasoning behind it.
- **[Impulse-Mechanics.md](Impulse-Mechanics.md)** — explosions, thrown
  chunks, and a liquid's own splash: one mechanism, three call sites.
- **[Reaction-Table.md](Reaction-Table.md)** — generated, current
  material-interaction rules. Regenerate with
  `tools/report_reactions.sh`, don't hand-edit the generated region.
- **[Shading-and-Colour.md](Shading-and-Colour.md)** — how a cell's
  material and variant become a pixel, and the traps specific to that.
- **[Tuning-At-a-Glance.md](Tuning-At-a-Glance.md)** — sand constants and
  their current values, as a scoreboard rather than prose.

**How to change it:**

- **[Adding-a-Material.md](Adding-a-Material.md)** — the checklist for
  adding a whole new material.
- **[Perf-Round-Guide.md](Perf-Round-Guide.md)** — the entry point for a
  fresh session told to run a sand performance round. Read this first,
  not the two files below.

**Discovery narratives** (how the above got the way it is, not a
reference for it):

- **[Simulation-Lessons.md](Simulation-Lessons.md)** — the bugs found and
  the reasoning behind each fix, from the first performance pass through
  the sleeping/friction/timestep design that shipped.
- **[Performance-Tuning-Attempts.md](Performance-Tuning-Attempts.md)** —
  the chronological record of every real-hardware performance attempt
  since, numbered in the order they happened.

## Related

- [`../plans/`](../plans) — plans that touch this app.
  `Reaction-Doc-Generator-Plan.md`'s brush-blurb phase is the only
  unbuilt one; `Metal-Smelting-Plan.md` shipped and is kept for the
  numbers tables the code cites.
- [`../notes/README.md`](../notes/README.md) — the hardware constraints
  (no PSRAM, the memory budget) this app's numbers are shaped by.
- [`../Testing-Guide.md`](../Testing-Guide.md) — how any of this gets
  verified, on host and on device.
