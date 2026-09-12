# Plan: bake icons from an atlas, as a system-wide facility

**Status**: planned 2026-09-10, not built. Written straight after the brush
screen's Phase 3 ([Sand-Brush-Screen-Plan.md](Sand-Brush-Screen-Plan.md))
shipped four hand-drawn icons in `apps/sand/sand_icons.h`, and the question
was asked whether writing icons as C bit literals scales.

It does not. But the header is not the part that fails.

---

## What actually does not scale

**The format, not the file.** `icons.h`'s bitmaps are `uint16_t` rows, one
bit per pixel, so `ICON_BITMAP_SIZE` is 16 and every icon in the tree is
capped at 16px wide forever. `ICON_BITMAP_MAX_BLOCKS` is derived from that
16. Nothing about a 24 or 32px glyph fits, and widening the row type is a
change to every bitmap already written.

**Authoring, which is worse.** You cannot see a picture written as bit
literals. That is not a hypothetical: while writing `sand_icons.h`, one row
of the BOOM glyph was transcribed with the wrong column set, breaking its
mirror symmetry, and the only thing that caught it was
`suite_sand_icons.c`'s symmetry assertion. The bug was invisible in review
and obvious in a paint program. A raster source removes the entire class.

**Ownership has nowhere to go.** `sand_icons.h` lives in `apps/sand/`, which
is right for a funnel and wrong for a check mark. `gfx/icons.h` today holds
exactly one shared glyph plus a comment explaining that `MU_ICON_CLOSE`,
`MU_ICON_COLLAPSED` and `MU_ICON_EXPANDED` are deliberately unbuilt because
nothing has asked for them — a note that predicts this problem. Two apps
wanting the same chevron have no shared place to put it.

---

## This is the fifth generated file, not a new pattern

Four already exist, each with a generator in `launcher/tools/` and its output
checked into the tree: `boot_anim_curve.h`, `boot_anim_timeline.h`,
`boot_anim_image.h`, `gfx/fonts/font_lmroman_40.h`. They share a convention
(docs/Launcher-Architecture.md, "Generated sources"):

- a banner naming the **exact** regenerate command;
- the generator validates itself before emitting anything;
- the shipped artifact is tested **independently of the generator** — against
  the underlying math where there is one, or, for an asset with no math to
  check against, structural facts a `_Static_assert` can pin down, plus using
  the shipped metrics for real;
- Pillow is imported lazily inside a function with an install hint, exactly
  as `gen_font.py` and `gen_boot_anim_image.py` both do.

`launcher/tools/gen_icons.py` is that shape. Nothing here needs a new
argument; it needs to follow the one already made four times.

---

## Source: an atlas plus a manifest

```
design/icons/system.png     the grid, 1 bit per pixel
design/icons/system.json    cell size, and a name per occupied cell
```

The manifest carries the grid geometry and the name of each cell.
`boot_anim_timeline.json` is the precedent for a hand-edited JSON beside a
generator, and a grid you can see all at once is the entire point of an
atlas — a set of icons is designed *as a set*, and consistency of weight and
optical size between them is the thing you cannot judge one file at a time.

**Strictly 1bpp.** The generator rejects any pixel that is neither fully on
nor fully off rather than guessing a threshold. These are pixel art; an
anti-aliased edge in the source means the artist exported wrong, and
silently rounding it produces a glyph nobody drew.

## Output: packed rows, and a name per icon

`launcher/main/gfx/icons_system.h`, holding one `rows[]` blob, a per-icon
record, and a generated enum of names:

```c
typedef struct {
    const uint8_t *rows;     /* stride bytes per row, MSB is column 0 */
    uint8_t w, h, stride;
    uint8_t blocks;          /* run-length rects this icon costs — see below */
} icon_t;
```

Packed `uint8_t` rows with an explicit stride is what lifts the 16px cap.
Every field is `const` and every array is `static const`, deliberately: see
the RAM and cache section below for why a single missing `const` is the one
edit that could quietly move this whole facility into DRAM.

---

## Bake the run count, and the budget stops being a runtime cliff

This is the reason to do this that has nothing to do with authoring comfort.

`ui_draw_bitmap()` renders an icon as run-length rectangles into microui's
command list, which is **8 KiB** (`MU_COMMANDLIST_SIZE`, cut down from
upstream's 256 KiB). Phase 6 measured the brush screen at roughly **5.4 KiB
of that** — two-thirds full, with four icons on it. `ui_draw_bitmap()`'s
own working buffer caps an icon at `UI_DRAW_BITMAP_MAX_BLOCKS` (48) runs.

Today the only thing standing between the tree and an overflowing command
list is `suite_sand_icons.c` asserting that the four icons that exist happen
to be small. A fifth icon with a dithered edge or a dotted border could
exceed either bound, and an overflowing microui command list is not a
graceful degradation.

The generator computes every icon's run count while packing it. Emitting
that count lets a `_Static_assert` per icon reject the artwork **at build
time**. A run count is scale-invariant — scaling changes each run's size,
never how many runs a row has, which Phase 3 established and the existing
suite already relies on — so one baked number is valid at every size the
icon is ever drawn.

---

## The budget that is not the command list: RAM, and the 32 KB cache

An icon facility that grows without bound is only safe if growing it cannot
take DRAM from an app or evict a hot loop. Three rules, in decreasing order
of how easy they are to get wrong.

**Baked data must stay in flash, and must be provably there.** Everything
the generator emits is `static const`, which on this target lands in
`.rodata` in flash rather than in DRAM — the same placement
`docs/notes/Optimization-Playbook.md` records for the boot photo ("the photo
is `static const`, so it lives in flash behind the XIP cache"). That keeps
icons entirely out of the pool `check_static_ram.py` guards, where the
framebuffer plus one sand grid already have to fit contiguously. **Verify it
rather than assume it**: a baked set must move `check_static_ram.py`'s
number by zero, and that is a one-line check to run when phase 1 lands, not
a claim to take on faith. A single missing `const` silently relocates the
whole table into DRAM.

**No runtime registry, no init-time copies.** Lookup is an index into a
generated `const` table, resolved at the call site. Nothing is assembled in
RAM at startup, nothing is cached in RAM after a draw, and no app "registers"
its icons at boot. This is also why lookup is by generated id and never by
name string: a name lookup wants a searchable structure, and a searchable
structure is the first step toward building one in RAM.

**Per-draw stack must not scale with icon size.** This is the one place the
current design actually fails the constraint. `ui_draw_bitmap()` extracts
runs into a stack array of `UI_DRAW_BITMAP_MAX_BLOCKS` (48) `icon_rect_t` —
768 bytes of transient DRAM on the UI task's stack. At 16x16 that is
merely wasteful; at 32x32 the worst case is 16 runs per row over 32 rows,
and a cap raised to match would put several KiB on a stack that already has
a checker complaining about a 2 KiB test frame.

So `ui_draw_icon()` should **stream** runs, emitting each `mu_draw_rect()`
as it is found instead of collecting them all first. Per-draw stack then
becomes O(1) in the icon's size, and `UI_DRAW_BITMAP_MAX_BLOCKS` stops being
a constraint on artwork at all — leaving the command-list budget as the only
ceiling, which the baked run count now enforces at build time anyway. The
array form exists today because pure geometry returning a buffer is easy to
host-test; an iterator keeps that property, since a test's callback can
collect into an array while the firmware's callback draws.

**On the cache, specifically: the risk is code, not icon data.** A 16x16
1bpp icon is 32 bytes — one cache line, against a 32 KB shared XIP cache.
Even a hundred of them, touched once each per frame, is noise next to the
sand simulation's own working set. What *can* hurt is
`icon_bitmap_blocks()` being `static inline` in a header: every call site
gets its own copy of the run-length extraction, and the Optimization
Playbook records exactly this failure mode — "eventually a function gets
folded into every one of its call sites and the hot loop stops fitting the
32 KB cache, and the technique that had been winning at every prior level
makes everything worse." One or two call sites is fine. If icon drawing
spreads across several apps, move the extraction out of line into a pure
`.c` that host tests can still link, and measure rather than assume which
way is cheaper.

## Ownership: one generator, two homes

Decided, not proposed: there is a shared system set for UI/UX vocabulary,
and apps may also provide their own. The obvious reading of "system-wide"
fights an existing rule, and the split is what resolves it. docs/Launcher-Architecture.md:
adding or removing an app touches no other file, and deleting the folder
deletes the app, its logic and its tests cleanly. Put sand's funnel in a
system atlas and deleting `apps/sand/` leaves artwork nothing draws.

Split by **ownership**, not by mechanism:

| | |
|---|---|
| `design/icons/system.png` → `gfx/icons_system.h` | vocabulary any app means the same way: check, close, chevrons, info, back |
| `apps/<name>/icons/<name>.png` → `apps/<name>/icons_<name>.h` | that app's own artwork; deleting the folder takes the art, the manifest and the baked header with it |

Same generator, same format, same tests. An app-specific icon that turns out
to be general moves by editing two PNGs, which is the right amount of
friction for a decision about shared vocabulary.

**Why icons may use a runtime table when fonts may not.** `gfx_font_roles.h`
argues hard that roles must resolve at compile time, because a registry
resolving a role variable at runtime would reference every candidate from one
translation unit and force all of them to link — and a coverage atlas is
**274 KiB**, so the linker dropping an unselected one is load-bearing. A
16x16 1bpp icon is 32 bytes; a hundred of them is about 3 KiB. The
calculus that governs fonts simply does not apply here. Say so in the header,
because the roles file argues the opposite case forcefully and the next
reader will reasonably wonder why icons ignore it.

---

## What the generator must reject before emitting

Following `gen_boot_anim_image.py`, which exits on any pixel it cannot
account for (down to "N of M panel slots were never written"):

- any pixel that is not strictly on or off;
- a named cell that is empty, or a non-empty cell with no name — an unnamed
  drawing in the atlas is a mistake, not a spare;
- duplicate names, or names that are not valid C identifiers;
- any icon whose run count exceeds the cap, naming the worst offender;
- a pack/unpack round trip that does not reproduce the source pixels exactly.

## Testing the artifact, not the generator

The shipped header is tested on its own terms: every icon non-empty, its
content bounding box inside its declared `w x h`, its baked `blocks` count
matching what `icon_bitmap_blocks()` actually produces, and declared
symmetries holding. `suite_sand_icons.c` already does exactly this and
generalizes almost unchanged.

**Do not assert the baked bytes against a Python re-implementation of the
packer.** That tests the generator twice and the artifact never — the
failure mode docs/Launcher-Architecture.md's convention is written to prevent.

---

## Phases

1. **The generator, proved against artwork we already trust.** `gen_icons.py`
   reading a PNG atlas, `design/icons/system.*`, `gfx/icons_system.h`.
   Purely additive: nothing is rewired, and the acceptance test is that the
   baked check mark is bit-for-bit the shape `icon_check_bitmap` already
   holds, with `suite_icons.c` green and untouched as the independent
   witness. **Record `check_static_ram.py`'s number before and after** — a
   baked set must move it by exactly zero, which is what proves the flash
   placement claimed above rather than asserting it.

2. **An SVG reader, and a real system atlas.** Source:
   [pixelarticons](https://github.com/halfmage/pixelarticons), MIT (Gerrit
   Halfmann, 2019), 1,036 free icons on a strict 24x24 grid with no
   anti-aliasing.

   **Why SVG turns out to be the easy format, not the hard one.** Their
   icons are a single `<path>` of nothing but `M`/`H`/`V`/`h`/`v`/`Z` at
   integer coordinates - every subpath is an axis-aligned rectangle, almost
   always 2x2. Verified directly against `check.svg` and `close.svg`. So the
   reader extracts rectangles and fills a grid **exactly**: no rasterizer,
   no Pillow, no threshold, and therefore none of the "silently rounding
   produces a glyph nobody drew" risk the PNG reader has to guard against.
   A ~30-line parser, and the same stdlib-only rule the rest of the
   generator already follows.

   Be as strict here as the PNG reader is about anti-aliasing: a curve, an
   arc, or a non-integer coordinate is a **rejection**, not something to
   approximate. The whole reason this source is safe is that it never needs
   approximating.

   **Bake only what the manifest names.** They publish a thousand icons; we
   want on the order of eight - check, close, info, alert, the chevrons,
   back, home. Flash cost is what we use.

   **Provenance is a first-class requirement, not paperwork.** The reason
   the defaulticon set was rejected is that its upstream had disappeared,
   leaving its licence unverifiable for an asset that would live in this
   tree permanently. So: carry the MIT notice in `design/icons/`, name it in
   the generated banner, and **pin the upstream commit in the manifest** so
   a re-bake years from now is reproducible and the provenance is still
   checkable.

3. **The packed format's streaming draw.** Stride-aware run extraction
   reshaped as an iterator, so `ui_draw_icon()` emits each rect as it is
   found rather than buffering an icon-sized array on the UI task's stack -
   see the RAM section above for why that is the one place the current
   design fails its own constraint. Keep an array-returning form for host
   tests, or have the test supply a collecting callback.

4. **Migrate sand's four.** Delete `sand_icons.h`.

   Decide here whether they get redrawn: ours are 2px strokes on a 16 grid
   (about 8x8 blocks), pixelarticons are 2px on 24 (about 12x12). Close, but
   not the same density - and two sets that nearly match read worse than two
   that clearly differ. Judge it with both on screen, not in the abstract.

5. **Structural asserts, and a per-atlas total.**

   This phase was planned as "`_Static_assert` the run counts, turning a
   runtime cliff into a build error" - but the cliff it named was
   `ui_draw_bitmap()`'s 48-slot stack buffer, and phase 3 deleted that
   outright. There is no per-icon ceiling left to assert against beyond
   `icon_t.blocks` being a `uint8_t`, which the generator already rejects
   above. Recorded rather than quietly reinterpreted, because a plan that
   still describes a solved problem sends the next reader looking for it.

   What is genuinely left is smaller: emit `_Static_assert`s in each
   generated header for facts a consumer's compile can then check - every
   offset inside the blob, the blob's size equal to the sum of each icon's
   `h * stride`, `blocks` within its type - so a generator bug fails at the
   call site rather than at runtime. Plus a per-atlas total block count, so
   a screen can assert its own icon budget.

   **The real successor to "turn the cliff into a build error" is not a
   static assert.** It is a host test that builds a screen's actual command
   list and asserts it fits `MU_COMMANDLIST_SIZE` - possible now that
   `microui.c` links into the host build, and better than anything per-icon
   because it measures the whole screen (panels, bezels, text and icons)
   rather than the icon share alone. Tracked separately; the brush screen
   sits at roughly 5.4 KiB of 8 KiB, currently visible only through a
   development-build log on a device someone is holding.

**Phase 4 is the real acceptance test of everything before it.** The four
sand icons are already pinned by structural assertions, so baking them must
produce output that passes `suite_sand_icons.c` unchanged. A generator that
cannot reproduce known-good artwork is not ready to be handed new artwork -
a check with teeth, unlike eyeballing a fresh bake and deciding it looks
fine.

## Considered and rejected

- **One PNG per icon, filename as the name.** No manifest to maintain, but a
  40-icon set becomes 40 files and you lose seeing the set together, which is
  what an atlas is for. Worth reconsidering if hand-editing grid coordinates
  turns out to be the annoying part in practice.
- **SVG sources needing a rasterizer.** Rejected, and then partly reinstated
  once the sources were actually read: a general SVG needs rasterizing, and
  these are pixel art, where the grid IS the design rather than a sampled
  approximation of a curve. But pixelarticons' SVGs contain only integer
  axis-aligned rectangles, so reading them is grid extraction, not
  rasterization - see phase 2. The rejection stands for SVG in general and
  falls for this specific, verified shape of file.

- **The defaulticon set** (16x16 monochrome PNGs, thin silhouettes).
  Rejected on two counts, either sufficient: its upstream
  (`defaulticon.com`) no longer resolves, so the licence cannot be verified
  for something checked in permanently; and the style is fine 1px-stroke
  silhouettes against our chunky 2px strokes, which reads as accidental when
  the two sit on one screen.
- **Runtime lookup by name string.** Costs a comparison per draw and throws
  away the enum's typo-catching. Names are for the manifest; ids are for the
  code.
- **Leaving icons app-private.** The check mark is already shared between the
  diagnostics app and the sand palette, and `gfx/icons.h` already names the
  next three that will want a home.
