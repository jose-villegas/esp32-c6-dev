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
└── main/
    ├── gfx.{h,c}       owns THE framebuffer, panel, primitives, text
    ├── touch.{h,c}     FT5x06 polling task
    ├── touch_fsm.{h,c} samples -> press/release events   (host-tested)
    ├── gesture.{h,c}   swipe recognition                 (host-tested)
    ├── app.h           the shell/app contract
    ├── ui_launcher.c   the home screen, built with microui
    ├── main.c          the frame loop and app switching
    └── apps/
        └── app_cube.c  3D cube
```

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

### 3. Apps are callbacks, not processes

One binary, one address space, one core. There is no isolation: a misbehaving
app can corrupt the shell. That is the accepted trade for instant switching and
no flash-partition machinery. Worth revisiting only if third-party apps ever
become a goal.

---

## The frame loop

Each iteration of `app_main`:

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

**1. Write `main/apps/app_yours.c`:**

```c
#include "../app.h"
#include "../gfx.h"

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

**2. Register it in `main.c`:**

```c
extern const app_t app_yours;

const app_t *const app_registry[] = {
    &app_cube,
    &app_yours,
};
```

`app_count` is computed from the array, and the launcher lists it
automatically.

**3. Add the source to `main/CMakeLists.txt`.**

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

- `docs/ESP32-C6-AMOLED-Notes.md` — the hardware constraints underneath all of
  this: memory budget, panel gotchas, touch quirks, flashing and recovery.
- `docs/Testing-Guide.md` — how to test any of it.
