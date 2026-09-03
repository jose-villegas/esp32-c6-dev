/*=============================================================================
 * app_sand - falling sand, poured with a finger and steered by tilting.
 *
 * Three pieces, each of which knows nothing about the others:
 *   sand.c  the automaton   - pure, host-tested, no idea a screen exists
 *   imu.c   the QMI8658     - raw counts, no idea what they will be used for
 *   here    the wiring      - grid size, colours, axis mapping, rendering
 *
 * WHY THE GRID IS COARSER THAN THE SCREEN
 *
 * A cell per pixel would be 368 x 448 = 165 KB of grid. After the framebuffer
 * takes 322 KB of the chip's ~424 KB there is nowhere near that left, so a
 * cell is a square block of `cell` x `cell` pixels, and `cell` is chosen from
 * the boot menu rather than fixed: ULTRA (2 px) gives a 184 x 224 grid, or
 * 41 KB; HIGH (3 px) gives 122 x 149, or 18 KB; NORMAL (4 px, the default)
 * gives 92 x 112, or 10 KB; LOW (6 px) gives 61 x 74, or about 4.5 KB;
 * VERY LOW (8 px) gives 46 x 56, or about 2.5 KB. All five still read as
 * grains rather than bricks - the choice trades fineness for the step budget
 * a finer grid costs, not for whether it looks right.
 *
 * Every allocation below is sized for the finest quality (2 px) regardless of
 * which one is active, so switching quality on the menu never reallocates
 * anything - it just changes how much of the same buffer is in use. That
 * matters for the same reason sand_exit() keeps the grid between visits: an
 * allocation that only ever happens once cannot fail because the heap
 * fragmented while something else was running.
 *
 * `cell` need not divide 368 or 448 evenly - grid_w/grid_h floor, so a
 * remainder just leaves an unredrawn margin at most cell-1 px wide along the
 * right and bottom edges, not an out-of-bounds write. At 2 px it divides
 * both evenly and there is no margin at all, but at 3 px it does not: 122 * 3
 * = 366 and 149 * 3 = 447, leaving a 2 px strip on the right and a 1 px strip
 * on the bottom that the grid never touches. At 6 px the margin is the
 * largest of any tier: 61 * 6 = 366 and 74 * 6 = 444, a 2 px strip on the
 * right and a 4 px strip on the bottom. That is harmless only because
 * the colour of an empty cell and the menu's background are the same value,
 * 0x0A0C14 - see COL_BACKGROUND - so the untouched strip is indistinguishable
 * from the screen around it. start_sim() still clears the screen explicitly
 * before the first frame rather than leaning on that coincidence alone.
 *===========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "../../app.h"
#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "../../input/imu.h"
#include "../../ui/ui.h"
#include "palette.h"
#include "row_runs.h"
#include "sand.h"
#include "sand_ui.h"
#include "tilt.h"
#include "util/intmath.h"   /* im_abs(), im_len() - see
                             * update_local_depth_gravity() below, which
                             * projects gravity's own direction into the
                             * vertical/horizontal weights LOCAL DEPTH's own
                             * comment describes */

static const char *TAG = "sand";

/* Same background as the launcher (see ui_launcher.c) and as an empty sand
 * cell (see material.c), so neither the menu-to-launcher nor the
 * menu-to-simulation transition has any visible seam. */
#define COL_BACKGROUND 0x0A0C14

/* Quality selects the simulation's cell size. Chosen on the boot menu and
 * held in `quality` below, which is deliberately file-scope and untouched by
 * sand_enter() so a choice made on one visit is still in effect the next -
 * see the comment on `quality` itself. */
typedef struct { const char *name; int cell; } quality_t;
static const quality_t qualities[] = {
    { "ULTRA",    2 },
    { "HIGH",     3 },
    { "NORMAL",   4 },
    { "LOW",      6 },
    { "VERY LOW", 8 },
};
#define QUALITY_COUNT ((int)(sizeof(qualities) / sizeof(qualities[0])))
#define QUALITY_DEFAULT 2        /* NORMAL */

/* Persists across app visits by design - only a device reboot resets this to
 * QUALITY_DEFAULT. Reset in sand_enter() would mean picking LOW, backing out
 * to the launcher, and coming straight back in throws the choice away. */
static int quality = QUALITY_DEFAULT;

/* The active grid shape, set from qualities[quality] in start_sim() and held
 * fixed for the rest of that run - changing quality mid-run would require
 * the grid to change shape under a live simulation, which nothing here
 * supports and the menu does not offer a way to trigger anyway. */
static int cell, grid_w, grid_h, block_cols, block_rows;

/* Every allocation is sized for CELL_MIN (the finest quality), never for
 * whichever quality happens to be active, so a quality switch is just a
 * change to cell/grid_w/grid_h/block_cols/block_rows and never touches the
 * heap - see the header comment above. */
#define CELL_MIN    2                          /* finest quality; sets every allocation size */
#define GRID_W_MAX  (GFX_WIDTH  / CELL_MIN)
#define GRID_H_MAX  (GFX_HEIGHT / CELL_MIN)

/* Duplicated rather than shared: sand.h has no business knowing the screen
 * size, so it cannot expose a ready-made block-count for this specific
 * grid - see sand_enable_sleeping()'s own comment. */
#define BLOCK_COLS_MAX ((GRID_W_MAX + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define BLOCK_ROWS_MAX ((GRID_H_MAX + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

/* Centered, absolutely-placed pair of buttons - see draw_menu(). UI_ROW_HEIGHT
 * is the shared row metric from ui.h, used here so the menu's buttons match
 * the height every other app UI's rows use.
 *
 * MENU_BTN_W is sized to the longest quality label plus margin: "QUALITY:
 * VERY LOW" is 17 characters, which at GFX_CHAR_W (16 px, see gfx.h) is
 * 272 px of text. mu_draw_control_text() in microui.c centers a button's
 * label and clips it to the button's own rect via mu_push_clip_rect() rather
 * than wrapping or shrinking it, so a label wider than its button is chopped
 * off at both ends with no warning and no crash. 300 leaves 34 px margins
 * either side of the panel (GFX_WIDTH 368) and 28 px of slack around the
 * longest label - if a future quality tier needs a longer label than this,
 * check its width against this number before assuming it will fit. */
#define MENU_BTN_W    300
#define MENU_BTN_H    UI_ROW_HEIGHT
#define MENU_BTN_GAP  20

/* How big a blob each touched frame drops, in pixels rather than cells - see
 * the comment above handle_pour_input()'s use of these for why. Large enough
 * that a tap is clearly a handful of sand rather than a speck. */
#define POUR_RADIUS_PX   10      /* was 5 cells at 2 px */

/* Pouring runs at a fixed rate too, for the same reason the simulation does.
 *
 * Spawning once per FRAME made the pour rate follow the framerate - and since
 * partial updates the framerate swings between 60 and over 200 depending on
 * how much is moving. Holding a finger down delivered three times as much sand
 * when the screen was quiet, and the sand arrived faster than the simulation
 * could move it, piling up under the finger. */
#define POUR_HZ       60
#define POUR_STEP_MS  (1000 / POUR_HZ)

/* The eraser is wider than the pour. Removing material is a corrective action
 * and wants to feel broad; pouring wants to feel placed. Pixels, not cells -
 * see POUR_RADIUS_PX above. */
#define ERASE_RADIUS_PX  16      /* was 8 cells at 2 px */

/* An emitter gets its OWN, more generous erase tolerance than material
 * does - two different radii for the same gesture, which looks like a
 * mistake until the target is considered rather than the gesture.
 *
 * Clearing material is an area operation: ERASE_RADIUS_PX sweeps a disc of
 * grains, and a small disc is precise in a useful way there - a wide one
 * would wipe out more of a scene than the finger meant to touch. An
 * emitter is not an area; it is a single point with no area of its own to
 * hit, so "precise" buys nothing and just makes the point easy to miss.
 * And in both cases the thing doing the aiming - a fingertip, on the order
 * of 90 px across - is far larger than either radius and completely covers
 * the target the whole time it is trying to hit it, so the eraser cannot
 * lean on the finger to narrow down where the point actually is the way it
 * could for a whole pile of sand. The point target needs tolerance for
 * that aim the area sweep does not.
 *
 * See handle_pour_input()'s erase branch below, which calls
 * sand_remove_emitters() at this radius in addition to sand_erase() at
 * ERASE_RADIUS_PX - not instead of it. sand_erase() already removes any
 * emitter within ITS OWN, smaller radius as part of turning material off
 * (see its own comment in sand.h), so this wider sweep simply subsumes
 * that guarantee rather than replacing it. */
#define ERASE_EMITTER_RADIUS_PX  32

/* Its own radius, not the eraser's borrowed one - a blast has to read as
 * bigger than a corrective tool, not the same size as one. 48 (three times
 * ERASE_RADIUS_PX) was an unmeasured starting point, picked only to look
 * obviously larger on screen than either existing brush.
 *
 * THE SHORT HISTORY, because this number has moved several times and
 * each move was a real, device-confirmed lesson: WAS 48, DOUBLED TO 96
 * on a device request for "a much bigger radius in general" - which
 * broke the feature outright on real hardware, twice, for two different
 * reasons a device flash caught each time (see SAND_IMPULSE_BUDGET_
 * BYTES's own comment for both). Neither failure was fixed by touching
 * this constant: the impulse buffer is a FIXED entry count (APP_IMPULSE_
 * MAX, decoupled from this radius entirely) chosen from the device's
 * real heap budget, and sand_explode() itself (sand.c) THINS its own
 * seeding density automatically whenever a disc's true cell count would
 * exceed whatever buffer it was actually given - see queue_outward_
 * impulse()'s own comment in sand.c for how. That made 96 px allocate
 * successfully - a real device confirmed it detonating without a crash -
 * but thinned to only ~28% of its own 7,213-cell disc against the
 * corrected 2,048-entry budget, and the user's own reaction to that
 * result on the actual board was "it's tiny but maybe that's as far we
 * can push it."
 *
 * IT WASN'T. Handed the actual measured tradeoff - 96 px thinned scores
 * 67.1 "grains outside the footprint" against build_sand_dune_scene(),
 * while a SMALLER radius that fits the same 2,048-entry budget at FULL
 * density (no thinning at all) scores 106-107 on the same metric, at the
 * cost of much less reach (2.3 vs 11.6 average max-throw) and less
 * destruction (48-79 vs 235) - the user chose the smaller, fully-seeded
 * blast: it reads as MORE powerful despite being physically smaller,
 * which is the whole reason "grains outside the footprint" was adopted
 * as this mechanic's own pass/fail criterion in the first place (see
 * that test's own comment in suite_sand.c - "the user's own criterion").
 *
 * 50 px (25 cells at CELL_MIN) IS THE ANSWER TO A SPECIFIC QUESTION, not
 * a round number: the largest radius whose exact_disc_count() (sand.c)
 * still fits inside APP_IMPULSE_MAX with ZERO thinning. Checked directly
 * rather than estimated - exact_disc_count(25) is 1,961, comfortably
 * under the 2,048-entry budget; exact_disc_count(26) is 2,121, already
 * over it. 25 cells is therefore the largest radius this budget can
 * still seed at full density, which is exactly what "small and dense"
 * means as a concrete number rather than a preference. Re-measured at
 * this exact radius and budget: 106.5 "grains outside the footprint"
 * against build_sand_dune_scene() (800-seed sweep, real sand_explode()),
 * landing right in the 106-107 range the estimate above predicted -
 * against 2.4 average max-throw and 78.7 average destroyed, both well
 * down from 96 px's 11.6 and 235.6, which is the reach and destruction
 * this choice deliberately gives up in exchange.
 *
 * DETONATE_RADIUS_PX is otherwise a free gameplay dial, same as it
 * always looked like one: raising it past 50 px re-engages thinning
 * (see the history above for what that costs), never whether it
 * allocates. Nothing below this line needs to move when it changes -
 * see APP_IMPULSE_MAX's own comment for why sizing is deliberately
 * independent of whatever this constant is set to. */
#define DETONATE_RADIUS_PX  50

/* A FIXED ENTRY COUNT, not a formula in DETONATE_RADIUS_PX - the single
 * most important change this constant went through. It used to be
 * `(355*r*r)/113 + 5*r + 3` at DETONATE_RADIUS_PX's own radius (in
 * cells), a hard upper bound on that radius's disc - see sand.c's
 * queue_outward_impulse() for where that formula and its pi-approximation
 * proof now live, since a disc's true size is what decides SEEDING
 * DENSITY at runtime today, not what decides this buffer's size at
 * compile time. Coupling the two was exactly the bug: a radius change
 * silently resized the allocation this constant makes, and nothing
 * checked whether the new size still fit the device until it didn't (see
 * DETONATE_RADIUS_PX's own comment for the incident).
 *
 * 2,048 is SAND_IMPULSE_BUDGET_BYTES / sizeof(impulse_t) exactly - 12,288
 * / 6 - chosen as a round entry count rather than left as a byte-only
 * figure so this constant reads the same way every other buffer's own
 * MAX in this file does (GRID_W_MAX, BLOCK_COLS_MAX, ...): a count of
 * things, with the byte cost one multiply away. See SAND_IMPULSE_BUDGET_
 * BYTES's own comment immediately below for the heap arithmetic this
 * count is answerable to, and the _Static_assert that keeps the two from
 * drifting apart if either is ever hand-edited on its own. */
#define APP_IMPULSE_MAX  2048

/* THE BUDGET APP_IMPULSE_MAX MUST NOT EXCEED - a HARDWARE decision, made
 * ONCE here, deliberately independent of DETONATE_RADIUS_PX or anything
 * else that might change for gameplay reasons. That independence is the
 * whole point: DETONATE_RADIUS_PX doubling once already sailed straight
 * past what the device can actually spare, and nothing caught it until a
 * device flash reported detonate as a total no-op - not weaker, not
 * shorter-ranged, NOTHING, because sand_explode()'s first line is
 * `if (s->impulse_buf == NULL) return;` and a failed malloc hits that
 * silently, with only an ESP_LOGE (see below, where impulse_buf is
 * allocated) that nobody was watching for. A radius-independent budget
 * means that specific failure mode cannot recur no matter how large a
 * future radius request gets - see DETONATE_RADIUS_PX's own comment for
 * how sand_explode() now spends whatever this budget affords instead of
 * demanding more of it.
 *
 * THIS BUDGET WAS WRONG ONCE ALREADY, AT 24,576 BYTES, AND A DEVICE
 * FLASH IS WHAT CAUGHT IT - not host arithmetic, which had already
 * signed off on that number and was still wrong. The mistake was sizing
 * against a real boot log's TOTAL FREE HEAP (76,068 bytes, everything
 * this app's own fixed buffers - dirty_rows/sleep_blocks/grid/row_run_*,
 * ~43,480 bytes together, none of them scaling with the blast radius -
 * subtracted from it). Total free heap is the wrong number for a SINGLE
 * malloc() call to be judged against: what a single allocation actually
 * needs is one contiguous run at least that large, and a heap can have
 * plenty of total free bytes while its largest unbroken run is much
 * smaller than their sum. That is exactly what a live serial capture at
 * 82851a9 found: `impulse_buf`'s malloc failing on THREE separate
 * detonate attempts, at THREE different quality settings (grid sizes
 * 18,178 / 10,304 / 4,514 bytes), with `heap_caps_get_largest_free_
 * block()` reporting an IDENTICAL 14,592 bytes every single time -
 * unmoved by a grid allocation that itself varied by nearly 4x across
 * those three runs. A number that does not move with the one thing in
 * this app that changes size is not describing this app's own
 * allocations at all; it is describing something upstream of them (heap
 * layout left behind by whatever ran before this app, most likely -
 * see start_sim()'s own comment on moving this allocation first, which
 * was the other half of this same fix) that 76,068 bytes of TOTAL free
 * heap never had any way to reveal.
 *
 * THE BUDGET IS NOW SET AGAINST THAT OBSERVED NUMBER, NOT TOTAL FREE
 * HEAP: 12 KB (12,288 bytes) against a measured 14,592-byte largest
 * block leaves 2,304 bytes (about 16%) of margin for allocator overhead
 * and whatever this specific board's fragmentation looks like on a run
 * that was not captured. That margin is deliberately real but not huge -
 * three identical captures in a row is a strong signal this number is a
 * structural property of this board's heap layout, not noise that might
 * land anywhere on the next boot, so a small margin is buying protection
 * against overhead and rounding, not against this number moving on its
 * own. STILL NOT A NUMBER THIS PROJECT HAS BISECTED TO ITS OWN FAILURE
 * THRESHOLD - it is one considered step below the one real data point
 * available, and the honest thing to say about it is exactly that: see
 * docs/Sand/Explosion-Plan.md's "Two failure modes to watch for by name"
 * for both incidents this constant has now been through and what each
 * one got wrong.
 *
 * WHY A FIXED BYTE BUDGET RATHER THAN A RADIUS CAP: a radius cap has to
 * be re-derived by hand every time either the radius or impulse_t's own
 * size changes (see impulse_t's comment in sand.h - it has already grown
 * once, from 4 bytes to 6), and a hand re-derivation is exactly the step
 * that got skipped the one time this mattered. A fixed byte budget needs
 * re-deriving only when the HARDWARE changes - a new board, more PSRAM,
 * a leaner framebuffer, or (as just happened) a better understanding of
 * what this same board's heap was already doing - and it never needs
 * touching just because a gameplay radius moved. See the _Static_assert
 * immediately below for the guard this buys: it now confirms two
 * independent, hand-chosen constants agree with each other, rather than
 * re-deriving one from a radius that might have drifted - and see that
 * assert's own message for what it CANNOT check, which is whether this
 * number is actually right on real hardware. Nothing at compile time
 * can check that; only a device flash can, which is exactly how the
 * 24,576-byte version of this constant was caught. */
#define SAND_IMPULSE_BUDGET_BYTES  12288

/* CONFIRMS TWO HAND-CHOSEN CONSTANTS AGREE WITH EACH OTHER - NOTHING
 * MORE. This assert cannot know, and does not claim to know, whether
 * SAND_IMPULSE_BUDGET_BYTES itself is actually safe on real hardware -
 * that is a fact about this board's live heap layout, discovered once
 * already by a device flash after host arithmetic said everything was
 * fine, and no compile-time check can substitute for the next one. What
 * this assert catches is the OTHER way these two constants can drift:
 * someone raising APP_IMPULSE_MAX (for a denser blast, say) without
 * checking it against the budget at all. Necessary, not sufficient - see
 * SAND_IMPULSE_BUDGET_BYTES's own comment for the failure mode this
 * assert is structurally unable to catch. */
_Static_assert(
    (unsigned long)APP_IMPULSE_MAX * sizeof(impulse_t) <= SAND_IMPULSE_BUDGET_BYTES,
    "APP_IMPULSE_MAX * sizeof(impulse_t) exceeds SAND_IMPULSE_BUDGET_BYTES - "
    "these are two independently-chosen constants that must agree. This "
    "assert passing is NOT proof detonate works on real hardware - this "
    "exact budget already failed a live device flash once at a larger "
    "value (24,576 bytes) that this same assert also happily passed, "
    "because the real failure was the budget being sized against total "
    "free heap instead of the largest contiguous block a single malloc() "
    "call actually needs - see SAND_IMPULSE_BUDGET_BYTES's own comment "
    "for that incident. Shrink APP_IMPULSE_MAX, or raise "
    "SAND_IMPULSE_BUDGET_BYTES only after a fresh device capture of "
    "heap_caps_get_largest_free_block() at the point impulse_buf is "
    "allocated - never from arithmetic alone.");

/* What the finger puts down.
 *
 * Selected from the palette panel (BOOT's release edge opens it - see
 * sand_ui.c's open_palette(), and sand_frame()'s own comment on why that is
 * the release and not the press; a tap on a tile selects it - see
 * draw_palette()'s own mu_button() loop and sand_ui.c's
 * sand_ui_tile_clicked()) rather than cycled
 * one button press at a time. Cycling was the palette's stand-in before the
 * panel existed, and it aged badly for the obvious reason: reaching the Nth
 * material cost N presses, and every material added since - eight of them -
 * pushed everything after it that much further away. The panel costs one
 * press to open and one tap to choose, whatever this list grows to.
 *
 * PWR still cycles PAINT/ERASE/DETONATE directly, unchanged from before the
 * panel existed - see sand_ui.c's handle_brush_input() and sand_mode_t's
 * own comment in sand_ui.h for why DETONATE rides along on this cycle
 * rather than living in the palette. A plain press is the cheaper action
 * for all three: a HOLD costs BUTTON_HOLD_US (600 ms) of waiting before it
 * even registers, every single time, and paying that tax on a control used
 * this often would make erasing feel sluggish next to the immediacy
 * pouring already has. A dedicated button's press has none of that cost.
 *
 * MAT_WOOD but not MAT_STEAM: every entry here costs a tile in the palette
 * panel - see BRUSH_COUNT and the _Static_assert on PALETTE_FITS below - so
 * only materials someone actually paints belong. Wood is something you
 * build a fire out of; steam is a byproduct you watch happen. Burning wood
 * is not listed either, because it is a STATE of wood rather than a
 * material - see reaction_t.burn_decay, and docs/Sand/Adding-a-Material.md
 * for this as a worked example. */
/* Whole CELLS rather than material ids, because an extended material
 * cannot be named by an id - its low nibble is its identity (MATX() in
 * material.h). An ordinary material is written CELL_MAKE(id, 0) and its
 * variant is chosen the usual way when it is painted. */
static const cell_t brushes[] = {
    CELL_MAKE(MAT_SAND, 0),  CELL_MAKE(MAT_WATER, 0),
    CELL_MAKE(MAT_STONE, 0), CELL_MAKE(MAT_GAS, 0),
    CELL_MAKE(MAT_FIRE, 0),  CELL_MAKE(MAT_WOOD, 0),
    CELL_MAKE(MAT_OIL, 0),   CELL_MAKE(MAT_LAVA, 0),
    CELL_MAKE(MAT_ACID, 0),  CELL_MAKE(MAT_GLASS, 0),
    CELL_MAKE(MAT_SNOW, 0),  CELL_MAKE(MAT_DIRT, 0),
    MATX(MATX_ICE),      MATX(MATX_PLANT),
};
#define BRUSH_COUNT ((int)(sizeof(brushes) / sizeof(brushes[0])))

/* The palette panel's grid is derived from BRUSH_COUNT, never hand-synced to
 * it - see palette.h. This is what makes adding a brush that pushes the
 * panel past the bottom of the screen (at either real orientation - see
 * PALETTE_FITS's own comment) a BUILD failure, rather than a silently
 * clipped row discovered by looking at the device. */
_Static_assert(PALETTE_FITS(BRUSH_COUNT),
               "the palette panel for BRUSH_COUNT brushes is taller than the "
               "screen at some orientation - see palette_cols()/PALETTE_TILE "
               "in palette.h");

/* Whether a brush places a persistent source ("a tap") instead of pouring -
 * toggled by tapping the already-selected tile in the palette (see
 * sand_ui.c's sand_ui_tile_clicked()) and read by handle_pour_input()
 * below. brush_mode_t itself now lives in sand_ui.h, alongside the state
 * machine that reads and writes this table.
 *
 * File-scope and deliberately NOT reset in sand_enter() or start_sim() - the
 * same treatment `quality` gets above, and for the same reason: this is a
 * deliberate setting the player made, not run state, so leaving the app (or
 * restarting the simulation) must not throw it away.
 *
 * That does leave an asymmetry worth knowing about. sand_init() clears
 * every REAL emitter on each start_sim(), but nothing clears brush_mode
 * alongside it - so after a restart, Water may still be flagged a source
 * while no tap exists anywhere on the fresh board. That is correct: the
 * flag says what a tap would place if the material were painted again, not
 * that one is currently placed. */
static uint8_t brush_mode[BRUSH_COUNT];   /* brush_mode_t per brush */

/* The UI state machine's own state: which screen is showing, which brush is
 * selected, whether the eraser is armed, and the palette's own bookkeeping
 * (the swallow guard, and what brush/mode the panel opened with) - see
 * sand_ui.h. Pointed at this file's own brushes[]/brush_mode[] tables
 * rather than owning copies of them, the same way sand_t borrows `cells`
 * instead of allocating its own grid.
 *
 * `ui.screen`, `ui.brush` and `ui.mode` replace the old file-scope
 * `screen`, `brush` and `mode` statics - one definition rather than three,
 * now that the state machine that reads and writes them lives in
 * sand_ui.c. Zero-initialised the same way those statics were: `ui.screen`
 * starts at SAND_UI_MENU (0), `ui.brush` at 0, `ui.mode` at SAND_MODE_PAINT
 * (0) - matching sand_enter()'s and start_sim()'s own resets below. */
static sand_ui_t ui = {
    .brushes     = brushes,
    .modes       = brush_mode,
    .brush_count = BRUSH_COUNT,
};

/* How long the mode label stays up after a change worth confirming - a PWR
 * press toggling erase, or the palette closing having actually changed the
 * brush or its mode (see sand_ui.h's opened_brush/opened_mode and sand_ui.c's
 * close_palette()). Long enough to read without hurrying, short enough not
 * to sit over the sand. */
#define LABEL_MS 1800

#define LABEL_MARGIN 18
#define LABEL_SCALE   2

/* Below this the board is being held still enough that the reading is noise,
 * and letting noise through means friction is quietly unlocked the whole time
 * the app is open. */
#define SHAKE_DEADZONE 40

/* The simulation runs at a FIXED rate, independent of the framerate.
 *
 * A grain moves one cell per step, so steps-per-second is literally how fast
 * sand falls. Stepping once per frame tied that to the framerate - which was
 * survivable while the framerate was flat, and stopped being so the moment
 * partial presents made it swing between 60 and 230 fps depending on how much
 * was moving. Sand would have fallen fastest when least was happening.
 *
 * 60 Hz is around the fastest that still reads as grains rather than streaks. */
#define SIM_HZ            60
#define SIM_STEP_MS       (1000 / SIM_HZ)

/* Never run more than this many steps to catch up after a stall.
 *
 * Two jobs. It stops the classic spiral, where a long frame schedules extra
 * steps that make the next frame longer still. And it caps how far a grain can
 * travel between two things the eye sees: a grain moves one cell per step, so
 * this IS the speed limit, and four cells in a frame is enough to read as a
 * jump rather than as movement. Better to let the simulation lose a little
 * time than to teleport the sand. */
#define SIM_MAX_CATCHUP   2

static uint8_t    *grid;
static uint8_t    *dirty_rows;   /* GRID_H_MAX bytes: which rows changed -
                                   * only the first grid_h are in use at any
                                   * quality below ULTRA */
static uint8_t    *sleep_blocks; /* BLOCK_COLS_MAX*BLOCK_ROWS_MAX bytes:
                                   * settled blocks to skip - see
                                   * sand_enable_sleeping() */
static impulse_t  *impulse_buf;  /* APP_IMPULSE_MAX entries: grains in
                                   * flight from DETONATE - see
                                   * sand_enable_impulses(). Scaffolding,
                                   * like sand_mode_t itself. */

/* Up to ROW_MAX_RUNS (row_runs.h) separate cell-index ranges per row - not
 * pixel ranges, and not a single min/max span - recording where a row's
 * material sat the last time it was drawn, so a run whose content just
 * vanished still sends far enough to clear its old pixels, and two
 * genuinely separate blobs in one row keep being sent separately instead
 * of one box spanning the gap between them. See row_runs.h. GRID_H_MAX *
 * ROW_MAX_RUNS entries each - only the first grid_h rows are in use at any
 * quality below ULTRA; row_run_n[cy] says how many of a row's ROW_MAX_RUNS
 * slots are actually in use. */
static uint16_t   *row_run_x0;
static uint16_t   *row_run_x1;
static uint8_t    *row_run_n;
static sand_t      sim;
static tilt_t      tilt;
static bool        failed;
static uint32_t    label_left_ms;    /* countdown for the mode label */

/* False from the moment start_sim() runs until the finger that pressed
 * START actually lifts. Without this, the same touch that tapped START is
 * still `input->down` on the first RUNNING frame, and handle_pour_input()
 * cannot tell that touch apart from a deliberate pour - it would drop a
 * blob of sand under wherever START happened to be the instant the
 * simulation starts. The same shape as sand_ui_t's own `swallow_release`
 * guard (see sand_ui.h) for the identical reason: wait for a release, not a
 * fixed delay, since a delay would either cut off a genuinely fast tap or
 * still fire early under a slow one. */
static bool        input_ready;

/* Which shell quarter the palette panel was last actually painted at - see
 * sand_frame()'s SAND_UI_PALETTE handling, which repaints the sand
 * underneath before redrawing the panel whenever display_shell_quarter()
 * has moved on since this was set. Meaningless while the panel is closed;
 * set fresh every time SAND_UI_OPEN_PALETTE fires, never read before then. */
static int         palette_drawn_quarter;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Rolling averages, purely for the log line - a release build has nobody
 * watching the serial console to read them, so it carries none of this. */
static uint32_t frames;
static int64_t  step_us_total;
static int64_t  draw_us_total;
static int64_t  rows_redrawn_total;
static int64_t  steps_total;

/* TEMPORARY: splits the same step/draw timing by whether a pour was
 * actually happening this frame, printed periodically rather than only on
 * exit - checking a specific claim (pouring costs more than a full board
 * moving under tilt alone) that the whole-session average above cannot
 * distinguish, since a real test session mixes both. Remove once answered
 * either way. */
static int64_t  pour_step_us_total, pour_draw_us_total;
static uint32_t pour_frames;
static int64_t  idle_step_us_total, idle_draw_us_total;
static uint32_t idle_frames;
static int64_t  split_log_at_us;

/* TEMPORARY, alongside the above: how many of block_cols*block_rows blocks
 * are actually awake (!sand_block_settled(), about to be examined at full
 * cost) right after a step - originally how many of GRID_H ROWS, back when
 * sleeping was row-shaped: that first round of capture showed the awake-row
 * count staying flat (or falling) while step cost kept climbing anyway,
 * which is what motivated replacing row-shaped sleeping with the
 * block-shaped scheme in sand.c - see sand_enable_sleeping(). Kept at block
 * granularity now to confirm that fix actually holds under the same test
 * (pour into a growing pile). */
static int64_t  pour_awake_total, idle_awake_total;

/* TEMPORARY, alongside the above: how many occupied cells sit inside those
 * awake blocks. The row-shaped measurement above is what pointed at the
 * real mechanism: step_one_row() walks every occupied cell in a row (now
 * block) it doesn't skip outright, not a fixed cost per awake unit, so a
 * wider pile made each awake row more expensive without needing more of
 * them to be awake - this is the number that confirmed it then, and checks
 * the block-shaped fix now. */
static int64_t  pour_awake_cells_total, idle_awake_cells_total;
#endif
/* Accumulated simulation time, in milliseconds scaled by 256. Scaled because
 * the flow rate is a fraction and a whole millisecond is too coarse a unit to
 * carry it - rounding to whole ms would make a slow flow stutter or stop. */
static uint32_t sim_accumulator_q8;
static uint32_t pour_accumulator_ms;

/*---------------------------------------------------------------------------
 * Sensor axes to screen axes
 *
 * The QMI8658 is soldered in some fixed orientation relative to the panel, and
 * nothing in the datasheet can tell us which - it is a board layout fact. These
 * two macros are the entire mapping, so correcting it is a one-line change.
 *
 * Determined by experiment, not from the datasheet, which describes the chip
 * and not how it was soldered down. Held upright the sensor reads about +1 g
 * on its X axis and roughly zero on Y, so the chip's X axis is the one running
 * down the screen - which is why the obvious guess (X to X, Y to Y) sent the
 * sand sideways.
 *
 * The Y axis then runs across the screen, but pointing left, hence the
 * negation. Both facts came from tilting the board and watching which way the
 * sand went; there is no way to derive them.
 *-------------------------------------------------------------------------*/
#define GRAVITY_SCREEN_X(s)  (-(s)->ay)
#define GRAVITY_SCREEN_Y(s)  ( (s)->ax)

/*---------------------------------------------------------------------------
 * Setup
 *-------------------------------------------------------------------------*/

static void sand_enter(void)
{
#if CONFIG_LAUNCHER_DEVELOPMENT
    frames = 0;
    step_us_total = 0;
    draw_us_total = 0;
    rows_redrawn_total = 0;
    steps_total = 0;
    pour_step_us_total = 0;
    pour_draw_us_total = 0;
    pour_frames = 0;
    idle_step_us_total = 0;
    idle_draw_us_total = 0;
    idle_frames = 0;
    pour_awake_total = 0;
    idle_awake_total = 0;
    pour_awake_cells_total = 0;
    idle_awake_cells_total = 0;
    split_log_at_us = esp_timer_get_time() + 2000000;
#endif
    /* Nothing else: no allocation, no sand_init(), no imu_init(), no starting
     * heap. Those only happen once START is pressed - see start_sim() - so
     * opening the app costs nothing beyond drawing the menu. */
    ui.screen = SAND_UI_MENU;

    /* The launcher's own output is still sitting in the framebuffer, and
     * ui_end()'s repaint-skip logic only knows about changes to the UI
     * command list, not about the screen having been replaced out from
     * under it - see ui_invalidate()'s own comment. Without this the menu
     * would compare equal to the launcher's last frame here and never
     * actually repaint. */
    ui_invalidate();
}

/* Seeds every row's run-tracking as one full-width span, as if the whole row
 * were occupied - see start_sim()'s call site for why "previous" has to
 * start out lying like that. Shared with sand_frame()'s SAND_UI_CLOSE_PALETTE
 * handling, which needs exactly the same lie for exactly the same reason:
 * whatever the panel just left in the framebuffer has to be forced out in
 * full on the first frame after it closes, not trusted to the sand's own
 * (much narrower) real extent. */
static void seed_row_runs_full_width(void)
{
    for (int i = 0; i < grid_h; i++) {
        row_run_x0[i * ROW_MAX_RUNS] = 0;
        row_run_x1[i * ROW_MAX_RUNS] = (uint16_t)grid_w;
        row_run_n[i] = 1;
    }
}

/* Marks the whole sand canvas for a full repaint on the next
 * draw_dirty_rows() call: every row's run-tracking reseeded as one
 * full-width span (seed_row_runs_full_width() just above), every row
 * flagged dirty, and the whole screen marked dirty in gfx so the panel's
 * send is not narrowed by whatever the sand itself would otherwise have
 * decided was worth sending.
 *
 * The shared answer to "something that was covering part of the sand just
 * moved or vanished, and what it stops covering has to come back":
 * SAND_UI_CLOSE_PALETTE below uses this when the panel closes, and the
 * SAND_UI_PALETTE handling uses it when the panel's own footprint moves
 * under a shell orientation change while it is still open - two different
 * triggers for the identical underlying fact, kept as one function so they
 * cannot drift into two slightly different reseeds of the same three
 * things.
 *
 * Only marks; does not call draw_dirty_rows() itself. Callers differ on
 * whether the repaint has to land THIS frame - the palette-open case still
 * has a panel to draw on top afterwards, so it calls draw_dirty_rows() right
 * after this - or can simply wait for the next ordinary frame to pick the
 * flags up on its own, which is what the close case below does. */
static void mark_sand_fully_dirty(void)
{
    seed_row_runs_full_width();
    memset(dirty_rows, 1, (size_t)grid_h);
    gfx_mark_all_dirty();
}

#if CONFIG_LAUNCHER_SELFTEST
/* Can a fresh entry into this app still get everything it needs, right now?
 *
 * Every frame-budget row in suite_sand.c measures sand_step() on a grid the
 * TEST allocated. Nothing asserted that the app's own allocation still
 * succeeds - so the campaign could have been measuring a simulation this
 * device could no longer enter, and the first sign of it was a "no memory
 * for the grid" screen someone happened to notice after a capture. This is
 * the missing check, and it lives HERE rather than in the suite so that it
 * uses these constants: a copy of the sizes in the test file would drift
 * the first time one of them changed.
 *
 * Allocates its own set rather than inspecting the app's, in the same order
 * the app uses - that order is load-bearing, see impulse_buf's own comment
 * in start_sim() below - and frees all of it before returning, so asking
 * the question cannot be what makes the answer no.
 *
 * impulse_buf is reported separately because the app treats it as optional:
 * losing it disables the blast mechanic, not the app. */
bool sand_app_alloc_selfcheck(size_t *out_largest_free, bool *out_impulses_ok)
{
    uint8_t   *t_dirty  = malloc(GRID_H_MAX);
    uint8_t   *t_blocks = malloc((size_t)BLOCK_COLS_MAX * BLOCK_ROWS_MAX);
    uint8_t   *t_grid   = malloc((size_t)GRID_W_MAX * GRID_H_MAX);
    uint16_t  *t_x0     = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t  *t_x1     = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t   *t_n      = malloc(GRID_H_MAX * sizeof(uint8_t));
    impulse_t *t_imp    = malloc((size_t)APP_IMPULSE_MAX * sizeof(impulse_t));

    const bool essential_ok = (t_dirty && t_blocks && t_grid &&
                               t_x0 && t_x1 && t_n);
    if (out_impulses_ok) {
        *out_impulses_ok = (t_imp != NULL);
    }
    if (out_largest_free) {
        *out_largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }

    free(t_imp);
    free(t_n);
    free(t_x1);
    free(t_x0);
    free(t_grid);
    free(t_blocks);
    free(t_dirty);
    return essential_ok;
}
#endif /* CONFIG_LAUNCHER_SELFTEST */

/* Builds the active grid at the chosen quality, then does everything
 * sand_enter() used to do unconditionally: allocate (only on the first-ever
 * call - see the header comment on why every allocation is sized for
 * CELL_MIN), seed row_runs, start the simulation and the IMU, and drop the
 * starting heap of sand. Called when START is pressed on the menu. */
static void start_sim(void)
{
    cell       = qualities[quality].cell;
    grid_w     = GFX_WIDTH  / cell;
    grid_h     = GFX_HEIGHT / cell;
    block_cols = (grid_w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    block_rows = (grid_h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;

    sim_accumulator_q8 = 0;
    pour_accumulator_ms = 0;
    ui.brush = 0;
    ui.mode = SAND_MODE_PAINT;
    label_left_ms = 0;
    failed = false;
    input_ready = false;

    if (dirty_rows == NULL) {
        dirty_rows = malloc(GRID_H_MAX);
    }
    if (sleep_blocks == NULL) {
        sleep_blocks = malloc((size_t)BLOCK_COLS_MAX * BLOCK_ROWS_MAX);
    }
    if (grid == NULL) {
        grid = malloc((size_t)GRID_W_MAX * GRID_H_MAX);
    }
    /* impulse_buf GOES LAST, AFTER grid, DELIBERATELY - TRIED GOING FIRST
     * INSTEAD AND A DEVICE FLASH MADE IT WORSE. A live serial capture
     * that showed impulse_buf's malloc failing with "largest free block
     * is 14592" at three different quality settings was misread once as
     * "impulse_buf never gets a fair shot because grid/dirty_rows/
     * sleep_blocks fragment the heap ahead of it" - grid's own request
     * (GRID_W_MAX * GRID_H_MAX) is actually a FIXED 41,216 bytes
     * regardless of quality (the varying numbers in that capture were
     * the ACTIVE grid_w*grid_h subset in use, not the allocation size),
     * and it succeeded cleanly in all three captures. So the real
     * picture those three captures agree on is: this heap reliably has
     * one contiguous run big enough for grid's 41,216 bytes, and roughly
     * 14,592 bytes left over after grid and the small buffers land -
     * which is a single largest-block ordering fact, not a fragmentation
     * problem this app's own allocation order was causing.
     *
     * Moving impulse_buf's smaller (12,288-byte, see APP_IMPULSE_MAX)
     * request to go FIRST was tried anyway, on the chance that ordering
     * still mattered - and a device flash of that build produced "no
     * memory for the grid" instead, a WORSE failure than impulse_buf
     * alone failing: impulse_buf grabbed a piece of the one heap region
     * big enough for it, and grid's subsequent 41,216-byte request then
     * found nowhere left to land, tripping the mandatory-buffer fallback
     * below (`ui.screen = SAND_UI_RUNNING` with the grid's own "could not
     * allocate" message). Reordering does not create more contiguous
     * space anywhere in the heap - it only decides who gets first pick of
     * what already exists - and on THIS device grid is the one allocation
     * that needs the single largest contiguous run, so it has to be the
     * one that picks first. This ordering (grid and the other mandatory
     * buffers before impulse_buf) is the one three real device captures
     * confirm actually works; do not move impulse_buf ahead of grid
     * again without a fresh device capture that shows it helping,
     * because the only capture that ever tried has already shown it
     * hurting. */
    if (impulse_buf == NULL) {
        impulse_buf = malloc((size_t)APP_IMPULSE_MAX * sizeof(*impulse_buf));
        /* LOUD, BUT NOT FATAL - unlike every buffer in the big OR-check
         * below. Those are all load-bearing for the simulation existing at
         * all; this one is not - sand_enable_impulses(NULL, ...) is a
         * documented, safe way to disable just the blast mechanic (see its
         * own comment in sand.h), and sand_explode() already no-ops
         * gracefully without it. Refusing to run the WHOLE app because the
         * one buffer detonate needs could not be found would strand a
         * player who only ever wanted to pour sand behind a "no memory"
         * screen for a feature they were not using. What must not happen
         * instead is silence: this exact allocation has already failed
         * silently on real hardware twice, at two different budgets this
         * app shipped believing were safe - see SAND_IMPULSE_BUDGET_
         * BYTES's own comment for both incidents and what each got wrong.
         *
         * THE %u THIS LOGS - largest_free_block, from heap_caps_get_
         * largest_free_block() - IS THE DIAGNOSTIC THAT SOLVED THIS. Not
         * a guess added for completeness: a live serial capture of this
         * exact line, at three different grid sizes, is what showed the
         * largest free block sitting at an identical 14,592 bytes
         * regardless of quality - the observation that turned "why did
         * this fail" into "the buffer was never sized against the right
         * number" (total free heap, not the actual largest contiguous
         * run malloc() has to satisfy from). Keep this argument in every
         * failure log this file ever adds for an allocation that matters -
         * total free bytes told a story that was flatly wrong twice
         * running; the largest block told the truth in one capture. */
        if (impulse_buf == NULL) {
            ESP_LOGE(TAG, "Could not allocate the %d-entry blast buffer "
                          "(%u bytes) - detonate will be a no-op this "
                          "session; largest free block is %u",
                     APP_IMPULSE_MAX,
                     (unsigned)((size_t)APP_IMPULSE_MAX * sizeof(*impulse_buf)),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
    if (row_run_x0 == NULL) {
        row_run_x0 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(*row_run_x0));
    }
    if (row_run_x1 == NULL) {
        row_run_x1 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(*row_run_x1));
    }
    if (row_run_n == NULL) {
        row_run_n = malloc(GRID_H_MAX * sizeof(*row_run_n));
    }
    /* impulse_buf is deliberately NOT in this list - see the loud-but-not-
     * fatal log right where it is allocated, above, for why a failure
     * there disables one optional mechanic rather than the whole app. */
    if (grid == NULL || dirty_rows == NULL || sleep_blocks == NULL ||
        row_run_x0 == NULL || row_run_x1 == NULL || row_run_n == NULL) {
        ESP_LOGE(TAG, "Could not allocate a %d x %d grid (%d bytes); "
                      "largest free block is %u",
                 GRID_W_MAX, GRID_H_MAX, GRID_W_MAX * GRID_H_MAX,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        failed = true;
        /* The "no memory for the grid" message lives on the RUNNING path in
         * sand_frame() - leaving screen at SAND_UI_MENU here would strand
         * the user on a menu whose only button silently re-fails the same
         * allocation forever, with no on-screen sign anything is wrong. */
        ui.screen = SAND_UI_RUNNING;
        return;
    }

    /* Full width, not empty: nothing clears the framebuffer on entry, so
     * whatever the launcher (or a previous app) left behind is still
     * sitting in it. Seeding "previous" as one run spanning the whole row
     * forces the first dirty pass over each row to send full width
     * regardless of how little of it the fresh grid actually occupies -
     * the same guarantee the old unconditional-full-width send gave for
     * free. Only once a row has genuinely been redrawn does its real,
     * narrower extent become trusted enough to send instead. */
    seed_row_runs_full_width();

    sand_init(&sim, grid, grid_w, grid_h, (uint32_t)esp_timer_get_time());
    /* Falling material looks like a rigid block without this: everything in
     * open air takes the same move on the same step, so a poured blob keeps
     * its shape all the way down. Per-material, because water and sand do not
     * disperse alike. */
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    /* Per-material, same reasoning as scatter above - only a transient
     * material (currently just gas) has a nonzero figure in the table, so
     * this is a no-op for sand, water and stone. */
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    /* Per-material, same reasoning as decay above - only acid has a
     * nonzero figure in the table, so this is a no-op for everything
     * else. */
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    /* Per-material too - only gas has a figure below full speed, so this
     * is a no-op for sand, water and stone. */
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);

    sand_track_dirty_rows(&sim, dirty_rows);

    /* Without this, a screen full of motionless sand is the most expensive
     * thing the simulation can hold rather than the least - every settled
     * grain runs the whole decision path each step to conclude nothing. */
    sand_enable_sleeping(&sim, sleep_blocks);

    /* DETONATE scaffolding - see sand_mode_t's own comment in sand_ui.h.
     * Enabled unconditionally rather than only once the mode is first
     * cycled to, so an allocation failure is caught here alongside every
     * other one above instead of surfacing later as a silent no-op the
     * first time someone actually cycles PWR round to it. */
    sand_enable_impulses(&sim, impulse_buf, APP_IMPULSE_MAX);
    tilt_reset(&tilt, IMU_COUNTS_PER_G);

    if (!imu_init()) {
        /* Not fatal. Without a sensor the gravity vector is a constant, so the
         * app degrades to plain downward sand rather than refusing to run. */
        ESP_LOGW(TAG, "No IMU - falling back to fixed downward gravity");
    }

    ESP_LOGI(TAG, "%d x %d grid, %d bytes, %d px cells",
             grid_w, grid_h, grid_w * grid_h, cell);

    /* Clears the menu's pixels out of the framebuffer, including the strip
     * at cell != 2 that the grid itself never redraws - see the header
     * comment - and forces the whole screen out to the panel so the first
     * frame of the simulation has no menu pixels left in it anywhere. */
    gfx_clear(material_palette()[SAND_EMPTY]);
    gfx_mark_all_dirty();

    ui.screen = SAND_UI_RUNNING;
}

static void sand_exit(void)
{
    /* The grid is kept between visits: it is the largest allocation the app
     * makes, and holding it means re-entry cannot fail because the heap
     * fragmented while something else was running. */
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (frames > 0) {
        ESP_LOGI(TAG, "%lu frames, %lld sim steps, step %lld us, draw %lld us, "
                      "%lld of %d rows redrawn per frame",
                 (unsigned long)frames, (long long)steps_total,
                 (long long)(step_us_total / frames),
                 (long long)(draw_us_total / frames),
                 (long long)(rows_redrawn_total / frames), grid_h);
    }
#endif
}

/*---------------------------------------------------------------------------
 * Drawing
 *-------------------------------------------------------------------------*/

/* Writes every cell of a row, empty ones included, so no separate clear is
 * needed - the background is simply the colour of an empty cell.
 *
 * Only rows the simulation reported as changed are touched, and each one tells
 * gfx which band it landed in. A settled pile therefore costs almost nothing
 * to draw AND almost nothing to send, which is where the real saving is: a
 * whole frame is 9.6 ms of bus time and drawing is a fraction of that. */
/* A band of light travelling across anything hatched.
 *
 * An early version of this aligned the shine to the board's tilt by
 * picking WHICH of two fixed diagonals it travelled along. That was a
 * nicer idea and it never became visible: the direction was right, the
 * repaint was right by the end, and three rounds of looking at it on the
 * device still could not see it. Two diagonals of single pixels differing
 * only in WHICH way they lean is simply not a difference the eye picks up
 * on a 184x224 grid, however correct the arithmetic underneath.
 *
 * Movement is a difference the eye cannot miss, which is the whole reason
 * the band exists at all - it sweeps, so the glass is doing something,
 * whatever else changes about it.
 *
 * GRAVITY IS BACK, but as a continuous angle rather than a choice of two.
 * shine_ux_q8/shine_uy_q8 (below) are a Q8 unit vector of the current
 * gravity direction - see material_shine_direction() in material.h - and
 * paint_row_n() projects each pixel onto it instead of onto the fixed
 * (1, 1) diagonal the band used before. Tilting the board now visibly
 * ROTATES which way the band runs, not merely which of two ways it leans,
 * which is a different and considerably less subtle claim than the one
 * that failed to show up before - but it is still a claim about a 184x224
 * grid that has not yet been confirmed on the device, and the same
 * "picked the wrong difference to make visible" failure mode applies
 * until it has.
 *
 * SHINE_PERIOD is the distance between bands along that direction, and the
 * band moves SHINE_STEP_PX every SHINE_STEP_MS - about 1.3 seconds for one
 * band to reach where the one before it started.
 *
 * ITS WIDTH IS ONE CELL, not a number of pixels, which is why no constant
 * for it appears here. Measured in pixels it was two cells thick at the
 * finest quality and two thirds of one at the coarsest, so the same glass
 * looked like a different material depending on a setting that has nothing
 * to do with it. paint_row_n() already receives the cell size as `n`, and
 * `n` is a compile-time constant at each of its call sites, so scaling by
 * it costs nothing at all.
 *
 * The period stays in PIXELS on purpose: the screen is the same size at
 * every quality, so pixel spacing is what keeps the same number of bands
 * across it. 64 rather than a rounder number because it is a power of two,
 * which turns the per-pixel wrap into a mask.
 *
 * Speed comes from the step SIZE, not from ticking more often, and the
 * difference is not cosmetic: every tick repaints all the rows holding
 * glass, so halving SHINE_STEP_MS would double that cost while doubling
 * SHINE_STEP_PX is free. */
#define SHINE_PERIOD   64      /* power of two - see the mask below */
#define SHINE_STEP_MS  40
#define SHINE_STEP_PX   2

static int      shine_offset;
static uint32_t shine_elapsed_ms;

/* The Q8 unit vector the shine band currently sweeps along - see
 * material_shine_direction()'s own comment in material.h for the fixed-
 * point convention and why gravity's direction is computed there, once a
 * frame, rather than here per pixel. Initialised to the plain (1, 1)
 * diagonal so a frame drawn before update_shine_direction() has ever run
 * (there is none in practice - sand_frame() calls it before the first
 * draw - but a static default of (0, 0) would be a silent trap for
 * whoever adds one) looks exactly like the mechanism always used to. */
static int shine_ux_q8 = 181;
static int shine_uy_q8 = 181;

/* Which rows had anything hatched in them last time they were painted, so
 * a tick of the shine can repaint those and leave the rest alone.
 *
 * Without this the shine would have to claim the whole screen every time it
 * moved, which at SHINE_STEP_MS is far too often to be affordable. A row
 * that is not repainted keeps its last answer, which stays true: nothing in
 * it changed, so whatever glass it had it still has. */
static uint8_t row_has_shine[GRID_H_MAX];

/* How much a WATER cell's grain hash is coarsened before it reaches
 * material_colours() - see the comment inside paint_row_n() where that
 * coarsening actually happens for the full account of why. 3 means an 8x8
 * block of cells shares one hash value; the knob to turn if foam's blobs
 * ever need to read bigger (raise this) or finer (lower it, back towards
 * 0 - one cell per hash, the speckle every other material still gets).
 *
 * RAISED from 2 (4x4): asked for blobs MUCH bigger than that, not another
 * small nudge - so this doubles the linear size again exactly the way the
 * previous bump (from 1's 2x2) did, landing on 8x8, 4x the area of the 4x4
 * puffs it replaces. Blobs that size read as proper drifting patches of
 * foam rather than the puffs 4x4 gave, which is the point: bigger still,
 * by the same doubling, not a differently-shaped change. */
#define FOAM_BLOB_SHIFT 3

/* How often the foam dither's phase advances - see material_set_foam_phase()
 * in material.h for what the phase is for. 90 ms is roughly 11 changes a
 * second: fast enough that the foam visibly shimmers instead of reading as
 * a fixed texture, slow enough that it does not strobe. A look tuned by eye
 * on the device, not measured - the first constant to move if foam ever
 * reads too twitchy (raise it) or too static (lower it). */
#define FOAM_PHASE_MS 90

/* Real time accumulated toward the next foam phase step, carried across
 * frames the same way shine_elapsed_ms is - see that variable's own use in
 * advance_shine() just below for the pattern this follows. Driven by dt_ms
 * rather than a frame count so foam animates at the same real-world rate
 * whatever the frame rate happens to be; a frame-count phase would shimmer
 * twice as fast at double the frame rate and freeze solid if frames ever
 * stalled. */
static uint32_t foam_elapsed_ms;

/*=============================================================================
 * A LIQUID INTERIOR'S LOCAL DEPTH - replaces a screen-position gradient with
 * one that follows each puddle's own shape.
 *
 * Reported from the device, twice, about the OLD mechanism (a plain affine
 * function of screen position, walked as a per-row depth_acc/depth_col_step
 * pair that no longer exists in this file, set up once a frame by a
 * material.c that no longer needs to either - see git log if the old shape
 * is ever wanted back): "the sensibility against gravity makes it behave
 * almost like platinum" - a uniform gradient swept across the WHOLE SCREEN,
 * independent of where the water actually is, reads as a metallic sheen
 * rather than depth through a medium - and "there is no arcs... maybe it's
 * better if the depth just follows the shape of the
 * puddle", asking, in so many words, for exactly what this is.
 *
 * LOCAL DEPTH, for a liquid cell: 0 if the neighbour one step TOWARD THE
 * SURFACE is not the SAME MATERIAL - a different liquid, empty space, or
 * solid all count as "not the same", the boundary of THIS material's own
 * body - otherwise, one more than that neighbour's own local depth. A
 * puddle with a rock poking through it dips back to a small depth right
 * where the rock breaks its surface, instead of painting straight through
 * the rock as if it were not there - "follows the shape of the puddle",
 * exactly what the second report above asked for. See suite_sand.c's
 * test_local_depth_follows_the_puddles_own_shape for the actual
 * before/after comparison, run through real sand_t/sand_step(), that
 * motivated this. THE OBSTACLE'S SHADOW THIS PRODUCES IS A KEPT, DELIBERATE
 * FEATURE, not a bug - see "THE SHADOW MUST FOLLOW GRAVITY" below for the
 * one requirement on it that changed.
 *
 * THIS MECHANISM HAS BEEN THROUGH FOUR SHAPES, each replacing the last after
 * a device report the previous one could not explain away. In order:
 *
 *   (1) a single dominant axis (`|gy| >= |gx|`) with hysteresis at the tie
 *       point - replaced because hysteresis only reduced how OFTEN the axis
 *       flipped, not how SEVERE the jump was on the rare frame it still
 *       did ("if i leave the device near a 45 degree position i can see
 *       artifacts for both direction trying to reconcile... we should have
 *       a single source of truth for this vector") - measured an 11-cell
 *       disagreement between the two axes' own readings on the same grid,
 *       all of it landing on-screen at once whenever the flip fired;
 *   (2) both axes computed unconditionally and BLENDED (a Q8 crossfade by
 *       gravity's own |gx|/|gy| ratio) - fixed the chatter (nothing left to
 *       flip) but introduced two of its own defects, a shadow beside every
 *       submerged obstacle and a depth that "breathed" with tilt angle on a
 *       flat surface;
 *   (3) both axes PROJECTED onto gravity, then combined with MAX rather
 *       than a blend - closed the shadow (a lower bound can only
 *       under-report, so max recovers the true depth from whichever axis
 *       was not blocked) and the tilt-inflation (a projected count already
 *       reads the true depth on a flat surface, so max of two things that
 *       both read it still reads it) - the full account of shapes (2) and
 *       (3), every defect, every rejected ceiling design, and the exact
 *       numbers behind each, is preserved in git log up to commit 3376c8e
 *       and is NOT repeated here; read it there before proposing another
 *       combiner-only fix, because an axis-aligned walk was ALWAYS going to
 *       hit the wall shape (4) exists to fix, however the two readings get
 *       combined;
 *   (4) THIS ONE - one walk that steps ALONG GRAVITY ITSELF, replacing the
 *       two axis-aligned walks and their combiner outright.
 *
 * THE SHADOW MUST FOLLOW GRAVITY - the defect shape (3) could not fix
 * because it is not a combiner defect at all. An axis-aligned walk can only
 * ever produce an axis-aligned SHADOW: whichever axis the obstacle blocks
 * resets to 0 and climbs back up walking straight along that axis, so the
 * shallow region behind a submerged obstacle is always a horizontal or
 * vertical band, never a diagonal one - no projection or combiner
 * downstream of that walk can rotate a shape the walk itself never drew.
 * Reported from the device after shape (3) had already shipped and closed
 * both of its own defects: the shadow's own direction still ran straight
 * down or straight sideways, never along the tilt. MEASURED, on a settled
 * pool with a submerged 3x3 stone, comparing the shadow's own deficit-
 * weighted centroid bearing against true gravity's bearing, across six
 * tilts (SHIPPED = shape (3), the two projected-axis walks; RAY = this
 * walk):
 *
 *     tilt from vertical   SHIPPED bearing        RAY bearing    true gravity
 *          33.1 deg          -90.0 (33.1 off)      -124.6 (1.5 off)   -123.1
 *          33.3 deg          +90.0 (33.3 off)       +57.6 (1.0 off)    +56.7
 *          37.8 deg          +90.0 (37.8 off)      +128.1 (0.2 off)   +127.8
 *          45.0 deg          no shadow at all       +45.0 (0.0 off)    +45.0
 *          16.7 deg          +90.0 (16.7 off)       +72.6 (0.7 off)    +73.3
 *          73.3 deg           +0.0 (16.7 off)       +17.9 (1.2 off)    +16.7
 *
 * SHIPPED's bearing is always exactly +/-90 or 0 and its error always
 * equals the tilt angle - the axis-aligned signature stated plainly. The 45
 * degree row shows SHIPPED with no shadow at all, not a small one: at the
 * exact tie point both projected axes reach the true surface equally, so
 * max() hides the shadow entirely there - which is also why the whole
 * effect visibly appears and disappears as the device rotates through that
 * angle, on top of never pointing the right way anywhere else. RAY is
 * within 1.5 degrees of true gravity at every tilt tried, INCLUDING the tie
 * point where SHIPPED loses the shadow completely.
 *
 * THE FIX: ONE walk that steps along the gravity ray by Bresenham, in place
 * of the two axis-aligned walks - not a third combiner shape layered on top
 * of them. Two regimes, switching at 45 degrees on `|gy| >= |gx|` exactly
 * like shape (1)'s own dominant-axis pick once did:
 *
 *   - VERTICAL-DOMINANT (`|gy| >= |gx|`): one ROW per step toward the
 *     surface. The cell one step back along the ray from (cx, cy) is
 *     `(cx + step, cy - vdir)`, `vdir = sign(gy)` (the real, already-
 *     adjacent grid row `vdir` steps away - `above`/`below` below, same
 *     pointers this file has always read for the vertical case), `step` a
 *     PER-ROW value shared by every column in that row (gravity does not
 *     change from one column to the next), computed fresh from `cy` alone
 *     every time this row is painted - see "THE ROW OFFSET, WITHOUT AN
 *     ACCUMULATOR" below for why it must be, and cannot be carried as
 *     running state, under this file's own sparse-repaint discipline.
 *   - HORIZONTAL-DOMINANT (`|gx| > |gy|`): the transpose - one COLUMN per
 *     step, source cell `(cx - hdir, cy + step)`, `hdir = sign(gx)`, `step`
 *     now a PER-CELL value (0 most cells, +/-1 wherever the ray's own
 *     diagonal drift crosses a row boundary), walked by a plain Bresenham
 *     accumulator reset at the start of every row - safe to reset per row
 *     BECAUSE a row-call always processes its own full width in one pass
 *     (see "THE HORIZONTAL WITHIN-ROW ACCUMULATOR" below), unlike the
 *     vertical case's per-row value, which spans separate calls and needs
 *     the closed form instead.
 *
 * THIS IS NOT SHAPE (1) AGAIN, even though it is once more a single,
 * discrete regime pick with a real seam at 45 degrees - the two prior
 * rejections do not apply here, and the reason is worth stating precisely
 * rather than assumed: shape (1)'s two sides measured DIFFERENT quantities
 * (a plain vertical cell count and a plain horizontal cell count, related
 * to the true depth by two different, angle-dependent factors), so a flip
 * between them was a jump in the reported value itself, however rarely it
 * fired. Both regimes here measure the SAME quantity - distance along the
 * gravity ray, in cells - so a regime flip changes only HOW that quantity
 * gets computed, not what it means; the two sides agree exactly at the
 * 45-degree crossing by construction (both walk the same ray there), so
 * there is no discrete jump in the reported depth to chatter on, only a
 * discrete change in bookkeeping mechanics, reset cleanly the same way a
 * `local_depth_v_reverse`/`local_depth_h_reverse` flip already was.
 * Measured directly, the same 30-to-60-degree sweep test_the_blend_has_no_
 * jump_crossing_45_degrees has used since shape (2): worst single-degree
 * step across the crossing, SHIPPED (shape 3) 1, THIS WALK 0.
 *
 * DEPTH IN CELLS IS `count * |g| / |dominant axis|`, applied ONCE, AT
 * COMBINE TIME, to a raw STEP COUNT - not baked into the climb itself. See
 * "THE COUNT MUST STAY A RAW COUNT" below for why: this is the one property
 * every prior shape's own history (git log, commit 3376c8e and earlier)
 * already proved is load-bearing, and it survives this rewrite unchanged.
 *
 * STORAGE: ONE walk now needs only ONE shared pair of arrays, not two -
 * local_depth_row_a[]/local_depth_row_b[] below (a plain double buffer,
 * pointer-swapped at the end of every row, never copied) replace
 * col_stable_depth[]/row_stable_depth[] together, and local_depth_top_row[]
 * replaces col_top_row[]/row_top_col[] together. See each array's own
 * comment for the mechanism and, for local_depth_top_row[] specifically,
 * for why the DEBOUNCE KEY it stores means something different in each
 * regime - a genuine finding from writing this, not a stylistic choice. */

/* THIS FRAME'S SCALE, in Q8 - how many eighths... no, straight Q8 units of
 * TRUE distance (along gravity) one raw STEP COUNT is worth, for whichever
 * regime is active this frame: `local_depth_scale_q8 = 256 * im_len(gx, gy)
 * / dominant_axis` (`dominant_axis` is `|gy|` when vertical-dominant, `|gx|`
 * when horizontal-dominant) - see update_local_depth_gravity() below for
 * where it is set, once a frame, and LOCAL DEPTH's own top comment for why
 * a single walk along the ray needs only ONE scale, not two weights blended
 * or maxed together.
 *
 * NOTE THE DIRECTION OF THIS RATIO IS THE OPPOSITE OF THE OLD BLEND/MAX
 * DESIGN'S OWN WEIGHT - worth stating plainly, because copying the old
 * formula's shape here silently, without re-deriving it, is exactly the
 * mistake this file's own host-side validation caught once already (see
 * git log for the commit this comment describes: a first draft reused the
 * old `component / len` ratio unchanged and read a fixed 10-cell planar
 * depth as 19 at 45 degrees). The OLD axis count walked a fixed SCREEN AXIS
 * regardless of gravity's own angle, so recovering true depth from it meant
 * SHRINKING the count by that axis's own share of gravity (`component /
 * len`, always <= 256, minimised at the 45-degree tie). THIS walk already
 * follows the gravity ray itself - each step of the count already covers
 * `len / dominant_axis` cells of TRUE distance, not one screen cell - so
 * recovering true depth means GROWING the count by that same ratio instead
 * (`len / dominant_axis`, always >= 256, exactly 256 only when gravity is
 * perfectly axis-aligned and the ray IS the screen axis). This is also
 * WHY LOCAL_DEPTH_COUNT_CEILING NEEDS NO RAISE THIS TIME - see that
 * constant's own comment below.
 *
 * PROJECTED AT COMBINE TIME, deliberately, not baked into the climb itself
 * - see "THE COUNT MUST STAY A RAW COUNT" in LOCAL DEPTH's own top comment
 * for why: a raw count is gravity-agnostic (means the same thing no matter
 * when it was accumulated), so scaling it fresh, from THIS frame's own
 * gravity, at the moment it is read, is always correct regardless of when
 * the count was built up - the property every earlier shape of this
 * mechanism already proved is load-bearing (git log, commit 3376c8e and
 * earlier) and this rewrite does not get to relitigate.
 *
 * ALSO SET HERE: `local_depth_vertical_dominant` (which regime is active
 * this frame, `|gy| >= |gx|`) and the two scan-direction flags this file
 * has always needed - `local_depth_v_reverse`/`local_depth_h_reverse`,
 * UNCHANGED in meaning from every earlier shape of this mechanism (descend
 * instead of ascend when that axis's own gravity component is negative).
 * Both flags now feed BOTH regimes, not one each - see LOCAL DEPTH's own
 * top comment, "THIS IS NOT SHAPE (1) AGAIN", for why `local_depth_v_
 * reverse` alone is also the correct row-processing order for the
 * horizontal-dominant regime's own cross-row reads, not merely the
 * vertical one's. */
static unsigned local_depth_scale_q8;
static bool local_depth_vertical_dominant;
static bool local_depth_v_reverse;
static bool local_depth_h_reverse;

/* |gx|, |gy| for THIS frame, stashed alongside the scale above for the same
 * reason - paint_row_n() below needs the exact integer RATIO between the
 * two axes (not `local_depth_scale_q8`, which already divides by
 * `im_len()`, a different quantity) to walk the vertical-dominant regime's
 * own per-row Bresenham offset, and the horizontal-dominant regime's own
 * within-row accumulator, without either needing gravity passed in as a
 * parameter. See "THE ROW OFFSET, WITHOUT AN ACCUMULATOR" and "THE
 * HORIZONTAL WITHIN-ROW ACCUMULATOR" in paint_row_n() for exactly how
 * these two are used. */
static unsigned local_depth_ax;
static unsigned local_depth_ay;

/* Last frame's local_depth_vertical_dominant/local_depth_v_reverse/local_
 * depth_h_reverse - not read by paint_row_n() at all, only by update_local_
 * depth_gravity() itself, to detect the ONE frame any of the three actually
 * changes. See that function's own comment for why a change in ANY of them
 * invalidates local_depth_row_a[]/local_depth_row_b[]/local_depth_top_row[]
 * together, as one unit, rather than each flag guarding its own separate
 * piece of state the way the two-walk design's v_reverse/h_reverse flips
 * once did. */
static bool local_depth_vertical_dominant_prev;
static bool local_depth_v_reverse_prev;
static bool local_depth_h_reverse_prev;

/* THE WALK'S OWN STORAGE - a plain double buffer, GRID_W_MAX entries each,
 * the same file-static persistence-across-calls pattern row_has_shine[]
 * above already uses for the same reason (sized for the finest quality
 * tier; a coarser one just uses less of it). Replaces BOTH col_stable_
 * depth[]/row_stable_depth[] together - one walk needs one chain, not two -
 * see LOCAL DEPTH's own top comment for why a SINGLE cx-indexed pair works
 * for EITHER regime, not only the vertical one.
 *
 * `local_depth_cur_row`/`local_depth_prev_row` are pointers into these two
 * buffers, POINTER-SWAPPED at the end of every paint_row_n() call (see that
 * function's own tail) rather than copied - "the row painted before this
 * one" always means whichever buffer `local_depth_prev_row` names at the
 * moment a new row starts, and "this row's own emerging values" always
 * means whichever buffer `local_depth_cur_row` names, however many frames
 * apart the two calls that used them actually were under this file's own
 * sparse-repaint discipline (STALE READINGS ARE ACCEPTED here exactly as
 * they always have been for this mechanism - see LOCAL DEPTH's own top
 * comment's forebears in git log for the accepted trade-off this inherits
 * unchanged).
 *
 * VERTICAL-DOMINANT reads `local_depth_prev_row[cx + step]` - a genuinely
 * DIFFERENT row's data, always (the source cell is one whole row away along
 * the ray) - so the buffer this function is currently WRITING into
 * (`local_depth_cur_row`) is never also read from during the same call;
 * there is no read/write aliasing to worry about, and the CX scan order
 * inside one call does not matter to this regime's own correctness (see
 * `cx_first`/`cx_step` below for the reason those still exist anyway).
 *
 * HORIZONTAL-DOMINANT reads either `local_depth_cur_row[cx - hdir]` (the
 * common case, `step == 0`: the source is the PREVIOUSLY-PROCESSED column
 * of THIS SAME row, already written earlier in this same call's own scan -
 * this is where the scan order set by `cx_first`/`cx_step` below actually
 * matters) or `local_depth_prev_row[cx - hdir]` (`step != 0`: the ray
 * crossed a row boundary at this column, so the source is the row
 * processed immediately before this one instead). Genuinely no aliasing
 * either way: the `cur_row` read only ever looks at an EARLIER cx in the
 * SAME scan, never the slot about to be written this same iteration, and
 * the `prev_row` read is a wholly separate buffer from whichever one is
 * being written this call. Verified in a host model before this was wired
 * in here, per this rewrite's own instructions - not assumed. */
static uint8_t local_depth_row_a[GRID_W_MAX];
static uint8_t local_depth_row_b[GRID_W_MAX];
static uint8_t *local_depth_cur_row = local_depth_row_a;
static uint8_t *local_depth_prev_row = local_depth_row_b;

/* WHICH ROW local_depth_prev_row[] ACTUALLY DESCRIBES - the one fact the
 * double buffer above never carried, and whose absence turned out to be the
 * whole of a reported device artifact: "a brief flip of colours" on a
 * settled pool, and "a huge spike" flipping the board to a straight
 * orientation. LOCAL_DEPTH_NO_ROW means "nothing painted yet, or the chain
 * was deliberately broken" (chosen outside 0..GRID_H_MAX-1 so no real row
 * can ever collide with it, the same trick local_depth_top_row[]'s own 255
 * uses).
 *
 * THE DEFECT, exactly. Every cross-row read in paint_row_n() below treats
 * local_depth_prev_row[qx] as "the count belonging to the cell one step
 * back along the ray, in row cy - vdir". That is only true when the row
 * painted immediately before this one WAS row cy - vdir. draw_dirty_rows()
 * sweeps surface-first precisely so that it usually is - but it only ever
 * paints DIRTY rows, and LOCAL_DEPTH_WAKE_MS's own tick marks only rows
 * that hold a LIQUID cell. So on a settled pool the sweep's FIRST row is
 * the surface row, the (empty) row above it is never painted, and
 * local_depth_prev_row[] still holds what the LAST row of the PREVIOUS
 * sweep left there - the DEEPEST row of the pool, saturated at
 * LOCAL_DEPTH_COUNT_CEILING.
 *
 * WHY THAT IS CATASTROPHIC RATHER THAN MERELY STALE. The surface row's
 * source is air, so `same_material` is false and the walk drops into the
 * hold-then-commit debounce below. A COMMIT writes 0 and all is well. A
 * HOLD instead "keeps climbing as if nothing happened" - and climbing FROM
 * A SATURATED COUNT means the surface itself reads fully saturated, which
 * every row beneath it then inherits as a same-material climb. The entire
 * body renders at maximum depth in one frame: not a shade or two out, the
 * gradient inverted end to end. Measured, host-side, on the user's own
 * scenario (a pool filling 40% of a 92x112 grid, settled under portrait
 * gravity, the SIMULATION THEN FROZEN so that every displayed change is
 * spurious by construction, gravity swept 0 to 90 degrees at one degree per
 * frame): 841 interior cells - a quarter of the pool - crossed a full shade
 * step in a single frame, 23 of the 24-cell band, at the first wake tick
 * after the 45-degree regime flip. With this guard: 83, and none of them in
 * the body of the pool (see the residual note below).
 *
 * WHY THE GUARD IS ON THE HOLD PATH ONLY, and not on every cross-row read.
 * A same-material climb reading a stale count is DELIBERATE and load-
 * bearing: a row repainted in isolation deep inside a pool inherits a value
 * that is stale but SATURATED, which is exactly right there, and is the
 * property test_a_sparse_repaint_does_not_band_a_tall_liquid_column pins
 * (still 0 banded pairs). Distrusting the buffer on that path too was tried
 * in the same harness and is strictly worse: it makes an isolated deep row
 * re-climb from 1, which is the banding that test exists to forbid. At a
 * BOUNDARY the stale value is not approximately right - it belongs to a
 * different body entirely - so 0 is the only honest carry, and it is also
 * what a coherent sweep would have produced, since the non-liquid row the
 * chain should have started from writes 0 into every column.
 *
 * THE RESIDUAL 83 CELLS are a one-to-five-column strip against the left
 * wall, where the ray leaves the grid (`qx_ok` false) and the walk restarts
 * from 0 by construction; which rows that happens on shifts as the per-row
 * Bresenham drift changes with the tilt. That is the wall's own shadow
 * moving, not a chain break, and it is left alone.
 *
 * -2, NOT -1, and the difference is load-bearing rather than stylistic: the
 * value this is compared against is `cy - vdir`, which ranges over
 * [-1, grid_h] as cy sweeps [0, grid_h) with vdir either sign. -1 is
 * therefore a REAL value that comparison can produce - the top row of the
 * grid with gravity pointing down - so a -1 sentinel would read as "the
 * chain is intact" for exactly the cells whose neighbour is off the top of
 * the screen, which is precisely the surface-flood case above. -2 is
 * outside that range at both ends. */
#define LOCAL_DEPTH_NO_ROW (-2)
static int local_depth_prev_cy = LOCAL_DEPTH_NO_ROW;

/* THE DEBOUNCE KEY - one array now, not two, but NOT a plain merge of
 * col_top_row[]/row_top_col[]'s own two conventions: the two regimes need
 * DIFFERENT things stored here, and forcing one convention onto both was
 * tried, found broken, and is worth recording precisely rather than only
 * the working design that replaced it.
 *
 * VERTICAL-DOMINANT keeps the OLD convention exactly: `local_depth_top_
 * row[cx]` holds the ROW INDEX of column cx's most recent boundary
 * request, and a reset only COMMITS once the SAME row asks for it again on
 * a later painted frame (255 = "nothing tracked yet", chosen the same way
 * the old arrays did - GRID_H_MAX is 224, comfortably under 255). This
 * still works for exactly the reason it always did: `local_depth_cur_row[]`
 * is column-indexed and a GIVEN column's own row-sweep visits that column's
 * slot roughly once per frame (once per row painted, chained down the
 * column across separate calls) - "the row" genuinely identifies a stable
 * physical location for that column across frames.
 *
 * HORIZONTAL-DOMINANT CANNOT REUSE THAT KEY, and this was found by testing,
 * not reasoned out in advance: unlike the vertical case, EVERY row-call
 * writes EVERY column's slot in local_depth_cur_row[] (a row-call always
 * walks its own full width), so a row-indexed key compares against a
 * DIFFERENT row's own index on almost every successive write to the same
 * slot - a column sitting permanently beside a real wall would ask for a
 * reset from a different `cy` on every single dirty row that touches it,
 * so `top_row[cx] == cy` would almost never match twice, and the debounce
 * would HOLD FOREVER instead of ever committing to a genuine, permanent
 * boundary. Measured directly, reproducing exactly this geometry (a
 * settled pool against a real side wall, gravity mostly horizontal): a
 * row-indexed key left the wall-adjacent column's own reported depth stuck
 * climbing indefinitely rather than reading near 0, the column beside a
 * REAL, PERMANENT wall - the single most common case this debounce has to
 * get right, not an edge case.
 *
 * THE FIX FOR THIS REGIME: `local_depth_top_row[cx]` instead stores a
 * PLAIN PENDING FLAG - any value other than 255 means "the immediately
 * preceding write to this slot was ALSO a boundary request, not yet
 * confirmed a second time"; 255 means "the preceding write was a genuine
 * same-material climb, or nothing has been written yet." A reset commits
 * once this flag is already pending, and STAYS pending (re-armed) on every
 * subsequent boundary request too - so a permanent wall commits to 0 on
 * every row that touches it after the first, while a single stray blink
 * (one row's own grain-settling noise misreading a boundary that is not
 * really there) still gets held rather than trusted immediately, and the
 * flag is explicitly cleared back to 255 on the very next confirmed
 * same-material climb so a long-past, unrelated blink cannot pre-arm a
 * later, different blink into an instant false commit. This is a genuine
 * REINTERPRETATION of what the stored byte means, not a coincidental reuse
 * - see paint_row_n()'s own "THE WALK ITSELF" comment for the exact
 * comparison each regime makes against this same array.
 *
 * BOTH REGIMES SHARE THE ARRAY, NOT JUST THE TYPE, because a REGIME FLIP
 * (see update_local_depth_gravity() below) always resets it wholesale
 * alongside local_depth_row_a[]/local_depth_row_b[] - the two regimes never
 * read a value the OTHER one wrote, by construction, so there is no
 * cross-regime confusion for either convention to guard against. */
static uint8_t local_depth_top_row[GRID_W_MAX];

/* local_depth_cur_row[]/local_depth_prev_row[] hold a plain CELL COUNT -
 * see LOCAL DEPTH's own top comment, "THE COUNT MUST STAY A RAW COUNT",
 * for why an eighths-of-a-cell (or any pre-scaled) accumulator was tried
 * for an earlier shape of this mechanism and rejected; that lesson
 * transfers unchanged. This is that count's saturation point.
 *
 * UNLIKE THE TWO-WALK DESIGN'S OWN LOCAL_DEPTH_COUNT_CEILING (34, raised
 * above MATERIAL_LIQUID_DEPTH_BAND's own 24), THIS ONE NEEDS NO RAISE - and
 * that is a genuine, load-bearing difference from this walk's own scale,
 * not an oversight carried over. The two-walk design's own weight was
 * `component / len`, ALWAYS <= 256 (minimised at the 45-degree tie, around
 * 183 of 256), so a count clamped at the plain band (24) could project to
 * BELOW the band at every angle except perfect axis alignment - "breathing"
 * - and the ceiling had to be raised past the band so a saturated count
 * still reached it after being shrunk. THIS walk's own scale is `len /
 * dominant_axis`, ALWAYS >= 256 (equal to 256 only at perfect axis
 * alignment, growing at every other angle) - so a count clamped at the
 * plain band ALREADY projects to AT LEAST the band at every angle, and the
 * combiner's own explicit clamp to MATERIAL_LIQUID_DEPTH_BAND (see "THE
 * WALK ITSELF" below) does the rest. Verified directly, host-side, against
 * a fully saturated cell swept across the same 0-90 degree range test_a_
 * saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle uses:
 * projected depth clamps to a flat MATERIAL_LIQUID_DEPTH_BAND at every
 * sample, with the ceiling equal to the band and no raise at all - the same
 * flatness the raised ceiling existed to buy back, without needing to buy
 * it back, because this walk's own scale points the opposite way. */
#define LOCAL_DEPTH_COUNT_CEILING MATERIAL_LIQUID_DEPTH_BAND

/* Called once per frame, alongside material_set_gravity() - same gravity
 * vector, same reason: material_colours()'s liquid interior needs THIS
 * frame's own local-depth scale, not last frame's, and working out the
 * scale, the active regime and both scan directions once here is what
 * keeps paint_row_n() itself down to one multiply, one shift and one
 * compare per cell for the projection (plus the walk's own debounce/climb
 * work), no divide there - the only divide this mechanism spends is the one
 * below, once a frame, plus one more per PAINTED ROW for the vertical-
 * dominant regime's own row offset (see "THE ROW OFFSET, WITHOUT AN
 * ACCUMULATOR" in paint_row_n() for why that one cannot be hoisted up to
 * here too). */
static void update_local_depth_gravity(int gx, int gy)
{
    const int ax = im_abs(gx), ay = im_abs(gy);

    /* ONE DIVIDE PER FRAME here (plus one more per PAINTED ROW for the
     * vertical-dominant regime - see paint_row_n()'s own "THE ROW OFFSET,
     * WITHOUT AN ACCUMULATOR"), the same budget class build_xflow()
     * (sand.c) already spends on its own per-frame q_q8, and the class
     * material_set_gravity()'s own setup already spends elsewhere in this
     * app; material_set_gravity() calls im_len() too, on the same (gx, gy),
     * so this is the second such call this frame, not the first (im_len()
     * has no state of its own to share between them, so there is nothing to
     * hoist). At gx == gy == 0 (flat, or free fall) there is no gravity
     * direction for the scale to mean anything against, so it falls back to
     * an arbitrary, harmless 256 (a straight 1:1, no scaling at all) rather
     * than a division by zero - nothing meaningfully "settles" with no
     * gravity direction anyway, so it does not matter which way this tie is
     * broken. */
    const int len = im_len(gx, gy);
    const bool new_vertical_dominant = (ay >= ax);
    const unsigned dom_axis = new_vertical_dominant ? (unsigned)ay : (unsigned)ax;
    local_depth_scale_q8 = (dom_axis != 0u)
        ? (256u * (unsigned)len) / dom_axis : 256u;
    local_depth_ax = (unsigned)ax;
    local_depth_ay = (unsigned)ay;

    /* Same meaning as every earlier shape of this mechanism - see local_
     * depth_v_reverse/local_depth_h_reverse's own comment above. */
    const bool new_v_reverse = (gy < 0);
    const bool new_h_reverse = (gx < 0);

    /* A CHANGE IN ANY OF THE THREE - which regime is dominant, or either
     * scan's own direction - invalidates the walk's shared state as ONE
     * unit, not three separately guarded pieces. Argued through, regime by
     * regime, for why each one earns a reset on its own (not merely copied
     * from the two-walk design's own two resets):
     *
     * A REVERSAL OF EITHER SCAN DIRECTION is the same failure the two-walk
     * design's own v_reverse/h_reverse flips already fixed, unchanged in
     * kind: "the neighbour toward the surface" relocates (above/below swap,
     * or the within-row scan direction swaps), so a row or column index
     * this array's debounce was tracking under the OLD direction describes
     * a boundary relationship the NEW direction does not have. Reproducing
     * the ORIGINAL device report this fixed (landscape lock, gy small and
     * tremor-noisy against a tall water column) against THIS mechanism
     * shows the identical shape of corruption without the reset: a raw
     * count compounding unboundedly across repeated direction flips instead
     * of resetting where it should.
     *
     * A REGIME FLIP (vertical-dominant <-> horizontal-dominant) is new to
     * THIS shape - shapes (2) and (3) never had it, because both regimes
     * ran every frame unconditionally there. Here, the SAME array slot
     * (indexed by cx) carries a different RECURRENCE depending on which
     * regime is active - a vertical-dominant count chains down rows at a
     * fixed column; a horizontal-dominant count chains across columns
     * within a row, only occasionally borrowing a value from the row above
     * or below. A value left behind by one regime is not simply "the same
     * count under different bookkeeping" to the other regime's own read
     * pattern - it is a value about a physically different neighbour
     * relationship. Reset here for the same reason the two scan-direction
     * flips already are: a brief, bounded bookkeeping correction rather
     * than trusting a stale interpretation across the switch.
     *
     * MEASURED, host-side, a single sharp jostle across the 45-degree line
     * in the middle of an otherwise steady near-horizontal hold (the
     * regime-flip analogue of the landscape-lock model already used to
     * measure the two scan-direction resets): the reset itself produces a
     * small, SELF-HEALING blip immediately after the jostle ends and
     * gravity returns to near-horizontal - 12 banded pairs the very next
     * measured frame, falling to 6 one frame later, 0 the frame after that
     * (LOCAL_DEPTH_WAKE_MS's own periodic wake finishes the job) - not the
     * "visually a no-op" result the scan-direction resets measured, but
     * bounded, transient, and gone within two frames at 30fps (well under
     * 100ms), against an artificially instantaneous gravity step no real
     * hand-tremor input produces (the tilt filter's own smoothing means a
     * real crossing ramps through several frames, not one). Worth
     * re-measuring on the device if a "brief pop near 45 degrees" report
     * ever comes back in about this mechanism specifically.
     *
     * A SCAN-DIRECTION FLIP ONLY EARNS THE RESET IF THE ACTIVE REGIME CAN
     * SEE IT, and that turned out to matter at exactly the orientations a
     * hand actually holds this board at. AT AXIS LOCK ONE COMPONENT SITS ON
     * ZERO: the device's own capture sidecars from the report this block was
     * re-examined for read tilt_x -12 and -195 against tilt_y 3342 and 4190
     * - portrait, with gx hovering either side of zero. `new_h_reverse` is
     * just `gx < 0`, so ordinary hand tremor flips it many times a second,
     * and EVERY ONE of those flips wiped the whole walk state. Measured,
     * host-side, over 40 frames of that exact tremor against a settled 40%
     * pool: 40 resets in 40 frames. Landscape is the mirror image with
     * `new_v_reverse` as the noisy flag: 39 in 40.
     *
     * WHAT THOSE RESETS COST, measured rather than assumed - they are NOT
     * the "flip of colours" this file's own local_depth_prev_cy comment
     * tracks down (with the tremor running and nothing else changing, the
     * displayed depth moved by at most 1 of 24 and no cell crossed a shade
     * step). What they do is DISABLE THE DEBOUNCE OUTRIGHT: local_depth_top_
     * row[] is wiped to "untracked" before every single frame's paint, so a
     * boundary can never be asked for a second time and can never COMMIT.
     * The surface of a settled pool holds at 1 instead of committing to 0
     * forever, and every row beneath inherits it - mean displayed depth over
     * the pool's 3956 interior cells measured 18.12 with the resets against
     * 17.58 without, permanently, plus a three-array wipe every frame for a
     * flag change nothing can observe.
     *
     * THE GATE IS EXACT ARITHMETIC, NOT A DEADBAND - deliberately, because
     * this mechanism already removed one tuned dead zone and should not
     * quietly grow another. Each condition below is a statement about
     * whether the flipped flag can change a number THIS grid's walk actually
     * computes, derived from the Bresenham arithmetic itself:
     *
     *   VERTICAL-DOMINANT: `new_h_reverse` only supplies `xsign`, the SIGN
     *     of the per-row drift. That drift's running total across the whole
     *     grid is floor(grid_h * ax / ay) (see paint_row_n()'s "THE ROW
     *     OFFSET, WITHOUT AN ACCUMULATOR"), so when `grid_h * ax < ay` every
     *     row's step is 0, and the sign of nothing is still nothing.
     *   HORIZONTAL-DOMINANT: `new_v_reverse` only picks the cross-row source
     *     (`toward_surface`, dereferenced only when `step != 0`) and the
     *     sweep order that gives `local_depth_prev_row[]` its meaning. The
     *     within-row accumulator adds ay per cell from 0 and fires at ax, so
     *     when `grid_w * ay < ax` no cell in any row ever reads across a row
     *     at all, and neither of those two can be observed.
     *
     * A REGIME FLIP is never gated - it always changes what the array slot
     * means, whatever the magnitudes are. Measured with the gate in place,
     * same harness: portrait tremor 0 resets in 40 frames and the mean back
     * to 17.58; landscape tremor 0 in 40; the genuine 45-degree crossing
     * still resets, 2 flips in the 12-frame portrait-to-landscape ramp,
     * unchanged. */
    const bool drift_observable = ((long)grid_h * (long)ax >= (long)ay);
    const bool cross_row_observable = ((long)grid_w * (long)ay >= (long)ax);
    const bool v_reverse_matters = (new_v_reverse != local_depth_v_reverse_prev) &&
        (new_vertical_dominant || cross_row_observable);
    const bool h_reverse_matters = (new_h_reverse != local_depth_h_reverse_prev) &&
        (!new_vertical_dominant || drift_observable);

    if (new_vertical_dominant != local_depth_vertical_dominant_prev ||
        v_reverse_matters || h_reverse_matters) {
        for (int cx = 0; cx < grid_w; cx++) {
            local_depth_row_a[cx] = 0u;
            local_depth_row_b[cx] = 0u;
            local_depth_top_row[cx] = 255u;
        }
        /* The wiped buffers describe no row at all now - say so, rather than
         * leaving the next painted row to chain off two arrays of zeroes as
         * if they were its real neighbour. See local_depth_prev_cy's own
         * comment above paint_row_n(). */
        local_depth_prev_cy = LOCAL_DEPTH_NO_ROW;
        local_depth_vertical_dominant_prev = new_vertical_dominant;
        local_depth_v_reverse_prev = new_v_reverse;
        local_depth_h_reverse_prev = new_h_reverse;
    }

    local_depth_vertical_dominant = new_vertical_dominant;
    local_depth_v_reverse = new_v_reverse;
    local_depth_h_reverse = new_h_reverse;
}

/* LOCAL DEPTH'S OWN PERIODIC WAKE - closes the same gap SHINE_STEP_MS
 * already closes for the travelling shine (see that constant's own comment
 * above, and advance_shine() below, for the pattern this mirrors almost
 * exactly).
 *
 * THE BUG: the walk above is recomputed from THIS FRAME's gravity, every
 * frame paint_row_n() runs for a row - but a settled, sleeping block does
 * not run it again once nothing is moving, because nothing calls
 * paint_row_n() for a row draw_dirty_rows() never marks dirty.
 * sand_enable_sleeping()'s own comment (sand.h) - "everything wakes when
 * the gravity direction changes... since either can free a grain" - is a
 * promise about the SIMULATION's sleeping blocks (BLOCK_ACTIVE, whether
 * PHYSICS gets re-examined), not about this file's dirty_rows[] (whether
 * PIXELS get repainted). A block can wake for physics, find nothing
 * actually needs to move, and go back to sleep without ever touching
 * dirty_rows[]. Ordinary hand wobble drifts gravity continuously - smoothed
 * by the tilt filter but never perfectly still - so local_depth_scale_q8
 * above keeps changing, frame after frame, with no cell in a settled
 * pool ever moving to earn that pool's rows a repaint. The result: a
 * sleeping block's displayed depth is stuck at whatever it was the last
 * time something nearby genuinely disturbed it, while a neighbouring block
 * still being redrawn for an unrelated reason repaints with the CURRENT
 * walk output - a hard, rectangular seam between "stale" and "fresh"
 * exactly where one block's
 * sleep boundary meets another's, in place of the smooth gradient a
 * puddle's own surface should read as.
 *
 * THE FIX is the same shape as the shine's: an unconditional periodic tick
 * that marks every row known to hold a liquid cell dirty, regardless of
 * what the simulation did that frame - see row_has_liquid[] just below,
 * advance_local_depth_wake() beside advance_shine() further down, and
 * draw_dirty_rows()'s own shine_moved block for the precedent this repeats
 * in the same shape.
 *
 * row_has_liquid[] GATES ON ANY LIQUID CELL, RIM INCLUDED - not only an
 * interior one, despite this array existing purely to feed the blend that
 * only an interior cell ever reads. An earlier version gated on interior
 * cells alone, which reproduced this exact staleness bug one level down:
 * ordinary grain-level settling noise flips a cell between rim
 * (`mask != 0`) and interior (`mask == 0`) classification constantly at any
 * real liquid surface, so a wide, shallow pool's edge rows can read as
 * all-rim for many consecutive ticks - the wake skips them entirely for as
 * long as that holds, which is unbounded - and when a cell in one of those
 * rows THEN flips back to interior, the very next wake tick repaints it
 * from CURRENT gravity/weight over a value that may be frozen from an
 * arbitrarily distant past. Gating on any liquid cell keeps every
 * liquid-bearing row on the same bounded refresh cadence regardless of how
 * its edges flicker between rim and interior, so by the time a cell IS
 * classified interior its row was already fresh as of the last wake tick -
 * see test_a_settled_edge_does_not_flicker_stale_to_fresh in suite_sand.c
 * for the reproduction and the measured collapse this closes.
 *
 * A SEPARATE clock from the shine's own, on purpose - not folded into one
 * shared tick for two features. Different feature, different rate,
 * independently tunable - the same reasoning that keeps FOAM_PHASE_MS's own
 * clock apart from the shine's rather than reusing it.
 *
 * 120 IS A STARTING POINT, a look/cost trade-off tuned by eye and by device
 * measurement, not a physical constant - exactly the same status
 * SHINE_STEP_MS's own comment gives that constant: speed comes from the
 * STEP SIZE, not from ticking more often, so a SHORTER value here tracks
 * gravity's drift more closely at a proportionally HIGHER redraw cost
 * (every row holding any liquid cell repainted in full, every tick), while
 * a longer value is cheaper and drifts more visibly out of date before the
 * next wake catches it up. Unlike glass, a large body of water, oil, lava
 * or acid can cover far more of the screen than a typical hatched scene
 * ever does, so this cost is worth watching closely on the device before
 * trusting 120 as final - it has not been measured there yet.
 *
 * WIDENING row_has_liquid[] TO RIM CELLS TOO does not change this estimate
 * in any way that matters: a row with liquid but no interior cell at all is
 * a thin strip sitting right at a pool's edge - one or two rows at the very
 * top, bottom, or side of a settled body, where the liquid is shallow
 * enough that every cell in the row happens to have an empty cardinal
 * neighbour at that instant. A pool's INTERIOR - the bulk of its rows, the
 * part this array already marked before the widening - is unaffected: a
 * row with any interior cell was already gated in, rim or not. The
 * widening can only ADD the handful of edge-only rows the old condition
 * used to skip; it cannot double the marked-row count the way gating on
 * "any liquid" from scratch would if the array previously gated on nothing
 * at all. */
#define LOCAL_DEPTH_WAKE_MS 120

/* Real time accumulated toward the next local-depth wake tick - carried
 * across frames the same way shine_elapsed_ms and foam_elapsed_ms are, for
 * the same reason: driven by dt_ms rather than a frame count, so the wake
 * fires at the same real-world rate whatever the frame rate happens to
 * be. */
static uint32_t local_depth_wake_elapsed_ms;

/* Which rows painted ANY liquid cell last time they were painted - rim or
 * interior - the liquid counterpart to row_has_shine[] above, kept for
 * exactly the same reason: without this the wake tick would have to claim
 * the whole screen every time it fires, which at LOCAL_DEPTH_WAKE_MS is far
 * too often to be affordable if the screen holds anything besides liquid. A
 * row that is not repainted keeps its last answer, which stays true only
 * until the next wake tick - see LOCAL DEPTH's own periodic-wake comment
 * above for why that staleness is exactly the bug this array's tick exists
 * to bound.
 *
 * NOT interior-only, on purpose, even though only an interior cell's depth
 * ever gets blended: gating on interior cells alone let a row go stale for
 * an unbounded time whenever its only liquid was classified as rim for a
 * stretch (ordinary edge-settling noise flips a cell between rim and
 * interior constantly) - see LOCAL DEPTH'S OWN PERIODIC WAKE's own comment
 * above for the full story and the reproduction that found it. Any liquid
 * cell, rim included, is enough to keep the row on the bounded cadence.
 *
 * Reset to 0 at the top of paint_row_n() right beside row_has_shine[cy]'s
 * own reset, and set to 1 the moment a cell in that row paints as ANY
 * liquid cell - see the population point inside paint_row_n() below, right
 * where `depth` is blended, for exactly which cells that is and why it is
 * recomputed there rather than read back out of material_colours(). */
static uint8_t row_has_liquid[GRID_H_MAX];

static inline void paint_row_n(gfx_color_t *fb, const gfx_color_t *pal,
                               int cy, const uint8_t *row, int n)
{
    gfx_color_t *out = fb + (cy * n) * GFX_WIDTH;
    row_has_shine[cy] = 0;
    row_has_liquid[cy] = 0;

    /* grid_w, not a parameter: it does not need to be a compile-time
     * constant the way n does - only the innermost dy/dx loops below are hot
     * enough, per pixel rather than per cell, to matter. */
    /* Off the grid is NOT empty - the walls are solid, the same reading
     * sand_at() gives out-of-bounds cells - so a wall lying against the
     * screen edge is not outlined there. */
    const uint8_t *above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t *below = (cy < grid_h - 1) ? row + grid_w : NULL;

    /* THE WALK ITSELF - ONE regime, chosen once a frame
     * (local_depth_vertical_dominant, update_local_depth_gravity() above),
     * not per cell. `toward_surface` is the one real, already-adjacent grid
     * row either regime's own cross-row read ever looks at - "above" or
     * "below", whichever is toward the surface this frame, per gy's OWN
     * sign, same NULL-off-the-grid convention `mask` above already relies
     * on. Read UNCONDITIONALLY, same as the vertical case has always been -
     * but now ALSO the horizontal-dominant regime's own occasional
     * cross-row source (see "THE HORIZONTAL WITHIN-ROW ACCUMULATOR"
     * below). */
    const uint8_t *toward_surface = local_depth_v_reverse ? below : above;

    /* IS local_depth_prev_row[] ACTUALLY THE ROW `toward_surface` POINTS
     * AT? - one compare, once per row, hoisted out of the cx loop entirely;
     * see local_depth_prev_cy's own comment above this function for the
     * device artifact this answers and the measured 841-to-83 it buys. Only
     * the hold-then-commit debounce below reads it: a same-material climb
     * deliberately keeps trusting a stale count (that is what makes a
     * sparsely repainted deep row read saturated rather than banded), and
     * only a BOUNDARY's carry is nonsense when the buffer belongs to some
     * other row. */
    const int local_depth_vdir = local_depth_v_reverse ? -1 : 1;
    const bool local_depth_chain_ok =
        (local_depth_prev_cy == cy - local_depth_vdir);

    /* Scan order for THIS row's own cx loop - unchanged in meaning from
     * every earlier shape of this mechanism (ascending unless gravity
     * points left), but now needed by BOTH regimes: the horizontal-
     * dominant regime's own within-row chain (`step == 0` below) reads an
     * EARLIER cx in this same scan, so the scan has to start from whichever
     * end `hdir` points away from. The vertical-dominant regime's own read
     * never depends on cx order (each column's source lives in a different
     * row's already-complete buffer), so sharing this same order for it
     * costs nothing and needs no separate flag. */
    const int hdir = local_depth_h_reverse ? -1 : 1;
    const int cx_first = local_depth_h_reverse ? grid_w - 1 : 0;
    const int cx_step  = local_depth_h_reverse ? -1 : 1;

    /* THE ROW OFFSET, WITHOUT AN ACCUMULATOR - the vertical-dominant
     * regime's own per-row horizontal drift ("step" in LOCAL DEPTH's own
     * top comment). Every column in this row shares the SAME step (gravity
     * does not change from one column to the next), so this is computed
     * ONCE per row-call - but it must NOT be a running Bresenham
     * accumulator carried across separate paint_row_n() calls the way a
     * naive port of a reference model would do it: paint_row_n() is only
     * ever called for DIRTY rows (LOCAL DEPTH's own top comment, "STALE
     * READINGS...ARE ACCEPTED"), so a row-to-row accumulator would silently
     * skip every row in between and drift out of sync with whichever row
     * is actually being painted. `cum(n) = floor(n * minor / dominant)` is
     * the exact total Bresenham drift after `n` row-steps starting from a
     * clean 0 - verified by hand against a plain step-by-step Bresenham
     * march before this was written this way, both give identical
     * sequences - so THIS row's own step is simply `cum(n) - cum(n - 1)`
     * for whatever `n` (this row's own distance, in the scan's own
     * direction, from the surface-most row) this particular row happens to
     * be, computable directly from `cy` alone with no memory of which rows
     * were painted before it. One divide, but once per PAINTED row here,
     * not once per cell - see update_local_depth_gravity()'s own comment
     * for the budget this spends against. Meaningless (and left 0) when
     * horizontal-dominant, or when gravity has no direction at all
     * (`local_depth_ay == 0` only happens together with `local_depth_ax ==
     * 0`, since vertical-dominant requires `ay >= ax`). */
    int local_depth_row_step = 0;
    if (local_depth_vertical_dominant && local_depth_ay > 0u) {
        const int vdir = local_depth_vdir;
        const int xsign = local_depth_h_reverse ? 1 : -1;
        const int n = (vdir > 0) ? cy : (grid_h - 1 - cy);
        const int cum_n  = (int)(((long)(n)     * (long)local_depth_ax) / (long)local_depth_ay);
        const int cum_n1 = (int)(((long)(n + 1) * (long)local_depth_ax) / (long)local_depth_ay);
        local_depth_row_step = xsign * (cum_n1 - cum_n);
    }

    /* THE HORIZONTAL WITHIN-ROW ACCUMULATOR - the horizontal-dominant
     * regime's own per-CELL row drift, a plain Bresenham march reset to 0
     * HERE, at the start of every row-call, and walked forward as the cx
     * loop below sweeps `cx_first`, `cx_first + cx_step`, ... in order.
     * Safe to reset per row-call, UNLIKE the vertical case's own per-row
     * value above: this accumulator's own state never needs to survive
     * PAST one call, because a single call always processes this row's
     * FULL width in one pass (there is no sparser granularity than "the
     * whole row" to worry about losing track of between calls). Emits 0
     * most cells and +/-1 (`ysign`) wherever the ray's own diagonal drift
     * crosses a row boundary - see LOCAL DEPTH's own top comment for the
     * two-regime split this and the row offset above are the two halves
     * of. */
    int local_depth_herr = 0;
    const int ysign = local_depth_v_reverse ? 1 : -1;

    for (int cx_i = 0; cx_i < grid_w; cx_i++) {
        const int cx = cx_first + cx_i * cx_step;

        /* Which cardinal neighbours are empty, kept as separate bits
         * rather than folded straight into a bool - a liquid rim needs to
         * know WHICH side is open, not merely that one is, so it can shade
         * itself by which way that side faces against gravity. See
         * MATERIAL_EDGE_* in material.h, which this has to agree with:
         * "above" is row - grid_w, i.e. cy - 1, which is UP the screen.
         *
         * UNCONDITIONAL, same as ever - every cell of every material pays
         * these four tests, and that has to stay true: this is the
         * hottest loop in the app, and nothing below may make the common
         * case (a non-water cell, or an interior cell of anything) pay
         * for more than this. */
        unsigned mask =
            ((cx > 0          && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT  : 0u) |
            ((cx < grid_w - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u) |
            ((above != NULL   && CELL_IS_EMPTY(above[cx]))   ? MATERIAL_EDGE_UP    : 0u) |
            ((below != NULL   && CELL_IS_EMPTY(below[cx]))   ? MATERIAL_EDGE_DOWN  : 0u);

        /* The four DIAGONAL bits, which only a WATER RIM cell ever reads
         * (material_colours()'s foam gate - see material.h's own comment
         * on MATERIAL_EDGE_UP_LEFT and friends). Computed only when they
         * can possibly matter: mask already came up non-zero on the
         * cardinal test above (so this cell is a rim at all - an interior
         * cell, water or otherwise, cannot be a rim and never reaches
         * here), AND the cell is water (every other material ignores the
         * diagonal bits entirely, so spending four more reads on a rim of
         * oil, lava, acid, or anything else buys nothing). Every other
         * cell - which is most of them, on a typical board - skips these
         * four tests completely, so the per-cell cost of adding foam is
         * paid only where foam can actually appear. */
        if ((mask & MATERIAL_EDGE_CARDINAL) != 0 &&
            CELL_MATERIAL(row[cx]) == MAT_WATER) {
            mask |=
                ((cx > 0          && above != NULL && CELL_IS_EMPTY(above[cx - 1])) ? MATERIAL_EDGE_UP_LEFT    : 0u) |
                ((cx < grid_w - 1 && above != NULL && CELL_IS_EMPTY(above[cx + 1])) ? MATERIAL_EDGE_UP_RIGHT   : 0u) |
                ((cx > 0          && below != NULL && CELL_IS_EMPTY(below[cx - 1])) ? MATERIAL_EDGE_DOWN_LEFT  : 0u) |
                ((cx < grid_w - 1 && below != NULL && CELL_IS_EMPTY(below[cx + 1])) ? MATERIAL_EDGE_DOWN_RIGHT : 0u);
        }

        /* WATER samples the grain hash at a COARSER grid than every other
         * material - shifted right by FOAM_BLOB_SHIFT on both axes, so an
         * 8x8 block of cells shares one hash value instead of each cell
         * rolling its own. The user wanted foam speckled in patches rather
         * than single cells, "so it sort of resembles mist near the foam",
         * and that is exactly what asking neighbouring cells the same
         * question buys: they now agree on whether to foam, in blocks,
         * instead of disagreeing one cell at a time. See
         * test_foam_blobs_are_bigger_than_one_cell in suite_sand.c.
         *
         * No new parameter anywhere for this - `hash` is simply computed
         * differently before it is handed to material_colours(), which
         * stays as ignorant of blobs as it is of anything else about
         * coordinates. That is only possible because water has exactly one
         * consumer of its hash: material_colours()'s foam dither (see that
         * function's own comment on its water branch, in material.c). Every
         * other material still gets material_grain_hash(cx, cy) - the FINE,
         * per-cell hash - completely unchanged: stone's and wood's speckle
         * and glass's hatch all depend on adjacent cells disagreeing, and
         * coarsening their hash the way water's is coarsened here would
         * flatten them into the same striping bug material_grain_hash()'s
         * own comment already tells the story of. */
        const bool cell_is_water = CELL_MATERIAL(row[cx]) == MAT_WATER;
        const unsigned hash = cell_is_water
            ? material_grain_hash(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)
            : material_grain_hash(cx, cy);

        /* THE PER-CELL COST OF LOCAL DEPTH, in full, now that only ONE
         * regime's walk runs per cell instead of two: one array read for
         * the source count, one array read+write for the debounce
         * (local_depth_top_row[] - see that array's own comment above this
         * function for why the comparison it makes differs by regime),
         * plus the combiner - one multiply, one shift, one compare for the
         * clamp, no divide (the only divides this mechanism spends are
         * update_local_depth_gravity()'s once-a-frame one and, for the
         * vertical-dominant regime only, one more per PAINTED ROW above,
         * not here). Computed for every cell, liquid or not - the same as
         * every earlier shape of this mechanism did - because material_
         * colours() is the only consumer that ever reads the result (only
         * for a liquid's interior), and a branch to skip this for
         * non-liquids would cost more than the comparisons it would save.
         *
         * `step` is this CELL's own horizontal-dominant row drift (0 most
         * cells) - meaningless, and left 0, when vertical-dominant, where
         * the row offset is instead the single `local_depth_row_step`
         * computed once above for the whole row. */
        int step = 0;
        if (!local_depth_vertical_dominant) {
            local_depth_herr += (int)local_depth_ay;
            if (local_depth_ax > 0u && local_depth_herr >= (int)local_depth_ax) {
                local_depth_herr -= (int)local_depth_ax;
                step = ysign;
            }
        }

        /* THE SOURCE CELL, one step back along the ray - see LOCAL DEPTH's
         * own top comment for the two regimes' own formulas this computes.
         * `qx` is the source COLUMN either way; `src_ptr` the real grid row
         * to read its MATERIAL from (for the same-material continuation
         * test); `src_arr` the buffer to read its COUNT from - `local_
         * depth_cur_row[]` only in the horizontal-dominant regime's own
         * common (`step == 0`) case, where the source is an EARLIER column
         * of THIS SAME row, already written earlier in this same scan;
         * `local_depth_prev_row[]` (the row painted immediately before this
         * one) otherwise. Genuinely no read/write aliasing either way - see
         * local_depth_row_a[]/local_depth_row_b[]'s own comment above this
         * function for why, verified in a host model before this was wired
         * in here. */
        const int qx = local_depth_vertical_dominant
            ? (cx + local_depth_row_step) : (cx - hdir);
        const bool qx_ok = (qx >= 0 && qx < grid_w);
        const bool cross_row = local_depth_vertical_dominant || (step != 0);
        const uint8_t *src_ptr = cross_row ? toward_surface : row;
        const uint8_t *src_arr = cross_row
            ? local_depth_prev_row : local_depth_cur_row;

        const bool here_liquid = material_of(row[cx])->kind == KIND_LIQUID;
        const bool same_material = here_liquid && qx_ok && src_ptr != NULL &&
            (CELL_MATERIAL(src_ptr[qx]) == CELL_MATERIAL(row[cx]));
        const unsigned src_count = qx_ok ? src_arr[qx] : 0u;

        /* THE HOLD-THEN-COMMIT DEBOUNCE ITSELF - see local_depth_top_row[]'s
         * own comment above this function for the full mechanism, why the
         * comparison below differs by regime, and the measured case (a
         * permanent wall) that the row-indexed key alone cannot get right
         * for the horizontal-dominant regime. */
        unsigned count;
        if (!here_liquid) {
            /* NOT a liquid cell: depth is irrelevant here - material_
             * colours() never reads it for anything but a liquid interior
             * - and must not be allowed to accumulate through this cell, or
             * a run of open air (or any other non-liquid material) above a
             * real boundary corrupts the value that boundary inherits.
             * Clean reset, and local_depth_top_row[cx] is left alone - this
             * cell is not a boundary request of any kind. */
            count = 0u;
        } else if (same_material) {
            /* Confirmed continuation of a liquid body - climb by one,
             * stopping at the SATURATION POINT rather than at a byte's own
             * 255 - see LOCAL_DEPTH_COUNT_CEILING's own comment for why
             * this needs no raise this time, unlike every earlier shape of
             * this mechanism. Not a boundary request, so vertical-dominant
             * leaves local_depth_top_row[cx] alone here, exactly as
             * before; horizontal-dominant instead clears it back to
             * "untracked" (255) - see that array's own comment for why a
             * stale pending flag from an unrelated earlier blink must not
             * be allowed to pre-arm a later, different blink into an
             * instant false commit. */
            count = src_count < LOCAL_DEPTH_COUNT_CEILING
                ? src_count + 1u : LOCAL_DEPTH_COUNT_CEILING;
            if (!local_depth_vertical_dominant) {
                local_depth_top_row[cx] = 255u;
            }
        } else {
            const bool committed = local_depth_vertical_dominant
                ? (local_depth_top_row[cx] == (uint8_t)cy)
                : (local_depth_top_row[cx] != 255u);
            if (committed) {
                /* Vertical-dominant: THIS EXACT ROW asked for a reset the
                 * last time it was painted too - a real, lasting boundary,
                 * not a blink. Horizontal-dominant: the immediately
                 * preceding write to this slot was ALSO a boundary request
                 * - same verdict, different key. Commit. */
                count = 0u;
            } else {
                /* HOLD: keep climbing as if nothing happened, and arm this
                 * slot's tracker so the NEXT write (vertical-dominant: the
                 * next time THIS row is painted; horizontal-dominant: the
                 * very next write to this slot, whichever row it comes
                 * from) will match and commit if the boundary is real and
                 * lasting.
                 *
                 * "AS IF NOTHING HAPPENED" MEANS CLIMBING FROM THE
                 * NEIGHBOUR'S OWN COUNT - which requires the buffer to
                 * actually hold that neighbour's count. When the chain is
                 * broken (`local_depth_prev_row[]` describes some other row
                 * entirely, because the row toward the surface was not
                 * painted immediately before this one) a cross-row carry
                 * here would import a number belonging to a different body:
                 * on a settled pool that number is the DEEPEST row's
                 * saturated count, and importing it at the surface floods
                 * the whole body to maximum depth in one frame. Carry 0
                 * instead - the same value the non-liquid row this chain
                 * should have started from would have written. See local_
                 * depth_prev_cy's own comment above this function for the
                 * device report, the measured 841-to-83, and why this guard
                 * belongs here and NOT on the same-material climb above. The
                 * compare is off the hot path by construction: only a
                 * boundary cell ever reaches this branch. */
                const unsigned carry =
                    (cross_row && !local_depth_chain_ok) ? 0u : src_count;
                count = carry < LOCAL_DEPTH_COUNT_CEILING
                    ? carry + 1u : LOCAL_DEPTH_COUNT_CEILING;
                local_depth_top_row[cx] = local_depth_vertical_dominant
                    ? (uint8_t)cy : 0u;
            }
        }
        local_depth_cur_row[cx] = (uint8_t)count;

        /* THE PROJECTION - `count * local_depth_scale_q8 >> 8`, applied
         * ONCE, here, then clamped to MATERIAL_LIQUID_DEPTH_BAND. No max, no
         * blend: there is only one reading this frame, from whichever
         * regime is active, so nothing else to combine it with - see LOCAL
         * DEPTH's own top comment for why a single walk along the true
         * gravity ray needs no combiner at all, not merely a simpler one. */
        const unsigned depth_raw = (count * local_depth_scale_q8) >> 8;
        const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

        /* A ROOT borrows `depth` for its own reading - how many of its
         * eight neighbours are root - see material_colours()'s own comment
         * in material.h on why that number stands in for an age a root
         * has nowhere to store. One byte-compare per cell to decide, the
         * same price metal's leading equality test already charges inside
         * material_colours(); the eight reads behind it are paid by root
         * cells only, on rows that are being repainted at all - and a
         * root, once grown, changes about as often as stone does. */
        const unsigned depth = (row[cx] == MATX(MATX_ROOT))
            ? material_root_neighbours(above, row, below, cx, grid_w)
            : depth_liquid;

        /* row_has_liquid[]'s own population point - see that array's
         * comment above paint_row_n() for the mechanism this feeds. ANY
         * liquid cell marks the row, rim included - not gated on `mask`
         * at all, unlike material_colours()'s own interior test (KIND_
         * LIQUID and `(mask & MATERIAL_EDGE_CARDINAL) == 0`, material.c) -
         * see row_has_liquid[]'s own comment for why the wider condition is
         * the fix, not an oversight. Reuses `here_liquid`, computed just
         * above for the walk itself, rather than asking material_of()
         * again. */
        if (here_liquid) {
            row_has_liquid[cy] = 1;
        }

        gfx_color_t col[3];
        const material_pattern_t pat =
            material_colours(row[cx], hash, mask, depth, col);
        gfx_color_t *p = out + cx * n;

        /* n is a compile-time constant at each of paint_row()'s call sites,
         * so these unroll away there even though cell itself is a runtime
         * value - see paint_row()'s own comment for why that split exists.
         *
         * FLAT and SPECKLED share this loop and are equally cheap: a
         * speckled cell simply arrived with a different colour, chosen
         * once per cell from its position. Only STRIPED does per-pixel
         * work, and only where a striped material is actually on screen. */
        if (pat == MATERIAL_HATCHED) {
            row_has_shine[cy] = 1;
        }

        if (pat != MATERIAL_HATCHED) {
            const gfx_color_t c = col[0];
            for (int dy = 0; dy < n; dy++) {
                for (int dx = 0; dx < n; dx++) {
                    p[dy * GFX_WIDTH + dx] = c;
                }
            }
            continue;
        }

        /* Diagonals BOTH ways, and brightest where two cross - which is
         * what makes it read as light caught on a pane rather than as a
         * pattern printed on one. A single family of lines was almost
         * invisible; the crossings are what the eye picks up.
         *
         * Both diagonals are drawn identically today - no gravity
         * asymmetry between them, and none wanted: this is the surface's
         * fixed woven texture, not the light landing on it. An earlier
         * version favoured whichever one aligned with the board's tilt;
         * see SHINE_PERIOD's own comment above for why a gravity-aligned
         * difference like that turned out to be imperceptible on this
         * grid and was dropped - the shine below is where gravity is
         * expressed now, as a continuously rotating angle rather than a
         * choice between these two.
         *
         * Measured in SCREEN pixels rather than within the block, so the
         * lines run unbroken from one cell into the next instead of
         * restarting at every boundary. That is the whole reason this
         * cannot be constant-folded the way the flat loop above is: the
         * phase depends on where the cell is.
         *
         * Still inside the block as far as the dirty-run tracking is
         * concerned - that works on grid CELLS (row_runs_find below), and
         * every cell is painted whatever its neighbours are, so no run is
         * broken by any of this. */
        const int base = (cx + cy) * n;
        const int diff = (cx - cy) * n;

        /* The shine's own axis, in Q8 screen-pixel units: this cell's
         * origin projected onto the current gravity direction (shine_ux_q8/
         * shine_uy_q8, updated once a frame - see material_shine_direction()
         * in material.h). Computed once per cell, same as base/diff above,
         * so the per-pixel loop below only ever adds dx/dy's own share of
         * the projection. */
        const int shine_base_q8 = (cx * n) * shine_ux_q8 + (cy * n) * shine_uy_q8;

        for (int dy = 0; dy < n; dy++) {
            for (int dx = 0; dx < n; dx++) {
                /* One pixel every eight, both ways. Wide bands were the
                 * first try and buried the pane - half the pixels were
                 * line and a quarter were shine, so the glass itself
                 * barely showed. Thin and sparse reads as light caught on
                 * a surface; thick reads as a pattern printed on one.
                 *
                 * `& 7` rather than a modulo because the period is a power
                 * of two, and it is fine on the negative values `w` takes
                 * left of the diagonal: two's complement just shifts the
                 * phase, which nothing here can tell apart from any other
                 * phase.
                 *
                 * Fixed to the (1, 1)/(1, -1) diagonals regardless of
                 * gravity - this is the WOVEN TEXTURE of the surface, not
                 * the light landing on it, and a texture that rotated with
                 * every tilt would look like the material itself was
                 * turning rather than like a fixed pane being lit from a
                 * new angle. Only the shine below follows gravity. */
                const bool grain = (((base + dx + dy) & 7) == 0) ||
                                   (((diff + dx - dy) & 7) == 0);

                /* SHINE: a band travelling along the CURRENT GRAVITY
                 * DIRECTION, advanced on a clock - see this function's own
                 * top comment for why both halves of that matter. Projecting
                 * (dx, dy) onto shine_ux_q8/shine_uy_q8 is a plain 2D dot
                 * product in Q8 fixed point; the `>> 8` back down to pixel
                 * units is an arithmetic right shift, sign-extending on this
                 * toolchain, so it is fine on the negative projections a
                 * pixel above or left of a cell's origin produces - the same
                 * trust the mask below places in two's complement.
                 *
                 * A mask rather than a modulo because SHINE_PERIOD is a
                 * power of two, and it is fine on the values left of the
                 * origin - two's complement shifts the phase, which nothing
                 * here can tell from any other phase.
                 *
                 * `< n` is the width: one CELL, so the band looks the same
                 * at every quality setting. n is a compile-time constant
                 * here, so this is a comparison against a literal. */
                const int shine_q8 = shine_base_q8 + dx * shine_ux_q8 + dy * shine_uy_q8;
                const int along = ((shine_q8 >> 8) + shine_offset)
                                  & (SHINE_PERIOD - 1);

                /* The band wins wherever it falls, including over the
                 * grain - it is the bright thing, and letting the grain
                 * override it would put dark notches through a highlight. */
                p[dy * GFX_WIDTH + dx] =
                    (along < n) ? col[2] : (grain ? col[1] : col[0]);
            }
        }
    }

    /* THE DOUBLE BUFFER'S OWN SWAP - once per row-call, after every column
     * has been read from and written to, not once per cell: local_depth_
     * cur_row[] just finished holding THIS row's own emerging values (the
     * horizontal-dominant regime's own within-row reads above needed it to
     * keep meaning "this row" for the whole scan), and now becomes "the row
     * painted before this one" for whichever row is processed NEXT - a
     * pointer swap, not a copy, so this costs two loads and two stores
     * regardless of grid_w. See local_depth_row_a[]/local_depth_row_b[]'s
     * own comment above this function for why swapping (rather than always
     * writing into the same array) is what keeps the two regimes' own reads
     * from ever aliasing the slot currently being written. */
    uint8_t *local_depth_tmp = local_depth_cur_row;
    local_depth_cur_row = local_depth_prev_row;
    local_depth_prev_row = local_depth_tmp;

    /* ...and record WHICH ROW that buffer now describes, so the next call
     * can tell whether its own cross-row read is looking at a real
     * neighbour or at whatever the last sweep happened to leave behind -
     * see local_depth_prev_cy's own comment above this function. One store
     * per painted row, beside a swap that already costs four. */
    local_depth_prev_cy = cy;
}

/* cell is chosen on the boot menu, so it is a runtime value here - but the
 * unrolled dy/dx loops in paint_row_n() only unroll when the compiler can
 * see their bound as a constant, and this is the hottest loop in the app,
 * running once per visible pixel of every changed row. Dispatching through a
 * switch gives each arm a literal `n`, so every quality level still gets the
 * unrolled version instead of paying for a runtime-bounded loop here.
 *
 * Every entry in qualities[] must have a matching case here. This switch
 * does not derive its arms from that table - it cannot, since n has to be a
 * compile-time constant - so the two are kept in sync by hand, and adding a
 * quality tier without adding its case is silent breakage: the build stays
 * clean, but the grid is stepped at the new cell size while still being
 * painted at whatever size the default arm falls back to. */
static void paint_row(gfx_color_t *fb, const gfx_color_t *pal, int cy,
                      const uint8_t *row)
{
    switch (cell) {
    case 2:  paint_row_n(fb, pal, cy, row, 2); break;
    case 3:  paint_row_n(fb, pal, cy, row, 3); break;
    case 4:  paint_row_n(fb, pal, cy, row, 4); break;
    case 6:  paint_row_n(fb, pal, cy, row, 6); break;
    case 8:  paint_row_n(fb, pal, cy, row, 8); break;
    /* Not reachable for any cell size in qualities[] above - if it is ever
     * hit, that table grew an entry this switch does not know about, which
     * is a bug there, not here. Falls back to the finest size (2) rather
     * than the coarsest, because 2 is the only n guaranteed to stay inside
     * every buffer regardless of what cell/grid_w/grid_h actually are, so a
     * missing case shows up as a visibly wrong render confined to the
     * buffer instead of as an out-of-bounds write past it. */
    default: paint_row_n(fb, pal, cy, row, 2); break;
    }
}

/* Draws one row, empty cells included, and reports up to ROW_MAX_RUNS
 * cell-index ranges (not pixel ranges) that currently hold material - see
 * row_runs.h for why multiple runs, and row_runs_find()/
 * row_runs_span_fallback() for the mechanism. */
static int draw_one_row(gfx_color_t *fb, const gfx_color_t *pal, int cy,
                        uint16_t *cur_x0, uint16_t *cur_x1)
{
    const uint8_t *row = &grid[cy * grid_w];

    paint_row(fb, pal, cy, row);

    int run_x0[ROW_MAX_RUNS], run_x1[ROW_MAX_RUNS];
    const int n = row_runs_find(row, grid_w, SAND_EMPTY, run_x0, run_x1);
    if (n < 0) {
        int x0, x1;
        row_runs_span_fallback(row, grid_w, SAND_EMPTY, &x0, &x1);
        cur_x0[0] = (uint16_t)x0;
        cur_x1[0] = (uint16_t)x1;
        return 1;
    }

    for (int i = 0; i < n; i++) {
        cur_x0[i] = (uint16_t)run_x0[i];
        cur_x1[i] = (uint16_t)run_x1[i];
    }
    return n;
}


/* Advances the travelling shine, and says whether it moved. */
static bool advance_shine(uint32_t dt_ms)
{
    shine_elapsed_ms += dt_ms;
    if (shine_elapsed_ms < SHINE_STEP_MS) {
        return false;
    }
    const uint32_t steps = shine_elapsed_ms / SHINE_STEP_MS;
    shine_elapsed_ms -= steps * SHINE_STEP_MS;
    shine_offset = (int)(((unsigned)shine_offset + steps * SHINE_STEP_PX)
                         & (SHINE_PERIOD - 1));
    return true;
}

/* Advances the local-depth wake clock, and says whether it fired this
 * frame - see LOCAL_DEPTH_WAKE_MS's own comment above paint_row_n() for why
 * this exists at all. Same accumulate/compare/carry-the-remainder shape as
 * advance_shine() just above, DELIBERATELY SIMPLER: the wake has nothing
 * analogous to shine_offset to advance BY. shine_offset is a travelling
 * band's own position, so advance_shine() has to fold `steps` into it to
 * land in the right place after however many ticks elapsed at once; the
 * wake has no position of its own, only "did a tick land this frame", so
 * `steps` here is consumed purely to reset the carry and never otherwise
 * used - firing at most once per call regardless of how many whole
 * intervals dt_ms actually covered, same as the shine's own tick, just
 * with nothing left to advance once it fires. */
static bool advance_local_depth_wake(uint32_t dt_ms)
{
    local_depth_wake_elapsed_ms += dt_ms;
    if (local_depth_wake_elapsed_ms < LOCAL_DEPTH_WAKE_MS) {
        return false;
    }
    const uint32_t steps = local_depth_wake_elapsed_ms / LOCAL_DEPTH_WAKE_MS;
    local_depth_wake_elapsed_ms -= steps * LOCAL_DEPTH_WAKE_MS;
    return true;
}

static void draw_dirty_rows(bool shine_moved, bool local_depth_woke)
{
    gfx_color_t *fb = gfx_framebuffer();

    /* A row is repainted when something in it CHANGED, and turning the
     * board changes nothing in the grid - a glass wall is static, so its
     * cells are identical before and after a tilt and no row is ever
     * marked. The reflection would then sit frozen at whatever orientation
     * the wall was last built or disturbed at, which is exactly what "the
     * diagonals still don't align with the device tilt" looks like: the
     * direction was right and the pixels were never asked for again.
     *
     * So the orientation is treated as another thing that can dirty a row.
     * Whole screen, because any cell of a hatched material anywhere is now
     * wrong and finding out which would cost a scan of the grid to save a
     * repaint the tilt itself already forces - a direction change mass
     * wakes every block (see sand.c), so the board is re-simulating
     * regardless. It happens only when the direction crosses into a
     * different eighth, not on every frame of a tilt. */
    /* Only the rows that actually hold something hatched, which is what
     * makes an animated shine affordable at all. */
    if (shine_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_shine[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    /* THE SAME MECHANISM, for a liquid interior's local depth instead of a
     * hatched material's shine - see LOCAL_DEPTH_WAKE_MS's own comment above
     * paint_row_n() for the bug this closes (gravity drifting a settled,
     * sleeping block's displayed depth stale with nothing left to mark its
     * rows dirty) and why it needs the same unconditional periodic tick the
     * shine already uses. Only the rows that actually held a liquid cell
     * last time they were painted, for the same affordability reason
     * row_has_shine[] gates the block above - RIM cells included, not only
     * interior ones, per row_has_liquid[]'s own comment (interior-only
     * gating let a row go stale for an unbounded time whenever its liquid
     * happened to read as all-rim). */
    if (local_depth_woke) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_liquid[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    /* 256 entries in flash, indexed by the raw cell byte: no material lookup,
     * no shade arithmetic, no colour conversion, and no RAM. */
    const gfx_color_t *pal = material_palette();

#if CONFIG_LAUNCHER_DEVELOPMENT
    int redrawn = 0;
#endif

    /* Ordinarily ascending - but see local_depth_row_a[]/local_depth_row_b[]'s
     * own comment in paint_row_n() for why a gravity-UP frame walks this
     * loop in the OPPOSITE order instead: local_depth_cur_row[]/local_
     * depth_prev_row[]'s own pointer swap means "the row painted before this
     * one" is only meaningful if rows are actually visited surface-first, or
     * a dirty row far from the surface reads whatever a DIFFERENT, farther
     * row left behind the last time IT was nearer the front of the sweep,
     * not last frame's own value for the row actually being painted. UNLIKE
     * the two-walk design (where only the vertical array needed this and
     * the horizontal one never did), BOTH regimes of the single ray walk
     * depend on row order now - see LOCAL DEPTH's own top comment for why
     * one row-order rule (driven by gy's sign alone) turns out to serve
     * both the vertical-dominant regime's own per-row chain AND the
     * horizontal-dominant regime's own occasional cross-row reads. Just
     * "does gravity point up" - `local_depth_v_reverse` alone - with no
     * "and which regime is active this frame" to ask alongside it: the same
     * rule the two-walk design would have needed one flag for, this one
     * needs for a different, cleaner reason (see paint_row_n()'s own "THE
     * ROW OFFSET, WITHOUT AN ACCUMULATOR"). Reversing here, rather than
     * juggling which array slot means "toward the surface" inside the depth
     * bookkeeping itself, is safe because nothing else in this loop depends
     * on row order - dirty_rows[cy], row_run_x0/x1/n, row_has_shine[cy] and
     * row_has_liquid[cy] are all indexed by cy directly, and
     * gfx_mark_dirty() below only ever unions a bounding box, which does
     * not care what order the boxes arrive in either. */
    const bool reverse_rows = local_depth_v_reverse;

    for (int i = 0; i < grid_h; i++) {
        const int cy = reverse_rows ? (grid_h - 1 - i) : i;
        if (!dirty_rows[cy]) {
            continue;
        }
        dirty_rows[cy] = 0;
#if CONFIG_LAUNCHER_DEVELOPMENT
        redrawn++;
#endif

        uint16_t cur_x0[ROW_MAX_RUNS], cur_x1[ROW_MAX_RUNS];
        const int cur_n = draw_one_row(fb, pal, cy, cur_x0, cur_x1);

        uint16_t *prev_x0 = &row_run_x0[cy * ROW_MAX_RUNS];
        uint16_t *prev_x1 = &row_run_x1[cy * ROW_MAX_RUNS];
        const int prev_n = row_run_n[cy];

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, prev_x0,
                                              prev_x1, prev_n, send_x0,
                                              send_x1);

        /* Written through the raw framebuffer, so gfx cannot see it. Saying
         * so is not optional - a missed mark leaves stale pixels on the
         * panel. */
        for (int i = 0; i < send_n; i++) {
            gfx_mark_dirty(send_x0[i] * cell, cy * cell,
                          (send_x1[i] - send_x0[i]) * cell, cell);
        }

        for (int i = 0; i < cur_n; i++) {
            prev_x0[i] = cur_x0[i];
            prev_x1[i] = cur_x1[i];
        }
        row_run_n[cy] = (uint8_t)cur_n;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    rows_redrawn_total += redrawn;
#endif
}

/* A fixed, saturated colour that appears nowhere in material_palette() (see
 * material.c's colour tables), rather than one derived per-material the way
 * the palette badge is (gfx_color_mix() against the face). At a badge's
 * size that derivation buys contrast against every material's own colour;
 * at a MARKER's size - EMITTER_MARKER_PX pixels, small even next to the
 * badge - there is no room for two nested tones to read as two tones at
 * all, so instead this picks a colour that sits outside the whole palette
 * and therefore reads against anything an emitter happens to be streaming. */
#define EMITTER_MARKER_COLOR 0xFF3EC8

/* The marker's fixed on-screen size, in pixels rather than cells - the same
 * reasoning POUR_RADIUS_PX's comment above gives for the pour/erase
 * brushes: a cell is not a physical size, it is 2 px at ULTRA and 8 px at
 * VERY LOW for the same object, so a marker drawn "one cell wide" would be
 * a different physical mark at every quality setting, and it would shrink
 * to nearly nothing at ULTRA specifically - the opposite of what a marker
 * that has to be findable by a finger needs. Findability is a property of
 * the finger, not of the grid, so the marker gets a size the grid has no
 * say over.
 *
 * Tuned by eye. At HIGH's 3 px cells this spans about four cells across,
 * so it does sit over a little of what the source underneath is actually
 * producing - an accepted trade for being visible at all, not an
 * oversight. */
#define EMITTER_MARKER_PX  12

/* Marks every placed emitter with a small square of fixed physical size -
 * see EMITTER_MARKER_PX and EMITTER_MARKER_COLOR above for its size and
 * colour. A placed tap that cannot be seen is just an invisible point the
 * sand happens to keep coming from, which leaves no way to tell where it is
 * or whether the tap even took.
 *
 * Called every frame, not once: draw_dirty_rows() just above repaints
 * whatever row an emitter sits in every time the simulation actually
 * changes something there - which is nearly every step the tap is active -
 * so a marker drawn only once would be eaten the very next time that row
 * redraws. This is exactly why draw_mode_label() below is drawn after the
 * rows rather than before - see its own comment.
 *
 * Cheap regardless: at most SAND_MAX_EMITTERS markers, each
 * EMITTER_MARKER_PX pixels square, so the drawing cost here is negligible.
 * Unlike the old one-cell marker, this one is bigger than the emitter's own
 * cell at every quality, so it can now cover a few pixels of neighbouring
 * cells that would not otherwise have gone out to the panel this frame -
 * a small, bounded addition next to the mostly-free ride the smaller
 * marker got from output the tap was already sending. */
static void draw_emitter_markers(void)
{
    const gfx_color_t marker = gfx_rgb(EMITTER_MARKER_COLOR);
    const int count = sand_emitter_count(&sim);

    for (int i = 0; i < count; i++) {
        int ex, ey;
        cell_t ecell;
        if (!sand_emitter_at(&sim, i, &ex, &ey, &ecell)) {
            continue;   /* not expected - see sand_emitter_count()'s contract */
        }
        (void)ecell;    /* the marker's colour is fixed, not the material's */

        /* Centred on the emitter's CELL centre, not its top-left corner -
         * the marker is now bigger than a cell at every quality, so
         * starting it at the corner would put the emitter visibly off to
         * the marker's lower-right instead of in its middle. */
        const int mid_x = ex * cell + cell / 2;
        const int mid_y = ey * cell + cell / 2;
        const int px = mid_x - EMITTER_MARKER_PX / 2;
        const int py = mid_y - EMITTER_MARKER_PX / 2;
        gfx_fill_rect(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX, marker);
        gfx_mark_dirty(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX);
    }
}

/* The colour a brush represents - the material's own colour, read straight
 * out of the palette so neither this nor draw_mode_label() below can
 * disagree with what actually comes out of the finger. Shared by both,
 * rather than each computing its own, for that same reason.
 *
 * An ordinary brush cell carries no shade of its own - every entry in
 * brushes[] is CELL_MAKE(mat, 0) - so this substitutes a representative
 * shade (13 of 16) rather than showing variant zero specifically. An
 * extended cell cannot take that shortcut: for a MAT_EXTENDED cell the low
 * nibble names WHICH extended material this is, not a shade, so bumping it
 * the way an ordinary variant is bumped would silently turn one extended
 * material into a different one. material_palette() is indexed by the raw
 * cell byte, so an extended cell is simply looked up as itself instead. */
static gfx_color_t brush_color(cell_t c)
{
    return material_palette()[
        cell_is_extended(c) ? c : CELL_MAKE(CELL_MATERIAL(c), 13)];
}

/* Which quarter turn currently reads as "upright" - 0/1/2/3 the same way
 * gfx_text_turned() numbers them (0 upright, 1 top-to-bottom, 2 upside down,
 * 3 bottom-to-top). Up is the opposite of gravity: compare magnitudes rather
 * than an angle, since whichever component of gravity dominates names the
 * axis, and its sign names the edge. Snapped to one of four because the font
 * can only be turned in quarters - a diagonal tilt picks whichever quarter
 * it is nearest.
 *
 * Used only by draw_mode_label() below now, to turn its text to follow the
 * board's physical "up". The palette panel used to share this - a second
 * caller of exactly this function - but no longer computes its own turn at
 * all; see draw_palette()'s own top comment for why, and display.h for the
 * module that decides orientation now.
 *
 * NOT the same decision display.h makes, and deliberately so - see
 * draw_mode_label()'s own comment just below for why this plain snap-to-
 * nearest is still the right tool here even though display_update()'s
 * hysteresis exists. */
static int gravity_quarter_turn(int gx, int gy)
{
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ay >= ax) {
        return (gy >= 0) ? 0 : 2;      /* down is down : board upside down */
    }
    return (gx >= 0) ? 3 : 1;          /* down is to the right : to the left */
}

/* Draws the mode label against whichever edge is currently UP.
 *
 * Up is the opposite of gravity, so the label follows the device rather than
 * the screen: turn the board on its side and the label moves to what is now
 * the top, and turns with it so it still reads the right way up. Anything else
 * looks like a bug the moment the device is not held upright.
 *
 * Snapped to one of four, because the font can only be turned in quarters -
 * a diagonal tilt picks whichever quarter it is nearest.
 *
 * STILL ITS OWN gravity_quarter_turn() CALL - NOT display_shell_quarter()
 *
 * This looks like an inconsistency next to draw_palette(), which now just
 * inherits the shell's orientation instead of computing one. It is not: the
 * two draw through different paths. draw_palette() goes through microui,
 * which every other described UI in this shell also goes through, and which
 * is exactly what ui_set_transform() reaches - that is the whole reason the
 * shell owning the transform is enough to turn the palette. draw_mode_label()
 * instead calls gfx_text_turned() straight onto the app's own canvas,
 * bypassing microui and the UI transform entirely, the same way the sand
 * grid itself is painted. A transform set on the UI layer has no effect on
 * either. So this is exactly the canvas-versus-chrome line: the palette is
 * chrome (drawn through microui, like the launcher and every boot menu), the
 * label is canvas (drawn straight onto this app's own framebuffer region,
 * like the sand grid itself), and only chrome follows the shell's transform
 * for free. The label keeps turning itself because nothing else will. */
static void draw_mode_label(int gx, int gy)
{
    /* The material's own name, so the label says what the finger will do
     * rather than merely that something changed - and, in BRUSH_SPAWN,
     * " SOURCE" appended so it also says WHICH thing: a tap that keeps
     * running, not a blob poured once. `len`/`span` just below are
     * computed from strlen(text), not hardcoded, so the longer text this
     * local buffer can hold is picked up automatically - nothing about the
     * layout below needs to know a suffix exists. */
    char text_buf[24];
    const char *text;
    if (ui.mode == SAND_MODE_DETONATE) {
        text = "DETONATE";   /* scaffolding - see sand_mode_t in sand_ui.h */
    } else if (ui.mode == SAND_MODE_ERASE) {
        text = "ERASE";
    } else if (ui.modes[ui.brush] == BRUSH_SPAWN) {
        snprintf(text_buf, sizeof text_buf, "%s SOURCE",
                material_name(brushes[ui.brush]));
        text = text_buf;
    } else {
        text = material_name(brushes[ui.brush]);
    }
    const int   len  = (int)strlen(text);
    const int   span = len * 8 * LABEL_SCALE;
    const int   tall = 8 * LABEL_SCALE;

    /* Which edge is up - see gravity_quarter_turn()'s own comment above for
     * why this must come from that shared function and not a second copy of
     * its ax/ay comparison. Only the 0..3 choice moved there; the positions
     * below, one per turn, stay exactly as they were. */
    const int turn = gravity_quarter_turn(gx, gy);

    int x, y;
    switch (turn) {
    case 0:                                             /* down is down */
        x = (GFX_WIDTH - span) / 2;
        y = LABEL_MARGIN;
        break;
    case 2:                                             /* board upside down */
        x = (GFX_WIDTH + span) / 2 - 8 * LABEL_SCALE;
        y = GFX_HEIGHT - LABEL_MARGIN - tall;
        break;
    case 3:                                             /* down is to the right */
        x = LABEL_MARGIN;
        y = (GFX_HEIGHT + span) / 2 - 8 * LABEL_SCALE;
        break;
    default:                                            /* turn == 1: down is to the left */
        x = GFX_WIDTH - LABEL_MARGIN - tall;
        y = (GFX_HEIGHT - span) / 2;
        break;
    }

    /* Coloured as the material itself, so the label needs no colour table of
     * its own - see brush_color()'s own comment for why an extended cell
     * cannot just have its variant bumped like an ordinary one. ERASE and
     * DETONATE have no material to read a colour from, so each gets one
     * picked by hand instead - a warmer orange for the corrective eraser, a
     * hotter red for DETONATE since it is the one mode here that can
     * actually rearrange the board. */
    gfx_color_t ink;
    if (ui.mode == SAND_MODE_DETONATE) {
        ink = gfx_rgb(0xFF3B3B);
    } else if (ui.mode == SAND_MODE_ERASE) {
        ink = gfx_rgb(0xFF8A5C);
    } else {
        ink = brush_color(brushes[ui.brush]);
    }

    gfx_text_turned(x, y, text, ink, LABEL_SCALE, turn);
}

/* Gap left between adjacent tiles, and around an unselected tile's swatch.
 * The panel paints no background of its own (see draw_palette()), so what
 * shows through here is the frozen sand frame underneath - which is what
 * separates one tile from the next.
 *
 * Kept deliberately narrow. The gap is scenery; the tiles are the point,
 * and every pixel spent widening it comes straight off a touch target that
 * is already only 92 px on a 322 ppi panel. */
#define PALETTE_GROUT   4

/* Thickness of each bezel edge - matches UI_BEZEL_THICKNESS (ui_style.h),
 * which is what actually draws each tile's bezel now (see draw_palette()
 * below and ui_bezel_spans()). Kept as its own constant here because the
 * badge's position is measured off it directly - the badge sits just inside
 * the bezel's inner edge - and that geometry belongs to this file's own
 * layout, not to the bezel style. */
#define PALETTE_BEZEL   3

/* The eligibility/spawn corner badge - see draw_palette()'s own comment on
 * its three states. 18px outer square against a 92px tile (PALETTE_TILE)
 * reads clearly at arm's length without crowding the tile's centred name
 * text; 2px of border leaves a 14px inner square, still comfortably legible
 * as its own square rather than a blur, and is the "1-2 px" a border needs
 * to read as a border rather than as another fill. 2px of margin off the
 * bezel keeps the badge from touching it. */
#define PALETTE_BADGE_SIZE    18
#define PALETTE_BADGE_INSET    2
#define PALETTE_BADGE_MARGIN   2

/* Fixed, not derived from the face - see the badge's own comment in
 * draw_palette() for why. gfx_color_mix() against a near-white face
 * (Snow) used to land the "armed" fill almost exactly on the face colour,
 * so armed and unarmed read as identical on that one tile. Maximum-
 * contrast pair, the same choice the tile names below already make and for
 * the same reason - see their own comment. */
#define PALETTE_BADGE_BORDER_COLOR  0x141414
#define PALETTE_BADGE_FILL_COLOR    0xF2F2F2

/* mu_Color from a 0xRRGGBB value, opaque - the microui drawing calls
 * draw_palette() below issues all work in 8-bit mu_Color, never in the
 * panel's own gfx_color_t, so every colour handed to them crosses that
 * boundary once here. Takes a plain 0xRRGGBB (a compile-time badge
 * constant, or brush_color()'s gfx_color_t already unpacked through
 * gfx_color_rgb888() - see that function's own comment in gfx_color.h for
 * why bit replication is what makes the unpacking exact). */
static mu_Color mu_color_hex(uint32_t rgb)
{
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF),
                    (int)(rgb & 0xFF), 255);
}

/* The material picker overlay - now a described microui frame like every
 * other UI in the shell (see ui_launcher.c), rebuilt every frame while
 * SAND_UI_PALETTE is the active screen rather than drawn on open, on
 * selection and on a quarter-turn change. ui_end() only repaints when the
 * command list actually changed, so a held-steady panel still costs nothing
 * - see ui.h's own top comment on that skip. sand_frame()'s SAND_UI_PALETTE
 * handling calls this unconditionally now; there is no more stored
 * "last drawn at this turn" to compare against.
 *
 * EACH TILE IS A REAL mu_button() NOW
 *
 * Used to be a manually bezelled rect, hit-tested separately by
 * palette_hit() on raw screen coordinates - see sand_ui.h's own "WHO
 * HIT-TESTS AND WHO DECIDES" comment for why that split existed and why it
 * does not any more. microui now lays each tile out AND hit-tests it,
 * through the very same mu_button() every other button in this shell
 * already uses; a click just tells this loop which tile index to hand to
 * sand_ui_tile_clicked(), which is where "what a click on this tile means"
 * still lives, unchanged, and still host-tested - see suite_sand_ui.c.
 *
 * THE WHOLE PANEL TURNS WITH THE BOARD - BUT NOT BY DECIDING SO ITSELF
 *
 * This function used to compute its own quarter turn from gravity
 * (gravity_quarter_turn()) and push it with ui_set_transform() before
 * ui_begin(), which is what made every tile drawn below - and hit-tested,
 * through the same transform - turn with the board. That decision now
 * belongs to the shell (see display.h and main.c's display sampling): by
 * the time this function runs, main.c has already called ui_set_transform()
 * for whatever quarter is current, so the panel simply inherits it. No
 * `turn` parameter, no ui_set_transform() call here any more - there would
 * be nothing for this function to base one on that display_shell_quarter()
 * does not already know, and a second opinion here could only disagree with
 * the shell's.
 *
 * Every tile still hit-tests through microui the same
 * feed_input()-through-the-inverse-transform path every other described UI
 * in this shell takes (see ui.c's "Touch to mouse" comment), so turning the
 * shell's transform turns this panel's hit-testing right along with its
 * drawing - nothing here hard-codes physical screen coordinates the way
 * palette_hit() used to. Nothing in this loop branches on the turn by hand,
 * either: draw_command() in ui.c derives the quarter turn from whatever
 * transform is in force and picks gfx_text_font()'s turned glyph path
 * accordingly, so mu_button()'s own centred label - and every rect this
 * loop draws - comes out turned for free. */
static void draw_palette(const input_t *input)
{
    mu_Context *ctx = ui_context();

    ui_begin(input);

    /* The tile names get their halo from the UI layer now - see
     * ui_text_halo() in ui_style.h: it derives a light halo from a dark ink
     * (and vice versa), which is what the nine hand-rolled draws this used
     * to replace did by hand for exactly one ink colour (black). Restored to
     * UI_TEXT_PLAIN wherever this panel is torn down - see sand_frame()'s
     * SAND_UI_CLOSE_PALETTE handling. */
    ui_set_text_style(UI_TEXT_OUTLINED);

    /* Bezelled, like every other button in this shell (see
     * ui_launcher.c) - raised at rest, and inverted for the one frame
     * microui's own hover/focus state says a finger is actually on it (see
     * styled_draw_frame() in ui.c). That per-tap press feedback is new
     * here; the hand-rolled panel never had it. It is NOT what marks a
     * tile as the selected brush, though - selection is this app's own
     * idea, not microui's, so it gets its own cue below, drawn after each
     * mu_button() rather than left to this style. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    /* MU_COLOR_BUTTON/MU_COLOR_TEXT are ONE shared slot on the context, not
     * one per button: mu_draw_control_frame()/mu_draw_control_text() read
     * straight out of ctx->style->colors[] at the moment a button's own
     * commands are BUILT (i.e. inside mu_button() below), which is what
     * lets this loop give every tile its own face and have that face baked
     * correctly into that tile's own command. But it also means whatever
     * this loop leaves in those two slots is what the NEXT thing to read
     * them - the boot menu's own START/QUALITY buttons, the next time this
     * app shows them - would draw with too. Saved once here and restored
     * once after the loop, rather than reset per iteration, since nothing
     * between tiles needs the old value back. */
    const mu_Color saved_button_color = ctx->style->colors[MU_COLOR_BUTTON];
    const mu_Color saved_text_color   = ctx->style->colors[MU_COLOR_TEXT];

    /* Black, for every tile - mu_button() draws each tile's own name in
     * this colour, and ui_text_halo() (armed by UI_TEXT_OUTLINED above)
     * derives a light halo from it at render time, so a dark ink still
     * reads against every swatch this panel can show - the same choice the
     * hand-rolled label draw used to make explicitly. Unlike the face
     * below this does not vary per tile, so it is set once here rather
     * than inside the loop. */
    ctx->style->colors[MU_COLOR_TEXT] = mu_color(0, 0, 0, 255);

    /* ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT - see ui.h: the
     * logical canvas swaps dimensions under a quarter-turn transform, the
     * same reasoning draw_menu() below already follows.
     *
     * cols is recomputed from ui_width() every time this function runs, not
     * cached anywhere - immediate mode already redescribes the whole panel
     * every frame (see this function's own top comment), so a plain call
     * here is all a genuinely resized canvas needs to be picked up; nothing
     * has to compare against a remembered value the way palette_drawn_quarter
     * still does below for the ghost-tile fix, which is about a stale
     * FOOTPRINT left in the framebuffer, not about this loop's own layout
     * math being wrong. */
    const int cols = palette_cols(ui_width());

    if (ui_begin_screen(ctx, "Sand Palette",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        for (int i = 0; i < BRUSH_COUNT; i++) {
            int x, y, w, h;
            palette_tile_rect(i, BRUSH_COUNT, cols, ui_width(), ui_height(),
                              &x, &y, &w, &h);

            /* Inset by the grout first so neighbouring tiles do not fuse
             * into one surface - the button itself, bezel included, is
             * laid out inside that inset rect, the same rect the old
             * hand-rolled bezel used to be drawn into by hand. */
            const int ix = x + PALETTE_GROUT;
            const int iy = y + PALETTE_GROUT;
            const int iw = w - 2 * PALETTE_GROUT;
            const int ih = h - 2 * PALETTE_GROUT;

            const mu_Color face =
                mu_color_hex(gfx_color_rgb888(brush_color(brushes[i])));
            ctx->style->colors[MU_COLOR_BUTTON] = face;

            /* The button IS the tile now: mu_layout_set_next() places it at
             * (ix,iy,iw,ih), mu_button() hit-tests it against microui's own
             * mouse state - fed from touch by feed_input() in ui.c, mapped
             * through the inverse transform there - draws its bezelled
             * face via styled_draw_frame(), and draws its own centred
             * label. That label doubles as the button's id (mu_get_id() in
             * microui.c hashes the label string): every brush in
             * BRUSH_COUNT has a distinct display name - Sand, Water,
             * Stone, Gas, Fire, Wood, Oil, Lava, Acid, Glass, Snow, Dirt,
             * Ice, Plant (material.c's own name tables) - so no two tiles
             * this loop draws can ever hash to the same id. */
            const char *name = material_name(brushes[i]);
            mu_layout_set_next(ctx, mu_rect(ix, iy, iw, ih), 0);
            const int clicked = mu_button(ctx, name);

            /* Hand the click over to sand_ui_tile_clicked() BEFORE this
             * tile draws its own selection ring and badge below, so a
             * toggle or a fresh selection shows up in THIS tile's own
             * pixels on the very same frame it happened, rather than
             * lagging a frame behind - see sand_ui.h's "WHO HIT-TESTS AND
             * WHO DECIDES" comment for what this call actually decides.
             * The return value goes unused here, the same as
             * SAND_UI_REDRAW_PALETTE already went unread from
             * sand_ui_step(): ui_end() below repaints on its own whenever
             * the command list this loop builds actually changed. */
            if (clicked) {
                sand_ui_tile_clicked(&ui, i);
            }

            /* The selection cue. ui_style's own bezel already inverts a
             * button's lit/shadowed edge pair, but only while microui's
             * hover/focus says a finger is on it THIS frame (see
             * styled_draw_frame() in ui.c) - there is no "selected" state
             * for a plain mu_button() to draw, since selection is this
             * app's own idea, not microui's. So the selected tile gets a
             * second, purpose-drawn cue on top: ui_bezel_spans() again,
             * called for its SUNKEN edge pair only (spans[1..4] - span[0]
             * is the flat face, already painted by mu_button() above, so
             * it is skipped rather than redrawn on top of itself).
             *
             * Those edges are mixed toward white and toward black off THIS
             * TILE'S OWN face colour (see UI_BEZEL_HIGHLIGHT/UI_BEZEL_SHADOW
             * in ui_style.h), not a fixed colour - which is what keeps the
             * ring visible at both ends of the swatch range: on Snow, a
             * near-white face, it is the mixed-toward-black edge that
             * reads; on Stone, a near-black face, it is the
             * mixed-toward-white edge that reads. A fixed light or dark
             * ring would vanish on one of those two the same way the old
             * white selection ring did, and the same way the badge's own
             * face-derived fill did before it was changed to a fixed pair
             * for the same reason - see PALETTE_BADGE_BORDER_COLOR/
             * PALETTE_BADGE_FILL_COLOR's own comment on that history. This
             * ring gets to derive from the face instead of needing a fixed
             * pair of its own, because unlike the badge it never has to
             * sit ON a face of unknown lightness - it sits at the tile's
             * own edge, always paired with the one face it was mixed
             * from, so there is no separate swatch it has to stay legible
             * against. */
            if (i == ui.brush) {
                ui_span_t spans[UI_BEZEL_MAX_SPANS];
                const int n = ui_bezel_spans(mu_rect(ix, iy, iw, ih), face,
                                             true, spans, UI_BEZEL_MAX_SPANS);
                for (int s = 1; s < n; s++) {
                    mu_draw_rect(ctx, spans[s].rect, spans[s].color);
                }
            }

            /* The badge: zero or more rects (and an icon) drawn after the
             * bezel, at the tile's top-right corner inside it. Three
             * states, not one:
             *
             *   eligible, BRUSH_POUR    an empty box - a near-black border
             *                           (PALETTE_BADGE_BORDER_COLOR) around
             *                           a near-white fill (PALETTE_BADGE_
             *                           FILL_COLOR). The slot exists;
             *                           nothing is armed.
             *   eligible, BRUSH_SPAWN   the same box, with a near-black
             *                           check mark drawn inside it (
             *                           MU_ICON_CHECK - draw_command()
             *                           routes that to icons.h's
             *                           icon_check(), the same artwork the
             *                           diagnostics app's checkboxes use).
             *                           The tap is armed.
             *   not eligible            nothing, as before -
             *                           material_can_emit() is false for
             *                           every KIND_STATIC material (stone,
             *                           glass, the whole extended range),
             *                           so the absence of a badge is what
             *                           makes that eligibility rule visible
             *                           on the panel instead of a fact
             *                           someone has to be told separately.
             *
             * The border and fill are a FIXED pair, not derived from the
             * face the way the bezel above is - see PALETTE_BADGE_BORDER_
             * COLOR/PALETTE_BADGE_FILL_COLOR's own comment for why: the
             * badge used to derive from the face the same way the bezel
             * still does, and it read fine on most swatches, but Snow's
             * face is itself near-white, so mixing toward white for the
             * armed fill landed almost exactly on the face colour - armed
             * and unarmed became indistinguishable on the one tile where
             * telling them apart matters most. A mark that has to read on
             * every swatch from snow's near-white to stone's near-black
             * cannot itself be made of the swatch it sits on.
             *
             * Laid out relative to the tile's own logical (ix, iy) corner,
             * like everything else in this loop - see this function's own
             * top comment on why that is enough to turn with the board: the
             * badge's rects go through draw_command() exactly like the
             * bezel spans above, so a quarter-turn transform carries this
             * along with the tile it sits on without this loop naming a
             * turn anywhere. */
            if (material_can_emit(brushes[i])) {
                const mu_Color border = mu_color_hex(PALETTE_BADGE_BORDER_COLOR);
                const mu_Color fill   = mu_color_hex(PALETTE_BADGE_FILL_COLOR);
                const int bx = ix + iw - PALETTE_BEZEL - PALETTE_BADGE_MARGIN
                             - PALETTE_BADGE_SIZE;
                const int by = iy + PALETTE_BEZEL + PALETTE_BADGE_MARGIN;
                const mu_Rect badge_rect =
                    mu_rect(bx, by, PALETTE_BADGE_SIZE, PALETTE_BADGE_SIZE);

                mu_draw_rect(ctx, badge_rect, border);
                mu_draw_rect(ctx,
                            mu_rect(bx + PALETTE_BADGE_INSET,
                                    by + PALETTE_BADGE_INSET,
                                    PALETTE_BADGE_SIZE - 2 * PALETTE_BADGE_INSET,
                                    PALETTE_BADGE_SIZE - 2 * PALETTE_BADGE_INSET),
                            fill);

                if (ui.modes[i] == BRUSH_SPAWN) {
                    mu_draw_icon(ctx, MU_ICON_CHECK, badge_rect, border);
                }
            }

            /* The name itself is no longer drawn here - mu_button() already
             * drew it, centred in (ix,iy,iw,ih), when it ran above (see
             * this loop's own comment at that call site). There is still no
             * per-turn positioning to do here: palette_label_origin()
             * existed to centre a string at any of four quarter turns by
             * hand, and that hand-rolled centring is exactly what
             * mu_draw_control_text()'s plain centring plus draw_command()'s
             * own turn handling replaces - see this function's own top
             * comment. draw_command() reads the quarter straight off
             * whatever transform is currently in force - main.c's, now, not
             * this function's own - and picks gfx_text_font()'s turned glyph
             * path itself, so this loop never has to know which turn is in
             * force to get a centred, correctly turned label. */
        }

        mu_end_window(ctx);
    }

    /* Restores what this loop borrowed - see the comment above
     * saved_button_color/saved_text_color for why leaving either mutated
     * would leak into the next thing drawn with MU_COLOR_BUTTON/
     * MU_COLOR_TEXT. */
    ctx->style->colors[MU_COLOR_BUTTON] = saved_button_color;
    ctx->style->colors[MU_COLOR_TEXT]   = saved_text_color;

    /* UI_NO_BACKGROUND is what keeps the frozen sand showing through
     * PALETTE_GROUT's gap between tiles - the same effect the hand-rolled
     * version got by simply never painting a panel-wide background fill.
     * Safe for the same reasons that comment gave: the simulation is
     * paused for as long as this panel is open (see sand_frame()'s
     * SAND_UI_PALETTE handling), so the frame underneath is frozen and
     * cannot bleed through or flicker, and every tile paints its own bezel
     * and face opaquely within its grout inset, so a repaint fully covers
     * whatever that tile last looked like. Closing the panel is unaffected
     * either way: sand_frame()'s SAND_UI_CLOSE_PALETTE handling reseeds
     * every row's runs to full width and marks everything dirty, so the
     * sand repaints in full regardless of what this panel did or did not
     * cover while it was open. */
    ui_end(UI_NO_BACKGROUND);
}

/*---------------------------------------------------------------------------
 * Frame
 *-------------------------------------------------------------------------*/

/* Where is down, and how hard? The GYROSCOPE says how fast the board is
 * turning, which sets how quickly the tilt filter tracks a genuine
 * reorientation. It is deliberately not what shaking is read from - see
 * tilt.h, and the note on rotating not being shaking.
 *
 * Falls back to straight down at full speed when there is no sensor. */
static void read_gravity_input(uint32_t dt_ms, imu_sample_t *sample, int *gx,
                               int *gy, int *flow, int *jostle,
                               int *rotation)
{
    *gx = 0;
    *gy = IMU_COUNTS_PER_G;
    *flow = 256;
    *jostle = 0;
    *rotation = 0;

    if (!imu_ready() || !imu_read(sample)) {
        return;
    }

    *rotation = imu_rotation_level(sample);

    /* Smooth the raw vector before anything looks at it, and hand tilt the
     * through-screen axis too: without it a device lying on a table is
     * indistinguishable from one in free fall. */
    tilt_update(&tilt, GRAVITY_SCREEN_X(sample), GRAVITY_SCREEN_Y(sample),
                sample->az, *rotation, dt_ms);

    *gx   = tilt_x(&tilt);
    *gy   = tilt_y(&tilt);
    *flow = tilt_strength(&tilt);

    const int shake = tilt_shake(&tilt);
    *jostle = shake > SHAKE_DEADZONE ? shake : 0;
}

/* Everything that decides WHICH edges open the palette, close it, select a
 * tile or toggle its mode now lives in sand_ui.c's sand_ui_step() - the
 * swallow-release guard, the brush/mode comparison that decides whether the
 * closing label shows, all of it - so it can be host-tested (see
 * suite_sand_ui.c and its own top comment on the four bugs this used to
 * ship). What is left here is CARRYING OUT what sand_ui_step() asks for:
 * sand_frame()'s dispatch below reads the returned action bits and does the
 * gfx/IMU/simulation work sand_ui_step() cannot do itself - see sand_ui.h's
 * own top comment on why the split sits where it does. */

/* Capped rather than looped to exhaustion: after a long frame the backlog is
 * dropped instead of dumping a pile in one go. */
static void handle_pour_input(const input_t *input, uint32_t dt_ms)
{
    if (ui.mode == SAND_MODE_DETONATE) {
        /* Fires on the press EDGE, not through the `applications` catch-up
         * loop below. That loop runs every frame while a finger is held, so
         * wiring DETONATE through it would detonate continuously for as
         * long as the screen is touched - a fine stress test (see the
         * plan's "full screen of packed sand, then rapid repeat presses"),
         * useless for looking at any ONE blast, which is the entire reason
         * this mode exists. input->pressed is the touch-down edge - true
         * for exactly one frame per tap - so this is one sand_explode()
         * call per press, however long the finger then stays down. */
        pour_accumulator_ms = 0;   /* do not let held time leak into paint/erase */
        if (input->pressed) {
            const int cx = input->x / cell;
            const int cy = input->y / cell;
            sand_explode(&sim, cx, cy, (DETONATE_RADIUS_PX + cell / 2) / cell);
        }
        return;
    }

    if (!input->down) {
        pour_accumulator_ms = 0;
        return;
    }

    /* A tap that places an emitter is a different action from pouring, and
     * takes over the touch entirely rather than sharing it: erasing still
     * wins outright (an emitter under the eraser is exactly what
     * sand_erase() already turns off, below), but once neither applies,
     * spawn mode skips the accumulator/pour path for this frame completely
     * - it must never both place a tap and pour a blob from the same
     * touch. */
    if (ui.mode == SAND_MODE_PAINT && ui.modes[ui.brush] == BRUSH_SPAWN) {
        /* Only on the PRESS edge, never every frame the finger stays down
         * - one tap places one tap. input->pressed fires exactly once per
         * physical press, so this call (and the log below, if it fails)
         * happens at most once per press for free - placing on every frame
         * instead would exhaust SAND_MAX_EMITTERS in a fraction of a
         * second and turn one drag into a dozen or more taps. */
        if (input->pressed) {
            const int cx = input->x / cell;
            const int cy = input->y / cell;
            if (!sand_add_emitter(&sim, cx, cy, brushes[ui.brush])) {
                ESP_LOGW(TAG, "emitter list full (%d) - tap ignored",
                         SAND_MAX_EMITTERS);
            }
        }
        return;
    }

    pour_accumulator_ms += dt_ms;

    int applications = (int)(pour_accumulator_ms / POUR_STEP_MS);
    if (applications > SIM_MAX_CATCHUP) {
        applications = SIM_MAX_CATCHUP;
        pour_accumulator_ms = 0;
    } else {
        pour_accumulator_ms -= (uint32_t)applications * POUR_STEP_MS;
    }

    const int cx = input->x / cell;
    const int cy = input->y / cell;
    /* The radii are defined in pixels and divided down here rather than
     * defined in cells, so a finger's-width brush stays a finger's width on
     * screen at every quality - a cell-based radius would instead have
     * covered twice the physical area at NORMAL that it does at ULTRA, since
     * a NORMAL cell is twice as many pixels across.
     *
     * Rounded to nearest (+ cell / 2 before dividing) rather than truncated,
     * because this is a physical size in pixels being converted to a count
     * of cells, and plain integer division biases that count small at every
     * quality where cell does not divide the radius evenly - a small bias at
     * 3 or 4 px, and a 40% shrink at 6 px, where 10 / 6 truncates to 1
     * instead of rounding to 2. Never rounds to 0 for any quality in the
     * table: the smallest result is POUR_RADIUS_PX at the coarsest cell,
     * (10 + 4) / 8 = 1. */
    for (int i = 0; i < applications; i++) {
        if (ui.mode == SAND_MODE_ERASE) {
            sand_erase(&sim, cx, cy, (ERASE_RADIUS_PX + cell / 2) / cell);
            /* Wider than the sweep above on purpose - see
             * ERASE_EMITTER_RADIUS_PX's own comment for why a point target
             * needs more aiming tolerance than an area sweep does. */
            sand_remove_emitters(&sim, cx, cy,
                                 (ERASE_EMITTER_RADIUS_PX + cell / 2) / cell);
        } else {
            sand_spawn_cell(&sim, cx, cy,
                            (POUR_RADIUS_PX + cell / 2) / cell,
                            brushes[ui.brush]);
        }
    }
}

/* Logs the direction whenever the NEAREST of the eight changes. Quiet when
 * the board is still, and it is what the axis mapping above was verified
 * against. The simulation itself uses the dithered direction, which changes
 * every frame by design and would be useless to log. */
static void log_direction_change(int gx, int gy, int jostle,
                                 const imu_sample_t *sample)
{
    static int last_dx = 99, last_dy = 99;
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    if (dx == last_dx && dy == last_dy) {
        return;
    }
    ESP_LOGI(TAG, "down is (%+d,%+d)  smoothed (%+6d,%+6d)  "
                  "raw (%+6d,%+6d)  shake %d",
             dx, dy, gx, gy, sample->ax, sample->ay, jostle);
    last_dx = dx;
    last_dy = dy;
}

/* Fixed-timestep accumulator, with the rate scaled by how hard gravity is
 * pulling in the plane of the screen.
 *
 * A grain moves one cell per step whatever gravity is doing, so steps per
 * second IS the speed of the sand. Scaling them by tilt is what gives the
 * simulation a throttle instead of an on/off switch: laid flat it coasts to
 * a stop over a moment rather than freezing between one frame and the next.
 * It is also the real behaviour, since a grain on a tray tilted by theta is
 * driven by g*sin(theta). */
static void run_sim_steps(int gx, int gy, int jostle, int flow,
                          uint32_t dt_ms)
{
    sim_accumulator_q8 += dt_ms * (uint32_t)flow;
    int steps = (int)(sim_accumulator_q8 / (SIM_STEP_MS * 256));
    if (steps > SIM_MAX_CATCHUP) {
        steps = SIM_MAX_CATCHUP;
        sim_accumulator_q8 = 0;      /* give up on the backlog */
    } else {
        sim_accumulator_q8 -= (uint32_t)steps * SIM_STEP_MS * 256;
    }

    for (int i = 0; i < steps; i++) {
        sand_step(&sim, gx, gy, jostle);
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    steps_total += steps;
#endif
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* TEMPORARY - counts how many of block_cols*block_rows blocks are awake
 * (not sand_block_settled(), about to be examined at full cost) right
 * now, and how many occupied cells sit inside those awake blocks
 * specifically - the count that actually drives step_one_row()'s cost,
 * since it walks every occupied cell in a block it doesn't skip outright
 * rather than paying one fixed cost per awake block. Must be called after
 * a step. Superseded the row version of this same diagnostic once
 * sleeping itself became block-shaped - see sand_enable_sleeping(). */
/* How many occupied cells sit in block (bx,by)'s own clipped span - split
 * out of count_awake() below purely to keep that loop's own complexity
 * down, not because this is reused elsewhere. */
static int count_occupied_in_block(int bx, int by)
{
    const int x0 = bx * SAND_BLOCK_W;
    const int x1 = (x0 + SAND_BLOCK_W < grid_w) ? x0 + SAND_BLOCK_W : grid_w;
    const int y0 = by * SAND_BLOCK_H;
    const int y1 = (y0 + SAND_BLOCK_H < grid_h) ? y0 + SAND_BLOCK_H : grid_h;

    int cells = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = &grid[(size_t)y * grid_w];
        for (int x = x0; x < x1; x++) {
            if (row[x] != SAND_EMPTY) {
                cells++;
            }
        }
    }
    return cells;
}

static void count_awake(int *out_blocks, int *out_cells)
{
    int blocks = 0, cells = 0;
    for (int by = 0; by < block_rows; by++) {
        for (int bx = 0; bx < block_cols; bx++) {
            if (sand_block_settled(&sim, bx, by)) {
                continue;
            }
            blocks++;
            cells += count_occupied_in_block(bx, by);
        }
    }
    *out_blocks = blocks;
    *out_cells  = cells;
}

/* TEMPORARY - see the pour_step_us_total etc. declarations above. Buckets
 * this frame's already-measured step/draw cost, and how many rows (and
 * occupied cells within them) were awake, by whether a pour was actually
 * happening, and logs both rolling averages every ~2s so the two can be
 * compared directly from one test session (pour for a bit, then just tilt
 * for a bit) rather than only ever seeing one whole-session average that
 * mixes both. */
static void track_pour_split(const input_t *input, int64_t step_us,
                             int64_t draw_us, int awake_blocks, int awake_cells,
                             int64_t now)
{
    if (input->down && ui.mode == SAND_MODE_PAINT) {
        pour_step_us_total += step_us;
        pour_draw_us_total += draw_us;
        pour_awake_total += awake_blocks;
        pour_awake_cells_total += awake_cells;
        pour_frames++;
    } else {
        idle_step_us_total += step_us;
        idle_draw_us_total += draw_us;
        idle_awake_total += awake_blocks;
        idle_awake_cells_total += awake_cells;
        idle_frames++;
    }

    if (now < split_log_at_us) {
        return;
    }
    if (pour_frames > 0) {
        const int64_t blocks = pour_awake_total / pour_frames;
        const int64_t cells  = pour_awake_cells_total / pour_frames;
        ESP_LOGI(TAG, "POURING:     %lu frames, step %lld us, draw %lld us, "
                      "%lld of %d blocks awake, %lld cells/awake block",
                 (unsigned long)pour_frames,
                 (long long)(pour_step_us_total / pour_frames),
                 (long long)(pour_draw_us_total / pour_frames),
                 (long long)blocks, block_cols * block_rows,
                 (long long)(blocks > 0 ? cells / blocks : 0));
    }
    if (idle_frames > 0) {
        const int64_t blocks = idle_awake_total / idle_frames;
        const int64_t cells  = idle_awake_cells_total / idle_frames;
        ESP_LOGI(TAG, "NOT POURING: %lu frames, step %lld us, draw %lld us, "
                      "%lld of %d blocks awake, %lld cells/awake block",
                 (unsigned long)idle_frames,
                 (long long)(idle_step_us_total / idle_frames),
                 (long long)(idle_draw_us_total / idle_frames),
                 (long long)blocks, block_cols * block_rows,
                 (long long)(blocks > 0 ? cells / blocks : 0));
    }
    pour_step_us_total = pour_draw_us_total = 0;
    idle_step_us_total = idle_draw_us_total = 0;
    pour_awake_total = idle_awake_total = 0;
    pour_awake_cells_total = idle_awake_cells_total = 0;
    pour_frames = idle_frames = 0;
    split_log_at_us = now + 2000000;
}
#endif

/* The boot menu: START begins the simulation at the current quality, and the
 * quality button cycles ULTRA/HIGH/NORMAL/LOW/VERY LOW and stays on the
 * menu. Modeled on ui_launcher.c's own frame - same ui_begin()/
 * ui_begin_screen()/mu_end_window()/ui_end() shape, one full-screen window
 * with no chrome.
 *
 * The shell (main.c) draws the home-swipe hint over whatever the app drew
 * and owns the swipe-up-to-exit gesture itself, so this menu needs no back
 * button of its own. */
static void draw_menu(const input_t *input)
{
    mu_Context *ctx = ui_context();

    ui_begin(input);

    /* ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT - see ui.h: the
     * logical canvas swaps dimensions under a quarter-turn transform. */
    if (ui_begin_screen(ctx, "Sand Menu",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        /* Both buttons centered as one block, so microui's default top-down
         * row layout is bypassed with mu_layout_set_next() and each button
         * placed at an absolute rect instead. */
        const int total_h = 2 * MENU_BTN_H + MENU_BTN_GAP;
        const int top      = (ui_height() - total_h) / 2;

        mu_layout_set_next(ctx,
                           ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top),
                           0);
        if (mu_button(ctx, "START")) {
            start_sim();
        }

        /* Built fresh each frame rather than cached: it is cheap, and
         * caching it would be one more thing to remember to invalidate
         * when quality changes. */
        char label[24];
        snprintf(label, sizeof label, "QUALITY: %s", qualities[quality].name);

        mu_layout_set_next(ctx,
                           ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H,
                                            top + MENU_BTN_H + MENU_BTN_GAP),
                           0);
        if (mu_button(ctx, label)) {
            quality = (quality + 1) % QUALITY_COUNT;
        }

        mu_end_window(ctx);
    }

    ui_end(COL_BACKGROUND);
}

static void sand_frame(uint32_t dt_ms, const input_t *input)
{
    /* SAND_UI_MENU is checked first, but a FAILED start_sim() sets
     * ui.screen to SAND_UI_RUNNING before returning specifically so it
     * falls through to the `failed` check below rather than getting caught
     * here - see that comment in start_sim(). Do not reorder these first
     * two checks, or change what SAND_UI_PALETTE does relative to them,
     * without keeping that path intact.
     *
     * The `failed` check sits before sand_ui_step() is ever called,
     * deliberately: a failed start must still reach the "no memory for the
     * grid" screen even though ui.screen is SAND_UI_RUNNING at that point,
     * and SAND_UI_PALETTE is never entered on that path (open_palette() is
     * only ever reached through sand_ui_step(), below) - but calling
     * sand_ui_step() any earlier would invite exactly that mistake the next
     * time a state is added here. */
    if (ui.screen == SAND_UI_MENU) {
        draw_menu(input);
        return;
    }

    if (failed) {
        gfx_clear(gfx_rgb(0x1A0C0C));
        gfx_text(20, GFX_HEIGHT / 2, "no memory for the grid", gfx_rgb(0xFF5C5C));
        return;
    }

    /* One call handles the whole of "which edges do what" - see
     * sand_ui_step()'s own comment in sand_ui.c for how it dispatches by
     * ui.screen internally, exactly mirroring the SAND_UI_PALETTE-then-
     * boot.released-then-PWR shape this function used to have inline. What
     * is left below is carrying out the returned action bits: opening or
     * closing the panel's gfx/accumulator side, redrawing it, or - for an
     * ordinary RUNNING frame - none of the above, and falling through to
     * the ordinary per-frame work. */
    const unsigned actions = sand_ui_step(&ui, input);

    if (actions & SAND_UI_CLOSE_PALETTE) {
        /* Forces a full repaint of the sand underneath the panel - see
         * mark_sand_fully_dirty()'s own comment for why this is the same
         * operation the SAND_UI_PALETTE handling below uses for an
         * orientation change while the panel is still open, and why it is
         * only marked here rather than drawn: this function returns
         * immediately below, so the actual redraw happens on the very next
         * ordinary frame's own call to draw_dirty_rows() rather than here.
         *
         * The two accumulators are zeroed below rather than left to keep
         * counting through the pause: left alone, the wall-clock time the
         * panel was open would cash in as a burst of catch-up steps and pour
         * the instant it closes, which would read as a stutter or a sudden
         * blob under the finger rather than as nothing having happened while
         * paused.
         *
         * SAND_UI_SHOW_LABEL is sand_ui_step()'s own answer to whether the
         * brush or its mode actually changed while the panel was open -
         * see close_palette()'s comment in sand_ui.c. Since cycling no
         * longer gives its own confirmation on the way past each material,
         * this is the only feedback closing the panel gets, and it should
         * say nothing when there is nothing to confirm. */
        if (actions & SAND_UI_SHOW_LABEL) {
            label_left_ms = LABEL_MS;
        }

        /* A text style is ambient context for this whole shell, not
         * per-frame state private to the panel - ui_set_text_style() stays
         * in force for every UI drawn after it until something changes it
         * again (see ui.h's own comment on why, and ui_set_button_style()'s
         * neighbouring comment for the contrast with a style that DOES
         * reset itself). Restoring UI_TEXT_PLAIN here, the moment the panel
         * is torn down, is what stops the outline from leaking into the
         * launcher or the sand boot menu the next time either draws a frame
         * - leaving this out is the obvious failure: the whole shell would
         * come up haloed after the palette had ever been opened once.
         *
         * THE TRANSFORM ITSELF IS NOT RESTORED HERE ANY MORE
         *
         * This used to also reset ui_set_transform(ui_transform_identity()),
         * for what was at the time the same reason as the text style:
         * draw_palette() left the transform turned, and something had to put
         * it back before the launcher or the sand boot menu drew again.
         *
         * That reasoning no longer applies, because the transform is no
         * longer this app's to leave turned OR to put back. main.c now owns
         * it for the whole shell (see display.h and main.c's display
         * sampling) and sets it from the board's actual orientation on its
         * own schedule, independent of whether this panel happens to be
         * open. If this app reset it to identity here, it would fight the
         * shell the moment the board was genuinely held sideways: the
         * launcher would snap upright the instant the palette closed, and
         * stay upright - wrong - until the shell's own next sample corrected
         * it, on a board that never stopped being sideways. An app must not
         * touch the shell's transform at all; it only ever inherits
         * whatever main.c has already set. */
        ui_set_text_style(UI_TEXT_PLAIN);

        sim_accumulator_q8 = 0;
        pour_accumulator_ms = 0;
        mark_sand_fully_dirty();
        return;
    }

    if (actions & SAND_UI_OPEN_PALETTE) {
        /* Clears the mode-label countdown so its full-screen redraw does
         * not paint sand straight back over the panel (see label_left_ms's
         * own use below).
         *
         * sand_ui_step() already flipped ui.screen to SAND_UI_PALETTE, so
         * falling through to the SAND_UI_PALETTE branch just below means
         * run_sim_steps()/handle_pour_input() are never reached again while
         * the panel is open - the sim is paused from the same frame the
         * panel becomes visible, not one frame later. draw_dirty_rows() is a
         * partial exception: the PALETTE branch calls it itself, but only on
         * an orientation change, to repaint what the panel's own move
         * uncovers - see that branch's own comment. */
        label_left_ms = 0;
    }

    if (ui.screen == SAND_UI_PALETTE) {
        /* Described every frame now, immediate-mode - see draw_palette()'s
         * own top comment. ui_end() inside it only repaints when the
         * command list actually changed, which is what keeps a held-steady
         * panel free without this needing to track "did anything actually
         * change" by hand the way the old drawn-on-change version did
         * (SAND_UI_REDRAW_PALETTE, and the stored `palette_turn` this
         * function used to compare a fresh reading against, are no longer
         * consulted here for that reason - sand_ui_tile_clicked() still
         * returns SAND_UI_REDRAW_PALETTE on a selection or a toggle, since
         * its callers elsewhere - the tests in suite_sand_ui.c - still
         * check it, but nothing here needs to read it any more).
         *
         * Orientation itself is no longer read here at all - it used to cost
         * a real IMU transaction every frame the panel was open, just to
         * recompute a `turn` that only fed draw_palette(). Now the shell has
         * already decided it (see main.c's display sampling), and
         * display_shell_quarter() below is a plain read of that decision,
         * not a sensor access. */
        const int quarter = display_shell_quarter();

        if (actions & SAND_UI_OPEN_PALETTE) {
            /* The panel just opened: the framebuffer still holds whatever
             * the app last drew (running sand, or the boot menu), and the
             * UI description this frame may well hash equal to some
             * earlier UI frame - the same trap ui_invalidate()'s own
             * comment in ui.h warns about for the launcher/app transition.
             * Forcing a repaint here is what makes the first frame actually
             * replace those pixels rather than comparing equal and leaving
             * them on screen. */
            ui_invalidate();

            /* Nothing to erase yet: the sand already fills the whole canvas
             * correctly (nothing else was drawn over it), so this is just
             * establishing the baseline the check just below compares
             * against on every later frame. */
            palette_drawn_quarter = quarter;
        } else if (quarter != palette_drawn_quarter) {
            /* The shell's orientation moved on since this panel was last
             * painted - the board was turned while the palette stayed open.
             * draw_palette() below paints UI_NO_BACKGROUND (see its own top
             * comment: the frozen sand showing through the grout between
             * tiles is the intended look, not a bug an opaque fill would be
             * a lazy way to paper over), so nothing ever erases a tile's OLD
             * footprint on its own. The panel is a square region placed by
             * ui_transform_quarter_turn(), which lands it somewhere
             * different on the physical screen at each quarter turn, so
             * whatever the previous footprint covered that the new one does
             * not is still sitting in the framebuffer as a ghost tile until
             * something repaints it.
             *
             * The sand simulation itself never rotates - draw_dirty_rows()
             * always paints in physical canvas coordinates, transform or no
             * transform - so "repaint" here just means putting the frozen
             * sand back the way it already looks, across the whole canvas
             * rather than working out exactly which pixels the old
             * footprint touched. mark_sand_fully_dirty() is the identical
             * three-step reseed SAND_UI_CLOSE_PALETTE uses above for the
             * same underlying reason (something that was covering the sand
             * moved) - see its own comment. Unlike that path, this one calls
             * draw_dirty_rows() itself, right here: there is still a panel
             * to draw on top afterward this same frame, rather than a
             * return that leaves the redraw for the next ordinary frame to
             * pick up.
             *
             * draw_emitter_markers() follows for the same reason the
             * ordinary running frame below always pairs it with
             * draw_dirty_rows(): the markers are drawn straight over the
             * grid's own pixels, not stored in it, so a full repaint of the
             * grid alone would erase them without this. Nothing here calls
             * advance_shine() or advance_local_depth_wake(), or passes
             * either's result along - the simulation is paused while the
             * panel is open, so this repaint happens only on an actual
             * orientation change, never once per frame; ticking either clock
             * on a static canvas would be paying an animation cost for a
             * picture that already looks right. */
            mark_sand_fully_dirty();
            draw_dirty_rows(false, false);
            draw_emitter_markers();
            palette_drawn_quarter = quarter;
        }

        draw_palette(input);
        return;
    }

    /* SAND_UI_RUNNING, and sand_ui_step() did not just open the panel: an
     * ordinary frame. `actions` here is whatever handle_brush_input() in
     * sand_ui.c returned - SAND_UI_SHOW_LABEL on a PWR press, or 0. */
    int gx, gy, flow, jostle, rotation;
    imu_sample_t sample = { 0 };
    read_gravity_input(dt_ms, &sample, &gx, &gy, &flow, &jostle, &rotation);

    if (actions & SAND_UI_SHOW_LABEL) {
        label_left_ms = LABEL_MS;
        if (input->power.pressed) {
            const char *mode_name = (ui.mode == SAND_MODE_DETONATE) ? "detonate"
                                   : (ui.mode == SAND_MODE_ERASE)    ? "erase"
                                   : material_name(brushes[ui.brush]);
            ESP_LOGI(TAG, "brush: %s", mode_name);
        }
    }

    /* The grid under the label has to be redrawn every frame the label is up,
     * including the frame it expires - that last redraw is what actually wipes
     * it off. Marking bands dirty is not enough on its own: the sand only
     * repaints rows the simulation changed, so without this the label would
     * leave a hole in the pile.
     *
     * draw_dirty_rows() alone is not enough either, now that it sends each
     * row only as wide as the sand actually occupies: a row with no sand
     * at all - exactly where a label drawn over clear screen sits - has
     * nothing to gather and nothing gets sent, so the cleared framebuffer
     * never reaches the panel and the label's old pixels are left stuck.
     * gfx_mark_dirty() does not know about the label either - it is drawn
     * through gfx_pixel(), a separate path draw_dirty_rows() has no view
     * into - so the fix is to just claim the whole screen outright on this
     * one frame rather than trust either path to infer it. */
    if (label_left_ms > 0) {
        label_left_ms = (dt_ms >= label_left_ms) ? 0 : (label_left_ms - dt_ms);
        memset(dirty_rows, 1, (size_t)grid_h);
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }

    /* input_ready's own guard: skip pour/erase/detonate/spawn entirely while
     * the START tap that opened this run is still held down - see
     * input_ready's own comment for why this waits for the release rather
     * than a fixed delay. */
    if (input_ready) {
        handle_pour_input(input, dt_ms);
    } else if (!input->down) {
        input_ready = true;
    }
    log_direction_change(gx, gy, jostle, &sample);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t0 = esp_timer_get_time();
#endif

    run_sim_steps(gx, gy, jostle, flow, dt_ms);

    /* Same gravity sand_step() above was just given, so a liquid rim's
     * highlight tracks the same tilt the sand itself is responding to.
     * Once per frame, not once per cell - see material_set_gravity()'s own
     * comment for why that split is what keeps the paint loop cheap. */
    material_set_gravity(gx, gy);

    /* Same reason, same frequency, for the travelling shine's own
     * direction - material_shine_direction() is pure and stateless (see
     * its own comment in material.h), so all it needs from here is
     * somewhere to land once a frame rather than being called from inside
     * paint_row_n()'s per-pixel loop. A tilt that has not yet crossed into
     * the next shine_offset tick still gets picked up within SHINE_STEP_MS
     * of changing - draw_dirty_rows() repaints every row_has_shine[] row
     * on each tick regardless of what moved it, and paint_row_n() always
     * reads whatever shine_ux_q8/shine_uy_q8 hold at the time it runs. */
    material_shine_direction(gx, gy, &shine_ux_q8, &shine_uy_q8);

    /* The liquid interior's LOCAL DEPTH walk needs its own per-frame facts
     * from this same gravity vector - the scale, which regime is dominant,
     * and which way each scan runs - but it is not material_set_gravity()'s
     * to compute: the persistent, cx-indexed arrays it feeds belong to THIS
     * file, which owns the row-by-row paint call sequence they are carried
     * across (see local_depth_row_a[]/local_depth_row_b[]'s own comment
     * above paint_row_n() for the full mechanism, and why material.c has
     * nothing left to do with depth at all). */
    update_local_depth_gravity(gx, gy);

    /* Water's foam gets its own per-frame fact, deliberately a separate
     * call from the one above rather than folded into it - see
     * material_set_foam_phase()'s own comment in material.h for why gravity
     * and phase must not be conflated. Driven by real elapsed time, not by
     * how many frames have run, so foam shimmers at the same rate on a slow
     * frame as a fast one - see FOAM_PHASE_MS's own comment for what that
     * buys and why a frame count would not. */
    foam_elapsed_ms += dt_ms;
    material_set_foam_phase(foam_elapsed_ms / FOAM_PHASE_MS);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t1 = esp_timer_get_time();
    int awake_blocks, awake_cells;
    count_awake(&awake_blocks, &awake_cells);
#endif

    /* The local-depth wake gets its own clock tick here, alongside the
     * shine's, for the same reason it gets its own elapsed-time counter and
     * its own row array rather than sharing either with the shine - see
     * LOCAL_DEPTH_WAKE_MS's own comment above paint_row_n(). Both are driven
     * by this same dt_ms because both need real elapsed time, not a frame
     * count, but they are two independent ticks at two independently tuned
     * rates, not one clock wearing two hats. */
    draw_dirty_rows(advance_shine(dt_ms), advance_local_depth_wake(dt_ms));

    /* After the rows, every frame - see draw_emitter_markers()'s own
     * comment for why once would not be enough. */
    draw_emitter_markers();

    /* On top of the sand, so it is never painted over. */
    if (label_left_ms > 0) {
        draw_mode_label(gx, gy);
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t2 = esp_timer_get_time();
    step_us_total += t1 - t0;
    draw_us_total += t2 - t1;
    frames++;
    track_pour_split(input, t1 - t0, t2 - t1, awake_blocks, awake_cells, t2);
#endif
}

/* Diagnostic only - see app_t's own comment in app.h for the contract
 * (a JSON object fragment, starting `{`, ending `}`, no trailing comma)
 * and screenshot.c's dump_state() for where this actually gets spliced in.
 *
 * Exposes tilt_x()/tilt_y() - the SMOOTHED gravity the sim actually acts
 * on, not the raw IMU counts device_state.h already reports. tilt.c's own
 * filter can differ substantially from the raw reading during a fast
 * rotation (see tilt.h's own top comment on TILT_TAU_MOVING_MS), which a
 * capture cannot otherwise tell apart from a raw ax/ay reading elsewhere
 * in the same dump.
 *
 * Originally added alongside two more fields - local_depth_in_deadzone and
 * local_depth_freeze_active - to debug a diagonal-dead-zone freeze that no
 * longer exists (LOCAL DEPTH's own top comment in this file: the blend
 * replaced the discrete axis choice the dead zone existed to protect, so
 * there is no freeze left to report on). Removed with that mechanism
 * rather than left behind reporting fields that no longer mean anything. */
static void sand_diagnostic_json(char *out, size_t len)
{
    snprintf(out, len, "{\"tilt_x\":%d,\"tilt_y\":%d}",
             tilt_x(&tilt), tilt_y(&tilt));
}

/* .home_gesture deliberately left unset (false) - see app_t's own comment.
 * A touch drag starting near a screen edge to steer or pour sand is easy to
 * mistake for the shell's swipe-home gesture, so this app needs that
 * detection off entirely rather than merely hiding its hint strip, and
 * provides its own way back to the launcher instead. */
const app_t app_sand = {
    .name           = "Falling Sand",
    .summary        = "Tilt to steer, touch to pour",
    .enter          = sand_enter,
    .frame          = sand_frame,
    .exit           = sand_exit,
    .diagnostic_json = sand_diagnostic_json,
};

APP_REGISTER(app_sand);
