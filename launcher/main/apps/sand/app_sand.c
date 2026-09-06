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

/* Centered, absolutely-placed buttons in draw_menu(). UI_ROW_HEIGHT matches
 * app UIs. MENU_BTN_W fits "QUALITY: VERY LOW" (272 px) plus margins, leaving
 * 34 px on each side of 368 px GFX_WIDTH. mu_draw_control_text() centers and
 * clips labels, chopping wider ones. 300 px width provides 28 px buffer. */
#define MENU_BTN_W    300
#define MENU_BTN_H    UI_ROW_HEIGHT
#define MENU_BTN_GAP  20

/* How big a blob each touched frame drops, in pixels rather than cells - see
 * the comment above handle_pour_input()'s use of these for why. Large enough
 * that a tap is clearly a handful of sand rather than a speck. */
#define POUR_RADIUS_PX   10      /* was 5 cells at 2 px */

/* Pouring runs at a fixed rate like the simulation. Spawning once per frame
 * made the pour rate follow the framerate, which swings between 60 and over
 * 200 due to partial updates. Holding a finger down delivered three times as
 * much sand when the screen was quiet, causing a pile-up under the finger. */
#define POUR_HZ       60
#define POUR_STEP_MS  (1000 / POUR_HZ)

/* The eraser is wider than the pour. Removing material is a corrective action
 * and wants to feel broad; pouring wants to feel placed. Pixels, not cells -
 * see POUR_RADIUS_PX above. */
#define ERASE_RADIUS_PX  16      /* was 8 cells at 2 px */

/* Emitter has larger erase tolerance than material for precision. Emitter is
 * a point, needing wider tolerance for aiming. Material uses ERASE_RADIUS_PX
 * for area sweep, precise but can overwipe. Emitter tolerance accounts for
 * finger size. See handle_pour_input()'s erase branch. */
#define ERASE_EMITTER_RADIUS_PX  32

/* Its own radius (not eraser's); blast must appear larger. 50 px (25 cells)
 * balances full density within APP_IMPULSE_MAX, scoring 106.5 "grains outside
 * footprint" with 2.4 avg max-throw and 78.7 avg destroyed. Larger radii
 * re-engage thinning. */
#define DETONATE_RADIUS_PX  50

/* FIXED ENTRY COUNT, not formula in DETONATE_RADIUS_PX. Was `(355*r*r)/113 +
 * 5*r + 3`. 2,048 is SAND_IMPULSE_BUDGET_BYTES / sizeof(impulse_t) - 12,288 /
 * 6. Round entry count matches GRID_W_MAX, BLOCK_COLS_MAX. See comments below
 * for heap arithmetic and _Static_assert. */
#define APP_IMPULSE_MAX  2048

/* BUDGET APP_IMPULSE_MAX is set to 12,288 bytes, based on observed heap
 * fragmentation, not total free heap, to prevent malloc failures. Fixed byte
 * budget avoids recalculating with radius changes */
#define SAND_IMPULSE_BUDGET_BYTES  12288

/* CONFIRMS TWO HAND-CHOSEN CONSTANTS AGREE. DOES NOT VALIDATE
 * SAND_IMPULSE_BUDGET_BYTES SAFETY. CATCHES MISMATCH IF APP_IMPULSE_MAX
 * RAISED WITHOUT CHECKING BUDGET. NECESSARY BUT NOT SUFFICIENT. SEE
 * SAND_IMPULSE_BUDGET_BYTES COMMENT FOR LIMITATIONS. */
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

/* Finger selection from palette panel. Tap selects, unlike cycling. PWR
 * cycles PAINT/ERASE/DETONATE. Press cheaper than hold. MAT_WOOD but not
 * MAT_STEAM. Whole cells used, not material ids. */
static const cell_t brushes[] = {
    CELL_MAKE(MAT_SAND, 0),  CELL_MAKE(MAT_WATER, 0),
    CELL_MAKE(MAT_STONE, 0), CELL_MAKE(MAT_GAS, 0),
    CELL_MAKE(MAT_FIRE, 0),  CELL_MAKE(MAT_WOOD, 0),
    CELL_MAKE(MAT_OIL, 0),   CELL_MAKE(MAT_LAVA, 0),
    CELL_MAKE(MAT_ACID, 0),  CELL_MAKE(MAT_GLASS, 0),
    CELL_MAKE(MAT_SNOW, 0),  CELL_MAKE(MAT_DIRT, 0),
    MATX(MATX_ICE),      MATX(MATX_PLANT),
    GUNPOWDER_CELL(0), /* dry, tone 0 - see brush_color()'s own comment for
                         * why the panel tile itself paints a different code */
};
#define BRUSH_COUNT ((int)(sizeof(brushes) / sizeof(brushes[0])))

/* The palette panel's grid is derived from BRUSH_COUNT, not hand-synced. This
 * causes a BUILD failure if adding a brush makes the panel exceed screen
 * height. See palette.h and PALETTE_FITS's comment. */
_Static_assert(PALETTE_FITS(BRUSH_COUNT),
               "the palette panel for BRUSH_COUNT brushes is taller than the "
               "screen at some orientation - see palette_cols()/PALETTE_TILE "
               "in palette.h");

/* A brush can place a tap by tapping a palette tile. This is handled by
 * `handle_pour_input()` in `sand_ui.h`. `brush_mode_t` persists across
 * simulations, indicating what a tap places, not its state. `sand_init()`
 * resets REAL emitters but not `brush_mode`, so Water may still be flagged as
 * a source if no tap is present. */
static uint8_t brush_mode[BRUSH_COUNT];   /* brush_mode_t per brush */

/* UI state: screen, brush, eraser, palette bookkeeping. Uses sand_ui.h's
 * brushes/brush_mode tables. Replaces file-scope screen, brush, mode statics
 * with ui.screen, ui.brush, ui.mode. Zero-initialised:
 * ui.screen=SAND_UI_MENU, ui.brush=0, ui.mode=SAND_MODE_PAINT. */
static sand_ui_t ui = {
    .brushes     = brushes,
    .modes       = brush_mode,
    .brush_count = BRUSH_COUNT,
};

/* Duration mode label stays after significant change, balancing readability
 * and non-obtrusiveness. */
#define LABEL_MS 1800

#define LABEL_MARGIN 18
#define LABEL_SCALE   2

/* Below this the board is being held still enough that the reading is noise,
 * and letting noise through means friction is quietly unlocked the whole time
 * the app is open. */
#define SHAKE_DEADZONE 40

/* The simulation runs at a fixed rate, independent of framerate. A grain
 * moves one cell per step, with steps-per-second controlling sand fall speed.
 * Linking steps to frame rate caused fluctuations (60-230 fps) due to partial
 * frames, making sand fall faster during lulls. 60 Hz is the max rate for
 * grains, not streaks. */
#define SIM_HZ            60
#define SIM_STEP_MS       (1000 / SIM_HZ)

/* Never run more than this many steps to catch up after a stall. Two jobs
 * stop the spiral where a long frame schedules extra steps, making the next
 * frame longer. This caps grain speed to four cells per frame to avoid
 * teleportation. Better to let the simulation lose time. */
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

/* Up to ROW_MAX_RUNS cell-index ranges per row, not pixel ranges. Tracks last
 * drawn material for clearing and distinct blobs. GRID_H_MAX * ROW_MAX_RUNS
 * entries, using only first grid_h rows below ULTRA. row_run_n[cy] shows used
 * slots. */
static uint16_t   *row_run_x0;
static uint16_t   *row_run_x1;
static uint8_t    *row_run_n;
static sand_t      sim;
static tilt_t      tilt;
static bool        failed;
static uint32_t    label_left_ms;    /* countdown for the mode label */

/* False from start_sim() until START finger lift. Without this, initial touch
 * triggers `input->down` on first RUNNING frame, making handle_pour_input()
 * misinterpret it as a deliberate pour, dropping sand. Similar to
 * `swallow_release` in sand_ui.h: wait for release, not a fixed delay, to
 * avoid misinterpretation. */
static bool        input_ready;

/* Shell quarter the palette panel was last painted at - see sand_frame()'s
 * SAND_UI_PALETTE handling. Repainted by display_shell_quarter() when moved
 * on. Irrelevant when closed; reset on SAND_UI_OPEN_PALETTE. */
static int         palette_drawn_quarter;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Rolling averages, purely for the log line - a release build has nobody
 * watching the serial console to read them, so it carries none of this. */
static uint32_t frames;
static int64_t  step_us_total;
static int64_t  draw_us_total;
static int64_t  rows_redrawn_total;
static int64_t  steps_total;

/* TEMPORARY: Splits step/draw timing by actual pour this frame, prints
 * periodically. Checks claim that pouring costs more than tilt alone. Remove
 * once claim answered. */
static int64_t  pour_step_us_total, pour_draw_us_total;
static uint32_t pour_frames;
static int64_t  idle_step_us_total, idle_draw_us_total;
static uint32_t idle_frames;
static int64_t  split_log_at_us;

/* TEMPORARY: Count of awake blocks (!sand_block_settled()) after step, at
 * block granularity, to verify fix in sand.c. */
static int64_t  pour_awake_total, idle_awake_total;

/* Measure occupied cells in blocks; confirms step_one_row() cost per row, not
 * unit. */
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

    /* Launcher output remains in framebuffer. ui_end() skips repaints based
     * on UI command list changes, not screen replacements. Without this, menu
     * would compare equal to launcher's last frame and never repaint. */
    ui_invalidate();
}

/* Seeds every row's run-tracking as one full-width span, as if the whole row
 * were occupied. Shared with sand_frame()'s SAND_UI_CLOSE_PALETTE handling,
 * which forces the panel to clear the framebuffer fully on the first frame
 * after closing, not trusting the sand's narrower real extent. */
static void seed_row_runs_full_width(void)
{
    for (int i = 0; i < grid_h; i++) {
        row_run_x0[i * ROW_MAX_RUNS] = 0;
        row_run_x1[i * ROW_MAX_RUNS] = (uint16_t)grid_w;
        row_run_n[i] = 1;
    }
}

/* Marks the sand canvas for full repaint: reseeds run-tracking, flags rows
 * dirty, marks screen dirty in gfx. Used when sand is uncovered, e.g.,
 * palette opens or closes. Only marks; does not call draw_dirty_rows(). */
static void mark_sand_fully_dirty(void)
{
    seed_row_runs_full_width();
    memset(dirty_rows, 1, (size_t)grid_h);
    gfx_mark_all_dirty();
}

#if CONFIG_LAUNCHER_SELFTEST
/* Checks if a fresh app entry can allocate needed grid, not just TEST.
 * Allocates its own set in app order, freeing all before return. impulse_buf
 * is optional. */
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

/* Builds the active grid at chosen quality, allocates on first call, seeds
 * row_runs, starts simulation and IMU, drops initial sand heap on START
 * press. */
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
    /* impulse_buf GOES LAST after grid to avoid heap fragmentation issues.
     * Moving impulse_buf first caused a device flash, with grid failing to
     * allocate memory. Grid needs the largest contiguous block, confirmed by
     * device captures. Do not reorder without new evidence showing
     * improvement. */
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

    /* Full width, not empty: launcher's leftover framebuffer persists.
     * Seeding "previous" as full row forces initial full-width send. Only
     * after redraw does narrower extent become trusted. */
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

    /* DETONATE scaffolding - see sand_ui.h. Enabled unconditionally to catch
     * allocation failures early. */
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

/* Writes empty cells. Pile drawing: 9.6 ms/frame. Grid: 184x224, dynamic
 * angles. Gravity rotates band (shine_ux_q8/shine_uy_q8). SHINE_PERIOD (64
 * px), SHINE_STEP_PX/MS control band. Band width: 1 cell, quality-variable.
 * Speed adjusted by step size. */
#define SHINE_PERIOD   64      /* power of two - see the mask below */
#define SHINE_STEP_MS  40
#define SHINE_STEP_PX   2

static int      shine_offset;
static uint32_t shine_elapsed_ms;

/* Q8 unit vector for shine band. See material_shine_direction() in material.h
 * for fixed-point convention. Initialised to (1, 1) to match legacy frame
 * draw before update_shine_direction(). */
static int shine_ux_q8 = 181;
static int shine_uy_q8 = 181;

/* Tracks rows hatched last paint to repaint only those. Without this, the
 * shine would claim the whole screen every SHINE_STEP_MS, which is too
 * frequent. Unchanged rows retain their last state. */
static uint8_t row_has_shine[GRID_H_MAX];

/* Which rows painted a CULLET cell last time they were painted - similar to
 * row_has_shine[], for the same reason: cullet's colour cycles
 * (CULLET_PHASE_MS) without changing the cell byte, so a settled heap would
 * freeze on the current tint. Populated in paint_row_n() with
 * row_has_shine[cy]. */
static uint8_t row_has_cullet[GRID_H_MAX];

/* Which rows painted a GLASS cell last time - the same mechanism again,
 * for glass_phase's own gravity drift: a settled pane has nothing in the
 * cell byte to mark its row dirty, so without this it freezes at
 * whatever shade it last painted and never answers a tilt again. */
static uint8_t row_has_glass[GRID_H_MAX];

/* How much a WATER cell's grain hash is coarsened before reaching
 * material_colours() - 3 means an 8x8 block of cells shares one hash value;
 * raise to increase blob size, lower to decrease towards 0 (one cell per
 * hash). RAISED from 2 (4x4) to double the area, improving blob appearance. */
#define FOAM_BLOB_SHIFT 3

/* Foam dither phase advance frequency: 90 ms (11 changes/s). Tuned by eye;
 * adjust if foam appears too twitchy (increase) or static (decrease). */
#define FOAM_PHASE_MS 90

/* Accumulated real-time toward foam phase, similar to shine_elapsed_ms. Uses
 * dt_ms for consistent real-world animation speed, avoiding frame-rate
 * dependency issues. */
static uint32_t foam_elapsed_ms;

/* Cullet colour cycle steps every 250 ms, 4 s for a 16-step loop. Slower than
 * foam for distinct visual effects. */
#define CULLET_PHASE_MS 250

/* Real-time accumulated time for next cullet phase, carried across frames
 * like shine_elapsed_ms and foam_elapsed_ms (see advance_shine() comment).
 * Unlike foam_elapsed_ms, not decremented (see advance_cullet() for details). */
static uint32_t cullet_elapsed_ms;

/* Bits of gravity_bearing_q16()'s range glass_phase drops - not a rate,
 * see advance_glass_phase(). Keeps an earlier, coarser version's pacing
 * (one sweep per sixteenth-turn) but resolves it into 256 shades instead
 * of jumping between 8, so a small tilt moves the shade a small amount. */
#define GLASS_PHASE_SHIFT 7

/* The last phase glass_phase actually painted at, so advance_glass_phase()
 * can tell whether this frame's snapshot differs enough to be worth
 * repainting - see that function's own comment. */
static int glass_last_phase;

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
 * exactly what the second report above asked for. See
 * suite_sand_liquid_depth.c's
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

/* IN Q8, `STEP COUNT = 256 * im_len(gx, gy) / dominant_axis`. `dominant_axis`
 * is `|gy|` or `|gx|`. SET IN update_local_depth_gravity(). USES ONE SCALE.
 * OLD SHRANK, NEW GROWS. NO RAISE FOR CEILING. PROJECTED AT COMBINE. RAW
 * IGNORES GRAVITY. SETS `local_depth_vertical_dominant`,
 * `local_depth_v_reverse`, `local_depth_h_reverse`. FLAGS APPLY TO BOTH. */
static unsigned local_depth_scale_q8;
static bool local_depth_vertical_dominant;
static bool local_depth_v_reverse;
static bool local_depth_h_reverse;

/* |gx|, |gy| for THIS frame, stashed alongside the scale. paint_row_n() needs
 * the exact integer RATIO between axes for Bresenham offset and horizontal
 * accumulator, without gravity. See "THE ROW OFFSET, WITHOUT AN ACCUMULATOR"
 * and "THE HORIZONTAL WITHIN-ROW ACCUMULATOR" in paint_row_n() for details. */
static unsigned local_depth_ax;
static unsigned local_depth_ay;

/* local_depth_vertical_dominant/local_depth_v_reverse/local_depth_h_reverse -
 * unread by paint_row_n(), read by update_local_depth_gravity() to detect
 * frame changes. A change in any invalidates
 * local_depth_row_a[]/local_depth_row_b[]/local_depth_top_row[] together. */
static bool local_depth_vertical_dominant_prev;
static bool local_depth_v_reverse_prev;
static bool local_depth_h_reverse_prev;

/* WALK'S STORAGE - double buffer, GRID_W_MAX entries each, persistent.
 * Replaces col_stable_depth[]/row_stable_depth[] with one buffer.
 * `local_depth_cur_row`/`local_depth_prev_row` are pointers, swapped at end
 * of paint_row_n(). No aliasing. VERTICAL reads `local_depth_prev_row[cx +
 * step]`, HORIZONTAL reads `local_depth_cur_row[cx - hdir]` or
 * `local_depth_prev_row[cx - hdir]`. Verified. */
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

/* DEBOUNCE KEY: One array. VERTICAL-DOMINANT uses `local_depth_top_row[cx]`
 * for cx's most recent row request, resetting only if repeated.
 * HORIZONTAL-DOMINANT uses a pending flag: non-255 indicates a request, 255
 * means climb or no request. Reset on pending, re-arm on requests. Both
 * regimes share the array, resetting on flips. */
static uint8_t local_depth_top_row[GRID_W_MAX];

/* local_depth_cur_row[]/local_depth_prev_row[] store CELL COUNT;
 * eighths-of-a-cell accumulator removed. This count caps at
 * MATERIAL_LIQUID_DEPTH_BAND (24). Unlike the two-walk design's
 * LOCAL_DEPTH_COUNT_CEILING (34), this design, scaled as `len /
 * dominant_axis` (≥256), naturally clamps at MATERIAL_LIQUID_DEPTH_BAND,
 * ensuring coverage at any angle. Confirmed by test_a_ showing consistent
 * liquid body shading regardless of tilt. */
#define LOCAL_DEPTH_COUNT_CEILING MATERIAL_LIQUID_DEPTH_BAND

/* Called per frame by material_set_gravity(). material_colours() needs this
 * frame's local-depth scale. Here, we calculate scale, regime, and scan
 * directions to minimize paint_row_n() operations: one multiply, shift, and
 * compare per cell, avoiding division. Division is done once per frame and
 * per row for vertical-dominant regime's row offset. */
static void update_local_depth_gravity(int gx, int gy)
{
    const int ax = im_abs(gx), ay = im_abs(gy);

    /* ONE DIVIDE PER FRAME plus one per PAINTED ROW for vertical-dominant
     * regime. build_xflow() and material_set_gravity() also use divides. At
     * gx == gy == 0, im_len() falls back to 256 to avoid division by zero. */
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

    /* A regime or scan direction change invalidates shared state. Direction
     * reversal relocates neighbors, invalidating indices. Regime flips alter
     * recurrence patterns, resetting counts. Measured resets show brief
     * blips. Scan-direction flips reset if active. Axis lock causes tremor
     * resets, disabling debounce. Gate uses exact arithmetic. Regime flips
     * change slot meaning. */
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

/* LOCAL DEPTH WAKE closes SHINE_STEP_MS gap. BUG: settled block depth freezes
 * as draw_dirty_rows() skips it. FIX: periodically mark rows with liquid
 * cells dirty. row_has_liquid[] checks any liquid, not just interior. 120
 * balances redraw and gravity drift. */
#define LOCAL_DEPTH_WAKE_MS 120

/* Real time accumulated toward the next local-depth wake tick - carried
 * across frames the same way shine_elapsed_ms and foam_elapsed_ms are, for
 * the same reason: driven by dt_ms rather than a frame count, so the wake
 * fires at the same real-world rate whatever the frame rate happens to
 * be. */
static uint32_t local_depth_wake_elapsed_ms;

/* Rows painted with any liquid cell last time; keeps wake tick affordable by
 * bounding staleness. Reset to 0 at start of paint_row_n(), set to 1 when any
 * liquid cell is painted. */
static uint8_t row_has_liquid[GRID_H_MAX];

static inline void paint_row_n(gfx_color_t *fb, const gfx_color_t *pal,
                               int cy, const uint8_t *row, int n)
{
    gfx_color_t *out = fb + (cy * n) * GFX_WIDTH;
    row_has_shine[cy] = 0;
    row_has_liquid[cy] = 0;
    row_has_cullet[cy] = 0;
    row_has_glass[cy] = 0;

    /* grid_w not a compile-time constant like n; only inner dy/dx loops
     * matter. Off-grid not empty; walls solid, same as sand_at()
     * out-of-bounds. */
    const uint8_t *above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t *below = (cy < grid_h - 1) ? row + grid_w : NULL;

    /* ONE regime per frame. `toward_surface` reads adjacent grid row
     * conditionally, "above" or "below" based on `gy` sign, using
     * NULL-off-the-grid convention. Also used by horizontal-dominant regime. */
    const uint8_t *toward_surface = local_depth_v_reverse ? below : above;

    /* local_depth_prev_row[] checks if `toward_surface` points at it, hoisted
     * out of the cx loop for an 841-to-83 improvement. Only the
     * hold-then-commit debounce reads it, as same-material climbs trust a
     * stale count, and BOUNDARY's carry is nonsense for other rows. */
    const int local_depth_vdir = local_depth_v_reverse ? -1 : 1;
    const bool local_depth_chain_ok =
        (local_depth_prev_cy == cy - local_depth_vdir);

    /* Scan order for THIS row's cx loop: ascending unless gravity points
     * left. Needed by both regimes. Horizontal-dominant regime reads an
     * earlier cx, so scan starts from the end `hdir` points away from.
     * Vertical-dominant regime's read doesn't depend on cx order, so sharing
     * this order costs nothing. */
    const int hdir = local_depth_h_reverse ? -1 : 1;
    const int cx_first = local_depth_h_reverse ? grid_w - 1 : 0;
    const int cx_step  = local_depth_h_reverse ? -1 : 1;

    /* ROW OFFSET without accumulator; vertical-dominant per-row horizontal
     * drift. Same step for all columns. No running Bresenham accumulator.
     * `cum(n) = floor(n * minor / dominant)` matches Bresenham. Row step is
     * `cum(n) - cum(n - 1)`. One divide per row. Meaningless in
     * horizontal-dominant or no gravity (`local_depth_ay == 0` only if
     * `local_depth_ax == 0`). */
    int local_depth_row_step = 0;
    if (local_depth_vertical_dominant && local_depth_ay > 0u) {
        const int vdir = local_depth_vdir;
        const int xsign = local_depth_h_reverse ? 1 : -1;
        const int n = (vdir > 0) ? cy : (grid_h - 1 - cy);
        const int cum_n  = (int)(((long)(n)     * (long)local_depth_ax) / (long)local_depth_ay);
        const int cum_n1 = (int)(((long)(n + 1) * (long)local_depth_ax) / (long)local_depth_ay);
        local_depth_row_step = xsign * (cum_n1 - cum_n);
    }

    /* Horizontal within-row accumulator reset per row-call, Bresenham march,
     * emits 0 or +/-1 (`ysign`) at row boundaries. See LOCAL DEPTH comment
     * for regime details. */
    int local_depth_herr = 0;
    const int ysign = local_depth_v_reverse ? 1 : -1;

    /* CULLET row-repaint gate's boundary. Raw cell byte of FIRST cullet
     * shade: every value from here to + SAND_CULLET_SHADES - 1 is cullet.
     * Testing membership is one unsigned subtract-and-compare, not a nibble
     * decode followed by range check. Computed once per row, not per cell. */
    const unsigned cullet_first = MAT_SAND * MATERIAL_VARIANTS + SAND_CULLET_BASE;

    for (int cx_i = 0; cx_i < grid_w; cx_i++) {
        const int cx = cx_first + cx_i * cx_step;

        /* Empty cardinal neighbours as bits, not bools, for liquid rim
         * shading by gravity. Align with MATERIAL_EDGE_* in material.h:
         * "above" is row - grid_w, i.e., cy - 1, UP. Every cell tests this
         * UNCONDITIONALLY. Critical for performance; non-water/interior cells
         * must not incur extra cost. */
        unsigned mask =
            ((cx > 0          && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT  : 0u) |
            ((cx < grid_w - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u) |
            ((above != NULL   && CELL_IS_EMPTY(above[cx]))   ? MATERIAL_EDGE_UP    : 0u) |
            ((below != NULL   && CELL_IS_EMPTY(below[cx]))   ? MATERIAL_EDGE_DOWN  : 0u);

        /* DIAGONAL bits for WATER RIM cells only, computed if mask is
         * non-zero and cell is water. Skipped for other cells. */
        if ((mask & MATERIAL_EDGE_CARDINAL) != 0 &&
            CELL_MATERIAL(row[cx]) == MAT_WATER) {
            mask |=
                ((cx > 0          && above != NULL && CELL_IS_EMPTY(above[cx - 1])) ? MATERIAL_EDGE_UP_LEFT    : 0u) |
                ((cx < grid_w - 1 && above != NULL && CELL_IS_EMPTY(above[cx + 1])) ? MATERIAL_EDGE_UP_RIGHT   : 0u) |
                ((cx > 0          && below != NULL && CELL_IS_EMPTY(below[cx - 1])) ? MATERIAL_EDGE_DOWN_LEFT  : 0u) |
                ((cx < grid_w - 1 && below != NULL && CELL_IS_EMPTY(below[cx + 1])) ? MATERIAL_EDGE_DOWN_RIGHT : 0u);
        }

        /* WATER samples grain hash at an 8x8 grid, shifted right by
         * FOAM_BLOB_SHIFT. This resembles mist near foam. material_colours()
         * uses this hash without knowing about blobs. Other materials use
         * material_grain_hash(cx, cy) for fine, per-cell hash. Coarsening
         * would cause striping. */
        const bool cell_is_water = CELL_MATERIAL(row[cx]) == MAT_WATER;
        const unsigned hash = cell_is_water
            ? material_grain_hash(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)
            : material_grain_hash(cx, cy);

        /* PER-CELL COST: One read, read+write, multiply, shift, compare.
         * Divides in `update_local_depth_gravity()` and vertical-dominant
         * regime's PAINTED ROW. `step` is horizontal-dominant row drift, 0 in
         * vertical-dominant. */
        int step = 0;
        if (!local_depth_vertical_dominant) {
            local_depth_herr += (int)local_depth_ay;
            if (local_depth_ax > 0u && local_depth_herr >= (int)local_depth_ax) {
                local_depth_herr -= (int)local_depth_ax;
                step = ysign;
            }
        }

        /* SOURCE CELL, one step back along the ray. `qx` is the source
         * COLUMN. `src_ptr` reads MATERIAL; `src_arr` reads COUNT. Uses
         * `local_depth_cur_row[]` if horizontal-dominant and `step == 0`,
         * otherwise `local_depth_prev_row[]`. No read/write aliasing. */
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
            /* NOT a liquid cell: depth is irrelevant; material_colours()
             * never reads it except for liquid interiors. Must not
             * accumulate. Reset, local_depth_top_row[cx] unchanged - not a
             * boundary request. */
            count = 0u;
        } else if (same_material) {
            /* Continues liquid body - climb by one to SATURATION POINT.
             * LOCAL_DEPTH_COUNT_CEILING explains no raise needed. Not a
             * boundary request; vertical-dominant leaves
             * local_depth_top_row[cx] alone, horizontal-dominant clears to
             * 255 to prevent unrelated stale pending flag from pre-arming a
             * later blink. */
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
                /* Keep climbing as if nothing happened, arming this slot's
                 * tracker. The NEXT write commits if the boundary is real.
                 * "AS IF NOTHING HAPPENED" means climbing from the neighbor's
                 * count. If the chain breaks, carry 0. See
                 * local_depth_prev_cy's comment for details. */
                const unsigned carry =
                    (cross_row && !local_depth_chain_ok) ? 0u : src_count;
                count = carry < LOCAL_DEPTH_COUNT_CEILING
                    ? carry + 1u : LOCAL_DEPTH_COUNT_CEILING;
                local_depth_top_row[cx] = local_depth_vertical_dominant
                    ? (uint8_t)cy : 0u;
            }
        }
        local_depth_cur_row[cx] = (uint8_t)count;

        /* THE PROJECTION - `count * local_depth_scale_q8 >> 8`, applied ONCE,
         * then clamped to MATERIAL_LIQUID_DEPTH_BAND. No max, no blend: only
         * one reading this frame, so no combiner needed. */
        const unsigned depth_raw = (count * local_depth_scale_q8) >> 8;
        const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

        /* A ROOT uses `depth` to count its root neighbors, representing age.
         * One byte-compare per cell, similar to metal's test. Eight reads per
         * root during repainting, as roots rarely change. */
        const unsigned depth = (row[cx] == MATX(MATX_ROOT))
            ? material_root_neighbours(above, row, below, cx, grid_w)
            : depth_liquid;

        /* row_has_liquid[] marks any liquid cell, including rim, unlike
         * material_colours()'s interior test. Uses `here_liquid` for
         * efficiency. */
        if (here_liquid) {
            row_has_liquid[cy] = 1;
        }

        /* row_has_cullet[] populated by array comment above. Raw range
         * compare on cell byte, not material_colours() or `v`/CELL_VARIANT.
         * `cullet_first` combines material and variant checks in one unsigned
         * compare. */
        if ((unsigned)(row[cx] - cullet_first) < SAND_CULLET_SHADES) {
            row_has_cullet[cy] = 1;
        }

        /* row_has_glass[]'s own population point, same shape as cullet's
         * just above - see that array's comment for the mechanism. */
        if (CELL_MATERIAL(row[cx]) == MAT_GLASS) {
            row_has_glass[cy] = 1;
        }

        gfx_color_t col[3];
        const material_pattern_t pat =
            material_colours(row[cx], hash, mask, depth, col);
        gfx_color_t *p = out + cx * n;

        /* n is a compile-time constant, so these unroll at call sites. FLAT
         * and SPECKLED share this loop; STRIPED does per-pixel work only for
         * on-screen striped materials. */
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

        /* The shine's own axis, in Q8 screen-pixel units: this cell's
         * origin projected onto current gravity (shine_ux_q8/shine_uy_q8,
         * updated once a frame - material_shine_direction(), material.h).
         * Computed once per cell; the loop below adds dx/dy's own share. */
        const int shine_base_q8 = (cx * n) * shine_ux_q8 + (cy * n) * shine_uy_q8;

        for (int dy = 0; dy < n; dy++) {
            for (int dx = 0; dx < n; dx++) {
                /* SHINE: a band travelling against the CURRENT GRAVITY
                 * DIRECTION, leaned 45 degrees (material_shine_direction()
                 * owns that turn), advanced on a clock. Projecting (dx, dy)
                 * onto shine_ux_q8/shine_uy_q8 is a plain 2D dot product in
                 * Q8 fixed point. */

                /* `>> 8` back to pixel units is an arithmetic right shift,
                 * sign-extending on this toolchain, so it is fine on the
                 * negative projections a pixel above or left of a cell's
                 * origin produces. */

                /* A mask, not a modulo, because SHINE_PERIOD is a power of
                 * two and it is fine on values left of the origin - two's
                 * complement shifts the phase, which nothing here can tell
                 * from any other phase. `< n` is the width: one CELL, so
                 * the band looks the same at every quality setting. */
                const int shine_q8 = shine_base_q8 + dx * shine_ux_q8 + dy * shine_uy_q8;
                const int along = ((shine_q8 >> 8) + shine_offset)
                                  & (SHINE_PERIOD - 1);

                /* The band wins wherever it falls - the bright thing
                 * catching the light - and the surface is a plain fill
                 * everywhere else. A second, woven diagonal texture used
                 * to sit under it (col[1]); dropped for reading as a
                 * printed grid rather than a surface - see git log. */
                p[dy * GFX_WIDTH + dx] = (along < n) ? col[2] : col[0];
            }
        }
    }

    /* Double buffer swap after each row, not per cell. local_depth_cur_row[]
     * becomes the previous row for the next row. Swap, not copy, to avoid
     * read-write aliasing. See local_depth_row_a[]/local_depth_row_b[]
     * comment. */
    uint8_t *local_depth_tmp = local_depth_cur_row;
    local_depth_cur_row = local_depth_prev_row;
    local_depth_prev_row = local_depth_tmp;

    /* Record buffer row for next call to check neighbour validity; see
     * local_depth_prev_cy comment. One store per row, adding to existing swap
     * cost of four. */
    local_depth_prev_cy = cy;
}

/* Cell choice on boot menu is runtime; switch in paint_row_n() unrolls dy/dx
 * loops for each quality level as compile-time constants. Each qualities[]
 * entry must match a case; mismatch causes silent breakage. */
static void paint_row(gfx_color_t *fb, const gfx_color_t *pal, int cy,
                      const uint8_t *row)
{
    switch (cell) {
    case 2:  paint_row_n(fb, pal, cy, row, 2); break;
    case 3:  paint_row_n(fb, pal, cy, row, 3); break;
    case 4:  paint_row_n(fb, pal, cy, row, 4); break;
    case 6:  paint_row_n(fb, pal, cy, row, 6); break;
    case 8:  paint_row_n(fb, pal, cy, row, 8); break;
    /* Unreachable for any cell size in qualities[]; falls back to size 2 to
     * avoid out-of-bounds writes. */
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

/* Cullet phase steps since app start - grows indefinitely. material_colours()
 * reads only masked value (one cycle). foam_elapsed_ms relies on similar
 * logic, using step counter instead of milliseconds for advance_cullet(). */
static unsigned cullet_phase_index;

/* Advances cullet colour cycle; checks if any row needs repainting. Unlike
 * advance_shine(), cullet_phase_index is in material.c due to
 * material_set_cullet_phase() comment in material.h. */
static bool advance_cullet(uint32_t dt_ms)
{
    cullet_elapsed_ms += dt_ms;
    if (cullet_elapsed_ms < CULLET_PHASE_MS) {
        return false;
    }
    const uint32_t steps = cullet_elapsed_ms / CULLET_PHASE_MS;
    cullet_elapsed_ms -= steps * CULLET_PHASE_MS;
    cullet_phase_index += steps;
    material_set_cullet_phase(cullet_phase_index);
    return true;
}

/* Advances local-depth wake clock, checks if it fired this frame. Similar to
 * advance_shine(), but simpler: no position to update, just resets carry and
 * fires at most once per call. */
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

/* A trig-free BEARING for (gx, gy): a signed value in Q16 units of a
 * quarter-turn (a full rotation spans 4.0, i.e. -131072..131072),
 * increasing monotonically all the way around the circle. Not a true
 * angle - the step size varies within a quadrant - but continuous. */

/* glass_phase used to read gy alone, which is blind to any tilt in the
 * gx direction: rotating within a quadrant where gy barely changes moved
 * nothing, and only the tilts that swung gy hard did anything - reading
 * as a few "cardinal" positions rather than a smooth sweep. */

/* Standard L1 pseudoangle trick: dx / (|dx| + |dy|) sweeps -1..1 across
 * one quadrant; folding by dy's sign and that ratio's own sign turns the
 * four separate ramps into one monotonic sweep over the full turn. */
static int gravity_bearing_q16(int gx, int gy)
{
    const int64_t ax = gx < 0 ? -(int64_t)gx : (int64_t)gx;
    const int64_t ay = gy < 0 ? -(int64_t)gy : (int64_t)gy;
    const int64_t denom = ax + ay;
    if (denom == 0) {
        return 0;   /* flat or free fall: no bearing to report */
    }
    const int64_t p_q16 = ((int64_t)gx << 16) / denom;   /* -65536..65536 */
    return (int)(gy < 0 ? (p_q16 - 65536) : (65536 - p_q16));
}

/* A SNAPSHOT of gravity's bearing, not a rate accumulated over time - an
 * earlier version accumulated bearing*dt_ms the way shine_offset does. */

/* That was the bug: bearing is essentially never zero (the board reads
 * SOME direction even sitting dead level), so accumulating it slid the
 * phase forever with nothing to show which part was an actual tilt. */

/* Reading it directly ties the phase to WHERE the board currently points,
 * not how long it has pointed there - hold a tilt and the phase holds
 * with it; change the tilt and the phase follows by exactly as much. */
static bool advance_glass_phase(int gx, int gy)
{
    const int phase = gravity_bearing_q16(gx, gy) >> GLASS_PHASE_SHIFT;
    const bool changed = phase != glass_last_phase;
    glass_last_phase = phase;
    material_set_glass_phase(phase);
    return changed;
}

static void draw_dirty_rows(bool shine_moved, bool local_depth_woke,
                             bool cullet_moved, bool glass_moved)
{
    gfx_color_t *fb = gfx_framebuffer();

    /* Rows repaint on change. Orientation shifts count as changes. Screens
     * fully repaint with hatched materials, as direction alters all blocks.
     * Repainting happens when direction crosses eighths. Only rows with
     * hatched materials repaint to save on animation. */
    if (shine_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_shine[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    /* Mechanism for liquid interior depth, like hatched material shine, uses
     * periodic tick. Rows with liquid cells last painted, including RIM
     * cells, are updated via row_has_liquid[]. */
    if (local_depth_woke) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_liquid[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    /* Same mechanism for cullet's colour cycle as shine or local depth - see
     * CULLET_PHASE_MS comment. Only rows with cullet cells last painted are
     * updated due to affordability. */
    if (cullet_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_cullet[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    /* THE SAME MECHANISM ONCE MORE, for glass's phase - see row_has_glass[]'s
     * own comment above for why a settled pane needs this at all. */
    if (glass_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_glass[cy]) {
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

    /* Ordinarily ascending; gravity-UP reverses due to
     * local_depth_cur_row[]/local_depth_prev_row[] swap. Both regimes require
     * consistent row order. `local_depth_v_reverse` suffices, avoiding
     * complex flags. Safe to reverse as other loop elements are `cy`-indexed
     * or unordered. */
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

/* A fixed, saturated colour outside material_palette() (see material.c),
 * instead of material-derived. At badge size, material-derived contrasts; at
 * MARKER size (EMITTER_MARKER_PX px), two tones don't read, so a
 * palette-external colour is used. */
#define EMITTER_MARKER_COLOR 0xFF3EC8

/* Marker fixed on-screen size in pixels, not cells. Cell size varies (2 px at
 * ULTRA, 8 px at VERY LOW), affecting marker visibility. Tuned by eye; spans
 * about four HIGH cells (3 px each). Trade-off for visibility. */
#define EMITTER_MARKER_PX  12

/* Marks placed emitters with a fixed-size square. See EMITTER_MARKER_PX and
 * EMITTER_MARKER_COLOR. Called every frame to ensure visibility. Cheap: up to
 * SAND_MAX_EMITTERS markers, each EMITTER_MARKER_PX pixels square. Covers
 * neighbouring cells better than old one-cell marker. */
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

/* The brush's color, picked from the palette, matches finger output. Shared
 * with draw_mode_label(). Ordinary brushes use 13 of 16 shades. STATIC cells,
 * with low nibble for material type, can't use this. material_palette() uses
 * raw byte for STATIC. Gunpowder uses low three bits for shade, shown at
 * GUNPOWDER_CELL(0) for darkest dry tone, and GUNPOWDER_CELL(2) for dark red
 * dry tone. */
static gfx_color_t brush_color(cell_t c)
{
    if (cell_is_gunpowder(c)) {
        return material_palette()[GUNPOWDER_CELL(2)];
    }
    return material_palette()[
        cell_is_extended(c) ? c : CELL_MAKE(CELL_MATERIAL(c), 13)];
}

/* Which quarter turn reads as "upright"? Compares gravity's dominant
 * component's sign for orientation, snaps to nearest quarter turn. Used by
 * draw_mode_label() to align text with board's physical "up". Different from
 * display.h's decision due to draw_mode_label()'s specific requirements. */
static int gravity_quarter_turn(int gx, int gy)
{
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ay >= ax) {
        return (gy >= 0) ? 0 : 2;      /* down is down : board upside down */
    }
    return (gx >= 0) ? 3 : 1;          /* down is to the right : to the left */
}

/* Draws mode label against UP edge, following device orientation, not screen.
 * Label snaps using gravity_quarter_turn(). draw_palette() uses microui,
 * while draw_mode_label() uses gfx_text_turned() directly on canvas. Canvas
 * elements ignore shell transform. */
static void draw_mode_label(int gx, int gy)
{
    /* Material name for clarity; in BRUSH_SPAWN, "SOURCE" appended to
     * distinguish continuous tap from one-time blob. `len`/`span` computed
     * from strlen(text), not hardcoded, so buffer size adapts automatically. */
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

    /* Coloured as the material itself; no colour table. brush_color()
     * explains why extended cells can't just bump variant. ERASE and DETONATE
     * use manual colours: warmer orange for ERASE, hotter red for DETONATE as
     * it rearranges the board. */
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

/* Gap between tiles and around unselected swatch. Narrow gap for touch
 * targets (92 px on 322 ppi). Shows frozen sand background from
 * draw_palette(). */
#define PALETTE_GROUT   4

/* Thickness of each bezel edge matches UI_BEZEL_THICKNESS (ui_style.h). Badge
 * position is measured off this constant, which belongs to this file's
 * layout. */
#define PALETTE_BEZEL   3

/* Eligibility/spawn corner badge: 18px outer square on 92px PALETTE_TILE
 * tile, 2px border, 2px margin. */
#define PALETTE_BADGE_SIZE    18
#define PALETTE_BADGE_INSET    2
#define PALETTE_BADGE_MARGIN   2

/* Fixed, not derived from face. gfx_color_mix() with near-white face (Snow)
 * caused "armed" fill to match face colour, making armed and unarmed
 * indistinguishable. Maximum-contrast pair, same as tile names below. */
#define PALETTE_BADGE_BORDER_COLOR  0x141414
#define PALETTE_BADGE_FILL_COLOR    0xF2F2F2

/* mu_Color from 0xRRGGBB value, opaque. microui drawing calls use 8-bit
 * mu_Color. Converts 0xRRGGBB to mu_Color; see gfx_color_rgb888() for
 * unpacking details. */
static mu_Color mu_color_hex(uint32_t rgb)
{
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF),
                    (int)(rgb & 0xFF), 255);
}

/* Material picker overlay now a microui frame, rebuilt every frame while
 * SAND_UI_PALETTE is active. EACH TILE IS A REAL mu_button(). Panel turns
 * with the board via inherited transform. No manual hit-testing or rotation. */
static void draw_palette(const input_t *input)
{
    mu_Context *ctx = ui_context();

    ui_begin(input);

    /* Tile names now get halo from UI layer via ui_text_halo() in ui_style.h.
     * Replaces hand-rolled draws for one ink colour (black). Restored to
     * UI_TEXT_PLAIN in sand_frame() with SAND_UI_CLOSE_PALETTE. */
    ui_set_text_style(UI_TEXT_OUTLINED);

    /* Bezelled button, raised at rest, inverted on hover/focus. Per-tap press
     * feedback is new. Selection cue drawn after mu_button(), not using
     * microui's selection. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    /* MU_COLOR_BUTTON and MU_COLOR_TEXT share a ctx->style->colors[] slot.
     * mu_draw_control_frame() and mu_draw_control_text() use these directly
     * for buttons. Subsequent buttons (e.g., boot menu) use the last set
     * values. These are saved and restored once, avoiding intermediate
     * states. */
    const mu_Color saved_button_color = ctx->style->colors[MU_COLOR_BUTTON];
    const mu_Color saved_text_color   = ctx->style->colors[MU_COLOR_TEXT];

    /* Black used for each tile name by mu_button() and UI_TEXT_OUTLINED.
     * ui_text_halo() derives a light halo at render time, ensuring dark text
     * reads against any swatch. Unlike the face below, this colour is set
     * once here rather than per tile. */
    ctx->style->colors[MU_COLOR_TEXT] = mu_color(0, 0, 0, 255);

    /* ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT (see ui.h). Logical
     * canvas swaps dimensions post-quarter-turn. cols recalculated from
     * ui_width() each frame, no caching needed. Immediate mode redraws panel
     * fully each frame. Use remembered values for ghost-tile fix as in
     * palette_drawn_quarter. */
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

            /* Button placed by mu_layout_set_next() at (ix,iy,iw,ih).
             * mu_button() hit-tests using microui's mouse state, fed via
             * feed_input() in ui.c, mapped through inverse transform. Draws
             * styled frame and centered label, the button's unique id from
             * mu_get_id() in microui.c. Each BRUSH_COUNT brush has a unique
             * name to prevent id collisions. */
            const char *name = material_name(brushes[i]);
            mu_layout_set_next(ctx, mu_rect(ix, iy, iw, ih), 0);
            const int clicked = mu_button(ctx, name);

            /* Hand click to sand_ui_tile_clicked() before drawing selection
             * ring and badge for immediate toggle or selection display in the
             * same frame. Return value unused. */
            if (clicked) {
                sand_ui_tile_clicked(&ui, i);
            }

            /* Selection cue using ui_bezel_spans() for SUNKEN edges, mixed
             * with tile's face colour to ensure visibility across lightness
             * ranges, unlike fixed badge border/fill. */
            if (i == ui.brush) {
                ui_span_t spans[UI_BEZEL_MAX_SPANS];
                const int n = ui_bezel_spans(mu_rect(ix, iy, iw, ih), face,
                                             true, spans, UI_BEZEL_MAX_SPANS);
                for (int s = 1; s < n; s++) {
                    mu_draw_rect(ctx, spans[s].rect, spans[s].color);
                }
            }

            /* Badge: zero or more rects (and icon) after bezel, top-right
             * corner. Three states: BRUSH_POUR (empty box, near-black border,
             * near-white fill), BRUSH_SPAWN (box with check mark), not
             * eligible (no badge). Gunpowder gets badge despite KIND_POWDER.
             * Fixed border/fill, not from face. Relative to tile's (ix, iy)
             * corner. */
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

        }

        mu_end_window(ctx);
    }

    /* Restores what this loop borrowed - see the comment above
     * saved_button_color/saved_text_color for why leaving either mutated
     * would leak into the next thing drawn with MU_COLOR_BUTTON/
     * MU_COLOR_TEXT. */
    ctx->style->colors[MU_COLOR_BUTTON] = saved_button_color;
    ctx->style->colors[MU_COLOR_TEXT]   = saved_text_color;

    /* UI_NO_BACKGROUND keeps frozen sand visible through PALETTE_GROUT gaps.
     * Safe as panel is paused during UI, preventing frame bleeding or
     * flicker. Each tile paints bezel and face, covering previous content.
     * Panel close reseeds runs, repainting sand fully. */
    ui_end(UI_NO_BACKGROUND);
}

/*---------------------------------------------------------------------------
 * Frame
 *-------------------------------------------------------------------------*/

/* Where is down? The GYROSCOPE measures board rotation speed for tilt
 * tracking. Not used for shaking (see tilt.h). Falls back to full-speed down
 * if no sensor. */
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

/* Decisions on palette actions moved to sand_ui.c's sand_ui_step() for host
 * testing. Here, sand_frame() executes the actions returned by
 * sand_ui_step(). */

/* Capped rather than looped to exhaustion: after a long frame the backlog is
 * dropped instead of dumping a pile in one go. */
static void handle_pour_input(const input_t *input, uint32_t dt_ms)
{
    if (ui.mode == SAND_MODE_DETONATE) {
        /* Fires on press EDGE, not via `applications` loop. Loop runs every
         * frame while finger is held, causing continuous detonation.
         * `input->pressed` is true for one frame per tap, ensuring one
         * `sand_explode()` call per press. */
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

    /* A tap that places an emitter differs from pouring, taking over touch
     * entirely. Erasing still wins, disabling any emitter under the eraser.
     * Spawn mode skips accumulator/pour path for this frame, preventing both
     * tap placement and blob pouring in one touch. */
    if (ui.mode == SAND_MODE_PAINT && ui.modes[ui.brush] == BRUSH_SPAWN) {
        /* On PRESS edge, not every frame. input->pressed fires once per
         * press, avoiding SAND_MAX_EMITTERS exhaustion. */
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
    /* Radii in pixels, rounded to nearest cell, ensuring finger-width brush
     * consistency across qualities. Prevents cell-based radius doubling at
     * NORMAL compared to ULTRA. Rounds up to avoid bias; never rounds to 0.
     * Smallest result is (10 + 4) / 8 = 1. */
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

/* Fixed-timestep accumulator scales rate by screen tilt. A grain moves one
 * cell per step, so steps per second is the sand speed. Tilt scaling acts as
 * a throttle, making grains coast to a stop when laid flat. This matches real
 * behaviour: grains on a tilted tray are driven by g*sin(theta). */
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
/* Counts awake blocks and their occupied cells, driving step_one_row()'s
 * cost. Must be called post-step. Replaced row-based version with
 * block-shaped sleeping. Counts occupied cells in clipped span of block
 * (bx,by) to simplify count_awake() loop. */
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

/* TEMPORARY - logs step/draw cost and awake rows/occupied cells, bucketed by
 * pour presence, with rolling averages every ~2s for comparison. */
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

/* Boot menu: START runs simulation at current quality; quality button cycles
 * ULTRA/HIGH/NORMAL/LOW/VERY LOW, staying on menu. Full-screen window with no
 * chrome, modeled on ui_launcher.c's frame. Shell handles home-swipe hint and
 * swipe-up-to-exit, no back button needed. */
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
    /* SAND_UI_MENU checked first. FAILED start_sim() sets ui.screen to
     * SAND_UI_RUNNING for `failed` check. Do not reorder checks or move
     * SAND_UI_PALETTE. `failed` check before sand_ui_step() ensures "no
     * memory for the grid" screen appears even if ui.screen is
     * SAND_UI_RUNNING, avoiding SAND_UI_PALETTE. Calling sand_ui_step()
     * earlier risks issues with new states. */
    if (ui.screen == SAND_UI_MENU) {
        draw_menu(input);
        return;
    }

    if (failed) {
        gfx_clear(gfx_rgb(0x1A0C0C));
        gfx_text(20, GFX_HEIGHT / 2, "no memory for the grid", gfx_rgb(0xFF5C5C));
        return;
    }

    /* One call handles "which edges do what". See sand_ui_step() in sand_ui.c
     * for dispatch by ui.screen. Remaining code executes returned action
     * bits: open/close panel's gfx/accumulator side, redraw, or none for
     * RUNNING frame, falling through to ordinary per-frame work. */
    const unsigned actions = sand_ui_step(&ui, input);

    if (actions & SAND_UI_CLOSE_PALETTE) {
        /* Forces full repaint under panel; see mark_sand_fully_dirty(). Uses
         * same op as SAND_UI_PALETTE on orientation change. Marked, not
         * drawn, to prevent burst on close. Zeroes accumulators to avoid
         * catch-up steps. SAND_UI_SHOW_LABEL indicates if brush/mode changed;
         * provides feedback on close. */
        if (actions & SAND_UI_SHOW_LABEL) {
            label_left_ms = LABEL_MS;
        }

        /* A text style in the shell persists until changed. Use
         * `ui_set_text_style()` ambiently, not per-frame. Restore
         * `UI_TEXT_PLAIN` to prevent leaks. The transform is no longer
         * restored here; `main.c` handles it. This app must not touch the
         * shell's transform. */
        ui_set_text_style(UI_TEXT_PLAIN);

        sim_accumulator_q8 = 0;
        pour_accumulator_ms = 0;
        mark_sand_fully_dirty();
        return;
    }

    if (actions & SAND_UI_OPEN_PALETTE) {
        /* Clears mode-label countdown to prevent full-screen redraw from
         * painting sand over the panel. sand_ui_step() sets ui.screen to
         * SAND_UI_PALETTE, pausing sim from the frame the panel opens.
         * draw_dirty_rows() is called in PALETTE branch only on orientation
         * change to repaint uncovered areas. */
        label_left_ms = 0;
    }

    if (ui.screen == SAND_UI_PALETTE) {
        /* Described every frame now, immediate-mode. ui_end() repaints only
         * when command list changes. No longer tracks "did anything change".
         * Orientation read from shell, avoiding IMU transaction. */
        const int quarter = display_shell_quarter();

        if (actions & SAND_UI_OPEN_PALETTE) {
            /* Panel opened: framebuffer retains app's last draw (e.g., sand,
             * boot menu). UI description may hash equal to earlier frame,
             * causing ui_invalidate() issue. Forcing repaint replaces pixels
             * instead of leaving them. */
            ui_invalidate();

            /* Nothing to erase yet: the sand already fills the whole canvas
             * correctly (nothing else was drawn over it), so this is just
             * establishing the baseline the check just below compares
             * against on every later frame. */
            palette_drawn_quarter = quarter;
        } else if (quarter != palette_drawn_quarter) {
            /* Shell orientation changed, panel repainted with
             * UI_NO_BACKGROUND. ui_transform_quarter_turn() moves panel,
             * leaving ghost tiles. draw_dirty_rows() and
             * mark_sand_fully_dirty() repaint sand. draw_emitter_markers()
             * prevents erasure. Simulation paused, no animations. */
            mark_sand_fully_dirty();
            draw_dirty_rows(false, false, false, false);
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

    /* The grid must redraw every frame, including expiration, to avoid sand
     * pile gaps. draw_dirty_rows() and gfx_mark_dirty() are insufficient. The
     * entire screen, including the label, is redrawn on the label's final
     * frame. */
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

    /* material_shine_direction() is pure and stateless. It needs a frame to
     * land, not per-pixel calls. Tilt within SHINE_STEP_MS is picked up.
     * draw_dirty_rows() repaints all shine rows each tick. paint_row_n()
     * reads current shine_ux_q8/shine_uy_q8. */
    material_shine_direction(gx, gy, &shine_ux_q8, &shine_uy_q8);

    /* LOCAL DEPTH walk needs per-frame facts from gravity vector - scale,
     * dominant regime, scan direction. Not material_set_gravity()'s
     * responsibility; arrays belong to this file, used in row-by-row paint
     * sequence (see local_depth_row_a[]/local_depth_row_b[] comments). */
    update_local_depth_gravity(gx, gy);

    /* Water's foam gets its own per-frame fact, separate from the above call.
     * Gravity and phase must not be conflated (see material_set_foam_phase()
     * in material.h). Driven by real elapsed time, not frame count, to ensure
     * consistent foam shimmer rate (see FOAM_PHASE_MS). */
    foam_elapsed_ms += dt_ms;
    material_set_foam_phase(foam_elapsed_ms / FOAM_PHASE_MS);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t1 = esp_timer_get_time();
    int awake_blocks, awake_cells;
    count_awake(&awake_blocks, &awake_cells);
#endif

    /* Local-depth wake, cullet cycle, and shine each have their own clock
     * tick and row array. Driven by dt_ms, not frame count. Glass's wake uses
     * gravity_bearing_q16(). */
    draw_dirty_rows(advance_shine(dt_ms), advance_local_depth_wake(dt_ms),
                     advance_cullet(dt_ms), advance_glass_phase(gx, gy));

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

/* Diagnostic tool - see app_t's comment in app.h and screenshot.c's
 * dump_state(). Exposes tilt_x()/tilt_y() - smoothed gravity data. tilt.c's
 * filter differs from raw IMU during fast rotation (TILT_TAU_MOVING_MS in
 * tilt.h). Removed local_depth_in_deadzone and local_depth_freeze_active to
 * fix diagonal-dead-zone freeze issue. */
static void sand_diagnostic_json(char *out, size_t len)
{
    snprintf(out, len, "{\"tilt_x\":%d,\"tilt_y\":%d}",
             tilt_x(&tilt), tilt_y(&tilt));
}

/* .home_gesture unset (false) to prevent touch drag near screen edge being
 * mistaken for swipe-home gesture. App provides its own launcher return
 * method instead. */
const app_t app_sand = {
    .name           = "Falling Sand",
    .summary        = "Tilt to steer, touch to pour",
    .enter          = sand_enter,
    .frame          = sand_frame,
    .exit           = sand_exit,
    .diagnostic_json = sand_diagnostic_json,
};

APP_REGISTER(app_sand);
