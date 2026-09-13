# Sand App Docs

The falling-sand app's own documentation folder, referenced by name from
[`../notes/README.md`](../notes/README.md) since this folder split out of
the platform notes once there was enough sand-specific material to justify
its own set. Eight files, two jobs:

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
- **[Metal.md](Metal.md)** — metal: smelted out of dirt by lava, and the
  only material that moves heat a long way.
- **[Shading-and-Colour.md](Shading-and-Colour.md)** — how a cell's
  material and variant become a pixel, and the traps specific to that.

**How to change it:**

- **[Adding-a-Material.md](Adding-a-Material.md)** — the checklist for
  adding a whole new material.

## Related

- [`../plans/`](../plans) — plans that touch this app. Only
  `Reaction-Doc-Generator-Plan.md`'s brush-blurb phase is still unbuilt.
- [`../notes/README.md`](../notes/README.md) — the hardware constraints
  (no PSRAM, the memory budget) this app's numbers are shaped by.
- [`../Testing-Guide.md`](../Testing-Guide.md) — how any of this gets
  verified, on host and on device.
