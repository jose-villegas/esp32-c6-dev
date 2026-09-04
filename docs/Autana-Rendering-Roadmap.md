# Autana: a rendering roadmap for the ESP32-C6 (and the S3 after it)

**Status**: proposal, written 2026-09-04. Nothing here is built. It sets the
order of investment for turning this shell into a small game engine
("Autana"), starting from where the two showcase apps stand today: `sand`
proves pixel pushing, `cube` proves real-time 3D. The three target games it
plans towards are the ones named by the maintainer: a gyro-and-buttons FPS,
a rolling-ball game with physics and lighting, and a platformer with
parallax and 2D lighting.

Every number below that is not marked *estimate* is measured, and its
source is named. The house rule from
[Optimization-Playbook.md](Notes/Optimization-Playbook.md) applies to this
document too: a plausible explanation of where time goes is not a measured
one, and every phase ends with a number, not a feeling.

---

## 1. Reference points, and what they say about our gap

Public software renderers on this class of chip are a useful sanity check
on what the budget buys, as long as they are read for their *technique*
rather than their headline number. A few that are representative:

- An ESP32-S3 demo on r/esp32 (November 2024): 480×320 over single-lane
  80 MHz SPI, a few hundred flat-shaded triangles at 48 fps by rendering
  and sending alternating fields, integer-only math with lookup tables,
  affine texture mapping, nothing allocated after startup. The author's
  own stated limit was the display bus, not the CPU.
- [andresragot/esp32_3d_engine](https://github.com/andresragot/esp32_3d_engine)
  and its P4/S3 sibling: a complete transform/clip/raster pipeline in C
  with a framebuffer, the conventional shape.
- The 1990s canon these all descend from — Doom's column and span
  renderers with a colormap for lighting, Quake's perspective correction
  every 16 pixels, the PS1's affine-with-subdivision — is the real
  reference library. None of it needs an FPU, a GPU, or more RAM than we
  have.

The S3 demo is the closest match in pixel count and panel interface, so it
is the one worth putting beside ours:

| | S3 SPI demo | This repo, ESP32-C6 |
|---|---|---|
| Pixels per frame | 480×320 = 153,600 | 368×448 = 164,864 |
| Bus | 1 lane × 80 MHz = 10 MB/s | 4 lanes × 40 MHz = 20 MB/s |
| Full frame on the bus | ~30 ms theoretical, 24 fps achieved | 16.5 ms theoretical, 17.6 ms measured |
| CPU | 2 × Xtensa LX7 @ 240 MHz, FPU | 1 × RISC-V @ 160 MHz, no FPU |
| Per-pixel work | flat fill (a store), later affine texture | Gouraud: 3 barycentric interpolations + RGB pack + 4 bbox compares |
| Triangles | a few hundred small ones | 12 that each cover thousands of pixels |
| Frame pipeline | interlaced fields | full frames, or interlaced via the Diagnostics toggle |

Three things fall out of that table:

1. **Their display bus is half the speed of ours.** They are bus-bound at
   24 fps full-frame; we are bus-bound at 56 fps full-frame (17.6 ms). The
   bus is not what puts our cube at 30 fps.
2. **Their per-pixel cost is roughly an order of magnitude lower.** A flat
   span is a run of stores, two pixels per 32-bit write. Our `shade_pixel()`
   (`launcher/main/apps/cube/app_cube.c:123-152`) does nine multiplies,
   three shifts, three clamps and a colour pack per pixel, inside the
   callback small3dlib invokes once per pixel with freshly computed
   barycentrics. The cube's 12 triangles cover far more pixels than a few
   hundred small ones, so **our rasterizer is fill-rate bound, theirs is
   bus bound.** That is the whole gap. "Triangles per second" is a count of
   small triangles; at this pixel count it is not the metric that binds.
3. **Nothing in that technique list is unavailable to us** except the
   second core, the FPU and 240 MHz. Integer math, lookup tables,
   interlacing, no runtime allocation, spans: the C6 can run all of it.

The historical breakdown in
[Display-and-Rendering.md](Notes/Display-and-Rendering.md#the-cube) says
the same thing in numbers: clear 5.2 ms, rasterize 28.1 ms, blit 25.0 ms,
15.5 fps. Those figures predate `-O2` and the 40 MHz retune and have never
been re-measured; `suite_cube_perf.c` exists to produce the current ones
and has never been run to a checked-in result. Re-running it is the first
task in Phase 0.

### 1.1 Console-era tricks worth stealing (Saturn, PS1)

The 1994 consoles are a better reference than any modern renderer because
they solved *our* problem: integer-only, no z-buffer, a few hundred KB of
RAM, and a fill-rate budget that had to be spent on purpose. The Saturn and
PS1 each had hardware for a trick; we would do the same trick in software,
which is fine, because in every case the trick's whole point is that it is
cheap per pixel. Where a trick lands in this plan:

| Trick | Origin | What it was | Our version | Where |
|---|---|---|---|---|
| **Line scroll** | Saturn VDP2 | a per-scanline horizontal offset table for background layers: parallax, wavy water, heat shimmer | a per-row offset per layer in the tile renderer; band mode makes it free since bands are row slices | r2d, platformer |
| **Rotation plane** ("Mode 7") | Saturn VDP2, SNES | a floor/ceiling as one affine-transformed texture, per scanline a constant (u,v) step | per-scanline affine floor: one load + one store per pixel, no polygons — the cheapest possible ground for the raycaster and a viable v1 table for the rolling ball before a heightfield mesh | raycaster floor, ball |
| **Mesh transparency** | Saturn VDP1 | a checkerboard of drawn/skipped pixels instead of real blending | already here: `gfx_fill_rect_dither` / `gfx_blit_dither` on the 4×4 Bayer table | done |
| **Distorted sprites** | Saturn VDP1 | every polygon is a quad, textured by *forward* mapping (walk texels, write pixels) | a sprite scaler/rotator for billboards: FPS enemies, the ball, particles. Forward mapping leaves gaps on magnification, so use it for shrink/rotate only | raycaster, ball |
| **Backgrounds on a separate layer** | Saturn VDP2 | sky and distant scenery as scrolling bitmap layers, polygons only on objects | a scrolling skybox strip drawn as a 2D blit before the 3D pass, never a triangle | raycaster, ball |
| **Ordering table** | PS1 GPU | no z-buffer; a bucket sort by depth (one linked list per depth slot), painter's order in O(n) | per-band OT instead of small3dlib's sort — cheaper than a comparison sort and gives band binning for free when the bucket key includes the band | r3d |
| **Affine texturing with subdivision** | PS1 | no perspective correction per pixel; near polygons are split so the warp stays small | perspective-correct every 8–16 px with linear steps between (the Quake cadence), and subdivide only the nearest floor quads | r3d |
| **4-bit CLUT textures** | PS1 | textures as 4- or 8-bit palette indices; a 64×64 4-bit texture is 2 KB | indexed textures in RAM, palette × light level folded into one colormap lookup (index → lit RGB565). This is what makes textures affordable on a 40 KB free block | r3d, raycaster |
| **Pre-lit vertices** | PS1 | lighting baked into vertex colours at authoring time; only moving lights computed at runtime | the same, plus Gouraud by stepping; point lights per vertex only | r3d, ball |
| **Depth-cue fog** | PS1 | colour blended toward a fog colour by depth | a light-level column in the colormap indexed by span depth — the Doom "light diminishing" table, one lookup per span | r3d, raycaster |
| **Subpixel precision** | what the PS1 *lacked* | integer vertex snapping gave the PS1 its polygon wobble | keep 4 fractional bits in span setup; it costs setup math only, not per-pixel, so we get the stability the PS1 could not afford | r3d |
| **Precomputed visibility** | Crash Bandicoot, Quake PVS | per-region lists of what can be seen, computed offline | per-cell or per-sector visibility baked into the map for the sector renderer; a raycaster does not need it | FPS, later |
| **Vertex animation, not skinning** | PS1 | keyframed vertex positions, interpolated | same: no bones, no per-vertex matrix palette | ball, FPS |

Two of these change *what* gets built, not just how: a Mode-7 floor plus
sprites is a legitimate first rolling-ball prototype that needs no mesh
rasterizer at all, and the ordering table replaces a sort we would
otherwise have written. The rest are the per-pixel discipline section 3.4
already asks for, with a console name attached so nobody has to rediscover
them.

---

## 2. The hardware, honestly

What we have, what the S3 adds, and what each fact means for a renderer.
Sources: ESP32-C6 and ESP32-S3 datasheets, the Waveshare wiki pages for
both boards, and this repo's own notes.

| | ESP32-C6 (this board) | ESP32-S3 (Waveshare AMOLED 1.8, same panel) | Consequence |
|---|---|---|---|
| Core | 1 × RV32IMAC, 4-stage in-order, 160 MHz | 2 × Xtensa LX7, 240 MHz | C6 has ~1/3 the cycles; there is no core to hand present() to |
| FPU | none | single-precision | engine is fixed-point everywhere, S3's FPU is a bonus never a dependency |
| SIMD | none | PIE 128-bit (16×8 / 8×16 lanes), inline asm only | any vector path is an optional per-target fast path |
| Integer mul/div | hardware, pipelined 32-bit mul and div; **64-bit div is a library call** | hardware | `__divdi3` and signed `/ 2^n` are the two known traps (playbook items 7 and 11) |
| RAM | 512 KB HP SRAM, ~424 KiB heap free with the raw panel driver | 512 KB SRAM **+ 8 MB octal PSRAM** | on the C6 the framebuffer is 76% of RAM; on the S3 it is a rounding error |
| Data cache | none — SRAM is direct-access | 32/64 KB, needed for PSRAM | C6 memory access is deterministic; S3 PSRAM is ~80 MB/s read, ~50 MB/s copy when it misses cache |
| Instruction cache | 32 KB, backed by 16 MB flash at 80 MHz QIO | 16/32 KB | hot loops must fit; layout alone moved a benchmark 3.2 → 3.9 ms |
| DMA | GDMA, 3 TX + 3 RX channels; async memcpy supported | GDMA | strip transfers already DMA; mem-to-mem copies could offload clears (SRAM contention — measure) |
| Display bus | QSPI, 40 MHz stable, 80 MHz corrupts corners (unresolved) | same panel, same driver | fixing 80 MHz halves present on both boards |
| Second processor | LP RISC-V @ 20 MHz, LP peripherals only | none | not usable: our I2C pins are not the LP I2C pins, and it cannot touch HP SRAM at speed |
| Graphics acceleration | none (`SOC_PPA_SUPPORTED` is P4-only) | none | scalar C, verified not assumed |

The two numbers to carry in your head for the C6:

- **Cycles per pixel.** 160 MHz at 60 fps is 2.67 M cycles per frame, or
  **16 cycles per pixel for a full-screen pass**. At 30 fps it is 32. At
  half resolution (184×224, the sand grid's own size) it is 65 at 60 fps.
  Every renderer design below is judged against that ceiling: a per-pixel
  path costing more than ~16 cycles cannot fill the screen at 60 fps no
  matter what the bus does.
- **RAM.** 322 KiB of the ~424 KiB heap is the framebuffer. The largest
  free block after `gfx_init()` and the sand grid is ~40 KB. There is no
  room for a z-buffer, textures in RAM, or a second buffer *as long as the
  framebuffer stays full-screen*. Section 3.3 is about changing that.

---

## 3. The five levers, in order of payoff per unit of risk

### 3.1 Measure first (Phase 0, no code that ships)

Nothing measures the frame a user sees: every sand budget row times
`sand_step()` alone, the present-cost rows time the bus alone, and the cube
has no checked-in numbers at all. The existing issue **esp32c6-e6c** ("a
real frame-time row: sim + draw + present") is the prerequisite for
everything in this document, and it says so itself. Add to it:

- Run `suite_cube_perf.c` on the device under the current `-O2` build and
  check the report in (all four variants: baseline, no HUD, no partial,
  interlaced). That replaces the stale 15.5 fps table.
- Count pixels the rasterizer touches per frame and divide: **cycles per
  covered pixel** is the metric that transfers to any future renderer.
  The `frame_x0..y1` bbox already accumulated in `shade_pixel()` is a
  coarse proxy; a counter behind `CONFIG_LAUNCHER_DEVELOPMENT` is exact.

### 3.2 Halve the bus (esp32c6-kfg)

At 40 MHz a full frame is 17.6 ms; at 80 MHz it measured 9.6 ms before the
corner corruption sent it back. The open issue lists the untried knobs:
`cs_ena_pretrans`/`cs_ena_posttrans`, pad drive strength, SPI mode, a
settle between `CASET`/`RASET`/`RAMWR` (the vendor driver issues them
back-to-back), and the panel datasheet's actual maximum. Zero hardware
risk, and the single largest fixed cost on the board goes from 17.6 to
9.6 ms — for **every** app, on **both** boards. Interlace stacks on top:
9.6 → ~4.8 ms per half-frame.

### 3.3 Overlap render and present, and stop paying for a full framebuffer

Today `gfx_present()` is synchronous: `main.c` calls `frame()`, then
present, which drains every queued DMA transfer before returning
(`gfx.c:1794-1796`). The CPU idles for the whole 17.6 ms. Issue
**esp32c6-91i** proposes overlapping the next sim step with the transfer
for the sand app. For a 3D renderer the same idea goes further, and it is
the one architectural change this roadmap asks for:

**Band-mode rendering.** Transform and light the scene once, bin triangles
into the seven 64-row bands (or fourteen 32-row ones), then rasterize band
*k+1* into a spare buffer while DMA sends band *k*. Frame time becomes
max(render, bus) + one band, instead of render + bus. This is how every
fast MCU renderer works, and it is what "tiled" *should* have meant in the
tiling experiment recorded in Display-and-Rendering.md — that attempt was
slower (10.7 → 15.5 fps) because small3dlib re-transformed and re-clipped
the whole scene per tile; it has no binning. Binning fixes that: the
per-band cost is only the triangles that touch the band.

Band mode also changes the memory picture completely:

| Buffer | Full-fb mode (today) | Band mode, 64-row | Band mode, 32-row |
|---|---|---|---|
| Colour | 322 KiB (one) | 2 × 47 KB | 2 × 23.5 KB |
| Depth (16-bit) | impossible (322 KiB more) | 47 KB | 23.5 KB |
| Total | 322 KiB | **141 KB** | **70 KB** |
| Freed for the app | — | ~180 KB | ~250 KB |

A per-band 16-bit z-buffer costs 47 KB and gives correct hidden-surface
removal for arbitrary meshes — the thing the cube's file header calls
unaffordable at 1.3 MB, because it assumed full-screen 32-bit float.
The transaction overhead of more bands is known: ~118 µs per
`draw_bitmap()`, so fourteen bands cost ~1.7 ms of fixed overhead per
frame against ~180 KB of extra RAM — worth it for a 3D app, measure it.

**What this does to rule 1 ("exactly one framebuffer").** The rule stays,
its *shape* becomes a mode owned by gfx, which is exactly where issue
**esp32c6-rpt** (resolution and colour mode as system settings) already
lands: the framebuffer's geometry and pixel format become runtime state,
an app declares what it needs at `enter()` (layout: full-fb or bands;
resolution: full or half), gfx grants it subject to the system-wide
maximum-resolution setting and reallocates, and `exit()` restores. Sand
and the UI keep full-fb mode with dirty bands; 3D apps ask for band mode. `gfx_present()` grows a band-streaming entry point
(`gfx_band_begin/submit/end`, name to be decided) that hands out the spare
band buffer and waits only on the *previous* band's DMA. The shell's loop
does not change: `frame()` still draws and returns, it just draws into
bands.

**The C6-specific catch, to measure not assume**: DMA and the CPU share
SRAM with no cache in between, so the rasterizer *will* run somewhat slower
while a transfer is in flight. The 1 ms busy-wait experiment in
Display-and-Rendering.md suggests the overlap is real and large, but
nobody has measured the contention penalty on a compute-heavy loop.

**Interlace and bands are panel-row shaped; the game is not.** The
maintainer's observation (2026-09-04) that different orientations favour
different interlace setups is a real constraint, and it splits "interlace"
into two independent halvings that today's toggle conflates:

- *Bus-side*: which rows are sent. This is always panel rows, because a
  QSPI window is a run of whole rows and every transaction costs ~118 µs
  — alternate *columns* cannot be sent cheaply. Its comb artifact appears
  on motion perpendicular to the field lines, so in portrait it combs on
  vertical motion (falling sand) and in landscape on horizontal motion
  (a scrolling platformer, an FPS turning). The same field split is
  benign in one orientation and the worst case in the other.
- *Render-side*: which pixels are computed. This can follow the app's own
  axis: a raycaster in portrait can cast every other column per frame
  (half the rays and half the fill, bus unchanged), a span renderer can
  skip alternate rows, and a scroller can skip whichever axis it is not
  scrolling along.

Two consequences for the design. First, the mode request at `enter()`
carries the interlace choice per axis and the app decides it against the
*user's* frame, not the panel's, using the quarter-turn orientation model
the UI layer already has; an app that rotates with the device re-picks
when the layout generation changes. Second, orientation also changes which
renderer maps naturally onto bands: in landscape a panel band is a group of
complete user-space *columns*, which is ideal for a raycaster (each band
is a set of whole rays), while in portrait a band is a slice through every
column. The raycaster's band pass should be written for both cases from
the start rather than assuming one.

**The panel remembers, so only change needs sending.** The panel keeps
what it was last sent; the dirty-band system already exploits that for
sand and the UI, and the cube's partial clear is a crude version for 3D.
It cannot be read back, so whatever tracks what is on the glass is our
state. For a *moving* camera (the FPS, a scroller) every pixel changes
every frame and there is nothing to exploit: band mode, full sends. For
a *fixed* camera (a rolling-ball screen, a non-scrolling platformer room)
the per-frame change is the union of the old and new bounding boxes of
the moving objects, and fill and bus both drop by an order of magnitude.
The two framebuffer modes each have a way to get it:

- *Retained framebuffer plus dirty rectangles* — the classic form, and
  how fixed-camera games of the PS1 era worked. Static geometry is drawn
  once; each frame restores the background under the old boxes (either
  re-rasterize the static triangles scissored to the box, or blit a
  pre-rendered background baked into flash like the boot photograph) and
  redraws the dynamic objects. Needs full-fb mode: 322 KiB, or ~80 KB at
  half-res.
- *Band mode with band-level dirtiness* — no retained buffer; the scene
  is the retained state. A band nothing moved through is neither rendered
  nor sent. A band something moved through is re-rendered whole from the
  scene and sent. No restore step, no second copy, and it composes with
  everything above; the tracker is per-band bookkeeping from object
  bounds, not pixels, so it is cheap and host-testable. Its cost is
  re-rasterizing the static geometry in touched bands, which a baked
  per-band background image removes.

This is a per-app choice through the same `enter()` request, and it
reaches into game design: a rolling-ball game with a fixed or stepwise
camera gets this win, one with a smooth follow camera does not.

### 3.4 Fill rate: the per-pixel path

The C6 budget is 16–32 cycles per pixel. Everything that costs more has to
be moved out of the per-pixel loop, into per-span, per-triangle, or
bake-time work:

- **Spans, not callbacks.** Rasterize scanlines into spans (x0, x1, and
  the per-span start values and deltas), then fill spans in a tight loop
  the compiler can see whole. small3dlib's design is a callback per pixel
  with barycentrics recomputed per pixel — good for a teaching library,
  wrong for this budget. Keep its transform half (issue **esp32c6-xnq**
  extracts exactly that family out of `boot_anim.h`); replace the
  rasterizer.
- **Step, don't interpolate.** Gouraud becomes three adds per pixel (or
  one add on a packed 5-6-5 accumulator with guard bits) instead of nine
  multiplies. Affine texture mapping becomes two adds and one load.
  Perspective correction, when wanted, is done once every 8 or 16 pixels
  with linear steps in between — the PS1/Quake trick, and small3dlib's
  `S3L_PERSPECTIVE_CORRECTION 2` does the same.
- **Two pixels per store.** RGB565 pairs into one 32-bit write, the same
  trick `gfx_clear()` already uses. Flat spans are `memset`-shaped.
- **Dithered span fills: Gouraud-looking shading at flat-fill cost.**
  Per-pixel dithering (threshold lookup, pick a colour) costs as much as
  stepped Gouraud and buys nothing. Span-level dithering is different: a
  4×4 Bayer pattern between two colours repeats every four pixels, so for
  a given row parity and shade level it is two 32-bit pattern words, and
  a dithered span is a flat fill alternating two words instead of one
  (plus a head and tail pixel for alignment). Shade varies along the span
  by splitting it where the level changes, each piece another pattern
  fill, a handful per span. With the colormap, 32 light levels dithered
  between neighbours read like several hundred while the table stays
  16 KB. It looks like a PS1 or Saturn, which at this size is a feature.
  Estimates for the rasterizer bench, to be replaced by measurements:

  | Fill | Cycles per pixel, *estimate* |
  |---|---|
  | Flat, 32-bit stores | 1–2 |
  | Dithered span, pattern words | 2–3 |
  | Stepped Gouraud, packed accumulator | 5–8 |
  | Per-pixel dither, threshold lookup | 5–8 |
- **Lighting through tables, not arithmetic.** Doom's colormap: a
  32-level × 256-entry table maps (light, colour index) to a packed RGB565
  word — one load per pixel, no per-channel math, and the table is 16 KB
  in flash or 16 KB in RAM if the icache misses show up. Dithered
  gradients (the Bayer machinery in `gfx_color.h`) replace true blending
  everywhere except at glyph scale.
- **Half resolution as a first-class mode.** 184×224 rendered, pixel-
  doubled on the way to the panel (the sand grid already lives at this
  size). Fill cost drops 4×, the band buffers drop 4×, and on a 1.8-inch
  AMOLED the doubling is barely visible. The bus bytes do not change
  (the panel has no scaler), so this is a CPU and RAM lever, not a bus
  lever — which is why 3.2 and 3.3 come first. Resolution is the app's
  choice at `enter()`, capped by a system-wide maximum the user sets
  (decision 1 in section 8): gfx grants min(requested, system max) and
  reports back what was granted.
- **Textures column-major for vertical spans** (raycaster) and row-major
  for horizontal ones; 64×64 RGB565 is 8 KB, so a handful live in flash
  behind the 32 KB icache and the hot ones can be copied into RAM at
  `enter()`.

### 3.5 Code shape, the levers this repo already knows

All from the playbook and the sand campaign, restated because a new
renderer will hit every one of them: verify inlining with `objdump`, never
trust the attribute (issue **esp32c6-e72** automates it); keep the hot loop
under the 32 KB icache and pin it with `aligned(32)`; no 64-bit divides,
no signed divides by powers of two; a unity build for cross-file inlining
(**esp32c6-pm6**) if the rasterizer spans files; host numbers predict
code-shape changes well and work-quantity changes badly; and the RTOS tick
and input tasks are a small, measurable tax (**esp32c6-bsc**). Allocate
everything an app needs once at `enter()` and free it at `exit()` — the
repo's "app exclusivity" convention and every MCU renderer's "allocate at
startup, never again" advice are the same rule.

---

## 4. A renderer per target game

The three games want three different renderers, and none of them wants a
general-purpose z-buffered triangle engine first. Each is listed with the
technique that fits the 16–32 cycles-per-pixel budget.

### 4.1 FPS with gyro and buttons → raycaster first, sectors later

A grid-map raycaster (Wolfenstein-style) is the right first FPS on this
class of hardware, and it is not a compromise — it is what the budget
buys at 60 fps:

- One ray per screen column: 368 rays, or 184 at half-res. Each is a DDA
  walk over a byte map, ~10–30 steps, all integer. Geometry cost is a few
  hundred thousand cycles per frame, *estimate*.
- Each column then fills one textured vertical span: one texture load and
  one store per pixel (column-major textures make the texture walk a
  pointer increment). That is well inside the budget with headroom for
  the colormap lookup.
- Sprites are billboards sorted by distance and clipped per column
  against the per-column depth (368 × 2 bytes, not a z-buffer).
- Floor and ceiling casting is the expensive optional: per-pixel affine
  on horizontal spans. Start with flat colours and a dithered distance
  gradient; add real floor texturing once the numbers say there is room.
- Gyro drives look (the tilt/shake library extraction, **esp32c6-1v0**,
  is the prerequisite); the two buttons move and act. Touch can be an
  on-screen stick if two buttons prove too few.

Band mode fits a raycaster naturally: columns are independent, so
rendering band *k* means rendering 64 rows of every column — the ray
results (hit distance, texture column, span bounds) are computed once per
frame and the per-band pass is only fills.

A sector/portal renderer (Doom-shaped: sloped-free rooms, varying floor
heights) is the natural second step and reuses everything above; a full
mesh FPS is the one thing *not* to aim for on the C6.

### 4.2 Rolling ball with physics and lighting → the "real 3D" showcase

This is where the band rasterizer from 3.3/3.4 earns its place:

- The world is a heightfield or tiled floor, rendered as a mesh with
  vertex lighting (Gouraud via span stepping) and an affine floor texture.
  A regular grid drawn back-to-front from the camera needs no z-buffer at
  all — painter's order is trivial on a grid — so the per-band depth
  buffer can be skipped until something non-grid appears.
- A cheaper v1 exists (section 1.1): a flat Mode-7 floor — one affine
  texture, per-scanline constant steps, one load and one store per
  pixel — with the ball and obstacles as sprites. It has no height, but
  it needs no mesh rasterizer, so it can be playable before r3d lands
  and then be replaced by the heightfield when slopes are wanted.
- The ball is a sphere, and a sphere is view-invariant: shade it as a disc
  with a baked normal lookup (a radius-squared table gives the normal per
  pixel, one N·L per pixel or a baked lit-disc sprite per light bucket).
  Its rolling is conveyed by a textured shell, which is an affine sprite
  rotation — or by the shadow and motion alone, which is cheaper and
  reads fine at this size. Shadow: a dithered dark ellipse.
- Physics is fixed-point 2.5D on the heightfield (position, velocity,
  slope from the height gradient); the gyro's gravity vector, from the
  same tilt library as the FPS, tilts the table. Collision is against the
  grid.
- Lighting: one directional light plus the vertex-baked ambient; point
  lights are a per-vertex cost, not per-pixel, so they are cheap.
- Camera: fixed per screen, or stepping between fixed positions, rather
  than a smooth follow. That is what unlocks band-level dirtiness
  (section 3.3): per frame only the bands the ball and its shadow moved
  through are re-rendered and sent, an order of magnitude less fill and
  bus than a full frame. A smooth follow camera gives that up for every
  frame it moves. Decide this before the level format is designed.

### 4.3 Platformer with parallax and 2D lighting → a tile engine

- Layered tile map with **parallax scrolling** (layers move at different
  rates). Each layer is a blit of 16×16 or 32×32 tiles; a screen is
  23×28 of them at 16 px.
- Scrolling defeats dirty tracking (every pixel moves), so the platformer
  needs 3.2 and 3.3 as much as the 3D apps do: a full frame every frame,
  streamed in bands. Band mode with a tile renderer is again natural —
  a band is a horizontal slice of the tile rows.
- **2D lighting**: a light map at tile (or 8 px) resolution multiplied
  through the colormap, plus per-pixel normal-mapped lighting only inside
  each light's radius (a dirty region, tens of thousands of pixels, not
  the screen). Dithered radial gradients handle soft edges.
- "Parallax" here means layered scrolling, confirmed by the maintainer
  2026-09-04. Per-pixel parallax *mapping* (a raymarch per pixel) is not
  on the table; if a depth cue on individual tiles is ever wanted, a
  normal map plus a one-step height-based UV offset costs a few cycles
  and gives most of the effect.

---

## 5. Architecture for Autana

Layers, in the include-qualified style the tree already uses, hardware
below and pure logic above, the hardware side calling the pure side and
never the reverse (the testing guide's rule):

```
platform/   board bring-up, display bus, input devices, timers, memory caps
            (today: gfx.c's driver half, input/, boot/) — one folder per board
gfx/        framebuffer or band buffers, present, primitives, dirty tracking
            (today's gfx.c minus the driver, plus band mode)
core/       fixed.h, intmath.h, rng.h, tween.h, an arena allocator,
            the authored-timeline system graduated from boot_anim (esp32c6-do5)
render/     r2d: tiles, sprites, blits, colormap lighting
            r3d: transform/clip (from small3dlib via esp32c6-xnq), bin, spans, z
            rc:  raycaster
game/       physics, entities, tilt/shake (esp32c6-1v0)
apps/       the games, each a folder, APP_REGISTER, no other file touched
```

Principles, each of which is already a repo habit:

- **Fixed-point only, C6 first.** The S3's FPU and SIMD are per-target
  fast paths behind the same interface, never the reference path. A
  renderer that needs a float is a renderer that does not run here.
- **Everything above `platform/` compiles on the host.** The boot
  animation editor already renders real frames through the real
  `gfx.c` on the host (`tools/boot_anim_render_host.c`); generalize that
  into a host runner that renders any app's frame to a `.bmp`, so the
  device screenshot tool and the host render can be diffed pixel-exact.
  That is the visual regression suite, and the TDD loop for a renderer.
- **The frame loop stays the shell's.** Band mode and present pipelining
  live in gfx and the shell; apps declare a mode at `enter()` and draw
  in `frame()`. Rule 2 is what makes 3.3 possible without touching apps.
- **Data is baked, not parsed.** Textures, colormaps, maps, timelines and
  fonts go through generators into headers with the regenerate command
  in their banner, validated by the generator and tested independently
  (the generated-files convention in `CLAUDE.md`).
- **Allocate at `enter()`, free at `exit()`, nothing in between.**
- **Per-target platform folders, one binary per board.** ESP-IDF picks
  the chip; `platform/<board>/` picks the bus clocks, the PSRAM policy
  (S3: double-buffer the full frame in PSRAM and present from core 1),
  and the input wiring. Same panel, same driver, same dirty tracker.

---

## 6. Phases, with the gate each one has to pass

Each phase ends when its number is measured on the device and checked
in. The tracker side is beads epic **esp32c6-ems** ("Autana rendering
roadmap"), whose children are the phases that had no issue before this
document; pre-existing issues are named where they apply.

| Phase | Work | Gate (measured, on device) |
|---|---|---|
| **0. Attribution** | esp32c6-e6c frame-time row; esp32c6-ems.1 re-run `suite_cube_perf` at `-O2` and check the report in, plus a cycles-per-covered-pixel counter | A table replacing the stale 15.5 fps figures |
| **1. Bus and overlap** | esp32c6-kfg (80 MHz root cause); esp32c6-91i (present pipelining) for sand; esp32c6-ems.2 **band mode** in gfx with a 2-band ring + optional per-band z; the mode switch from esp32c6-rpt | Full-frame present ≤ 10 ms; cube in band mode at ≥ 50 fps or bus-capped, whichever is lower |
| **2. r3d v1** | esp32c6-ems.3 own span rasterizer: flat, Gouraud, affine texture, colormap lighting; transform/clip via esp32c6-xnq; triangle binning; half-res mode | ≥ 60 fps on a 500-triangle textured scene at half-res (bus permitting: needs 80 MHz or interlace), ≥ 30 at full; cycles/pixel ≤ 16 for flat, ≤ 24 textured |
| **3. Raycaster and the FPS prototype** | esp32c6-ems.4 `render/rc`, column-major textures, per-column depth sprites, gyro look via esp32c6-1v0, buttons move | 60 fps full-res walls + sprites, playable on the glass |
| **4. Rolling ball** | esp32c6-ems.5 heightfield mesh on r3d, lit-disc ball, 2.5D fixed-point physics, gyro gravity | 30+ fps full-res, physics stable at dt 16–33 ms |
| **5. r2d and the platformer** | esp32c6-ems.6 tile layers, parallax, sprites, colormap light map, radius-limited normal lighting | 60 fps scrolling at full-res in band mode |
| **6. S3 port** | esp32c6-ems.7 `platform/esp32s3`, PSRAM full-frame double buffer, present on core 1, FPU/PIE fast paths behind the same interfaces | Same three games, same tests, on the second board |
| **Throughout** | graduate boot-anim tech (esp32c6-do5, esp32c6-xnq); esp32c6-ems.8 host render harness; inlining-cliff gate (esp32c6-e72) | Every graduated piece has a second consumer and a reference test |

Phase 0 is a week of measurement and no shipping code. Phases 1 and 2 are
the investment: they are where the architecture changes, and every game
after them is mostly content plus one renderer module. Phases 3–5 can be
reordered by appetite; the raycaster is listed first because it is the
cheapest path to something that is unmistakably a game.

---

## 7. What not to do

- **Do not chase triangles per second.** The metric that transfers is
  cycles per covered pixel and bytes per frame on the bus.
- **Do not add a full-screen z-buffer, a second full framebuffer, or
  LVGL.** Band mode makes the first two unnecessary and the third is
  already ruled out in Launcher-Architecture.md.
- **Do not swizzle the framebuffer into tiles** — parked on purpose in
  Display-and-Rendering.md, and band mode gets the same transfer property
  without touching every draw call.
- **Do not build on small3dlib's per-pixel callback.** Keep its transform
  half, replace its rasterizer.
- **Do not use floats anywhere an S3 fast path could hide them.** One
  float in the reference path means the C6 build silently drops to
  software emulation.
- **Do not trust a host win on a work-quantity change** (playbook item 9);
  host numbers are for code shape.

---

## 8. Open decisions

1. ~~Half-res as the default for 3D apps?~~ **Decided 2026-09-04: a
   per-app choice, under a system-wide maximum.** Each app declares the
   resolution it wants at `enter()`; the shell carries a configurable
   "max resolution" setting (Settings app, esp32c6-rpt) that caps what
   any app gets, so the same firmware can be dialled down for battery or
   heat without touching an app. gfx resolves the effective mode as
   min(app request, system max) and pixel-doubles on the way out when
   they differ; the app is told the resolution it actually received, the
   way it is already told the screen height as a parameter.
2. ~~Band height: 64 rows or 32?~~ **Decided 2026-09-04: a parameter,
   settled by a sweep.** Band mode is built with the band height as a
   compile-time constant (divisors of 448: 64, 32, 16), and Phase 1 ends
   with a device sweep across them measuring present time, rasterizer
   time under DMA contention, and RAM freed, in the same style as the
   `GATHER_MAX_PIXELS` and `LEAF_REFINE_MAX_RUNS` sweeps recorded in
   Display-and-Rendering.md. The winner becomes the default; the
   parameter stays so the S3 can pick its own.
3. ~~"Parallax" in the platformer~~ **Decided 2026-09-04: layered
   parallax scrolling**, not per-pixel parallax mapping.
4. ~~Own rasterizer vs. deeper small3dlib configuration.~~ **Decided
   2026-09-04: own rasterizer.** The seam: copy small3dlib's vector,
   matrix, projection and clipping routines into `render/r3d` under this
   repo's fixed-point conventions (the same move esp32c6-xnq makes for
   the boot animation's helpers), and write the rasterizer, binning and
   ordering table fresh. Everything in `S3L_drawTriangle` that fights the
   budget is structural, not a knob: barycentrics computed for every
   pixel even when flat, a per-pixel callback bound at include time (one
   copy per translation unit, which is why a shared render module cannot
   sit on it), compile-time resolution macros that decision 1 already
   ruled out, no binning, no texture or material concept, a comparison
   sort where an ordering table is cheaper, and 9 fractional bits. The
   rasterizer is on the order of 600–1000 lines; the parts worth tests
   are subpixel edge setup with a top-left fill rule (no cracks, no
   overdraw between neighbours), near-plane clipping, and the
   perspective-correction cadence, all pixel-exact against a slow
   reference on the host. small3dlib stays vendored only until the boot
   animation stops including it, then the component is deleted.

---

## 9. Working from this document

The workflow here is that a planning model writes the issue, an
implementing model builds it, and a reviewing model checks it, each in a
fresh session with no memory of this conversation. So the document and
the beads issues are the whole contract. Rules for each role:

**Implementer (one issue at a time):**

1. Read the issue's `bd show` in full, including its notes, then only the
   sections of this document it names, the three rules in
   [Launcher-Architecture.md](Launcher-Architecture.md), and
   [Optimization-Playbook.md](Notes/Optimization-Playbook.md). Nothing
   else is required reading.
2. The issue's acceptance criteria are the definition of done. If a
   criterion cannot be met, say so in the issue notes and stop; do not
   redefine it.
3. Write the failing test first (`docs/Testing-Guide.md`): a host test for
   pure logic, a `DEVICE_BUILD` row for anything timed. A test never seen
   red proves nothing. Sand's byte-identical fingerprint rule applies to
   anything that touches the sand app.
4. Measure on the device with the script the issue names, and check the
   report in under that tool's `results/`. A host number is a hypothesis
   (playbook item 9). Verify inlining with `objdump`, not the attribute
   (item 3), and diff `.bss` for every build variant (item 8).
5. Fixed point only in the reference path. No `float`, no `double`, no
   64-bit divide, no signed divide by a power of two in a hot loop.
6. Do not reopen a settled decision (section 8). If the work shows one is
   wrong, append the evidence to the issue and stop; changing it is the
   planner's call. Do not widen scope into a neighbouring phase.
7. Leave the trail: a `bd update --append-notes` with what was measured,
   what was tried and rejected, and what the next issue needs to know.
   Update the docs the change makes wrong in the same commit.

**Reviewer:**

- Every number in the PR is either measured on the device with its source
  named, or explicitly marked as an estimate. No "should be faster".
- The issue's acceptance criteria are met literally, and the gate in the
  phase table (section 6) is met or the shortfall is stated.
- The three rules hold: one framebuffer (or the band ring, as a gfx-owned
  mode), one frame loop owned by the shell, apps as callbacks that draw
  and return.
- No new file-scope `static` buffer in any build variant without a
  `.bss` diff in the PR; malloc at `enter()`, free at `exit()`.
- Nothing floating point reached the reference path; any S3-only fast
  path sits behind the same interface as the C6 path.
- The hot functions the issue names still inline (`objdump -t`), and the
  hot loop is under the 32 KB icache.
- Anything graduated out of an app or the boot animation has a second
  consumer and a reference test, or it stays where it was.
- Docs updated alongside the code, and the issue notes carry the
  measurements, not just the PR description.

---

## Related

- [Launcher-Architecture.md](Launcher-Architecture.md) — the three rules
  band mode has to respect.
- [Notes/Display-and-Rendering.md](Notes/Display-and-Rendering.md) — every
  bus and dirty-tracking number cited above, and the parked ideas.
- [Notes/Optimization-Playbook.md](Notes/Optimization-Playbook.md) — the
  code-shape rules a new renderer will hit.
- [Notes/Board-and-Memory.md](Notes/Board-and-Memory.md) — the memory
  budget band mode is designed against.
- [Settings-App-Plan.md](Settings-App-Plan.md) and beads esp32c6-rpt — the
  mode switch the framebuffer geometry lands in.
