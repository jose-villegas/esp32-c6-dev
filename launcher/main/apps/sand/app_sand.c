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
 * the boot menu rather than fixed: HIGH (2 px) gives a 184 x 224 grid, or
 * 41 KB; MEDIUM (3 px, the default) gives 122 x 149, or 18 KB; LOW (4 px)
 * gives 92 x 112, or 10 KB; VERY LOW (6 px) gives 61 x 74, or about 4.5 KB.
 * All four still read as grains rather than bricks - the choice trades
 * fineness for the step budget a finer grid costs, not for whether it looks
 * right.
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
    { "HIGH",     2 },
    { "MEDIUM",   3 },
    { "LOW",      4 },
    { "VERY LOW", 6 },
};
#define QUALITY_COUNT ((int)(sizeof(qualities) / sizeof(qualities[0])))
#define QUALITY_DEFAULT 1        /* MEDIUM */

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
 * PWR still toggles the eraser directly, unchanged from before the panel
 * existed - see sand_ui.c's handle_brush_input(). Erase is
 * pressed often enough, and is purely binary (on/off, nothing to browse),
 * that a plain press is the cheaper action for it: a HOLD costs
 * BUTTON_HOLD_US (600 ms) of waiting before it even registers, every single
 * time, and paying that tax on a control used this often would make erasing
 * feel sluggish next to the immediacy pouring already has. A dedicated
 * button's press has none of that cost.
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
 * `ui.screen`, `ui.brush` and `ui.erasing` replace the old file-scope
 * `screen`, `brush` and `erasing` statics - one definition rather than
 * three, now that the state machine that reads and writes them lives in
 * sand_ui.c. Zero-initialised the same way those statics were: `ui.screen`
 * starts at SAND_UI_MENU (0), `ui.brush` at 0, `ui.erasing` at false -
 * matching sand_enter()'s and start_sim()'s own resets below. */
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
                                   * quality below HIGH */
static uint8_t    *sleep_blocks; /* BLOCK_COLS_MAX*BLOCK_ROWS_MAX bytes:
                                   * settled blocks to skip - see
                                   * sand_enable_sleeping() */

/* Up to ROW_MAX_RUNS (row_runs.h) separate cell-index ranges per row - not
 * pixel ranges, and not a single min/max span - recording where a row's
 * material sat the last time it was drawn, so a run whose content just
 * vanished still sends far enough to clear its old pixels, and two
 * genuinely separate blobs in one row keep being sent separately instead
 * of one box spanning the gap between them. See row_runs.h. GRID_H_MAX *
 * ROW_MAX_RUNS entries each - only the first grid_h rows are in use at any
 * quality below HIGH; row_run_n[cy] says how many of a row's ROW_MAX_RUNS
 * slots are actually in use. */
static uint16_t   *row_run_x0;
static uint16_t   *row_run_x1;
static uint8_t    *row_run_n;
static sand_t      sim;
static tilt_t      tilt;
static bool        failed;
static uint32_t    label_left_ms;    /* countdown for the mode label */

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
    ui.erasing = false;
    label_left_ms = 0;
    failed = false;

    if (dirty_rows == NULL) {
        dirty_rows = malloc(GRID_H_MAX);
    }
    if (sleep_blocks == NULL) {
        sleep_blocks = malloc((size_t)BLOCK_COLS_MAX * BLOCK_ROWS_MAX);
    }
    if (grid == NULL) {
        grid = malloc((size_t)GRID_W_MAX * GRID_H_MAX);
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
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    /* Per-material too - only gas has a figure below full speed, so this
     * is a no-op for sand, water and stone. */
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);

    sand_track_dirty_rows(&sim, dirty_rows);

    /* Without this, a screen full of motionless sand is the most expensive
     * thing the simulation can hold rather than the least - every settled
     * grain runs the whole decision path each step to conclude nothing. */
    sand_enable_sleeping(&sim, sleep_blocks);
    tilt_reset(&tilt, IMU_COUNTS_PER_G);

    if (!imu_init()) {
        /* Not fatal. Without a sensor the gravity vector is a constant, so the
         * app degrades to plain downward sand rather than refusing to run. */
        ESP_LOGW(TAG, "No IMU - falling back to fixed downward gravity");
    }

    /* A starting heap, so the app is doing something the moment it opens
     * rather than presenting an empty screen and no clue what to do. */
    sand_spawn(&sim, grid_w / 2, grid_h / 4, grid_w / 5, MAT_SAND);

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
 * This replaces a version that aligned the shine to the board's tilt. That
 * was a nicer idea and it never became visible: the direction was right,
 * the repaint was right by the end, and three rounds of looking at it on
 * the device still could not see it. Two diagonals of single pixels
 * differing only in WHICH way they lean is simply not a difference the eye
 * picks up on a 184x224 grid, however correct the arithmetic underneath.
 *
 * Movement is a difference the eye cannot miss, which is the whole reason
 * to prefer this. The band sweeps, so the glass is doing something.
 *
 * SHINE_PERIOD is the distance between bands along the diagonal, and the
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

/* Which rows had anything hatched in them last time they were painted, so
 * a tick of the shine can repaint those and leave the rest alone.
 *
 * Without this the shine would have to claim the whole screen every time it
 * moved, which at SHINE_STEP_MS is far too often to be affordable. A row
 * that is not repainted keeps its last answer, which stays true: nothing in
 * it changed, so whatever glass it had it still has. */
static uint8_t row_has_shine[GRID_H_MAX];

static inline void paint_row_n(gfx_color_t *fb, const gfx_color_t *pal,
                               int cy, const uint8_t *row, int n)
{
    gfx_color_t *out = fb + (cy * n) * GFX_WIDTH;
    row_has_shine[cy] = 0;

    /* grid_w, not a parameter: it does not need to be a compile-time
     * constant the way n does - only the innermost dy/dx loops below are hot
     * enough, per pixel rather than per cell, to matter. */
    /* Off the grid is NOT empty - the walls are solid, the same reading
     * sand_at() gives out-of-bounds cells - so a wall lying against the
     * screen edge is not outlined there. */
    const uint8_t *above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t *below = (cy < grid_h - 1) ? row + grid_w : NULL;

    for (int cx = 0; cx < grid_w; cx++) {
        const bool edge =
            (cx > 0            && CELL_IS_EMPTY(row[cx - 1])) ||
            (cx < grid_w - 1   && CELL_IS_EMPTY(row[cx + 1])) ||
            (above != NULL     && CELL_IS_EMPTY(above[cx]))   ||
            (below != NULL     && CELL_IS_EMPTY(below[cx]));

        gfx_color_t col[3];
        const material_pattern_t pat =
            material_colours(row[cx], material_grain_hash(cx, cy), edge, col);
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
         * The gravity-aligned diagonal is the one drawn slightly stronger
         * (it picks `lit` first), so the grain still follows the board.
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
                 * phase. */
                const bool grain = (((base + dx + dy) & 7) == 0) ||
                                   (((diff + dx - dy) & 7) == 0);

                /* SHINE: a band travelling along the diagonal, advanced on
                 * a clock rather than aimed by anything. A mask rather
                 * than a modulo because SHINE_PERIOD is a power of two,
                 * and it is fine on the values left of the origin -
                 * two's complement shifts the phase, which nothing here
                 * can tell from any other phase.
                 *
                 * `< n` is the width: one CELL, so the band looks the same
                 * at every quality setting. n is a compile-time constant
                 * here, so this is a comparison against a literal. */
                const int along = (base + dx + dy + shine_offset)
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

static void draw_dirty_rows(bool shine_moved)
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

    /* 256 entries in flash, indexed by the raw cell byte: no material lookup,
     * no shade arithmetic, no colour conversion, and no RAM. */
    const gfx_color_t *pal = material_palette();

#if CONFIG_LAUNCHER_DEVELOPMENT
    int redrawn = 0;
#endif

    for (int cy = 0; cy < grid_h; cy++) {
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
 * brushes: a cell is not a physical size, it is 2 px at HIGH and 6 px at
 * VERY LOW for the same object, so a marker drawn "one cell wide" would be
 * a different physical mark at every quality setting, and it would shrink
 * to nearly nothing at HIGH specifically - the opposite of what a marker
 * that has to be findable by a finger needs. Findability is a property of
 * the finger, not of the grid, so the marker gets a size the grid has no
 * say over.
 *
 * Tuned by eye. At MEDIUM's 3 px cells this spans about four cells across,
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
    if (ui.erasing) {
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
     * cannot just have its variant bumped like an ordinary one. */
    const gfx_color_t ink =
        ui.erasing ? gfx_rgb(0xFF8A5C) : brush_color(brushes[ui.brush]);

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
 * turning. It sets how quickly the tilt filter tracks a genuine
 * reorientation, and - raw, unlike everything that filter smooths - it is
 * also what tells the sand how hard it is currently being flicked; see
 * sand_set_flick() and the comment above SAND_REBOUND_GAIN in sand.h. It is
 * deliberately not what shaking is read from - see tilt.h, and the note on
 * rotating not being shaking.
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
    if (!ui.erasing && ui.modes[ui.brush] == BRUSH_SPAWN) {
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
     * covered twice the physical area at LOW that it does at HIGH, since a
     * LOW cell is twice as many pixels across.
     *
     * Rounded to nearest (+ cell / 2 before dividing) rather than truncated,
     * because this is a physical size in pixels being converted to a count
     * of cells, and plain integer division biases that count small at every
     * quality where cell does not divide the radius evenly - a small bias at
     * 3 or 4 px, and a 40% shrink at 6 px, where 10 / 6 truncates to 1
     * instead of rounding to 2. Never rounds to 0 for any quality in the
     * table: the smallest result is POUR_RADIUS_PX at the coarsest cell,
     * (10 + 3) / 6 = 2. */
    for (int i = 0; i < applications; i++) {
        if (ui.erasing) {
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
static void run_sim_steps(int gx, int gy, int jostle, int flow, int rotation,
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

    sand_set_flick(&sim, rotation);
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
    if (input->down && !ui.erasing) {
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
 * quality button cycles HIGH/MEDIUM/LOW and stays on the menu. Modeled on
 * ui_launcher.c's own frame - same ui_begin()/ui_begin_screen()/
 * mu_end_window()/ui_end() shape, one full-screen window with no chrome.
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
             * advance_shine() or passes shine_moved - the simulation is
             * paused while the panel is open, so this repaint happens only
             * on an actual orientation change, never once per frame; ticking
             * the shine on a static canvas would be paying an animation cost
             * for a picture that already looks right. */
            mark_sand_fully_dirty();
            draw_dirty_rows(false);
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
            ESP_LOGI(TAG, "brush: %s",
                     ui.erasing ? "erase" : material_name(brushes[ui.brush]));
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

    handle_pour_input(input, dt_ms);
    log_direction_change(gx, gy, jostle, &sample);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t0 = esp_timer_get_time();
#endif

    run_sim_steps(gx, gy, jostle, flow, rotation, dt_ms);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t1 = esp_timer_get_time();
    int awake_blocks, awake_cells;
    count_awake(&awake_blocks, &awake_cells);
#endif

    draw_dirty_rows(advance_shine(dt_ms));

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

const app_t app_sand = {
    .name    = "Falling Sand",
    .summary = "Tilt to steer, touch to pour",
    .enter   = sand_enter,
    .frame   = sand_frame,
    .exit    = sand_exit,
};

APP_REGISTER(app_sand);
