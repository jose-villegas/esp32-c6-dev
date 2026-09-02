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
 * SURFACE (one step in the -gravity direction) is not the SAME MATERIAL - a
 * different liquid, empty space, or solid all count as "not the same", the
 * boundary of THIS material's own body - otherwise, one more than that
 * neighbour's own local depth, clamped to 255. A puddle with a rock poking
 * through it now dips back to a small depth right where the rock breaks its
 * surface, instead of painting straight through the rock as if it were not
 * there - exactly the "follows the shape of the puddle" the second report
 * asked for. See suite_sand.c's test_local_depth_follows_the_puddles_own_
 * shape for the actual before/after comparison, run through real
 * sand_t/sand_step(), that motivated this.
 *
 * THIS USED TO PICK ONE DOMINANT AXIS - `|gy| >= |gx|`, with hysteresis
 * against the previous frame's own pick near the tie point - and compute
 * ONLY that axis's depth, the same single-ray idiom sand.c's build_xflow()
 * and sand_gravity_direction() use to bracket a genuinely diagonal gravity
 * for the SIMULATION's own movement. NOT ANY MORE. Reported from the
 * device, AFTER the hysteresis fix had already landed: "if i leave the
 * device near a 45 degree position i can see artifacts for both direction
 * trying to reconcile, and the jump between the axis is also quickly
 * reflected in the bands... we should have a single source of truth for
 * this vector."
 *
 * Hysteresis only ever reduced how OFTEN the axis flipped - it did nothing
 * about how SEVERE each flip was on the rare frame it still fired. Measured
 * directly, on an irregular pool (water with a stone island in it, settled
 * at 45 degrees): the vertical-mode depth and the horizontal-mode depth are
 * DIFFERENT QUANTITIES computed from the same grid, and they disagreed by a
 * MEAN of 3.4 cells and a MAX of 11 cells across the pool's own liquid
 * cells, worst right at the obstacle's edges. With only DEPTH_RANGE (4)
 * discrete brightness steps total, an 11-cell disagreement firing all at
 * once, across every cell near that boundary simultaneously, is a visible
 * "pop" no matter how rarely hysteresis lets it happen - and real sensor
 * noise is not perfectly bounded, so a device left sitting near 45 degrees
 * long enough will eventually see at least one flip regardless.
 *
 * HYSTERESIS FIXED FREQUENCY. THIS FIXES SEVERITY. And fixing severity
 * means frequency stops mattering at all: there is no longer a discrete
 * axis to flip between, so there is nothing left to chatter and nothing
 * left to pop, however gravity moves. Both the vertical-mode depth and the
 * horizontal-mode depth are now computed for EVERY liquid cell, EVERY
 * frame, unconditionally - no flag, no "which regime am I in" state of any
 * kind - and COMBINED. Gravity itself becomes the sole, continuous input to
 * that combination: there is no separate boolean state that can ever fall
 * out of step with it, which is the "single source of truth" the report
 * above asked for.
 *
 * THIS SHIPPED FIRST AS A BLEND - a plain Q8 crossfade, `(vdepth*(256-w) +
 * hdepth*w) >> 8` with `w = 256|gx|/(|gx|+|gy|)` - and stayed that shape
 * through the debounce fix below, THE SATURATING CLIMB's own clamp, and the
 * direction-flip reset, all still exactly as the rest of this comment
 * describes them. Modelled before writing THAT version: at the SAME
 * worst-case cell (the 11-cell disagreement above), stepping gravity from 30
 * to 60 degrees in 3-degree increments, the blended value moved by at most
 * ONE index step per 3 degrees of tilt across the sweep - a crossfade, not a
 * jump.
 *
 * IT IS NOT WHAT SHIPS NOW. Reported from the device, again, against the
 * blend itself this time rather than against the axis choice it replaced -
 * two DIFFERENT defects, both caught on annotated screenshots
 * (`screenshot_20260902_110354`/`_110406`, see their own .json sidecars for
 * the exact tilt every number below was measured at, and app_tilt_x/
 * app_tilt_y for the gravity this file actually saw).
 *
 * DEFECT ONE - "weird rectangles" of wrongly-shallow water beside every
 * submerged obstacle, flipping which SIDE with the sign of gx: the
 * signature of the horizontal walk, specifically, getting blocked by the
 * obstacle while the vertical walk still reaches the real surface fine, and
 * the BLEND still handing the blocked walk weight proportional to its own
 * axis's share of gravity rather than zero. Measured luminance of the water
 * beside all 5 obstacles in both device frames: 63.1 vs 55.1 (about 11
 * cells) on the RIGHT at tilt_x=+2070, the mirror image on the LEFT at
 * tilt_x=-2366 - exactly where a walk that looks toward the surface gets cut
 * short by an inclusion in its own path.
 *
 * DEFECT TWO - the blend has no notion of a PROJECTION: each axis walk
 * counts cells along that AXIS, not distance along GRAVITY, so blending two
 * axis counts makes the reported depth of a cell at a fixed true
 * perpendicular depth change with tilt angle alone, no obstacle involved at
 * all. Modelled on an ideal planar surface perpendicular to gravity, true
 * depth 10 cells: the blend reports 10.0, 13.2, 14.6, 14.1, 14.6, 13.2, 10.0
 * across 0, 15, 30, 45, 60, 75, 90 degrees from vertical - up to 46%
 * inflation from tilt alone, exactly the "it creates a band... the shade
 * just gains length depending on which direction has more magnitude"
 * complaint.
 *
 * THE FIX, replacing the blend outright rather than patching it: PROJECT
 * each axis walk onto the gravity direction, then take the MAX of the two
 * projections rather than a weighted average of them. See update_local_
 * depth_gravity() and paint_row_n()'s own "THE COMBINER ITSELF" comment for
 * the mechanism as it stands now, and why max - not a blend of the
 * projections - is the combiner defect one actually needed: each axis walk
 * is a LOWER BOUND on the true depth (it can terminate early at an
 * inclusion rather than at the free surface), and max-of-lower-bounds is
 * the tighter estimate, where averaging instead lets either blocked axis
 * drag the result down. Re-measured on the same worst-case cell and the
 * same 30-to-60-degree sweep the blend was originally proven against: the
 * max combiner is still a crossfade at the 45-degree crossing, not a jump,
 * because both projections are continuous functions of gravity's own angle
 * and equal each other exactly at the crossing - see the re-verified
 * "PROVEN LOAD-BEARING" block in test_the_blend_has_no_jump_crossing_45_
 * degrees (suite_sand.c), a test that keeps its old name because it still
 * pins the same property, even though the mechanism inside it no longer is
 * one.
 *
 * DEFECT THREE - found by measurement AFTER defects one and two's own fix
 * had already shipped and every test above was green, so worth recording as
 * its own defect rather than folded silently into the fix history: THE
 * FIRST SHAPE OF THIS FIX projected once, AT COMBINE TIME, from a plain
 * CELL COUNT that saturated at a FIXED MATERIAL_LIQUID_DEPTH_BAND (24)
 * regardless of gravity's angle - i.e. `depth = max(min(vcount,24)*wv_q8,
 * min(hcount,24)*wh_q8) >> 8`. That fixed the shadow and the under-band
 * tilt inflation (defects one and two), but a FULLY SATURATED cell (both
 * counts clamped at the same 24) still reports `(24 * w) >> 8`, which is
 * BELOW 24 whenever w < 256 - so the entire interior of a deep pool dims
 * and brightens as the device rotates, even far from any obstacle. Measured
 * through the real material_colours() ramp, sweeping gravity 0 to 90
 * degrees against a cell saturated on both axes: rendered luminance reads
 * 55.1 flat from 0-30 and 60-90 degrees, but 63.3 - one whole shade step
 * brighter, DEPTH_RANGE is 4 - from 35-55 degrees, a global "breathing" of
 * the pool's entire body across the exact tilt window (33.3 and 37.8
 * degrees) the device's own two screenshots were captured at. A SCALED
 * per-axis ceiling was tried next, to let a low-weight axis's own count
 * climb further before clamping - it removes this breathing but was
 * measured to reintroduce THE SATURATING CLIMB's own banding defect at
 * full strength (see that section's own comment further down for the
 * numbers); rejected for that reason.
 *
 * A SECOND DESIGN WAS TRIED AND ALSO REJECTED: project EVERY STEP instead
 * of once at the end - col_stable_depth[]/row_stable_depth[] holding
 * PROJECTED DEPTH DIRECTLY, in eighths of a cell, climbing by that frame's
 * own per-step increment (0-8 eighths) instead of a flat `+1`, saturating
 * at a FIXED ceiling in the SAME eighths units (so a fully-saturated axis
 * reads the same value at every angle, closing the breathing completely -
 * measured flat luminance across the full 0-90 sweep, confirmed by
 * test_a_saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle,
 * suite_sand.c, which still exists and still must stay flat). IT WAS
 * REJECTED FOR A DIFFERENT, MORE FUNDAMENTAL REASON than the scaled
 * ceiling: a plain CELL COUNT is gravity-agnostic - it means the same thing
 * (some number of consecutive same-material cells) no matter what gravity
 * is doing, so projecting it FRESH at combine time, every time, from
 * whatever the CURRENT frame's own weight is, is always correct regardless
 * of when the count was accumulated. An EIGHTHS accumulator has no such
 * property: each increment already has a specific frame's gravity baked
 * into it, so a value built up while gravity pointed one way carries that
 * angle with it, uncorrected, into a later frame where gravity has since
 * rotated - and nothing at read time can undo that, because the stale
 * angle is not stored anywhere separately, only its effect is. That is
 * fatal for a mechanism whose entire storage model is "values persist
 * across frames while gravity continuously rotates" (col_stable_depth[]'s
 * own comment above, VERTICAL DEPTH). Measured directly: test_a_sparse_
 * repaint_does_not_band_a_tall_liquid_column (suite_sand.c) - the exact
 * regression test THE SATURATING CLIMB's own fix exists to guard - failed
 * again under the eighths design at 52 banded pairs (threshold 8), and
 * critically, changing that test's own ceiling parameter between 255 (no
 * clamp) and the eighths design's own fixed cap made NO DIFFERENCE (both
 * measured 52) - proving the regression was not about WHERE the ceiling
 * sat at all, but about gravity's angle drifting while stale eighths
 * values were still live: freezing the test's own gravity sway to a fixed
 * angle dropped the failure to 0. Rejected.
 *
 * THE ACTUAL FIX keeps EVERYTHING from the first (rejected) design above -
 * col_stable_depth[]/row_stable_depth[] hold a plain CELL COUNT again,
 * climbing by a flat `+1`, and the combiner projects AT COMBINE TIME,
 * exactly as it did when defects one and two were first closed - and
 * changes exactly one number: the ceiling that count saturates at.
 * LOCAL_DEPTH_COUNT_CEILING (defined with THE SATURATING CLIMB below)
 * replaces the plain MATERIAL_LIQUID_DEPTH_BAND the first attempt used,
 * raised just far enough that `ceiling * min(wv_q8, wh_q8 at the worst
 * angle) >> 8` still reaches MATERIAL_LIQUID_DEPTH_BAND even at the worst
 * (45-degree) tilt, where a Q8 weight bottoms out around 183 of 256 - see
 * that constant's own comment for the exact value and how it was measured,
 * not merely calculated. Because storage is back to a gravity-agnostic
 * COUNT, DEFECT THREE (breathing) is still closed - a saturated count now
 * has enough headroom that its projection reaches the band at every angle
 * - but the eighths design's fatal flaw cannot recur, because nothing
 * about a count depends on when it was accumulated. THE COMBINER now needs
 * an EXPLICIT clamp to MATERIAL_LIQUID_DEPTH_BAND after the max (the raised
 * ceiling means `count * weight >> 8` can exceed the band, unlike the
 * original 24-cap design where it never could) - see "THE COMBINER ITSELF"
 * in paint_row_n() for where that clamp lives now.
 *
 * VERTICAL DEPTH needs a value that persists ACROSS paint_row_n() CALLS - a
 * column's vertical depth depends on the row above (or below) it, painted
 * in a SEPARATE call - so col_stable_depth[] below (declared alongside its
 * own row-tracking array a little further down; see that array's own
 * comment for the full mechanism) is a plain file-static array, GRID_W_MAX
 * entries, the same pattern row_has_shine[] above already uses for the same
 * reason (sized for the finest quality tier; a coarser one just uses less
 * of it). It is walked UNCONDITIONALLY every row, every frame now - not
 * gated on "vertical happens to be dominant" - in the direction matching
 * gy's OWN sign, because there is no dominant axis left to gate it on.
 *
 * AN EARLIER VERSION OF THIS SPLIT THE VERTICAL WALK INTO TWO ARRAYS: a
 * raw, undebounced col_local_depth[] that a separate col_stable_depth[]
 * supposedly "rode on" as a debounced parallel. It did not, by the time
 * anyone re-checked the code against the comment describing it:
 * col_stable_depth[cx]'s own climb read only its OWN prior stored value
 * (and the v_same_material test both blocks happened to share) - never
 * col_local_depth[cx], never the value written into it. A closed loop,
 * written and read by nothing outside itself, kept alive only by comments
 * that kept describing an intent the code had stopped carrying out.
 * Removed entirely once found, along with its horizontal counterpart
 * (h_running_depth, paint_row_n() below); col_stable_depth[] is now the
 * ONLY array for the vertical case, and every comment in this section
 * describes what it actually computes rather than a two-stage design that
 * quietly stopped existing without its own comments noticing. See
 * suite_sand.c's own history at the same discovery for the matching
 * test-mirror lesson: its mirrors of this mechanism had drifted the same
 * way, for the same reason, and needed the same correction.
 *
 * HORIZONTAL DEPTH needs a persisting array too, for a DIFFERENT reason
 * than the vertical case: not because it chains across separate
 * paint_row_n() calls made for DIFFERENT ROWS within one frame (it does
 * not - the neighbour toward the surface is one column over in the SAME
 * row, which one call already walks start to finish) but because the
 * hold-then-commit debounce every axis here uses needs to remember, from
 * the last time THIS ROW was painted, what it last decided - a fact that
 * has to survive until the NEXT FRAME repaints the same row, a separate
 * call again. row_stable_depth[] below (the horizontal counterpart to
 * col_stable_depth[], keyed by ROW instead of column) is that memory. It
 * ALSO always runs now, walked in the direction matching gx's OWN sign,
 * independent of whether horizontal ends up mattering to the combined
 * result this frame.
 *
 * WHICH WAY A ROW OR COLUMN SCAN HAS TO GO, so that "the neighbour toward
 * the surface" is always the one already processed: ascending for the
 * vertical walk unless gy is NEGATIVE (gravity up), and ascending for the
 * horizontal walk unless gx is NEGATIVE (gravity left) - two independent
 * sign checks now, one per axis, rather than one flag gating whichever axis
 * used to be dominant. See local_depth_v_reverse/local_depth_h_reverse's
 * own comment below.
 *
 * ROW ORDER, SPECIFICALLY: draw_dirty_rows() below normally walks cy
 * ascending; this is safe to reverse for a gravity-up frame instead of
 * needing a trickier fix inside the depth bookkeeping itself, because
 * nothing else that loop does depends on row order - dirty_rows[cy],
 * row_run_x0/x1/n, row_has_shine[cy] and row_has_liquid[cy] are all
 * indexed by cy directly and read/written independently per row, and
 * gfx_mark_dirty() only ever unions a bounding box of (y, height) pairs
 * (dirty_mark(), gfx_dirty.h), which does not care what order the boxes
 * arrive in either - the same reasoning
 * f9679df's own commit already established still holds, unchanged; only the
 * CONDITION under which the reversal fires got simpler, since it is now
 * just "does gravity point up" with no "and is vertical dominant this
 * frame" to ask alongside it. See draw_dirty_rows() itself for where the
 * reversal actually happens, and col_stable_depth[]'s own comment below for
 * WHY the vertical walk specifically needs it - the horizontal walk does
 * not, see row_stable_depth[]'s own comment for why not.
 *
 * STALE READINGS UNDER THE DIRTY-ROW OPTIMISATION ARE ACCEPTED, not a bug to
 * chase down: only dirty rows call paint_row_n() at all (draw_dirty_rows()
 * below skips any row whose dirty_rows[cy] is not set), so a column's
 * stored local depth for a row that has not repainted in a while reflects
 * whatever the puddle looked like the last time that row WAS painted, not
 * necessarily its current shape. This costs nothing extra to accept - no new
 * full-grid pass, which is the entire reason this stays cheap - and it
 * matches the precedent already established and accepted for foam's own
 * drift: only dirty rows repaint, so the animation (or, here, the depth
 * reading) shows where the water is actually moving, which is nearly
 * everywhere it is worth seeing. This was already accepted back when only
 * the vertical case existed; it now applies in exactly the same way,
 * unconditionally, to both the vertical and the horizontal walk, rather
 * than only when vertical happened to be dominant. Do not "fix" this into a
 * full-grid pass without re-deciding that trade-off on purpose.
 *
 * THERE IS NO AXIS TO FLIP BETWEEN FRAMES ANY MORE, and so no reset to
 * perform: col_stable_depth[] is written every frame, unconditionally, by
 * the same vertical walk regardless of what gravity's horizontal component
 * is doing, so there is no "other regime" whose stale readings could ever
 * leak in - the failure mode the old per-frame axis-flip's memset used to
 * guard against (a horizontal-dominant frame silently skipping the array
 * for many frames, then a flip back exposing whatever a long-past vertical
 * frame left there) cannot happen when the array is never skipped in the
 * first place. */

/* THIS FRAME'S PROJECTION WEIGHTS, in Q8 - how much of the VERTICAL walk's
 * raw COUNT and how much of the HORIZONTAL walk's raw COUNT each represents
 * of a distance measured along the gravity vector itself (0 = "this axis is
 * perpendicular to gravity, contributes nothing", 256 = "this axis IS
 * gravity, its raw count already is the distance"). `wv_q8 =
 * 256|gy|/im_len(gx,gy)`, `wh_q8 = 256|gx|/im_len(gx,gy)` - see
 * update_local_depth_gravity() below for where they are set, once a frame,
 * and LOCAL DEPTH's own top comment for DEFECT TWO, the tilt-scale bug a
 * plain blend of two axis COUNTS produced and this projection closes.
 *
 * PROJECTED AT COMBINE TIME, deliberately, not baked into the climb itself -
 * see LOCAL DEPTH's own top comment for why an EIGHTHS accumulator (project
 * every step, store the result) was tried and rejected: it broke the
 * gravity-agnostic property a plain cell COUNT has, which is load-bearing
 * for THE SATURATING CLIMB's own protection below. col_stable_depth[]/
 * row_stable_depth[] hold a COUNT and nothing else; these weights are read
 * fresh, from THIS frame's own gravity, every time that count is projected
 * - never stored alongside it.
 *
 * NOTE THESE DO NOT NECESSARILY SUM TO 256 the way a complementary blend
 * pair always did by construction - im_len() is only an approximate length
 * (~1% short on the diagonal, ~8% over near a 2:1 ratio; see its own
 * comment in intmath.h), so wv_q8 + wh_q8 drifts a little either side of
 * 256 depending on gravity's own angle. This does not matter to the
 * combiner below: each weight is used only to scale ITS OWN axis's count,
 * never against the other's, so a uniform scale error in the shared
 * denominator becomes a uniform scale error in the final depth (at most
 * about 2 of the 24-cell band, under half of one of the four rendered shade
 * steps - acceptable and in keeping with this file's integer-only
 * convention, but worth stating rather than leaving unexamined).
 *
 * AT GX == 0 (straight-down or straight-up gravity), wv_q8 is EXACTLY 256
 * and wh_q8 is EXACTLY 0 - im_len(0, gy) reduces to |gy| exactly (the
 * approximation only bites when both components are nonzero) - which makes
 * the combiner below collapse to `depth == vdepth`, identical to the
 * mechanism's behaviour before this file had a horizontal axis at all.
 * test_local_depth_follows_the_puddles_own_shape (suite_sand.c) depends on
 * exactly this and is unaffected by any of this section's history for
 * exactly this reason - see that test's own comment.
 *
 * ALSO SET HERE: which way each of the two independent scans has to run so
 * "the neighbour toward the surface" is always the one already processed -
 * descending when that axis's OWN gravity component is negative (gravity up
 * for the vertical scan, gravity left for the horizontal one), ascending
 * otherwise. See LOCAL DEPTH's own comment above for the full mechanism
 * these feed. */
static unsigned local_depth_weight_v_q8;
static unsigned local_depth_weight_h_q8;
static bool local_depth_v_reverse;
static bool local_depth_h_reverse;

/* Last frame's local_depth_v_reverse/local_depth_h_reverse - not read by
 * paint_row_n() at all, only by update_local_depth_gravity() itself, to
 * detect the ONE frame a reversal actually happens on. See that function's
 * own comment for why a flip needs to invalidate col_stable_depth[]/col_
 * top_row[] (or row_stable_depth[]/row_top_col[]) and why "no reset needed"
 * - true for which AXIS is dominant - does not extend to this. */
static bool local_depth_v_reverse_prev;
static bool local_depth_h_reverse_prev;

/* THE VERTICAL LOCAL DEPTH ITSELF - a hold-then-commit debounce over an
 * unconditional column-wise climb, feeding the BLEND's vertical component
 * directly. GRID_W_MAX entries, file-static for the same cross-call-
 * persistence reason VERTICAL DEPTH's own comment above gives.
 *
 * (AN EARLIER VERSION OF THIS COMMENT described this array as "debouncing"
 * a separate raw walk, col_local_depth[], that supposedly fed it. It did
 * not: the climb below reads only its OWN prior stored value, never a raw
 * array's - see VERTICAL DEPTH's own comment above for the full story of
 * that dead array's removal. What follows describes what this array has
 * always actually computed.)
 *
 * Only ever accumulates through LIQUID cells
 * (`material_of(row[cx])->kind == KIND_LIQUID` gates it, see paint_row_n()
 * below) - any non-liquid cell resets it to a clean 0 rather than letting a
 * run of open air pollute the value a real boundary inherits - and a RESET
 * only commits once the SAME ROW has asked for it on two consecutive
 * painted frames, tracked by row index in col_top_row[cx], not by
 * column-chain position.
 *
 * Row-keyed on purpose: a settled pool's topmost cell blinking empty/full
 * for one frame never gets its neighbour row to ask twice from the SAME
 * row index, so it is absorbed; a genuine, lasting change (the pool
 * draining, an obstacle appearing) keeps asking from the same new row and
 * commits within one extra frame - not something a human eye can tell
 * from immediate. That "one extra frame" claim holds for a column with
 * exactly ONE reset point; see KNOWN LIMITATION below for what happens
 * with two.
 *
 * WHY draw_dirty_rows() BELOW REVERSES ROW ORDER FOR A GRAVITY-UP FRAME:
 * this array is shared across every row of the SAME column, updated once
 * per row as draw_dirty_rows() below sweeps cy across the whole grid
 * within a single frame - the climb at row cy reads whatever this same
 * array held after the PREVIOUS row painted, so "the previous row" has to
 * mean the row nearer the surface, or a dirty row far from the surface
 * reads a stale value some other, farther row left behind the last time
 * IT happened to be nearer the front of the sweep. That is exactly why the
 * reversal exists (see draw_dirty_rows()'s own comment on
 * local_depth_v_reverse for where it fires) - it is THIS array's own
 * cross-row dependency the reversal protects, nothing to do with a raw
 * walk. row_stable_depth[]/row_top_col[] below have no equivalent
 * requirement - see that array's own comment for why not.
 *
 * KNOWN LIMITATION - NOT MERELY "A FRAME'S LAG": a column with TWO
 * persistent reset points (a pool's own surface, and an obstacle poking
 * through its interior, say) has both competing for the single
 * col_top_row[cx] slot - and, traced exactly through
 * test_local_depth_follows_the_puddles_own_shape's own obstructed column
 * (surface boundary at row 2, resume-point at row 9 just below a two-cell
 * rock plug): NEITHER ever commits, not merely one lagging the other by a
 * frame. Every pass, the LATER point in scan order (row 9) ends the pass
 * holding the tracker; on every following pass, the EARLIER point (row 2)
 * is checked first, finds a mismatch (tracker says 9, not 2), holds, and
 * overwrites the tracker to 2 - so by the time row 9 is reached again in
 * that SAME pass, the tracker no longer says 9 either, and it holds too.
 * This is a permanent, deterministic oscillation once both points exist,
 * not a transient one more frame would resolve: nothing about repeating
 * the same pass again changes which point the scan visits first. In THIS
 * geometry it costs little in practice - the obstacle's own non-liquid
 * cells reset col_stable_depth[cx] to a clean 0 immediately before row 9's
 * request either way, so a permanent HOLD (climbing once from that fresh
 * 0) reads as "depth 1, forever" at that row rather than "depth 0,
 * forever" a genuine commit would give: every row below the plug reads
 * exactly one unit deeper than a true reset would show, a constant offset,
 * not a growing or unbounded error. True per-row memory would remove the
 * limitation entirely, at the cost of one byte per CELL rather than per
 * column - unaffordable on this device's heap (grid itself already costs
 * 41,216 bytes, see start_sim()'s own comment on that allocation), so this
 * is where the trade lands. See test_local_depth_follows_the_puddles_own_
 * shape's own comments in suite_sand.c for the exact traced numbers this
 * paragraph states.
 *
 * A REAL GRAVITY-REVERSAL RESET IS STILL NEEDED, and is not the same thing
 * as the competing-boundary limitation just above: when
 * local_depth_v_reverse itself flips, the row this column's boundary sits
 * at relocates (top of the column versus bottom), and the two candidate
 * rows compete forever for the single col_top_row[cx] slot in exactly the
 * way just described, holding instead of ever committing - see
 * update_local_depth_gravity()'s own comment below for the measured
 * corruption this produced and the reset that closes it. */
static uint8_t col_stable_depth[GRID_W_MAX];

/* The row index of column cx's most recent boundary request, confirmed or
 * still just a one-frame candidate - see col_stable_depth[]'s own comment
 * above for the full mechanism. 255 means "nothing tracked yet" (GRID_H_MAX
 * is 224, comfortably under 255, so that value can never be a real row). */
static uint8_t col_top_row[GRID_W_MAX];

/* THE HORIZONTAL LOCAL DEPTH ITSELF - mirrors col_stable_depth[]/
 * col_top_row[] exactly, transposed: keyed by ROW (cy) instead of column
 * (cx). What needs to persist ACROSS FRAMES for the horizontal case is
 * state keyed by THIS ROW, remembering what happened the last time this
 * same row was painted - exactly mirroring what col_top_row[cx] remembers
 * about the last time this same COLUMN was painted.
 *
 * (AN EARLIER VERSION OF THIS COMMENT described this array as debouncing a
 * separate raw walk, h_running_depth, that supposedly fed it. It did not -
 * see col_stable_depth[]'s own comment above for the matching correction on
 * the vertical side; h_running_depth was removed once this was noticed.)
 *
 * NO ROW-ORDER REVERSAL DEPENDENCY, UNLIKE col_stable_depth[] - this is the
 * one place the vertical/horizontal symmetry breaks, worth stating
 * explicitly rather than leaving a reader to assume it is needed here too.
 * col_stable_depth[cx]'s climb chains ACROSS ROWS: it is shared by every
 * row of the same column, and draw_dirty_rows() below visits those rows
 * one paint_row_n() call at a time within a single frame's sweep, so which
 * row is visited FIRST matters (see that array's own comment). row_stable_
 * depth[cy]'s climb never chains across rows at all - a single call to
 * paint_row_n() walks every column of ITS OWN row start to finish in one
 * pass, so the only ordering this array ever depends on is the CX scan
 * direction inside that one call (governed by cx_first/cx_step below,
 * already correct by construction), never which ROW draw_dirty_rows()
 * paints before which other row. draw_dirty_rows()'s row-order reversal
 * exists entirely for col_stable_depth[]'s sake; this array would read
 * identically whichever order rows happened to be visited in.
 *
 * Same reset rule as the vertical version: a RESET only commits once the
 * SAME ROW has asked for it from the SAME COLUMN on two consecutive painted
 * frames, tracked by column index in row_top_col[cy]. A one-frame blink of a
 * horizontal neighbour (the same class of transient col_stable_depth[] was
 * built to catch on the vertical side) is absorbed instead of swinging
 * straight into the blend, which is exactly the gap that made the diagonal
 * dead zone's removal visible as flicker in the first place: with no freeze
 * left to hide behind, this undebounced reading used to reach the screen
 * raw wherever horizontal carried real blend weight - significant well
 * before 45 degrees and growing toward 90.
 *
 * KNOWN LIMITATION - NOT MERELY "LAST WRITER WINS FOR A FRAME": the SAME
 * permanent-oscillation limitation col_stable_depth[]'s own comment
 * describes for two competing reset points in a column applies here,
 * transposed onto a row: a ROW that crosses more than one separate liquid
 * boundary side by side (two puddles divided by dry ground, or simply the
 * row's own left/right edge alongside an interior obstacle) has all of
 * them competing for the single row_top_col[cy] slot, and by the same
 * argument traced there, NONE of them ever commits while all persist - not
 * just the earliest-scanned one losing out for a single frame. See
 * test_the_blend_has_no_jump_crossing_45_degrees's own comments in
 * suite_sand.c for a traced instance of exactly this (that test row's own
 * left edge permanently competing with the deliberately placed obstacle).
 *
 * NO "WHICH AXIS IS DOMINANT" RESET IS NEEDED, for the same reason col_
 * stable_depth[]/col_top_row[] need none: both arrays here are computed the
 * same way every frame, for every row, regardless of the blend weight -
 * there is no "other regime" whose stale reading could ever leak in the way
 * the old single-axis-choice design's flip used to. That is a DIFFERENT
 * question from whether REVERSING THIS AXIS'S OWN WALK DIRECTION needs an
 * invalidation - it does; see update_local_depth_gravity()'s own comment
 * below for the bug that showed up when it was missing (two different, both
 * legitimate, boundary rows/columns alternately asking for a reset as
 * local_depth_h_reverse itself flips, forever competing for one tracking
 * slot) and the reset that closes it. */
static uint8_t row_stable_depth[GRID_H_MAX];

/* The column index of row cy's most recent boundary request - see row_
 * stable_depth[]'s own comment above for the full mechanism. 255 means
 * "nothing tracked yet" (GRID_W_MAX is 184, comfortably under 255, so that
 * value can never be a real column). */
static uint8_t row_top_col[GRID_H_MAX];

/* col_stable_depth[]/row_stable_depth[] hold a plain CELL COUNT - see LOCAL
 * DEPTH's own top comment for why an eighths-of-a-cell accumulator was
 * tried instead and rejected (it broke the gravity-agnostic property a
 * count has, which THE SATURATING CLIMB's own protection below depends on).
 * This is that count's saturation point - MATERIAL_LIQUID_DEPTH_BAND (24)
 * itself, RAISED, not the plain band constant any more; see THE SATURATING
 * CLIMB's own comment just below, "THE CEILING IS RAISED, NOT THE BAND
 * ITSELF", for the exact value, the reasoning, and how it was measured
 * rather than only calculated.
 *
 * 34 IS chosen so the projection reaches the band EXACTLY at the worst
 * (45-degree) tilt angle - `ceil(MATERIAL_LIQUID_DEPTH_BAND*256/183)` - but
 * the true FLOOR below which the breathing this fixes becomes VISIBLE
 * again is lower, 27, and that floor depends on constants that live
 * elsewhere and could silently drift out of sync with this one: material_
 * colours()'s shade index is `DEPTH_RANGE*(MATERIAL_LIQUID_DEPTH_BAND -
 * depth)/MATERIAL_LIQUID_DEPTH_BAND` (material.c, `DEPTH_RANGE` == 4,
 * integer division), so any projected depth from 19 up to
 * MATERIAL_LIQUID_DEPTH_BAND itself renders the IDENTICAL shade - the floor
 * is whichever ceiling's own worst-angle projection still lands at 19
 * (`27 * 183 >> 8 == 19`). If DEPTH_RANGE, that integer division, or the
 * worst-angle weight (183, itself a consequence of im_len()'s own
 * approximation) ever changes, this floor has to be RE-MEASURED against
 * test_a_sparse_repaint_does_not_band_a_tall_liquid_column and test_a_
 * saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle (suite_
 * sand.c), not assumed to still be 27. 34 was kept over a smaller in-range
 * value (down to 27) because it is the value with no shortfall at all at
 * the worst angle, not merely one that stays under the visible-step floor -
 * see THE SATURATING CLIMB's own comment for the measurement that confirmed
 * 34 does not reopen the sparse-repaint regression either. */
#define LOCAL_DEPTH_COUNT_CEILING 34u

/*=============================================================================
 * THE SATURATING CLIMB - why col_stable_depth[]/row_stable_depth[] stop at
 * LOCAL_DEPTH_COUNT_CEILING and not at a byte's own 255.
 *
 * Reported from the device against the commit right above this one (882f2e7,
 * the v_reverse/h_reverse flip reset): "it's flickery even during normal
 * movement sometimes, creating lines inside the water... it seems to be
 * creating huge jumps in the depth color i think the flickering comes from
 * trying to react to the rim, seems like its fighting between two shade
 * values in a couple of frames of difference."
 *
 * THE FLIP RESET WAS NOT THE CAUSE, and this is worth writing down because
 * the obvious next move - soften, delay or hysteresis-gate that reset -
 * would have cost another device round trip for nothing. Measured directly,
 * on a host model driving the REAL sand_t/sand_step() through the REAL
 * dirty-row sparsity (sand_track_dirty_rows()), the real wake tick and the
 * real row-order reversal, under landscape-lock gravity taken from the
 * device's own capture sidecars (tilt_x steady at 3650-3932, tilt_y small
 * and crossing zero: -204, 147, -175, 183, -115, 29, -135, -131): running
 * that model WITH 882f2e7's reset and WITHOUT it produces the same banding
 * to two decimal places - 2.81 versus 2.80 vertical shade transitions per
 * water column, 10.09 versus 10.09 isolated one-cell lines per frame, across
 * flip rates from 0.4 to 3.1 per second and repaint sparsities from 0 to 65
 * rows per frame. The reset is, visually, a no-op. It stays (it still does
 * real bookkeeping work - see update_local_depth_gravity() below), but it
 * never had the blast radius the report was describing.
 *
 * WHAT THE BANDING ACTUALLY NEEDS, from the same model, by ablation - the
 * severe artifact being a TWO-shade-step jump between vertically adjacent
 * interior cells, of only four steps total (DEPTH_RANGE, material.c):
 *
 *     as shipped (882f2e7)                0.6 per frame, worst frame 51
 *     every row repainted every frame     0.0 per frame, worst frame  0
 *     vertical walk contributing nothing  0.0 per frame, worst frame  0
 *     debounce removed entirely           0.7 per frame, worst frame 57
 *
 * Two necessary conditions, and the debounce is not one of them: the
 * DIRTY-ROW SPARSITY, and the VERTICAL walk's contribution to the blend.
 *
 * THE MECHANISM. col_stable_depth[cx] is a RUNNING accumulator walked down a
 * column - each painted row's value is the previous PAINTED row's plus one.
 * That is only the cell's real depth if the rows are painted as a contiguous
 * chain from the boundary, which is exactly what draw_dirty_rows() does NOT
 * do: it paints only dirty rows. A row painted in isolation therefore
 * inherits whatever row happened to be painted before it - possibly a
 * distant row, possibly the last row of the previous frame - and gets a
 * freshly computed value that is about a different cell entirely. Its
 * vertical neighbours, meanwhile, still show the coherent values the last
 * wake tick left them. That is not the "stale readings are accepted" trade
 * LOCAL DEPTH's own comment above signs off on - a stale row keeps its last
 * GOOD value - it is a wrong value landing on precisely the rows that are
 * being repainted, i.e. the ones the eye is already watching. One row, one
 * cell tall, several shades off its neighbours: a line inside the water.
 *
 * WHY IT IS A LANDSCAPE-LOCK REPORT specifically, and why portrait was
 * always fine. Near landscape the water stands as a column against a side
 * wall spanning the WHOLE screen height, so the vertical walk's range is the
 * grid's own height (112 rows at this quality tier) and beyond - the
 * accumulator is free to climb to 255. Near portrait the same walk runs down
 * a settled pool perhaps 30 cells deep, so a broken chain can be wrong by at
 * most 30. Modelled both: the portrait scene shows 0 two-step jumps and 0
 * isolated lines with or without any of this, while the landscape scene
 * shows up to 51.
 *
 * THE FIX, and why THIS shape rather than 882f2e7's. material_colours()
 * (material.c) clamps `depth` to DEPTH_SATURATE_CELLS, which IS this
 * constant, before shading with it: past 24 cells the panel cannot tell one
 * depth from another. But the two axes are blended BEFORE that clamp, so an
 * unsaturated value gets averaged against one that is far past saturation
 * and the far one drags the result somewhere neither input would ever have
 * rendered on its own. A cell sitting exactly ON the horizontal surface
 * (hdepth 0) inside a full-height column (vdepth 200) renders at a blended
 * depth of 7 rather than 0 - a surface cell painted as if it were seven
 * cells under. Clamping each axis to the band BEFORE the blend makes the two
 * numbers commensurable with the scale they are about to be rendered on, and
 * bounds every error the broken chain can produce to 24 raw units instead of
 * 255 - which, at landscape lock's own vertical blend weight (9-13 of 256),
 * is at most 1.2 depth units, well under one of the four shade steps.
 *
 * IT IS THE INVARIANT sand_priv.h ALREADY CLAIMS. mark_depth_band()'s own
 * comment states, as the reason it only ever dirties a band of
 * MATERIAL_LIQUID_DEPTH_BAND rows around a new surface, that "anything
 * further than that already saturates to the same flat body colour whether
 * the true depth is one cell more or a hundred". That was simply not true of
 * this file before this change - a cell a hundred rows away DID render
 * differently, because its unclamped vdepth reached the blend intact - so
 * the simulation's pour-staleness fix was under-marking against an
 * assumption the renderer broke. MATERIAL_LIQUID_DEPTH_BAND's own comment
 * (material.h) asks for exactly this: the two "have to agree by
 * construction, not by coincidence". Now they do.
 *
 * MEASURED, same model, same scenes, WITH this clamp:
 *
 *     landscape, slow sway   2.81 -> 1.72 transitions/column (worst frame
 *                            27.80 -> 13.53); 2-step jumps 51 -> 0
 *     landscape, tremor only 1.50 -> 0.79 transitions/column (worst frame
 *                            14.29 -> 4.00); 2-step jumps 14 -> 0;
 *                            isolated lines 7.74 -> 4.16 (worst 183 -> 45)
 *     portrait               3.02 -> 3.02, 0 -> 0 - a no-op, as it must be:
 *                            that pool never reaches 24 in the first place
 *
 * For scale, the same transitions-per-column measurement run over the
 * device's own captures reads 2.96-7.04 on the build before 882f2e7 and
 * 3.57-25.31 on 882f2e7 itself (with up to 74 two-step jumps in one frame).
 * The unclamped model lands in the second range and the clamped model in the
 * first.
 *
 * IT ALSO GIVES THE GRADIENT BACK, which was not the goal and is the larger
 * effect. Dumping the rendered shade index across the landscape-lock pool,
 * one row per line, unclamped against clamped (4 = surface, 0 = fully
 * saturated):
 *
 *     unclamped   .....211111100000000.   clamped   .....333332222221111.
 *                 .....221111110000000.             .....333332222221111.
 *                 .....222111111000000.             .....333332222221111.
 *
 * Unclamped, the vertical accumulator running to 255 down a full-height
 * column pushed nearly the whole pool past the saturation point: the body
 * renders as one flat darkest tone with a thin two-step rim, and the little
 * variation left is exactly the row-to-row noise the report called lines.
 * Clamped, the same frame reads as an even 3-2-1 ramp inward from the
 * surface, the same on every row. The device captures show the crushed
 * version - shade 1 covering most of the water body - which is the same
 * symptom from the other end: this pool was not merely banded, it had lost
 * most of the depth cue the whole feature exists to provide.
 *
 * THE BLEND ITSELF IS UNTOUCHED BY THIS COMMIT - still the continuous Q8
 * crossfade, still driven solely by gravity's own |gx|/|gy| ratio, with no
 * discrete axis and nothing to chatter (LOCAL DEPTH's own top comment).
 * Clamping only brings the two ENDS of that crossfade onto the scale it is
 * interpolating for; if anything it makes the crossfade gentler, since the
 * two endpoints can no longer be 231 apart.
 *
 * (THE BLEND ITSELF WAS LATER REPLACED, by a projection-then-max combiner -
 * see LOCAL DEPTH's own top comment, DEFECT ONE/TWO, and "THE CEILING
 * STOPPED BEING A FIXED CONSTANT" further below for why a fixed
 * MATERIAL_LIQUID_DEPTH_BAND ceiling, correct for this commit's own blend,
 * had to become gravity-aware once the combiner started scaling each axis
 * down by its own projection weight. This paragraph is left as it was
 * written, describing accurately what THIS commit did and did not touch.)
 *
 * WHAT THIS DOES NOT FIX, on purpose: a broken chain inside a pool SHALLOWER
 * than the band still renders that one row wrong, by up to its own depth.
 * Fixing that needs either a depth byte per CELL (41,216 bytes on top of the
 * grid's own - unaffordable, the same trade col_stable_depth[]'s comment
 * already refuses) or advancing the accumulator across skipped rows, which
 * is a full-grid pass every frame and would need to be re-decided on the
 * device against LOCAL DEPTH's own "do not fix this into a full-grid pass"
 * note and measured there. This closes the case the device actually
 * reported - a chain broken across a hundred rows - for the cost of a
 * different constant in four comparisons.
 *
 * THE CEILING IS RAISED, NOT THE BAND ITSELF - LOCAL_DEPTH_COUNT_CEILING
 * (30) is a bigger number than MATERIAL_LIQUID_DEPTH_BAND (24), but the
 * combiner still clamps its OUTPUT to MATERIAL_LIQUID_DEPTH_BAND (see THE
 * COMBINER ITSELF, paint_row_n() below) - the band itself, and everything
 * downstream of it (DEPTH_SATURATE_CELLS in material.c, mark_depth_band()
 * in sand_priv.h), is completely unchanged. Only how far the RAW COUNT is
 * allowed to climb before that clamp moved. Three other shapes were tried
 * first and rejected, each after being measured against THIS SECTION's own
 * test (test_a_sparse_repaint_does_not_band_a_tall_liquid_column, suite_
 * sand.c) - not out of caution, out of a reproduced failure each time:
 *
 * ATTEMPT ONE, the count clamped at the plain band itself (`depth =
 * max(min(vcount,24)*wv_q8, min(hcount,24)*wh_q8) >> 8`, no further clamp
 * needed since 24*256>>8 never exceeds 24): closed DEFECT ONE and DEFECT
 * TWO (LOCAL DEPTH's own top comment) but left DEFECT THREE - a fully-
 * saturated cell's reported depth still depended on gravity's angle,
 * because clamping the RAW COUNT at 24 and THEN projecting by a sub-256
 * weight always reports below 24. Device-measured as a global "breathing"
 * of the whole pool's saturated interior, one whole shade step, across the
 * exact tilt window the device's own screenshots were taken at - see
 * DEFECT THREE in LOCAL DEPTH's own top comment for the numbers.
 *
 * ATTEMPT TWO, scaling the ceiling ITSELF per axis (`ceil(BAND*256/w)`,
 * capped at 255) so a fully-climbed low-weight axis's count could still
 * project to the full band: fixed the breathing, but scaling the ceiling
 * up for a low-weight axis ALSO scales up the ABSOLUTE SIZE of the swing a
 * BROKEN CHAIN on that axis can produce, before its own weight gets a
 * chance to damp it down - a stale value inherited from an unrelated row
 * (exactly the failure this section's own fix bounds) can land anywhere
 * from 0 to the ceiling, and a ceiling chosen so `ceiling * w ~= BAND*256`
 * keeps that swing's PROJECTED size close to the FULL BAND for every
 * weight, including small ones - exactly cancelling the protection this
 * section's own fixed ceiling relies on. Measured directly: substituting
 * the scaled formula into this test's own mirror, same landscape-lock scene
 * (gravity mostly horizontal, vertical axis low-weight and long-running -
 * no real boundary within the grid's own height): 200 banded pairs in the
 * worst frame, indistinguishable from this test's own RED case (255
 * ceiling, i.e. no clamp at all). Rejected.
 *
 * ATTEMPT THREE, stop projecting a COUNT at combine time at all - col_
 * stable_depth[]/row_stable_depth[] accumulating PROJECTED DEPTH directly,
 * in eighths of a cell, climbing by that frame's own per-step increment
 * (0-8 eighths) instead of a flat `+1`, saturating at a fixed ceiling in
 * the SAME eighths units: closed the breathing completely (a saturated
 * axis reads the identical value at every angle, by construction) but was
 * REJECTED for a more fundamental reason than attempt two's: a plain cell
 * COUNT is gravity-agnostic (it means the same thing regardless of when it
 * was accumulated), so projecting it fresh at combine time is always
 * correct; an eighths value bakes a specific frame's own gravity into every
 * increment, so a value built up under an OLD angle carries that angle,
 * uncorrected, into a LATER frame where gravity has since rotated. Measured
 * directly against this exact section's own test: 52 banded pairs in the
 * worst frame (threshold 8), and critically, changing the test's ceiling
 * parameter between 255 and the eighths design's own cap made NO
 * DIFFERENCE (both measured 52) - proving the regression was about
 * gravity's angle DRIFTING while stale values were still live, not about
 * where the ceiling sat; freezing the test's own gravity sway to a fixed
 * angle dropped the failure to 0, confirming it. Rejected.
 *
 * ATTEMPT FOUR, SHIPPED: keep everything about attempt one - col_stable_
 * depth[]/row_stable_depth[] hold a plain CELL COUNT again, climbing by a
 * flat `+1`, projected at COMBINE TIME exactly as before - and change
 * exactly one number: raise the ceiling that count saturates at, from the
 * band itself (24) to LOCAL_DEPTH_COUNT_CEILING (34), enough headroom that
 * `ceiling * w >> 8` still reaches the band even at the WORST angle (45
 * degrees, where a Q8 weight bottoms out around 183 of 256 for either
 * axis - `ceil(24*256/183) == 34`, and `34 * 183 >> 8 == 24` exactly; see
 * this constant's own comment for the exact value chosen and how the
 * range it was tested against was measured, not merely calculated from
 * that one number in isolation). Because
 * storage stayed a gravity-agnostic COUNT, attempt three's fatal flaw
 * cannot recur - nothing about a count depends on when it was accumulated
 * - while the raised headroom closes attempt one's breathing the same way
 * attempt two tried to, without attempt two's own regression, because the
 * ceiling is FIXED (not scaled per axis, so it cannot inflate a stale
 * swing's absolute size the way a scaled ceiling did). THE COMBINER now
 * needs an EXPLICIT clamp to MATERIAL_LIQUID_DEPTH_BAND after the max (the
 * raised ceiling means `count * weight >> 8` CAN exceed the band now,
 * unlike attempt one's design where it never could) - see THE COMBINER
 * ITSELF in paint_row_n() for where that clamp lives. */

/* Called once per frame, alongside material_set_gravity() - same gravity
 * vector, same reason: material_colours()'s liquid interior needs THIS
 * frame's own local-depth projection weights, not last frame's, and working
 * out the weights and both scan directions once here is what keeps
 * paint_row_n() itself down to two multiplies, a compare and a shift (plus
 * the clamp) per cell, no divide, for LOCAL DEPTH's own comment above to
 * budget against. */
static void update_local_depth_gravity(int gx, int gy)
{
    const int ax = im_abs(gx), ay = im_abs(gy);

    /* TWO DIVIDES PER FRAME, not per cell - the same budget class
     * build_xflow() (sand.c) already spends on its own per-frame q_q8, and
     * the class material_set_gravity()'s own setup already spends
     * elsewhere in this app; material_set_gravity() calls im_len() too, on
     * the same (gx, gy), so this is the second such call this frame, not
     * the first (im_len() has no state of its own to share between them,
     * so there is nothing to hoist). At gx == gy == 0 (flat, or free fall)
     * there is no gravity direction for either projection to mean anything
     * against, so both weights fall back to an arbitrary, harmless 128
     * (half of a full-weight axis) rather than a division by zero -
     * nothing meaningfully "settles" with no gravity direction anyway, so
     * it does not matter which way this tie is broken; see the "wv_q8 ==
     * 256, wh_q8 == 0" invariant local_depth_weight_v_q8/_h_q8's own
     * comment relies on above for straight-down gravity - that invariant
     * only has to hold when len is nonzero, which this fallback is not
     * pretending to satisfy. */
    const int len = im_len(gx, gy);
    local_depth_weight_v_q8 = (len != 0) ? (256u * (unsigned)ay) / (unsigned)len : 128u;
    local_depth_weight_h_q8 = (len != 0) ? (256u * (unsigned)ax) / (unsigned)len : 128u;

    /* Two independent sign checks, one per axis - not one flag gating
     * whichever axis used to be dominant - because both scans run every
     * frame now. See local_depth_v_reverse/local_depth_h_reverse's own
     * comment above. */
    const bool new_v_reverse = (gy < 0);
    const bool new_h_reverse = (gx < 0);

    /* A REVERSAL OF THIS AXIS'S OWN WALK DIRECTION invalidates the debounce
     * that rides on it - a DIFFERENT failure from the "which axis is
     * dominant" flip LOCAL DEPTH's own top comment already explains
     * is a non-issue now. Measured directly, reproducing a device report of
     * a visible pop specifically near the "landscape lock" orientation:
     * hold the device close to landscape (gx large and steady, pressed
     * against the wall; gy small and hand-tremor-noisy, so it crosses zero
     * often - 829 sign flips over a 6000-frame model) against a tall water
     * column standing against that wall. Every time local_depth_v_reverse
     * flips, "the neighbour toward the surface" (v_toward_surface below)
     * swaps from `above` to `below` or back, which relocates WHICH ROW in
     * the column is the genuine boundary - the top row when ascending, the
     * bottom row when descending - between two candidates that are BOTH
     * real, not one real boundary and one blip. col_top_row[cx] has only one
     * slot, shared by both: because the tracked row keeps changing out from
     * under it every ~7 frames (this noisy a gy crosses zero that often),
     * the "same row asked twice -> commit" branch almost never fires, so
     * col_stable_depth[cx] takes the HOLD branch on nearly every encounter -
     * climbing (`+1`) instead of resetting - and compounds without bound.
     * Traced at one column across five consecutive flips: 40, 119, 199, 200,
     * before finally, by chance, landing two consecutive asks on the same
     * row and dropping to 0. That is a raw-accumulator swing of up to 200
     * (of a 0-255 range) leaking into the displayed, BLENDED depth as a real
     * jump - small in this axis's own blend weight near landscape lock, but
     * still visible, which is exactly why the report was specific to that
     * orientation: near landscape, gy is the small/noisy component (so this
     * axis's own reverse flag flips often) while vdepth still carries a
     * nonzero blend weight; near portrait the symmetric risk sits on
     * local_depth_h_reverse, but there hdepth's blend weight is the one near
     * zero, so the same corruption stays invisible.
     *
     * THE FIX: the moment this axis's OWN reverse flag actually changes
     * value, the debounce state it feeds is stale by construction - the
     * "row most recently asked" and "depth accumulated so far" both describe
     * a boundary relationship that direction no longer has - so both get
     * invalidated before any cell paints under the new direction: the
     * tracker back to its "untracked" sentinel (255, matching col_top_row[]/
     * row_top_col[]'s existing convention) and the accumulator back to a
     * clean 0, exactly the same clean reset already used above for "not a
     * liquid cell at all" - not a partial fix that clears only the tracker
     * and lets HOLD keep climbing from an old, direction-stale value. A
     * clean 0 is not itself a visible defect: draw_dirty_rows() below wakes
     * every liquid row on its own periodic tick (LOCAL_DEPTH_WAKE_MS) in
     * ascending/descending order matching this same reversal, so a settled
     * column's real depth is rebuilt row-by-row within that same tick,
     * bounded by the pool's own depth range rather than by how many
     * consecutive flips have happened. O(grid_w) (or grid_h) work, but only
     * on the frame a reversal actually lands - measured at roughly 5 times a
     * second at 30fps in the noisy-gy model above, trivial next to
     * paint_row_n()'s own per-cell cost. See
     * test_a_direction_flip_does_not_corrupt_the_boundary_debounce in
     * suite_sand.c for the reproduction and the bound this closes.
     *
     * WHAT THIS RESET IS AND IS NOT, restated after the banding report that
     * followed it (THE SATURATING CLIMB's own comment above): it is a
     * BOOKKEEPING correction, not a rendering one. Modelling the device's
     * own landscape-lock gravity through real physics and real dirty-row
     * sparsity showed running WITH this reset and WITHOUT it to be visually
     * indistinguishable - 2.81 versus 2.80 vertical shade transitions per
     * water column - so it is not a knob to reach for when something looks
     * wrong on the panel. It earns its place elsewhere: in a pool SHALLOWER
     * than MATERIAL_LIQUID_DEPTH_BAND, in the orientation where that axis
     * carries the large blend weight, the HOLD-compounding climb would
     * otherwise pin a genuinely 6-cell-deep column at the saturating 24 and
     * render it as fully deep. The clamp bounds how far that can go; this
     * reset is what stops it going there at all. */
    if (new_v_reverse != local_depth_v_reverse_prev) {
        for (int cx = 0; cx < grid_w; cx++) {
            col_top_row[cx] = 255u;
            col_stable_depth[cx] = 0u;
        }
        local_depth_v_reverse_prev = new_v_reverse;
    }
    if (new_h_reverse != local_depth_h_reverse_prev) {
        for (int cy = 0; cy < grid_h; cy++) {
            row_top_col[cy] = 255u;
            row_stable_depth[cy] = 0u;
        }
        local_depth_h_reverse_prev = new_h_reverse;
    }

    local_depth_v_reverse = new_v_reverse;
    local_depth_h_reverse = new_h_reverse;
}

/* LOCAL DEPTH'S OWN PERIODIC WAKE - closes the same gap SHINE_STEP_MS
 * already closes for the travelling shine (see that constant's own comment
 * above, and advance_shine() below, for the pattern this mirrors almost
 * exactly).
 *
 * THE BUG: the blend above is recomputed from THIS FRAME's gravity, every
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
 * by the tilt filter but never perfectly still - so local_depth_weight_v_q8/
 * _h_q8 above keep changing, frame after frame, with no cell in a settled
 * pool ever moving to earn that pool's rows a repaint. The result: a
 * sleeping block's displayed depth is stuck at whatever it was the last
 * time something nearby genuinely disturbed it, while a neighbouring block
 * still being redrawn for an unrelated reason repaints with the CURRENT
 * combiner output - a hard, rectangular seam between "stale" and "fresh"
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

    /* LOCAL DEPTH's vertical walk reads a FIXED neighbour row - "above" or
     * "below", whichever is toward the surface this frame, per gy's OWN
     * sign - chosen ONCE for the whole row, not per cell: gravity does not
     * change from one column to the next. NULL exactly when that neighbour
     * is off the grid (the top or bottom edge), which the per-cell check
     * below reads as "not the same material" - the same off-grid-is-a-wall
     * convention `mask` above already relies on. Read UNCONDITIONALLY now,
     * every row, every frame - there is no "vertical is dominant" gate left
     * to ask about. See col_stable_depth[]'s own comment above this
     * function for the full mechanism. */
    const uint8_t *v_toward_surface = local_depth_v_reverse ? below : above;

    /* LOCAL DEPTH's horizontal walk needs the whole ROW scanned
     * surface-first, so each cell's read of row_stable_depth[cy] (the
     * running value carried across this whole scan - see that array's own
     * comment) always sees an already-processed neighbour's own result -
     * exactly why this direction, and only this one, needs the SCAN itself
     * reversed rather than which array slot is read. This now governs the
     * loop's own iteration order UNCONDITIONALLY, by gx's OWN sign: the
     * vertical walk does not care what order cx takes (each column's depth
     * is independent of every other column's, via col_stable_depth[]), so
     * reordering the scan for the horizontal walk's sake never disturbs
     * it. */
    const int cx_first = local_depth_h_reverse ? grid_w - 1 : 0;
    const int cx_step  = local_depth_h_reverse ? -1 : 1;

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

        /* THE PER-CELL COST OF LOCAL DEPTH, in full, now that BOTH walks run
         * unconditionally and get COMBINED: two comparisons against a
         * neighbour's material id (one per axis), one array read+write for
         * the vertical hold-then-commit debounce (col_stable_depth[]/
         * col_top_row[] - see that array's own comment above this function
         * for the full mechanism, including the two bugs an earlier version
         * of it had and the dead second array an even earlier version
         * carried alongside it for nothing), one more array read+write for
         * the horizontal debounce (row_stable_depth[]/row_top_col[] - see
         * that array's own comment for the mechanism, transposed onto rows),
         * plus the combiner itself - two multiplies, one compare, one shift,
         * plus the clamp (one more compare), no divide (the only divides are
         * update_local_depth_gravity()'s, once a frame, not here) - see THE
         * COMBINER ITSELF's own comment below for the exact arithmetic and
         * why the clamp is new. Computed for every cell, liquid or not - the
         * same as the old depth_acc was - because material_colours() is the
         * only consumer that ever reads the result (only for a liquid's
         * interior), and a branch to skip this for non-liquids would cost
         * more than the comparisons it would save. */
        const bool v_same_material = (v_toward_surface != NULL) &&
            (CELL_MATERIAL(v_toward_surface[cx]) == CELL_MATERIAL(row[cx]));

        /* THE VERTICAL HOLD-THEN-COMMIT DEBOUNCE ITSELF, feeding THE
         * COMBINER below directly - see col_stable_depth[]'s own comment
         * above this function for the full mechanism (and the two bugs an
         * earlier version of this block had, found from exactly this kind
         * of trace). There is no freeze branch here any more: the diagonal
         * dead zone and the axis Schmitt trigger it used to protect are
         * both gone (LOCAL DEPTH's own top comment) - the combiner never
         * has a "wrong regime" reading for a freeze to guard against, so
         * this is exactly main's original debounce, just no longer wrapped
         * in a branch that could suspend it. */
        unsigned vdepth;
        if (material_of(row[cx])->kind != KIND_LIQUID) {
            /* NOT a liquid cell: depth is irrelevant here - material_
             * colours() never reads it for anything but a liquid interior
             * - and must not be allowed to accumulate through this cell,
             * or a run of open air (or any other non-liquid material)
             * above a real boundary corrupts the value that boundary
             * inherits. Clean reset, and col_top_row[cx] is left alone -
             * this cell is not a boundary request of any kind. */
            vdepth = 0u;
        } else if (v_same_material) {
            /* Confirmed continuation of a liquid body - climb by one,
             * stopping at the SATURATION POINT rather than at a byte's own
             * 255 - see THE SATURATING CLIMB's own comment above this
             * function for why that clamp is the whole fix for the banding
             * report, why it is now LOCAL_DEPTH_COUNT_CEILING rather than
             * the plain MATERIAL_LIQUID_DEPTH_BAND (raised to fix DEFECT
             * THREE's breathing without the two rejected designs' own
             * regressions), and for why it stays FIXED rather than scaling
             * with this axis's own weight either way. Not a boundary
             * request either, so col_top_row[cx] is left alone here too. */
            vdepth = col_stable_depth[cx] < LOCAL_DEPTH_COUNT_CEILING
                ? col_stable_depth[cx] + 1u : LOCAL_DEPTH_COUNT_CEILING;
        } else if (col_top_row[cx] == (uint8_t)cy) {
            /* THIS EXACT ROW asked for a reset the last time it was
             * painted too - a real, lasting boundary, not a blink.
             * Commit. */
            vdepth = 0u;
        } else {
            /* A reset is being asked for at a row that was NOT the tracked
             * boundary - either nothing was tracked yet, or the boundary
             * moved. HOLD: keep climbing as if nothing happened, and start
             * tracking THIS row, so it asking again next time it is
             * painted will match and commit. */
            vdepth = col_stable_depth[cx] < LOCAL_DEPTH_COUNT_CEILING
                ? col_stable_depth[cx] + 1u : LOCAL_DEPTH_COUNT_CEILING;
            col_top_row[cx] = (uint8_t)cy;
        }
        col_stable_depth[cx] = (uint8_t)vdepth;

        const bool has_h_neighbour =
            local_depth_h_reverse ? (cx < grid_w - 1) : (cx > 0);
        const int h_neighbour_cx = local_depth_h_reverse ? cx + 1 : cx - 1;
        const bool h_same_material = has_h_neighbour &&
            (CELL_MATERIAL(row[h_neighbour_cx]) == CELL_MATERIAL(row[cx]));

        /* THE HORIZONTAL HOLD-THEN-COMMIT DEBOUNCE, transposed onto rows -
         * feeding THE COMBINER below directly, exactly the same shape as
         * the vertical block above. See row_stable_depth[]/row_top_col[]'s
         * own comment above this function for the full mechanism and its
         * accepted limitation; this block mirrors the vertical debounce
         * block above line for line, with cy standing in for "this axis's
         * own coordinate" (cx there) and cx standing in for "the other
         * axis's coordinate being tracked" (cy there). */
        unsigned hdepth;
        if (material_of(row[cx])->kind != KIND_LIQUID) {
            /* NOT a liquid cell - same reasoning as the vertical branch
             * above: must not accumulate through it, and row_top_col[cy] is
             * left alone since this is not a boundary request. */
            hdepth = 0u;
        } else if (h_same_material) {
            /* Confirmed continuation of a liquid body - climb by one,
             * saturating at LOCAL_DEPTH_COUNT_CEILING for the same reason
             * the vertical branch above does (THE SATURATING CLIMB's own
             * comment) - fixed, not scaled by this axis's own weight
             * either. Not a boundary request, so row_top_col[cy] is left
             * alone here too. */
            hdepth = row_stable_depth[cy] < LOCAL_DEPTH_COUNT_CEILING
                ? row_stable_depth[cy] + 1u : LOCAL_DEPTH_COUNT_CEILING;
        } else if (row_top_col[cy] == (uint8_t)cx) {
            /* THIS EXACT COLUMN asked for a reset the last time THIS ROW
             * was painted too - a real, lasting boundary, not a blink.
             * Commit. */
            hdepth = 0u;
        } else {
            /* A reset is being asked for at a column that was NOT the
             * tracked boundary for this row - either nothing was tracked
             * yet, or the boundary moved. HOLD: keep climbing as if nothing
             * happened, and start tracking THIS column, so it asking again
             * next time this row is painted will match and commit. */
            hdepth = row_stable_depth[cy] < LOCAL_DEPTH_COUNT_CEILING
                ? row_stable_depth[cy] + 1u : LOCAL_DEPTH_COUNT_CEILING;
            row_top_col[cy] = (uint8_t)cx;
        }
        row_stable_depth[cy] = (uint8_t)hdepth;

        /* THE COMBINER ITSELF - PROJECTS each axis's raw COUNT onto the
         * gravity direction (multiplying by THIS FRAME'S OWN Q8 weight,
         * local_depth_weight_v_q8/_h_q8), then takes the MAX of the two
         * projections rather than a weighted average. See LOCAL DEPTH's own
         * top comment for DEFECT ONE and DEFECT TWO, the device-measured
         * shadow and tilt-scale bugs this closes, and why max - not a blend
         * of the projections - is the combiner the fix actually needs: each
         * axis walk is a LOWER BOUND on the true depth (it can terminate
         * early at an inclusion rather than at the free surface), so
         * max-of-lower-bounds is the tighter estimate, where an average
         * instead lets either blocked axis drag the result down
         * proportionally to its own weight - exactly the "shadow" defect
         * one is. On an ideal planar surface both projections equal the
         * true depth exactly, so max is also exact there, closing defect
         * two's tilt-dependent inflation.
         *
         * `local_depth_weight_v_q8`/`_h_q8` are each 0-256, computed once
         * per frame - the whole reason a per-cell divide is never needed.
         * Shifting AFTER the max rather than shifting each product
         * separately saves one of the two shifts the naive order would
         * cost (right shift by a fixed amount is monotonic, so
         * max(a,b)>>8 == max(a>>8, b>>8) for the non-negative values here) -
         * two multiplies, one compare, one shift, plus the clamp below.
         *
         * AN EXPLICIT CLAMP TO MATERIAL_LIQUID_DEPTH_BAND NOW FOLLOWS THE
         * MAX, unlike the ORIGINAL shape of this combiner (before
         * LOCAL_DEPTH_COUNT_CEILING was raised above the band itself) - see
         * THE SATURATING CLIMB's own comment above this function, "THE
         * CEILING IS RAISED, NOT THE BAND ITSELF", for why: vdepth/hdepth
         * are now bounded to [0, LOCAL_DEPTH_COUNT_CEILING], a number BIGGER
         * than MATERIAL_LIQUID_DEPTH_BAND, specifically so a low-weight
         * axis's projection can still reach the band at the worst tilt
         * angle - which means a HIGH-weight axis's own projection, at the
         * SAME raised ceiling, CAN exceed the band, and must be clamped
         * back down here. material_colours() (material.c) also clamps
         * `depth` to DEPTH_SATURATE_CELLS independently regardless - a
         * second, unrelated safety net - but this clamp is no longer
         * redundant the way it was before the ceiling was raised: without
         * it, an unsaturated but high-weight cell could report a depth
         * ABOVE the band, which material_colours() would still clamp for
         * SHADING purposes but which nothing else in this file expects to
         * see past MATERIAL_LIQUID_DEPTH_BAND (col_top_row[]/row_top_col[]
         * store row/column indices, not depths, so this is purely about
         * `depth` itself staying in the range every comment in this file
         * already assumes it does). */
        const unsigned v_proj = vdepth * local_depth_weight_v_q8;
        const unsigned h_proj = hdepth * local_depth_weight_h_q8;
        const unsigned depth_raw = (v_proj > h_proj ? v_proj : h_proj) >> 8;
        const unsigned depth = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

        /* row_has_liquid[]'s own population point - see that array's
         * comment above paint_row_n() for the mechanism this feeds. ANY
         * liquid cell marks the row, rim included - not gated on `mask`
         * at all, unlike material_colours()'s own interior test (KIND_
         * LIQUID and `(mask & MATERIAL_EDGE_CARDINAL) == 0`, material.c) -
         * see row_has_liquid[]'s own comment for why the wider condition is
         * the fix, not an oversight. */
        if (material_of(row[cx])->kind == KIND_LIQUID) {
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

    /* Ordinarily ascending - but see col_stable_depth[]'s own comment in
     * paint_row_n() ("WHY draw_dirty_rows() BELOW REVERSES ROW ORDER...")
     * for why a gravity-UP frame walks this loop in the OPPOSITE order
     * instead: col_stable_depth[cx]'s climb is shared across every row of
     * the SAME column, so it needs the row nearer the surface (here, the
     * BOTTOM of the screen) painted first, or a dirty row further from the
     * surface reads whatever a DIFFERENT, farther row left behind the last
     * time IT was nearer the front of the sweep, not last frame's own value
     * for the row actually being painted. row_stable_depth[] has no such
     * dependency (see its own comment for why not), so this reversal is
     * entirely for the vertical array's sake. THE CONDITION IS SIMPLER now
     * than it used to be: the vertical walk runs every frame unconditionally
     * (see LOCAL DEPTH's own top comment), so this is just "does gravity
     * point up" - `local_depth_v_reverse` alone - with no "and is vertical
     * dominant this frame" to ask alongside it any more. Reversing here,
     * rather than juggling which array slot means "toward the surface"
     * inside the depth bookkeeping itself, is safe because nothing else in
     * this loop depends on row order - dirty_rows[cy], row_run_x0/x1/n,
     * row_has_shine[cy] and row_has_liquid[cy] are all indexed by cy
     * directly, and gfx_mark_dirty() below only ever unions a bounding box,
     * which does not care what order the boxes arrive in either. */
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

    /* The liquid interior's LOCAL DEPTH combiner needs its own per-frame
     * facts from this same gravity vector - the projection weights and
     * which way each of the two independent scans runs - but it is not
     * material_set_
     * gravity()'s to compute: the persistent per-column and per-row arrays
     * it feeds belong to THIS file, which owns the row-by-row paint call
     * sequence they are carried across (see col_stable_depth[]'s own
     * comment in paint_row_n() below for the full mechanism, and why
     * material.c has nothing left to do with depth at all). */
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
