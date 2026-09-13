# Plan: the sand app's brush screen, and the UI primitives it needs

**Status**: built 2026-09-10. Phases 1-6 plus this doc pass, all seven
landing on the branch before merge.
Two divergences from the plan below, both found during implementation, not
predicted by it:

- **Phase 2's `ui_set_text_scale()` never shipped.** The plan sketched a
  render-time global; what landed instead is `ui_set_font_scaled()`, which
  interns `{ font, scale }` pairs so `mu_Font` itself carries the scale -
  the same reason `ui_set_font()` already needed no `ui_invalidate()`. A
  global read at render time would have needed invalidating on every size
  change, and a screen mixing two sizes hits that every frame, permanently
  defeating the repaint skip. See
  [`Launcher-Architecture.md`](../Launcher-Architecture.md#text-at-more-than-one-size)
  for the full argument, already written up beside `ui_set_font()` in
  `ui.c` before this plan even reused it.
- **The size caption reads `POUR SIZE`, not `POUR BRUSH SIZE`.** At 368px
  portrait the caption row is 232px once the value box takes its 80, and
  the design's wording needs 240. The dropped word carries no information
  the panel doesn't already say twice over (it's the brush screen; the
  segment above already reads POUR). `suite_brush_screen.c` now measures
  every fixed string on this screen against its own rect at both
  orientations, which is what caught it - not eyeballed on a screenshot.
  Put to the maintainer with the alternatives (smaller caption, or the value
  on its own line) and accepted as-is, so the shorter wording is the design
  now rather than a deviation from it.

- **The slider draws in microui's default chrome, not the design's gold.**
  `ui_slider_int()` takes its track and knob colours from `MU_COLOR_BASE`/
  `MU_COLOR_BORDER`/`MU_COLOR_BUTTON`, and the screen overrides those for its
  segments but not for the slider. Seen on a host render and accepted as-is,
  so it is the design now - recorded because a reader comparing the screen to
  the original mockup would otherwise read grey-instead-of-gold as a bug.

Deliberately deferred, unchanged from the plan: the info button draws but
has no handler behind it (a separate, unbuilt panel), and nothing on either
screen persists across an app restart - brush, mode and every radius reset
with `sand_ui_t` the same way everything else in it already does.

---

The sand app gets a second full-screen panel — a **brush screen** — from a
supplied pixel-art design: the current material with its name and an info
button, the three brush modes as a segmented toggle, and a size slider for
the selected mode.

Most of this plan is not the screen. The screen is a few hundred lines of
layout; getting it to look like the design needs six things `ui/` cannot do
today, and those are the interesting work. Each is a shell-level primitive
that any app gets afterwards, which is the same reasoning `ui_style.h` and
`icons.h` were split out under.

---

## The design, read as a list of demands on `ui/`

Three stacked panels on a bordered dark ground:

1. **Header** — a textured material swatch, a small `MATERIAL` caption above
   a large material name, and an info button at the far right.
2. **Brush mode** — a `BRUSH MODE` caption over three segments: POUR, ERASE,
   BOOM. Exactly one is lit; each carries an icon above its label.
3. **Brush size** — `POUR BRUSH SIZE` on the left, `06 PX` right-aligned on
   the same line, over a slider with a filled track and a chunky knob.

What that costs, against what exists today:

| The design asks for | Today | Gap |
|---|---|---|
| A slider you drag | `mu_slider_ex` exists | `feed_input()` presses and releases in the **same frame**, so a slider can only jump to a tap and can never be dragged |
| Two text sizes on one screen | `GFX_GLYPH_SCALE` is a compile-time `2` for all microui text | no per-frame text scale |
| Icons for pour/erase/boom/info | `icons.h` has exactly one hand-drawn glyph (the check) | no icon set, and no way for an app to put its own artwork into the command list |
| Panels with captions | window frames only | no section-frame primitive |
| One segment lit out of three | none — the palette hand-draws a sunken bezel after the fact | no segmented control |
| A textured material swatch | palette tiles are a flat `brush_color()` fill | no swatch texture |

Two constraints bound every answer below:

- **A style emits commands, not pixels.** Anything painted outside microui's
  command list is invisible to the repaint hash and survives as a stale
  smear. See `ui_style.h`'s own header comment.
- **The command list is 8 KiB** (`MU_COMMANDLIST_SIZE`, cut down from
  upstream's 256 KiB in `components/microui/include/microui.h`). At roughly
  32 bytes per `mu_draw_rect`, that is on the order of 250 rects per frame
  for the whole screen — a real ceiling once icons and a textured swatch are
  drawing as runs of rectangles. Phase 6 adds a high-water reading rather
  than guessing.

---

## Decisions taken before planning

- **PWR opens the brush screen.** It no longer cycles PAINT/ERASE/DETONATE;
  the segmented control owns mode selection now. BOOT still opens the
  material palette, unchanged. The two panels are siblings, not pages of one
  overlay.
- **BOOM is promoted.** `SAND_MODE_DETONATE` stops being scaffolding: it
  gets an icon, a label and its own size. The "temporary / deletable"
  comments in `sand_ui.h` and `app_sand.c` come out in the same change that
  ships the screen — a shipped control must not describe itself as a
  stopgap.
- **One size per mode.** POUR, ERASE and BOOM each remember their own
  radius, and the caption renames with the selection. Their current
  constants (10 / 16 / 50 px) become the three defaults; a single shared
  slider would flatten reaches that are deliberately different.

## Deliberately deferred

- **The info button draws but does nothing.** It is in the design and it is
  in the layout; the panel behind it is a separate piece of work (file it as
  its own issue when the screen lands). Flagged here so nobody reads the
  dead button as a bug.
- **Persistence.** Sizes and mode live in RAM and reset with the app, as
  everything in `sand_ui_t` already does.

---

## Phases

Each phase is one PR, lands green, and is useful on its own. Phases 1–4 all
touch `launcher/main/ui/`, so they are **run one at a time** — concurrent
changes to `ui/` would conflict, each compiling against the other's
half-finished edits.

### Phase 1 — a pointer that stays down

**Why first:** it is the one gap that blocks another phase outright, and the
one with real bug history behind it.

`feed_input()` (`ui/ui.c`) synthesizes a hover frame, then next frame sends
mousedown **and mouseup together**. microui's slider only tracks while the
mouse is held, so under today's bridge a slider jumps to where you tapped
and then ignores your finger entirely. Buttons are unaffected — microui
submits on the one-frame `mouse_pressed` — which is exactly why holding is
safe to introduce.

- Extract the translation into `ui/ui_pointer.h` + `ui_pointer.c`: pure
  `input_t` → a short list of pointer events (move / down / up), no microui
  calls and no gfx. Same split `touch_fsm.c` and `button_fsm.c` already use,
  and what makes it host-testable — `suite_ui.c` is device-only, so the
  current behaviour has no host coverage at all.
- `ui.c` replays the events into `mu_input_*`, keeping the transform mapping
  it already does (`to_logical()`), and keeps the off-screen park on lift.
- Change the policy: hold down while the finger is down; release on the real
  release edge; move every frame in between.
- New host suite `test/suites/suite_ui_pointer.c`: the two-frame
  hover-then-press sequence still comes out of a tap (it is what makes touch
  work at all — do not lose it); a drag yields down, N moves, up; a lift
  produces exactly one up; a finger already down when the UI opens does not
  synthesize a press.

**Watch it fail first:** write the drag test against the current
same-frame-release policy and see it fail before changing the policy.

### Phase 2 — text at two sizes, and panels to put it in

**The scale belongs to the font, not to a global.** The obvious design — a
`ui_set_text_scale()` global read at render time — is the same shape as
`ui_set_text_style()` and would therefore need `ui_invalidate()` on every
change, because a left-aligned string re-rendered at a new scale can leave
the text command's bytes identical and the hash blind. A screen with two
sizes sets the scale at least twice per frame, so it would invalidate every
frame and permanently defeat the repaint skip — on exactly the kind of
mostly-static panel that skip exists for.

`ui_set_font()` already gets this right and says why in its own comment: a
`mu_Font` is **baked into every `mu_TextCommand`**, so a font change is
different bytes and the hash sees it unaided. So carry the scale the same
way — `mu_Font` points at an interned `{ font, scale }` pair rather than a
bare `gfx_font_t`, and a size change becomes a different pointer in the
command list. No invalidate, no per-frame thrash, and the mechanism is one
the file already argues for rather than a second one beside it.

- Intern the pairs in a small fixed table with stable addresses, so the same
  (font, scale) always hashes identically frame to frame.
- `ui_set_font_scaled(font, scale)`; `ui_set_font(font)` keeps its current
  meaning at `GFX_GLYPH_SCALE`. `measure_text_width()`,
  `measure_text_height()` and `draw_command()` unwrap the pair instead of
  reading the `GFX_GLYPH_SCALE` constant.
- Contained change: nothing outside `ui.c` casts a `mu_Font` today.
- `ui_measure_text()` in `ui.h`: what the current font and scale would
  measure. An app right-aligning `06 PX` on a caption line needs this and
  should not be re-deriving the font role and scale to get it.
- `ui_panel_spans()` in `ui_style.h`: the rects of one section frame, face
  plus border, pure geometry returning spans exactly as `ui_bezel_spans()`
  does. Extend `test/suites/suite_ui_style.c`.

### Phase 3 — an icon set, and app-owned artwork

- Generalize `icon_check_blocks()` (`gfx/icons.h`) into
  `icon_bitmap_blocks(const uint16_t *bitmap, ...)` — the run-length,
  integer-scale and content-centring logic is not specific to the check.
  `icon_check_blocks()` becomes a wrapper, so `suite_icons.c` stays green as
  a regression check on the extraction.
- `ui_draw_bitmap(ctx, rect, bitmap, color)` in `ui.c`: emits the runs as
  `mu_draw_rect()` calls. That is what puts app artwork **in the command
  list** rather than on the framebuffer behind the hash's back, and it means
  no new `MU_ICON_*` id and no patch to `components/microui/`.
- The artwork itself is the sand app's, not the shell's:
  `apps/sand/sand_icons.h` — funnel, X, burst, and the info `i`,
  hand-drawn 16×16 the way `icon_check_bitmap` is, and for the reason its
  comment gives (a generated diagonal reads as a staircase). Deleting the
  app folder deletes them, per the app-is-a-folder rule.
- Host suite `apps/sand/suite_sand_icons.c`: structural facts only — every
  icon is non-empty, its runs fit `ICON_CHECK_MAX_BLOCKS`, the ones meant to
  be symmetric are. Never assert artwork against the code that draws it.

### Phase 4 — a slider worth touching

Depends on phase 1.

- `ui/ui_slider.h`: pure geometry — track, filled portion and knob rects for
  a value, plus the inverse (a touch x → a value), with integer steps and
  clamping. Host suite `suite_ui_slider.c`: value → pixel → value round
  trips, the knob never leaves the track at either end, out-of-range x
  clamps rather than wraps.
- `ui_slider_int()` in `ui.c`: `mu_get_id` + `mu_update_control` with
  `MU_OPT_HOLDFOCUS` so a drag keeps focus once it starts, emitting the
  spans above. Integer valued — the design shows `06 PX`, and
  `mu_slider_ex`'s float and `"%.2f"` are the wrong shape for that.

### Phase 5 — the screen

- `sand_ui.h`/`sand_ui.c`: a `SAND_UI_BRUSH` screen; PWR opens and closes
  it; PWR's mode cycling deleted; `sand_ui_mode_clicked()` alongside the
  existing `sand_ui_tile_clicked()`, following the same "the caller
  hit-tests, this module decides what it means" rule the palette's header
  comment sets out; per-mode radii in `sand_ui_t` with min/max. Same
  `swallow_release` discipline as the palette.
- Tests go in `suite_sand_ui.c` — the file that exists because four
  edge-ownership bugs shipped out of this exact logic. The new one to pin:
  **the PWR press that opens must not also close.** Open on the press, close
  on a later press, never on the same edge.
- `apps/sand/brush_screen.h`/`.c`: pure layout. Given a canvas width and
  height, return every rect — panels, swatch, info button, three segments,
  slider track. Taken as parameters, not read from `gfx.h`, which is what
  keeps it host-testable (the same rule `palette.c` follows). Host suite
  asserts the layout holds at **both** 368×448 and 448×368, since the shell
  can be under a quarter turn: nothing overlaps, nothing leaves the canvas,
  every tap target stays finger-sized.
- `app_sand.c`: `draw_brush_screen()` using the phase 1–4 primitives, and
  `handle_pour_input()` reading the per-mode radius instead of
  `POUR_RADIUS_PX` / `ERASE_RADIUS_PX` / `DETONATE_RADIUS_PX` (which become
  the defaults). Rotation handled the way `draw_palette()` already does it —
  it inherits the shell's transform because it goes through microui.

### Phase 6 — the material swatch, and the command budget

- `apps/sand/sand_swatch.h`: a deterministic pattern of shade variants for a
  given brush cell, pure, from `MATERIAL_SHADE_SPAN` — so the swatch is made
  of the same shades the material actually renders with rather than a
  second, drifting definition of what sand looks like. Host-tested for
  determinism and in-range variants.
- Drawn as command-list rects, with a fixed cell count.
- A `CONFIG_LAUNCHER_DEVELOPMENT`-only high-water reading of
  `ctx->command_list.idx` in `ui_end()`, logged like the other dev counters.
  The 8 KiB ceiling above is currently an estimate; this makes it a number.

### Phase 7 — docs

- `docs/Launcher-Architecture.md`: the pointer change (it revises the
  "pressed look is on hover" passage), the new primitives, the text-scale
  rule and why it needs both a reset and an invalidate.
- `docs/sand/Architecture.md`: the second screen and the retired PWR cycle.
- Backlog: the info panel, and anything a phase deferred.

---

## Verification

Host tests after every phase (`./launcher/test/run_tests.sh`, under a
second). The screen itself cannot be judged by a test — flash a `--dev`
build and capture it with `./launcher/tools/screenshot.sh`, which reaches
the device over the same serial connection and needs neither `idf.py` nor
PowerShell. That is the iteration loop for the look: screenshot, compare
against the design, adjust the layout module.
