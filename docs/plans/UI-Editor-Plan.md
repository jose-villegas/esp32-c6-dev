# Plan: a UI editor, and the layout format underneath it

**Status**: planned 2026-09-10, not built. Written after the sand app's brush
screen shipped and a host-side preview of it landed
([`Sand-Brush-Screen-Plan.md`](Sand-Brush-Screen-Plan.md),
`apps/sand/tools/brush_screen_preview.c`).

The objective is a tool where a screen is **authored visually and edited
again later** - not screenshotted and re-typed. The brush screen should open
in it as an editable instance. It is a component of the engine direction in
[`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md), sibling
to the level editor already banked as `bd esp32c6-ems.9`.

---

## This pattern already runs in this repo

`tools/boot_anim_editor_server.py` serves `tools/boot_anim_editor.html` and
answers `POST /render` with **a real frame rendered by the real firmware
code** - `main/gfx/gfx.c` plus `main/boot/boot_anim.c`, unmodified, compiled
for the host. It writes the edited payload to a *scratch* copy of
`boot_anim_timeline.json` placed on the include path ahead of the committed
one, so a draft never touches the real header. It hashes the payload so
scrubbing time never recompiles, and it has a build-and-flash path out the
back.

That is the architecture, running, for one payload. A level editor
(`ems.9`) proposes it for a second. This plan is the third:

| | authored data | generator | rendered by |
|---|---|---|---|
| boot animation | `boot_anim_timeline.json` | `gen_boot_anim_timeline.py` | real `boot_anim.c` + `gfx.c` on host |
| level editor (`ems.9`) | material blocks | bake to a header | real sand code on host |
| **UI editor (this)** | **a screen's layout** | **bake to a header** | **real `gfx.c` + pure geometry on host** |

They are not three tools. They are one pattern with three payloads, and the
pattern is already proven.

## The one thing that has to change

`brush_screen_layout()` computes rects from arithmetic over `#define`s -
`HEADER_H`, `MODE_H`, `SIZE_H`, gaps, `UI_MARGIN`, a centred remainder. **An
editor cannot edit arithmetic, only data.**

So a screen becomes authored JSON, baked by a generator into a header that
the same pure function consumes. Exactly the move the boot animation already
made, and the fifth instance of the generated-file convention in CLAUDE.md
(banner naming the regenerate command, generator validates before emitting,
shipped artifact tested independently of the generator).

**The device never sees the editor, and never sees JSON.** It links a static
baked table: no runtime layout engine, no solver, no allocation, no RAM
cost - the same deal fonts, icons and the boot timeline already have. That
distinction is the whole reason this is affordable here and LVGL was not:
the cost of a retained UI system is paid at build time, on a laptop, or it
is not paid at all.

## What already exists, and does not need building

- **The render path.** `apps/sand/tools/brush_screen_preview.c` renders the
  screen at both orientations, on a host, through the real `gfx.c` and the
  real `ui_style.h` / `ui_slider.h` / `gfx/icon.h` geometry. That is what a
  `/render` endpoint needs; it is already written.
- **Host-linkable everything.** `gfx.c` (behind its `ESP_PLATFORM` guards),
  the pure geometry headers, the baked icon atlases, and `microui.c` - the
  last of these linked for `suite_ui_pointer_microui.c` and available now.
- **The validation.** `suite_brush_screen.c` already asserts, at both
  368x448 and 448x368: everything inside the canvas, no panel overlap, equal
  segment widths, a 44px floor on every tap target, and every fixed string
  measured against its own rect.

That last one matters more than it looks - see below.

## The data model: grow it from need, and do not build a solver

The trap is a general constraint system. That is the LVGL mistake relocated
to build time: it makes the model unbounded and turns the editor into a
programming language with a mouse.

What the screens in this tree actually need today is roughly six concepts: a
vertical stack, fixed-versus-fill heights, gaps, margins, centred slack, and
rows within a panel. Express those, and add a seventh when a real screen
demands it rather than in anticipation.

**Text is a first-class constrained thing, not a string dropped in a rect.**
Both real defects the brush screen shipped were text-versus-box - a value
box sized by eye at 64px for a string needing 80, and a caption row 232px
wide for wording needing 240 - and the weakness the host preview then found
in portrait is a *scale policy* failing: a long material name steps down
until it is the same size as the caption above it, and the type hierarchy
collapses. So an entry carries its string source, its scale policy and its
box **together**. An editor that cannot say "this will not fit at this
scale" would let you draw those same bugs, visually, and call it a design.

## Phases

1. **Layout as authored data. No editor.** A screen's JSON, a generator, a
   baked header, and `brush_screen_layout()` reading the table instead of
   computing it.

   **Acceptance: `suite_brush_screen.c` passes untouched, and the baked
   rects are identical to what the function produces today.** Same discipline
   the icon baker used - a generator that cannot reproduce known-good output
   is not ready to produce new output. This phase changes no pixels.

2. **Validation moves into the generator.** It refuses, at bake time and for
   every orientation, a layout that overlaps, leaves the canvas, drops a tap
   target below 44px, or gives a string a box it does not fit in. The host
   suite keeps its own assertions as the independent witness - the generator
   checking itself is not a test.

3. **The editor server.** Point the boot-anim pattern at this payload: serve
   a page, `POST /render`, write the draft to a scratch JSON, run the
   existing preview tool, return the PNG. Both orientations side by side,
   because that is where composition decisions actually get made.

4. **Direct manipulation.** Drag and resize in the browser, writing back to
   the JSON. Deliberately last: it is the least load-bearing part, and a
   format that only a GUI can produce is a format nobody can review in a
   diff.

5. **A second screen proves the model.** The palette, or the launcher. A
   format that has only ever expressed one screen has proven nothing about
   being a format.

## Considered and rejected

- **A runtime layout engine.** The thing microui was chosen over. Layout
  resolved on device costs RAM and cycles for a picture that is identical
  every frame; bake it instead.
- **A general constraint solver.** Unbounded model, unbounded editor, and
  every screen in this tree is a stack of panels. Revisit only when a real
  screen cannot be expressed.
- **Round-tripping generated C.** The generated header is output and never
  input - the convention every other generated file here already follows.
  Parsing back what a generator emitted is how the authored source and the
  artifact drift.
- **A JavaScript reimplementation of the renderer for the editor.** The boot
  animation editor rejected exactly this and says so in its own header: it
  renders through the real C rather than a JS twin. A preview that is not
  the shipping renderer is a preview of something that does not exist.
- **Editing the mockup instead.** A design image is an input to authoring,
  not the authored artifact. The brush screen already diverges from its
  mockup in two accepted places, and those decisions live in the plan, not
  in a PNG.

## Related

- [`../Building-a-Screen.md`](../Building-a-Screen.md) - how a screen is built by hand today
- [`../UI-Lessons.md`](../UI-Lessons.md) - what the constraints above cost to learn
- [`Icon-Baker-Plan.md`](Icon-Baker-Plan.md) - the same authored-data-to-baked-header pattern, for artwork
- [`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md) - the engine direction this serves
