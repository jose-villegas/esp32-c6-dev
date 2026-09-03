# Launcher Architecture

How the shell, the apps and the screen fit together, and why ownership is
arranged this way. Read this before adding an app or changing the frame loop.

Living document: update it when the structure changes.

---

## Layout

```
launcher/
├── components/
│   ├── microui/        MIT, patched for this chip (see below)
│   └── small3dlib/     CC0, header-only
├── tools/
│   └── gen_zeta_curve.py   generates main/boot/boot_anim_curve.h
└── main/
    ├── main.c          the frame loop and app switching
    ├── app.h           the shell/app contract
    ├── boot/           runs once each, before the frame loop exists
    │   ├── post.{h,c}          power-on self test
    │   ├── post_ui.{h,c}       the POST report, on screen
    │   ├── selftest.{h,c}      runs the suites at boot (diagnostics build)
    │   ├── boot_anim.{h,c}     the startup animation  (the .h is host-tested)
    │   └── boot_anim_curve.h   GENERATED - see tools/gen_zeta_curve.py
    ├── gfx/            the panel, and the one framebuffer
    │   ├── gfx.{h,c}           owns THE framebuffer, primitives, text
    │   ├── gfx_color.h         what a pixel is            (host-tested)
    │   ├── gfx_dirty.h         which bands changed        (host-tested)
    │   └── icons.{h,c}         artwork no font provides   (host-tested)
    ├── ui/             microui integration, shared by the shell and apps
    │   ├── ui.{h,c}
    │   ├── ui_style.h          how a control's frame looks (host-tested)
    │   └── ui_launcher.c       the home screen
    ├── input/          the devices a finger reaches
    │   ├── touch.{h,c}         FT5x06 polling task
    │   ├── touch_fsm.{h,c}     samples -> press/release    (host-tested)
    │   └── gesture.{h,c}       swipe recognition           (host-tested)
    ├── util/           arithmetic that belongs to no layer
    │   ├── fixed.h             fixed-point multiply/divide (host-tested)
    │   └── intmath.h
    └── apps/
        └── cube/       one folder per app - see "An app is a folder"
            └── app_cube.c
```

**Includes are layer-qualified** - `"gfx/gfx.h"`, not `"gfx.h"` - including
between two files in the same folder. Uniform is the point: a reader should
not have to know where a file lives to read its includes, and an app reaching
past `ui` into `gfx` should be visible at the line that does it.

**Three words that are not interchangeable**, because the code uses all three
and means something different by each:

| | |
|---|---|
| **shell** | the frame loop and the app switching — `main.c`, whose log tag is literally `shell` |
| **launcher** | the home screen the shell draws when no app is running — `launcher/main/ui/ui_launcher.c`. This is what you reach after booting. |
| **boot** | what runs once before the loop exists and never again — `boot/` |


---

## Generated sources

Three generated files live in the tree, each following the same four rules
below: `main/boot/boot_anim_curve.h` (`tools/gen_zeta_curve.py`),
`main/boot/boot_anim_timeline.h` (`tools/gen_boot_anim_timeline.py`, from
`main/boot/boot_anim_timeline.json`), and `main/boot/boot_anim_image.h`
(`tools/gen_boot_anim_image.py`, from `design/boot/boot.png`).

`boot_anim_curve.h` holds the zeta function evaluated along the critical
line. That is not something to compute on a chip with no FPU, and it never
changes, so `tools/gen_zeta_curve.py` computes it once in double precision
and the result ships in flash. `boot_anim_image.h` holds the same idea
applied to a photograph the boot animation crossfades to: no PNG decoder on
this chip, no PSRAM to decode into, so the pixel data - already rotated into
panel space and packed into the panel's own byte-swapped RGB565 - ships in
flash the same way.

Four rules, and the last is the one that matters:

**The generator lives in `tools/`, the output in the tree it belongs to.**
Generated output is checked in, not built. A build-time generator would put
Python on the critical path of every clean build, on a project whose whole
toolchain story is already long enough.

**The output says so, and says how.** A banner naming the exact command that
produces it, on the first line, where someone about to hand-edit it will see
it before they start.

**The generator validates itself before emitting anything.** `gen_zeta_curve.py`
checks its own zeta against known values and exits rather than printing a
plausible-looking table of wrong numbers. A generator that half-works is worse
than one that fails, because its output looks fine.

**The shipped artifact is tested independently of the generator.** This is the
rule with teeth. A generator checking itself proves nothing about the file
actually in the repo, which can be stale, hand-edited, or produced by an older
version of the constants. So `suite_boot_anim.c` tests the numbers that ship,
against the mathematics rather than against the generator: the curve must
reach the axis at each of the five known zero heights, and must stay well
clear of it everywhere else. No table of plausible numbers passes both halves
by accident. `boot_anim_image.h` has no underlying math to check pixel
content against - its independent check is instead two `_Static_assert`s in
`boot_anim.c` pinning the shipped array's shape to the panel's own
`GFX_WIDTH`/`GFX_HEIGHT`, plus real visual verification through
`tools/boot_anim_editor_server.py`'s render view (the same workflow used for
every other render-affecting change in this tree).

**Two different rules for keeping a generated file current, by design.**
`boot_anim_editor_server.py`'s dev server updates both `boot_anim_timeline.h`
and `boot_anim_image.h` automatically, but not the same way. The timeline is
live state typed into the browser: `render()` writes it to a disposable
scratch copy on every scrub of the playhead, and only "Build & Flash"
overwrites the real, committed `boot_anim_timeline.json`/`.h` - a draft the
user is still editing must never land in the tree as a side effect of moving
a slider. The photograph has no draft concept - `design/boot/boot.png` is a
real file on disk, not something the editor holds live state for - so
`_ensure_image_current()` regenerates the REAL, committed `boot_anim_image.h`
in place whenever the PNG's mtime is newer, on every request, exactly like
running `gen_boot_anim_image.py` by hand would. The next asset type decides
which rule it follows the same way: state the browser is actively editing
gets a scratch copy and waits for an explicit save; a file on disk that the
generator only mirrors gets regenerated in place on demand.

---

## How it fits together

Input flows up from the panel, drawing flows down into one framebuffer, and the
shell sits in the middle deciding who gets called.

```mermaid
flowchart TB
    FT["FT5x06 touch controller"] -->|"I2C, only when INT asserted"| TP
    TP["touch.c<br/><i>polling task, 100 Hz</i>"] -->|"sample + timestamp"| FSM
    FSM["touch_fsm.c<br/><i>press / release edges</i>"] -->|"input_t"| SHELL

    SHELL["main.c<br/><i>the one frame loop</i>"]
    SHELL -->|"is it a home swipe?"| GEST["gesture.c"]
    SHELL -->|"launcher showing"| UI["ui_launcher.c<br/><i>microui command list</i>"]
    SHELL -->|"app running"| APP["apps/app_cube.c<br/><i>small3dlib</i>"]

    UI --> FB
    APP --> FB
    SHELL -->|"home hint"| FB

    FB[("gfx.c<br/><b>the single framebuffer</b><br/>368 x 448 x 2 = 322 KiB")]
    FB -->|"7 full-width strips, QSPI DMA"| PANEL["SH8601 AMOLED"]
```

Two things the diagram makes obvious that prose does not:

- **Every drawing path converges on one buffer.** Nothing else allocates
  pixels, because nothing else can afford to.
- **Hardware touches the system at exactly two points** — `touch.c` at the top,
  `gfx.c` at the bottom. Everything between them is ordinary logic, which is
  why `touch_fsm` and `gesture` can be tested on a laptop.

---

## Three rules that shape everything

### 1. There is exactly one framebuffer

368 × 448 × 2 bytes = **322 KiB**, out of roughly 424 KiB of RAM. A second one
is not affordable, so "who owns the pixels" is settled architecturally rather
than negotiated per app: `gfx` owns it, everything else draws into it.

This is also why the 3D renderer is small3dlib. It owns no framebuffer — it
hands back every rasterized pixel through a callback — and with `S3L_Z_BUFFER 0`
it keeps no depth buffer either, resolving visibility by sorting triangles
back-to-front. A conventional colour+depth rasterizer would want ~1.3 MB here.

### 2. There is exactly one frame loop, and it belongs to the shell

Apps do not loop, do not present, do not block and do not yield. An app's
`frame()` draws and returns. Consequences:

- switching apps is instant — no teardown of a running loop
- no app can wedge the device by forgetting to `vTaskDelay`
- the shell decides when to present, and can draw its own chrome afterwards

The one thing that runs a loop of its own is the startup animation, and it
runs *before* this one exists - see `boot_anim.c`. The rule is about apps: an
app must not loop because the shell has to stay able to switch away from it,
and at boot there is nothing to switch to yet. `show_post_failures()` blocks
for the same reason.

### 3. Apps are callbacks, not processes

One binary, one address space, one core. There is no isolation: a misbehaving
app can corrupt the shell. That is the accepted trade for instant switching and
no flash-partition machinery. Worth revisiting only if third-party apps ever
become a goal.

---

## Startup

Before the loop below ever runs, `app_main()` goes through a fixed order, and
the order is most of the point:

```
post_run_before_display()   the SD card, while SPI2 is still free
gfx_init()                  panel up, framebuffer allocated
post_run_after_display()    the rest of the health check
                            -> a failure holds the screen for 8 s
selftest_run()              diagnostics builds only
boot_anim_run()             the startup animation, ~3 s
touch_start(), buttons_start()
ui_launcher_init()
```

The animation goes after the health checks, so a board with a fault says so
before the device does anything decorative, and before touch starts, because
there is nothing yet for a tap to reach.

It draws three axes - the complex plane zeta's *value* lives in, as a floor,
and the height *t* up the critical line, straight up - and then plots
`zeta(1/2 + it)` climbing that axis. Where the curve touches the vertical
axis, zeta is zero, and those five points are the first five nontrivial zeros.

The camera orbits as the curve climbs, pitching from a side-on view toward a
near-top-down one in step with the curve's own progress, so the helix seen
from the side ends up reading almost face-on, as the spiral it always was -
see `boot_anim_view()` in `boot_anim.h`.

Three files, split by what can be tested where:

| | |
|---|---|
| `boot_anim_curve.h` | the curve, as a generated table. Zeta cannot be had cheaply on a chip with no FPU, and the curve never changes, so `tools/gen_zeta_curve.py` computes it once in double precision and it ships in flash. |
| `boot_anim.h` | the projection, the spline, the colour and the timeline - integer arithmetic, no hardware header, so `test/suites/suite_boot_anim.c` checks all of it on a host. |
| `boot_anim.c` | gfx calls and the loop. |

The suite checks the shipped table against the mathematics rather than against
itself: the curve must reach the axis at each of the five known zero heights,
and must stay well clear of it everywhere else. No table of plausible-looking
numbers passes both halves by accident.

## The frame loop

The shell is a two-state machine. `current == NULL` means the launcher is
showing; anything else is the running app.

```mermaid
stateDiagram-v2
    [*] --> Launcher

    Launcher --> Launcher: ui_launcher_frame()<br/>draws the app list
    Launcher --> Running: tap an entry<br/><i>app->enter()</i>

    Running --> Running: app->frame(dt_ms, input)<br/>+ home hint
    Running --> Launcher: swipe up from bottom<br/><i>app->exit()</i>
```

Only `enter()` and `exit()` run on the transitions, and both run exactly once,
which is why an app may assume `enter()` has happened before any `frame()` and
that `exit()` will follow the last one.

Each iteration:

```
touch_read()          latched press/release edges from the polling task
    |
    +-- launcher showing?  ui_launcher_frame()  -> returns chosen app or -1
    |
    +-- app running?       gesture_is_home_swipe()?  -> exit() and go home
                           otherwise app->frame(dt_ms, input) + home hint
    |
gfx_present()         blit, and wait for the DMA to drain
vTaskDelay(1)         yield so the idle task can feed the watchdog
```

Currently **~42 fps** on the launcher screen. The blit dominates at ~25 ms; the
cube app is slower because rasterizing costs ~28 ms on top.

`dt_ms` is clamped to 250 ms so a stall does not make animation jump.

---

## Adding an app

Three steps.

**1. Write `main/apps/<name>/app_<name>.c`** (e.g. `main/apps/sand/app_sand.c`):

```c
#include "../../app.h"
#include "../../gfx/gfx.h"

static void yours_enter(void) { /* reset state */ }

static void yours_frame(uint32_t dt_ms, const input_t *input)
{
    gfx_clear(gfx_rgb(0x101010));
    gfx_text(20, 20, "hello", gfx_rgb(0xFFFFFF));
}

static void yours_exit(void) { /* release what enter() took */ }

/* Exported as the struct, not a pointer to it, so the registry can take its
 * address in a static initializer. */
const app_t app_yours = {
    .name    = "Your App",
    .summary = "what it does",
    .enter   = yours_enter,
    .frame   = yours_frame,
    .exit    = yours_exit,
};
```

**2. Register it — from inside its own file:**

```c
APP_REGISTER(app_yours);
```

That is the whole registration. There is no central list, and **no other file
needs editing** — not `main.c`, not `CMakeLists.txt`.

### An app is a folder

Everything an app owns lives in `main/apps/<name>/`:

```
main/apps/sand/
├── app_sand.c        entry point: gfx, IMU, frame loop        (NOT host-portable)
├── material.c/.h     what a cell is made of                    (pure, const data)
├── tilt.c/.h         accelerometer -> steering direction        (pure)
├── sand.c/.h         the automaton: grid, movement, friction     (pure)
├── sand_liquid.c     cross-flow levelling, the wall-rebound splash (pure)
├── sand_gas.c        rising and dispersing, the reverse-order pass (pure)
├── sand_reactions.c  fire chemistry: ignition, spread, burnout      (pure)
├── sand_priv.h       small inline helpers shared by the .c files above
├── row_runs.c/.h     per-row dirty-span detection and reconciliation (pure)
├── suite_sand.c      tests for the automaton               (pure + a device block)
├── suite_row_runs.c  tests for row_runs                                (pure)
├── suite_tilt.c      tests for the tilt filter                         (pure)
└── tools/            sand-only host tooling (sweeps, report generators)
```

`tools/` is not new territory needing its own rule - it follows straight from
the one above. An app's `tools/` folder holds tools that reference ONLY that
app; anything spanning app *and* shell code (a sweep that touches a
shell-owned header alongside an app one, say) stays in the shared
`launcher/tools/` instead. `main/apps/sand/tools/block_size_sweep.ps1` is the
first tenant - it only ever touches `sand.h` - while
`launcher/tools/sweeps/row_leaf_sweep.ps1`, which sweeps a sand constant
*and* a `launcher/main/gfx/gfx_dirty.h` constant together, stays shared for exactly that
reason.

See `docs/Sand/Sand-Simulation.md` for how the pieces above fit together - the
material system, the water model, and why the liquid logic is split into its
own file.

Deleting the app is deleting the folder. Its code, its logic and its tests go
with it, and nothing is left dangling.

Three mechanisms make that true:

- **The build globs `apps/**/*.c`** (with `CONFIGURE_DEPENDS`, so a new folder
  is picked up without a manual reconfigure).
- **Apps register themselves.** `APP_REGISTER` emits a constructor into
  `.init_array`, which ESP-IDF runs before `app_main()`.
- **The glob excludes `apps/*/tools/`.** That same recursive glob would
  otherwise sweep an app's host-side tooling into the firmware image right
  alongside its real sources - and `WHOLE_ARCHIVE` (below) force-links
  whatever it finds, so a stray `main()` under a `tools/` folder would get
  compiled into the device build rather than dropped by `--gc-sections`.
  `launcher/main/CMakeLists.txt` filters `/apps/[^/]*/tools/` out of
  `discovered_apps` for exactly this reason, structurally rather than by
  naming each tool.

The naming convention the host test runner relies on: **`app_*.c` is the
hardware-facing entry point**, and everything else in the folder is portable
logic it can compile. That split is not bureaucracy — it is what forces an
app's logic to be separable from its wiring, and it is the only reason a
falling-sand automaton can be tested on a laptop.

> **`WHOLE_ARCHIVE` is load-bearing.** The component becomes `libmain.a`, and a
> linker only extracts an archive member that resolves an undefined symbol.
> Since nothing references an app by name any more — the entire point — the
> object would never be extracted, its constructor would never run, and the app
> would silently vanish from the menu. Not a link error: a smaller binary and a
> shorter list. This was caught by the release image shrinking *below* its
> pre-app size.

Menu order is by name. Constructor order follows link order, which is not
something to depend on, so the shell sorts before showing the list.

**Bench-only apps** live in `apps/diagnostics/`, excluded by folder when
`CONFIG_LAUNCHER_DEVELOPMENT` is off — structural rather than a name check.
Diagnostics re-runs POST, which cycles the audio rail and takes the display off
SPI2, so it has no business being reachable in a shipped image. See
[Testing-Guide](Testing-Guide.md#release-builds-contain-no-test-code) — note in
particular that `REQUIRES` must **not** be gated this way.

Diagnostics ships in any development build, `--dev` included, not just
`--diag` — that is what frees the RAM the on-device test suites would
otherwise hold, letting a `--dev` build reach the gfx debug-overlay
checkboxes without sand's grid allocation failing for want of heap. Its own
toggle page still mixes two shapes, but the app itself no longer does:
the "run self test suite" button and its result line are genuinely
SELFTEST-only (`#if CONFIG_LAUNCHER_SELFTEST` inside `app_diagnostics.c` —
`selftest_run()` does not exist as a symbol outside a SELFTEST build) and
compile out of `--dev`, while the POST report and the rest of the toggle
page (gfx debug overlays, interlace, the orientation readout) are
DEVELOPMENT-shaped and ship in both. Splitting that surviving DEVELOPMENT
content into its own Settings app is still open; see
[Settings-App-Plan.md](Settings-App-Plan.md).

### Drawing a UI, in the shell or in an app

`launcher/main/ui/ui.c` owns the microui integration; `ui_launcher.c` is just one caller.
An app builds a UI the same way:

```c
mu_Context *ctx = ui_context();

ui_begin(input);
if (mu_begin_window_ex(ctx, "Settings", rect, opts)) {
    if (mu_button(ctx, "Clear")) { ... }
    mu_end_window(ctx);
}
ui_end(UI_NO_BACKGROUND);   /* draw over the app instead of clearing */
```

That gets the touch handling - which is not obvious, see the comment on
`feed_input()` - and the repaint logic below, for free.

#### Styling a control

`ui_style.h` decides how a control's frame *looks*, separately from what it
*is*. Two styles exist for buttons:

| | |
|---|---|
| `UI_BUTTON_FLAT` | microui's own — a flat fill plus a one-pixel border. The default. |
| `UI_BUTTON_BEZEL` | lit from the top left, inverted while a finger is on it. What the launcher uses. |

```c
ui_begin(input);
ui_set_button_style(UI_BUTTON_BEZEL);   /* every frame - see below */
```

Three things about it are worth knowing before adding a style of your own.

**The hook is microui's, not a patch.** `mu_Context` carries a `draw_frame`
function pointer that every frame goes through — button, checkbox, slider,
scrollbar, window background — with a rect and a colour id. `ui_init()` saves
the one microui installed and puts its own in front, so `UI_BUTTON_FLAT` and
every non-button frame are still literally upstream's code, and nothing in
`components/microui/` is edited.

**A style emits commands, not pixels.** It would be simpler to call
`gfx_fill_rect()` and paint the edges directly, and it would break the repaint
skip below: that works by hashing microui's command list, so an edge drawn
outside the list is invisible to the hash and survives on screen as a stale
smear after the control underneath it changes. Styles return spans;
`styled_draw_frame()` turns spans into `mu_draw_rect()` calls; the hash sees
all of it.

**Style does not persist across frames.** `ui_begin()` resets it, so a caller
that wants a style states it every frame. That is the immediate-mode reading —
style is part of the frame's description, like everything else — and it is load
bearing here, because the whole shell shares one `mu_Context`: without the
reset, the launcher opting in would leave the sand app's overlay buttons
bezelled too.

One detail worth spelling out, because it is the opposite of what a desktop
toolkit would do: **the pressed look is on hover, not on focus.** On a mouse,
hover means "the pointer is near" and focus means "the button is held"; on a
touchscreen the pointer does not exist until a finger is already on the glass,
so hover *is* contact. A tap renders `MU_COLOR_BUTTONHOVER` for every frame the
finger is down and `MU_COLOR_BUTTONFOCUS` for the single frame the press lands
on, so sinking the bezel only on focus would flash it for one frame out of a
press lasting dozens.

The geometry and the shading are pure functions in the header, the same split
`icons.h` makes, so `test/suites/suite_ui_style.c` checks the shape on a host
without linking `gfx.c` or even `microui.c` — nobody can eyeball five
overlapping rectangles reliably.

#### Immediate mode versus dirty bands

These fight, and the fight would have hit apps, not just the launcher.
Immediate mode rebuilds and repaints the UI every frame, which means clearing
every frame, which marks every band dirty and forces a full ~17 ms transfer -
discarding the saving that partial updates exist to provide.

The resolution: an immediate-mode UI is **rebuilt** every frame but not
necessarily **changed**. microui's command list is a complete description of
the output, so two frames that hash the same *are* the same picture.

A retained-mode engine knows what changed because changing it is an explicit
act - you mutate a node, it marks its canvas dirty. Immediate mode throws that
signal away by construction, so we recover it from the other end: compare
output where an engine compares intent. Same destination, opposite direction.

#### One window is one canvas

The canvas split comes free with it. microui already groups commands by root
container, each with its own rect, so each window is hashed, repainted and
band-marked on its own. A live readout in one window does not force a static
toolbar in another to repaint - which is the point of splitting canvases at
all.

```mermaid
flowchart TB
    BUILD["ui_end()"] --> HASH["hash each window's<br/>command range"]
    HASH --> CMP{"differs from<br/>last paint?"}
    CMP -->|no| DIRTY{"its bands already<br/>dirty underneath?"}
    CMP -->|yes| PAINT["repaint this canvas"]
    DIRTY -->|no| SKIP(("skip<br/><i>no draw, no transfer</i>"))
    DIRTY -->|yes| PAINT
    PAINT --> OVER["also repaint any<br/>window above it<br/>that overlaps"]
```

Two rules that have to be respected:

- **Painter's order.** Windows are drawn back to front, so repainting one means
  repainting anything above it that overlaps, or the repaint erases what was on
  top.
- **`ui_invalidate()`.** If something replaced the screen in a way `ui_end()`
  cannot detect - returning to the launcher after an app has been running - the
  UI must be told its pixels are gone. Otherwise it compares an unchanged
  command list, skips, and leaves the app's last frame on screen.

Measured: an idle launcher went from 66.7 fps to the 1 kHz tick ceiling,
because it now paints and sends nothing at all. `test/suites/suite_ui.c` covers
the independence claim directly - it builds two windows, changes one, and
asserts the other's bands stay clean.

### What an app may and may not do

| | |
|---|---|
| Draw via `gfx_*`, or straight into `gfx_framebuffer()` | yes |
| Read `input_t` for touch | yes |
| Animate using `dt_ms` | yes |
| Call `gfx_present()` | **no** — the shell presents |
| Loop or block or `vTaskDelay` | **no** — return promptly |
| Keep a framebuffer of its own | **no** — there is only one |

Anything expensive belongs in `enter()`, not `frame()`.

---

## Input

`touch.c` polls the FT5x06 at 100 Hz on its own task, above the render loop's
priority. Sampling is decoupled from rendering on purpose: a frame is ~24 ms
and a quick tap can be shorter, so polling once per frame drops taps.

`touch_fsm` interprets the samples and latches press/release edges, so an event
that happens entirely between two frames still reaches the next one. Edges are
consumed when read, so each is delivered exactly once.

Two hardware facts drive the design, both documented in the platform notes:
reads are gated on the INT line because the controller NACKs when idle, and a
release needs a 60 ms quiet period because INT means "data ready" rather than
"finger down" and drops briefly mid-touch.

---

## Why microui, not LVGL

LVGL is present in the build - it is a transitive dependency of the Waveshare
BSP package, `main/idf_component.yml` pulls that in for the display and touch
drivers - but nothing here calls into it. `gfx.c` drives the panel directly
(`esp_lcd_new_panel_sh8601`, not `bsp_display_new()`/`bsp_display_start()`),
so it never runs; confirmed rather than assumed by checking the linked
binary, which carries zero `lv_*` symbols.

Three constraints, all already documented elsewhere in this project, point
the same direction once put next to each other:

**The memory budget has no room for it.** There is no PSRAM - the whole
budget is ~424 KiB of internal SRAM, and the framebuffer alone is 322 KiB of
that (see `docs/Notes/Board-and-Memory.md`). LVGL costs roughly **67 KiB**
before a single widget is allocated - about 16% of the entire chip's memory
gone before drawing anything. microui needed patching too (upstream sizes
`mu_Context` for desktop, 256 KiB for the command list alone), but that is a
one-time struct-layout edit down to a small fixed size, not a standing tax
on every frame the way a persistent widget tree and style system are.

**This device runs apps that own their entire framebuffer.** The falling-sand
simulation and the cube renderer each drive the panel directly, on their own
schedule, with no widget tree in between. A retained-mode toolkit wants to
own the display and the refresh cycle - exactly what those apps already do
for themselves. microui's command-list model asks for nothing: it turns a UI
description into a list of rectangles, text and icons once a frame, and
whoever is drawing paints that list whenever they like, into whatever they
like. It composes with an app that owns its own frame loop; a retained-mode
toolkit would compete with it for the same job.

**The whole render pipeline here is built around skipping unchanged frames**,
because a full transfer costs ~17 ms (measured: 16,998 us of bus time, the
frame being 94% bus-bound - see `gfx.h`) and most frames do not need one. An
immediate-mode command list is exactly the shape that trick needs - see
[Immediate mode versus dirty bands](#immediate-mode-versus-dirty-bands)
below for the mechanism. LVGL has its own separate invalidation and redraw
system, built around owning the display - adopting it would mean reconciling
two damage-tracking systems, or replacing the one already built for every
other app, rather than reusing it for free.

**The real cost, for balance:** microui encodes a mouse's interaction model
(point, then click), and a touchscreen cannot produce that sequence - the
pointer does not exist until a finger is already down. Every control needs a
synthesised hover frame to compensate, costing one frame (~24 ms) of input
latency on every tap. That friction is specific to picking an immediate-mode,
mouse-shaped toolkit; a touch-native widget system would not have it. It was
worth paying given the three constraints above, but it is a real trade-off,
not a free win.

## The microui integration

microui is immediate-mode and draws nothing itself: each frame it turns the UI
description into a list of rectangles, text and icons, and `ui_launcher.c` walks
that list painting into the framebuffer.

That command-list model is why it suits this device. A retained-mode toolkit
wants to own the display and the refresh cycle, which fights an app like the
cube that renders its own frames. Here the shell renders primitives whenever it
likes, into whatever it likes.

### Two things to know before touching it

**The vendored header is patched.** Upstream sizes `mu_Context` for desktop —
the command list alone is 256 KiB, which does not fit beside the framebuffer.
Sizes are reduced in `components/microui/include/microui.h`, with upstream
values in trailing comments. They are edited in the header rather than
overridden from our side because they determine the struct's layout, and two
translation units disagreeing would corrupt it silently.

**Touch needs a synthesised hover frame.** `mu_update_control()` only
establishes hover on a frame where the button is *not* held, and a control only
submits once focused — the mouse sequence "point, then click". A touchscreen
never produces the first half, because the pointer does not exist until a finger
is already down.

So `feed_input()` delivers a press across two frames: position only, then the
button-down. One frame of latency, ~24 ms, and taps register every time.

**This applies to every microui control**, not just buttons — anything reacting
to a press goes through `mu_update_control()`. Adding a slider or checkbox
requires nothing new, but reworking input handling means preserving this.

---

## Related

- `docs/Notes/` — the hardware constraints underneath all of this: memory
  budget, panel gotchas, touch quirks, flashing and recovery. Start at
  `docs/Notes/README.md`.
- `docs/Sand/Sand-Simulation.md` — the falling-sand app in depth: materials, the
  water model, momentum, and why its liquid logic is its own file.
- `docs/Testing-Guide.md` — how to test any of it.
- `docs/Settings-App-Plan.md` — planned split of the Diagnostics app's
  developer-toggle page into its own Settings app.
