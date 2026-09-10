# Building a Screen

The entry point for a session told to build or change a UI screen in this
shell. Read this start to finish before writing anything. For how the UI
works see [`Launcher-Architecture.md`](Launcher-Architecture.md); for the
*record* of what each rule below cost to learn, see
[`UI-Lessons.md`](UI-Lessons.md). This page is instructions, not narrative.

## The loop

0. **Decide what owns what, before any code.** Three files, three jobs, and
   the split is what makes a screen testable at all:
   - **layout** (`<screen>.c/.h`) - pure geometry, canvas width and height
     taken as parameters. No `gfx.h`, no hardware.
   - **state** (`*_ui.c/.h`) - which screen is up, what a click MEANS, what
     the screen remembers. Pure, no drawing.
   - **drawing** (`app_*.c`) - the only file that calls `ui_*`/`gfx_*`, and
     the only one the host runner cannot compile.

1. **Write the layout module first, and test it before drawing anything.**
   Every rect the screen needs, from one function. It is the cheapest thing
   to get right and the most expensive to retrofit.

2. **Put the screen's fixed strings in the layout module**, beside the rects
   they must fit, and measure them in the test. A caption the layout has
   never seen is a caption nothing can prove fits.

3. **State machine next**, still without drawing. The caller hit-tests and
   reports WHICH thing was hit; this module decides what that means.

4. **Then draw.** By this point the geometry and the behaviour are both
   already pinned by host tests, and the drawing code is mostly transcription.

5. **Verify in order:** `run_tests.sh`, then `check_app_sources.sh` (the only
   thing that compiles `app_*.c` without a device), then flash and look.

Never invert 1 and 4. A layout bug found on glass costs a flash cycle; the
same bug costs a second on a laptop.

## Exact commands

```sh
./launcher/test/run_tests.sh          # host suites, <1 s - the TDD loop
./launcher/test/check_app_sources.sh  # compiles app_*.c against host stubs
./launcher/tools/build_flash.sh --dev # --dev, always: screenshot.sh needs it
./launcher/tools/screenshot.sh -o shot.png   # lossless PNG, does not reset
```

`idf.py -B build.dev build` is also worth running for anything touching
device-only files: it is a real cross-compile and catches what host stubs
cannot.

## House rules

These are not style preferences. Each one is a bug that shipped.

- **Never hand-roll a hit test.** Go through a real control, or build one
  from `mu_get_id()` + `mu_update_control()`. Hand-tested coordinates do not
  survive a transform, which is what kept the palette from rotating.
- **Never assume `GFX_WIDTH`/`GFX_HEIGHT` in layout.** Ask `ui_width()` /
  `ui_height()`; they swap under a quarter turn.
- **Never paint pixels outside the command list.** `ui_end()` hashes that
  list to skip repaints, so anything drawn behind its back survives as a
  stale smear. The scrim is the one deliberate exception, and only because
  it must *not* be re-applied per repaint.
- **Every tap target is at least 44px** in its smaller dimension.
- **Assert every layout invariant at both 368x448 and 448x368.**
- **Styles are part of the frame's description.** `ui_begin()` resets the
  button style; state what you want every frame.

## How to do the things a screen usually needs

### A control microui does not have

`mu_button()` centres one label and cannot stack an icon over text. Build it
the way `ui_slider_int()` does: `mu_get_id()` from a unique name,
`mu_update_control()`, then read
`ctx->mouse_pressed == MU_MOUSE_LEFT && ctx->focus == id` for the click and
emit the frame, artwork and label as commands. Ids must be unique per
control or two of them collide.

Hand the click to the state module; let it decide what it means.

### More than one text size

`ui_set_font_scaled(gfx_font_ui(), scale)`. The UI font is the 1bpp 8x8
bitmap, so integer scales stay crisp; an 8bpp atlas font would blur above 1.

**Do not add a render-time global for a UI setting.** Anything read at
render time is invisible to the repaint hash and needs `ui_invalidate()` on
every change - which a two-size screen hits every frame, defeating the skip
entirely. Settings that ride *inside* the command list (the font, and so the
scale) are free. Ask which kind you are adding before you add it.

### Text that must fit

Measure it: `ui_measure_text()`, or `gfx_font_text_width()` in a host test.
Both text bugs on the brush screen were layout constants chosen by eye
before glyph metrics existed. The drawing clipped correctly, which is why
nothing caught them.

### Artwork

`ui_draw_bitmap()` emits a bitmap as run-length rects into the command list.
Application artwork lives in the app's own folder so deleting the app
deletes it. Structural facts only in tests - non-empty, bbox in range, run
count under the cap, declared symmetries - never assert artwork against the
code that draws it.

### A panel over a paused app

1. `ui_end(UI_NO_BACKGROUND)` so the frozen app survives in the gaps.
2. Dim it **once** with `gfx_fill_rect_blend()` - see the scrim section in
   [`Launcher-Architecture.md`](Launcher-Architecture.md), and note it is
   once per repaint of the backdrop, never per frame.
3. Draw the panel over it.

On close, force a full repaint of the app underneath and reset any
accumulators the pause built up.

### Knowing what a screen costs

`MU_COMMANDLIST_SIZE` is 8 KiB and everything drawn spends it - roughly 250
rects for a whole screen. A `CONFIG_LAUNCHER_DEVELOPMENT` build logs the
high-water mark from `ui_end()`. Check it before adding a texture or a
fifth icon; the brush screen already sits at about two thirds.

## Testing rules specific to UI

- **Layout, state and geometry are all host-testable. Test them there.**
  Only drawing needs a device.
- **A pure module's unit test does not test its consumer.** If the consumer
  is portable, link it and drive it. `suite_ui_pointer_microui.c` exists
  because an event-list suite stayed green while no button in the shell
  could be pressed.
- **Watch every behavioural test fail first, with a mutation that COMPILES.**
  A stub that trips `-Werror` prints no test lines and proves nothing - grep
  the run for `error:` to be sure you saw an assertion.
- **Vary the fixture's arbitrary starting condition** and confirm the
  assertion still catches what it claims.
- **A UI suite's file-scope objects are firmware `.bss`.** Diagnostics
  builds link every suite, and UI fixtures are exactly the ones that get
  large - a microui context alone is 10,744 bytes, more than the
  framebuffer-plus-grid budget has to spare. Allocate them in
  `fixture()`, and run `tools/build_diag_check.sh`: it is the only local
  check that sees this at all.

## Checklist before flashing

- [ ] layout asserted at both orientations, nothing overlapping or off-canvas
- [ ] every fixed string measured against its own rect
- [ ] every tap target >= 44px
- [ ] clicks routed through a real control, decided by the state module
- [ ] nothing painted outside the command list
- [ ] `run_tests.sh` green, `check_app_sources.sh` green
- [ ] new behavioural tests seen red first, on a mutation that compiled

## Related

- [`Launcher-Architecture.md`](Launcher-Architecture.md) - the mechanisms
- [`UI-Lessons.md`](UI-Lessons.md) - what each rule here cost
- [`Testing-Guide.md`](Testing-Guide.md) - suites, runners, build variants
