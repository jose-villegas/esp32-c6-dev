# UI Lessons

The discovery narrative for the UI layer - the bugs found while building the
sand app's brush screen and the reasoning behind each fix, in the order they
came up. For how the UI actually works today, read
[`Launcher-Architecture.md`](Launcher-Architecture.md) instead; the two are
deliberately separate, one a reference and the other a history, exactly as
[`sand/Simulation-Lessons.md`](sand/Simulation-Lessons.md) sits beside
[`sand/Sand-Simulation.md`](sand/Sand-Simulation.md).

The brush screen was the first UI in this shell more complicated than a list
of buttons: two text sizes, a segmented control, a draggable slider, icons,
a textured swatch, and a panel over a paused simulation. Almost none of that
existed in `ui/` beforehand. What follows is what building it actually
taught, including the part that shipped broken.

---

## The toolkit's own rules decide more than the design does

Six things the design asked for were not "write some drawing code" problems.
Each turned out to be a constraint of microui's model, and the design was
downstream of it every time:

| The design wanted | What actually decided the answer |
|---|---|
| a draggable slider | the touch-to-mouse bridge, not the widget |
| two text sizes | the repaint hash, not the font |
| icons | the command list, not the artwork |
| one segment lit of three | microui has no "selected", so the app supplies the cue |
| a panel over paused sand | blending reads the destination |

The useful generalisation: before designing a control, work out which
microui invariant governs it. The drawing is the easy half and almost never
the half that goes wrong.

## Hit-testing belongs to microui, never to hand-rolled coordinates

Already learned once before this screen, in the palette: it used to draw
tiles as microui commands and hit-test them by hand against raw screen
coordinates. That split is what kept the panel from ever rotating - a
transform moved the drawing without moving where the hit-test looked.

The rule that came out of it, and that the brush screen followed from the
start: **the caller hit-tests through a real control and reports WHICH thing
was hit; a pure module decides what that MEANS.** `sand_ui_tile_clicked()`
and `sand_ui_mode_clicked()` are both that shape. It keeps the decision
host-testable and it keeps the geometry honest under a transform.

Where a control microui does not have is needed - a segment with an icon
above a label - build it from `mu_get_id()` + `mu_update_control()` rather
than hand-rolling a hit test. `ui_slider_int()` is the in-tree example.

## A touchscreen needs two synthesized hover frames, and this shipped broken

The worst bug of the whole feature, and the most instructive.

microui encodes a mouse: point, then click. A touchscreen cannot produce the
first half - the pointer does not exist until a finger is already down - so
`ui_pointer.c` synthesizes it. Originally the press was delivered across two
frames, with mousedown and mouseup fired **together** on the second.

Making a slider draggable meant holding `mouse_down` across frames. That
change was reviewed twice, shipped, and left **no app reachable from the
launcher**: every button drew its pressed frame and returned 0 forever.

The mechanism, worth internalising because nothing about it is guessable:

- `mu_mouse_over()` requires `in_hover_root()`;
- `mu_begin()` copies `hover_root` from the **previous** frame's
  `next_hover_root`, so it lags a frame;
- `mu_update_control()` marks a control hovered only when the mouse is over
  it **and `mouse_down` is clear**;
- focus is only ever taken from a control that is already hovered.

The old same-frame release left `mouse_down` clear during the widget pass,
so hover and focus landed together and the button submitted. Holding meant
hover was never established at all. The fix is two MOVE-only frames before
the press: the first establishes which window the finger is in, the second
marks the control hovered, then `DOWN` lands on something focusable. Tap
latency went from ~24 ms to ~48 ms, and that is the real price of a
mouse-shaped toolkit on glass.

**`Launcher-Architecture.md` already documented the constraint that got
broken** - "`mu_update_control()` only establishes hover on a frame where
the button is *not* held" - in the very section about this mechanism. Nobody
re-read it. Before changing a mechanism, read the doc about that mechanism.

## A pure module's unit test is not a test of its consumer

`suite_ui_pointer.c` asserted the event list `ui_pointer_step()` produces,
and was fully green throughout the outage above. It had to be: the events
were correct. **A list of events is not a click** - only microui decides
that, and microui's rules are the ones that were violated.

So `microui.c` now links into the host build for exactly one suite,
`suite_ui_pointer_microui.c`, which drives the real widgets: a tap must
submit exactly once, a hold must not re-fire, a drag must move a slider.
Red-checked by reverting the policy, where it reproduces the field symptom
rather than merely failing.

The general rule: **if a pure module's output is consumed by something with
state rules of its own, and that consumer is portable, link the consumer
into the host tests.** Asserting the output alone verifies half a system.
The same lesson in the simulation is
[`sand/Simulation-Lessons.md`](sand/Simulation-Lessons.md)'s isolated-state
debounce that passed while the real per-row integration was broken.

## Text must be measured against the box it has to fit

Two separate bugs on one screen, both the same shape, both invisible to
every test that existed:

- `SIZE_VALUE_W` was 64px by eye; `"06 PX"` is 5 characters at scale 2 = 80px.
- the size caption row is 232px in portrait; the design's `POUR BRUSH SIZE`
  needs 240px. Every variant clipped its own tail.

Neither is a drawing bug - the drawing clipped correctly. They are layout
constants chosen before glyph metrics existed.

The fix generalises: **a screen's fixed strings live in its layout module,
beside the rects they must fit**, and a host test measures each against its
own rect at every orientation. A caption the layout has never seen is a
caption nothing can prove fits. `brush_screen.h` holds the strings;
`suite_brush_screen.c` measures them.

## The scale belongs to the font, not to a global

The obvious way to get two text sizes is a render-time global. It is also
wrong here, and the reason is the repaint hash.

`ui_end()` skips a repaint when the command list hashes identical. A setting
read at **render** time is invisible to that hash, so it needs
`ui_invalidate()` on every change - and a screen with two sizes changes it
at least twice per frame, invalidating every frame and permanently defeating
the skip on exactly the kind of static panel the skip exists for.

`ui_set_font()` already had the answer in its own comment: a `mu_Font` is
baked into every `mu_TextCommand`, so anything carried inside it is visible
to the hash for free. The scale therefore rides in an interned
`{ font, scale }` pair. **When adding a UI setting, ask whether it lands in
the command list or only at render time** - that decides whether it needs
invalidating, and whether it can be per-frame at all.

## Blending reads the destination, so a scrim compounds

Dimming the frozen app behind a panel is one call: `gfx_fill_rect_blend()`
with black. The trap is that blending mixes into the pixel it *reads*, and
the app behind a panel is frozen - nothing repaints it while the panel is
up. Applied every frame, the second application lands on the first's own
output and the backdrop walks to black while the user sits there.

So a scrim is applied **once per repaint of what is underneath**, which for
these panels is two moments: the frame it opens, and a turn taken while open.
`suite_gfx_color.c` pins the arithmetic so the rule cannot rot into a comment
nobody believes.

The more robust form, for when panels stop being opaque and static, is local
rather than global: whoever repaints a region restores the app underneath,
re-scrims that region, then draws. Genuine translucency requires it, because
a see-through panel must composite over fresh pixels every repaint.

## Layout is pure, parameterised, and tested at both orientations

The shell can be under a quarter turn, so `ui_width()`/`ui_height()` swap and
**nothing may assume `GFX_WIDTH`/`GFX_HEIGHT`**. Layout that takes the canvas
size as a parameter is host-testable without gfx, which is the same split
`palette.c` already used and the reason a layout bug can be caught on a
laptop.

Every invariant is asserted at **both** 368x448 and 448x368 - inside the
canvas, no overlap, equal segment widths, and a 44px floor on every tap
target. A layout that only holds in portrait is the specific bug that suite
exists to catch. The panel block is also `_Static_assert`ed against the
shorter canvas, mirroring `palette.h`'s `PALETTE_FITS`.

## The command list is a real ceiling, and it is now measured

`MU_COMMANDLIST_SIZE` is 8 KiB, cut down from upstream's 256 KiB. Everything
drawn - panel frames, bezels, icons as run-length rects, a textured swatch -
spends it. The brush screen measures ~5.4 KiB: two thirds full.

That number was an estimate until `ui_end()` gained a
`CONFIG_LAUNCHER_DEVELOPMENT`-only high-water log. **Artwork has a cost in
this budget**, which is why a baked icon carries its run count (see
[`plans/Icon-Baker-Plan.md`](plans/Icon-Baker-Plan.md)) - so an icon that
would overflow becomes a build error rather than a runtime cliff.

## Related

- [`Launcher-Architecture.md`](Launcher-Architecture.md) - how the UI works today
- [`sand/Simulation-Lessons.md`](sand/Simulation-Lessons.md) - the same genre for the simulation
- [`notes/Optimization-Playbook.md`](notes/Optimization-Playbook.md) - the performance equivalent
- [`plans/Icon-Baker-Plan.md`](plans/Icon-Baker-Plan.md) - where artwork is going
