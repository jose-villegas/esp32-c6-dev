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
#include "../../gfx.h"
#include "../../imu.h"
#include "../../ui.h"
#include "palette.h"
#include "row_runs.h"
#include "sand.h"
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

/* SCREEN_PALETTE is entered from SCREEN_RUNNING (BOOT held) and left back to
 * it (any BOOT event) - see handle_brush_input()'s boot.held check and
 * handle_palette_input() below. It never appears on the MENU or failed
 * paths, which is why sand_frame()'s dispatch checks it only after those -
 * see the comment on that dispatch. */
typedef enum { SCREEN_MENU, SCREEN_RUNNING, SCREEN_PALETTE } screen_t;
static screen_t screen;

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

/* What the finger puts down.
 *
 * BOOT cycles the material and PWR toggles the eraser, rather than putting the
 * eraser in the cycle. Two reasons, and both are about which button does what:
 *
 *   BOOT is a plain GPIO with no side effects, so it is the safe one to press
 *   repeatedly. PWR's PMU powers the board off on a long hold by default (REG
 *   0x27 offlevel, 4-10 s) - firmware could disable that via REG 0x22 bit 1,
 *   but nothing here does, so as shipped a long hold on PWR still cuts power,
 *   which is a poor property for the control used most often.
 *
 *   Keeping erase out of the cycle means returning from it lands back on the
 *   material already chosen, instead of walking through all of them.
 *
 * A stand-in until the palette overlay; without some way to choose, water and
 * stone exist and cannot be reached.
 *
 * MAT_WOOD but not MAT_STEAM: every entry here costs the player a button
 * press to cycle past, so only materials someone actually paints belong.
 * Wood is something you build a fire out of; steam is a byproduct you
 * watch happen. Burning wood is not listed either, because it is a STATE
 * of wood rather than a material - see reaction_t.burn_decay, and
 * docs/Sand/Adding-a-Material.md for this as a worked example. */
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
 * it - see palette.h. This is what makes adding a 15th brush a BUILD failure
 * if it ever pushed the panel past the bottom of the screen, rather than a
 * silently clipped row discovered by looking at the device. */
_Static_assert(PALETTE_FITS(BRUSH_COUNT),
               "the palette panel for BRUSH_COUNT brushes is taller than the "
               "448px screen - see PALETTE_COLS/PALETTE_TILE in palette.h");

/* How long the mode label stays up after the PWR button is pressed. Long
 * enough to read without hurrying, short enough not to sit over the sand. */
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
static int         brush;            /* index into brushes - BOOT cycles it */
static bool        erasing;          /* PWR toggles it, independently */
static uint32_t    label_left_ms;    /* countdown for the mode label */

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
    screen = SCREEN_MENU;

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
 * start out lying like that. Shared with close_palette(), which needs
 * exactly the same lie for exactly the same reason: whatever the panel just
 * left in the framebuffer has to be forced out in full on the first frame
 * after it closes, not trusted to the sand's own (much narrower) real
 * extent. */
static void seed_row_runs_full_width(void)
{
    for (int i = 0; i < grid_h; i++) {
        row_run_x0[i * ROW_MAX_RUNS] = 0;
        row_run_x1[i * ROW_MAX_RUNS] = (uint16_t)grid_w;
        row_run_n[i] = 1;
    }
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
    brush = 0;
    erasing = false;
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
         * sand_frame() - leaving screen at SCREEN_MENU here would strand the
         * user on a menu whose only button silently re-fails the same
         * allocation forever, with no on-screen sign anything is wrong. */
        screen = SCREEN_RUNNING;
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

    screen = SCREEN_RUNNING;
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

/* Draws the mode label against whichever edge is currently UP.
 *
 * Up is the opposite of gravity, so the label follows the device rather than
 * the screen: turn the board on its side and the label moves to what is now
 * the top, and turns with it so it still reads the right way up. Anything else
 * looks like a bug the moment the device is not held upright.
 *
 * Snapped to one of four, because the font can only be turned in quarters -
 * a diagonal tilt picks whichever quarter it is nearest. */
static void draw_mode_label(int gx, int gy)
{
    /* The material's own name, so the label says what the finger will do
     * rather than merely that something changed. */
    const char *text = erasing ? "ERASE" : material_name(brushes[brush]);
    const int   len  = (int)strlen(text);
    const int   span = len * 8 * LABEL_SCALE;
    const int   tall = 8 * LABEL_SCALE;

    /* Which edge is up. Compare magnitudes rather than an angle: whichever
     * component of gravity dominates names the axis, and its sign the edge. */
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    int x, y, turn;
    if (ay >= ax) {
        if (gy >= 0) {
            turn = 0;                                  /* down is down */
            x = (GFX_WIDTH - span) / 2;
            y = LABEL_MARGIN;
        } else {
            turn = 2;                                  /* board upside down */
            x = (GFX_WIDTH + span) / 2 - 8 * LABEL_SCALE;
            y = GFX_HEIGHT - LABEL_MARGIN - tall;
        }
    } else {
        if (gx >= 0) {
            turn = 3;                                  /* down is to the right */
            x = LABEL_MARGIN;
            y = (GFX_HEIGHT + span) / 2 - 8 * LABEL_SCALE;
        } else {
            turn = 1;                                  /* down is to the left */
            x = GFX_WIDTH - LABEL_MARGIN - tall;
            y = (GFX_HEIGHT - span) / 2;
        }
    }

    /* Coloured as the material itself, so the label needs no colour table of
     * its own - see brush_color()'s own comment for why an extended cell
     * cannot just have its variant bumped like an ordinary one. */
    const gfx_color_t ink =
        erasing ? gfx_rgb(0xFF8A5C) : brush_color(brushes[brush]);

    gfx_text_turned(x, y, text, ink, LABEL_SCALE, turn);
}

/* Gap left between adjacent tiles, and around an unselected tile's swatch -
 * the panel's own background (cleared first, below) shows through it as
 * grout, which is what visually separates one tile from the next. */
#define PALETTE_GROUT   2

/* Thickness of each bezel edge - see draw_palette() below. */
#define PALETTE_BEZEL   3

/* The material picker overlay - drawn ONCE when the panel opens (see
 * open_palette()) and left sitting in the framebuffer for as long as it is
 * up, never redrawn per frame. That is only safe because nothing else draws
 * while SCREEN_PALETTE is active - sand_frame() skips the sim step and the
 * sand's own redraw entirely for that screen (see its dispatch) - so
 * anything added later that paints during the pause (a clock, an animation)
 * would need to redraw this too, or it will be silently painted over and
 * never restored. */
static void draw_palette(void)
{
    int px, py, pw, ph;
    palette_panel_rect(BRUSH_COUNT, &px, &py, &pw, &ph);

    /* Clear the panel's own background first, so no sand shows through
     * between tiles - this is also what erases a pressed-in bezel that has
     * just moved off a tile, since every tile gets redrawn below anyway. */
    gfx_fill_rect(px, py, pw, ph, gfx_rgb(COL_BACKGROUND));

    for (int i = 0; i < BRUSH_COUNT; i++) {
        int x, y, w, h;
        palette_tile_rect(i, BRUSH_COUNT, &x, &y, &w, &h);

        const gfx_color_t face = brush_color(brushes[i]);

        /* Highlight and shadow are derived from the face colour rather than
         * fixed greys, for the same reason the name text below is drawn
         * black-then-white instead of one fixed ink: the swatches run from
         * snow's near-white to stone's near-black, and a fixed highlight
         * would vanish on the pale ones while a fixed shadow vanished on
         * the dark ones. ~40% (100 of 255) is the tuned-by-eye amount. */
        const gfx_color_t hi = gfx_color_mix(face, gfx_rgb(0xFFFFFF), 100);
        const gfx_color_t sh = gfx_color_mix(face, gfx_rgb(0x000000), 100);

        /* The selected tile inverts the bezel - shadow on top/left, highlight
         * on bottom/right - so it reads as pressed in rather than raised.
         * That is the entire selection indicator; there is no separate ring
         * or border drawn for it. */
        const gfx_color_t top_left  = (i == brush) ? sh : hi;
        const gfx_color_t bot_right = (i == brush) ? hi : sh;

        /* Inset by the grout first so neighbouring tiles do not fuse into
         * one surface, then bezel within that inset rect: top/left edges
         * PALETTE_BEZEL thick in top_left, bottom/right edges the same in
         * bot_right, and the remaining interior in the face colour. Plain
         * fill rects, drawn top/left then bottom/right so the two corners
         * where an edge of each colour would otherwise meet (top-right,
         * bottom-left) resolve to whichever is drawn second - the same
         * simple corner convention as classic 3D control borders.
         *
         * A future round adds a per-brush "spawn" flag with a corner badge
         * on flagged tiles - not built here, out of scope for this panel.
         * x/y/w/h above are already this tile's own rect, so that badge
         * would just be one more gfx_fill_rect at a tile corner, drawn after
         * the face below; nothing about this loop's structure needs to
         * change to add it. */
        const int ix = x + PALETTE_GROUT;
        const int iy = y + PALETTE_GROUT;
        const int iw = w - 2 * PALETTE_GROUT;
        const int ih = h - 2 * PALETTE_GROUT;

        gfx_fill_rect(ix, iy, iw, PALETTE_BEZEL, top_left);              /* top */
        gfx_fill_rect(ix, iy, PALETTE_BEZEL, ih, top_left);              /* left */
        gfx_fill_rect(ix, iy + ih - PALETTE_BEZEL, iw, PALETTE_BEZEL, bot_right); /* bottom */
        gfx_fill_rect(ix + iw - PALETTE_BEZEL, iy, PALETTE_BEZEL, ih, bot_right); /* right */
        gfx_fill_rect(ix + PALETTE_BEZEL, iy + PALETTE_BEZEL,
                     iw - 2 * PALETTE_BEZEL, ih - 2 * PALETTE_BEZEL, face);

        /* Centred in the tile. The longest names are 5 characters, 80px at
         * GFX_GLYPH_SCALE (see palette.h's column-count reasoning), inside
         * a 92px tile - 6px of margin either side, never clipped. */
        const char *name = material_name(brushes[i]);
        const int   len  = (int)strlen(name);
        const int   tx   = x + (w - len * GFX_CHAR_W) / 2;
        const int   ty   = y + (h - GFX_CHAR_H) / 2;

        /* Drawn twice, black then white one pixel up-left, rather than in
         * one fixed ink colour - a swatch runs from snow's near-white to
         * stone's near-black, and no single colour reads on all of them. */
        gfx_text_scaled(tx + 1, ty + 1, name, gfx_rgb(0x000000), GFX_GLYPH_SCALE);
        gfx_text_scaled(tx, ty, name, gfx_rgb(0xFFFFFF), GFX_GLYPH_SCALE);
    }
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

/* Opens the palette panel: pauses the sim (see sand_frame()'s dispatch,
 * which returns immediately after calling this rather than falling through
 * to the rest of a RUNNING frame), clears the mode-label countdown so its
 * full-screen redraw does not paint sand straight back over the panel (see
 * label_left_ms's own use in sand_frame()), and draws the panel once - see
 * draw_palette()'s own comment on why once is enough. */
static void open_palette(void)
{
    label_left_ms = 0;
    screen = SCREEN_PALETTE;
    draw_palette();
}

/* Closes the palette panel and forces a full repaint of the sand underneath
 * it - the same seeding start_sim() uses to force a fresh grid's first
 * frame out in full, reused here via seed_row_runs_full_width() rather than
 * duplicated, so draw_dirty_rows() treats every row as needing a
 * full-width send on the very next frame and paints over every pixel the
 * panel touched.
 *
 * The two accumulators are zeroed rather than left to keep counting through
 * the pause: left alone, the wall-clock time the panel was open would cash
 * in as a burst of catch-up steps and pour the instant it closes, which
 * would read as a stutter or a sudden blob under the finger rather than as
 * nothing having happened while paused. */
static void close_palette(void)
{
    sim_accumulator_q8 = 0;
    pour_accumulator_ms = 0;
    seed_row_runs_full_width();
    memset(dirty_rows, 1, (size_t)grid_h);
    gfx_mark_all_dirty();
    screen = SCREEN_RUNNING;
}

/* Only reachable while SCREEN_PALETTE - see sand_frame()'s dispatch, which
 * calls this instead of the normal RUNNING handlers and returns.
 *
 * Any BOOT edge closes the panel, deliberately without asking which one:
 * being forgiving here is what guarantees there is no way to get stuck in
 * the panel. It cannot close on the SAME edge that opened it: the opening
 * edge is boot.held, and edges are read-and-cleared once per frame (see
 * buttons_read()) before this function ever runs for the first time - by
 * the next frame that edge is gone, and the release that follows the hold
 * fires no `released` edge at all (button_fsm.h's contract) - see the
 * comment on that composition where sand_frame() calls open_palette(). */
static void handle_palette_input(const input_t *input)
{
    /* On the RELEASE, never on the press. Closing on boot.pressed split a
     * single physical press across two screens: the panel went away on the
     * press edge, and the matching release edge arrived several frames
     * later with screen already back to SCREEN_RUNNING, where
     * handle_brush_input() consumed it and cycled the brush forward one.
     * Tapping a tile and then closing therefore selected the material
     * AFTER the one tapped.
     *
     * Acting on the release keeps both edges of a press inside the state
     * that started it - the same reason cycling itself moved off .pressed.
     * boot.held is safe alongside it because button_fsm suppresses the
     * release of a press that became a hold, so a hold delivers exactly
     * one of these two and never both. */
    if (input->boot.released || input->boot.held) {
        close_palette();
        return;
    }

    /* A tap that hits no tile does nothing - erase and selection state are
     * both untouched. The panel stays open either way; only BOOT closes it,
     * so picking a material is not a one-shot action. */
    if (input->released) {
        const int hit = palette_hit(input->x, input->y, BRUSH_COUNT);
        if (hit >= 0) {
            brush = hit;
            erasing = false;   /* choosing a material means you want to place it */
            draw_palette();    /* redraw so the highlight moves to the new tile */
        }
    }
}

/* input->boot.released rather than .pressed: a HOLD fires .pressed at the
 * press edge and .held at 600ms while still down (see button_fsm.h's
 * contract), so cycling on .pressed would make every panel-opening hold
 * also cycle the brush underneath it. The FSM guarantees a press that
 * becomes a hold delivers no .released edge at all, which makes .pressed
 * and .held mutually exclusive for one physical press with no bookkeeping
 * here - moving to .released is what makes that guarantee do the work. */
static void handle_brush_input(const input_t *input)
{
    if (input->boot.released) {
        brush = (brush + 1) % BRUSH_COUNT;
        erasing = false;      /* choosing a material means you want to place it */
        label_left_ms = LABEL_MS;
    }
    if (input->power.pressed) {
        erasing = !erasing;
        label_left_ms = LABEL_MS;
    }
    if (input->boot.released || input->power.pressed) {
        ESP_LOGI(TAG, "brush: %s",
                 erasing ? "erase" : material_name(brushes[brush]));
    }
}

/* Capped rather than looped to exhaustion: after a long frame the backlog is
 * dropped instead of dumping a pile in one go. */
static void handle_pour_input(const input_t *input, uint32_t dt_ms)
{
    if (!input->down) {
        pour_accumulator_ms = 0;
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
        if (erasing) {
            sand_erase(&sim, cx, cy, (ERASE_RADIUS_PX + cell / 2) / cell);
        } else {
            sand_spawn_cell(&sim, cx, cy,
                            (POUR_RADIUS_PX + cell / 2) / cell,
                            brushes[brush]);
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
    if (input->down && !erasing) {
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
 * ui_launcher.c's own frame - same ui_begin()/mu_begin_window_ex()/
 * mu_end_window()/ui_end() shape, one full-screen window with no chrome.
 *
 * The shell (main.c) draws the home-swipe hint over whatever the app drew
 * and owns the swipe-up-to-exit gesture itself, so this menu needs no back
 * button of its own. */
static void draw_menu(const input_t *input)
{
    mu_Context *ctx = ui_context();

    ui_begin(input);

    if (mu_begin_window_ex(ctx, "Sand Menu",
                           mu_rect(0, 0, GFX_WIDTH, GFX_HEIGHT),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                           MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        /* Both buttons centered as one block, so microui's default top-down
         * row layout is bypassed with mu_layout_set_next() and each button
         * placed at an absolute rect instead. */
        const int total_h = 2 * MENU_BTN_H + MENU_BTN_GAP;
        const int top      = (GFX_HEIGHT - total_h) / 2;
        const int x        = (GFX_WIDTH - MENU_BTN_W) / 2;

        mu_layout_set_next(ctx, mu_rect(x, top, MENU_BTN_W, MENU_BTN_H), 0);
        if (mu_button(ctx, "START")) {
            start_sim();
        }

        /* Built fresh each frame rather than cached: it is cheap, and
         * caching it would be one more thing to remember to invalidate
         * when quality changes. */
        char label[24];
        snprintf(label, sizeof label, "QUALITY: %s", qualities[quality].name);

        mu_layout_set_next(ctx, mu_rect(x, top + MENU_BTN_H + MENU_BTN_GAP,
                                        MENU_BTN_W, MENU_BTN_H), 0);
        if (mu_button(ctx, label)) {
            quality = (quality + 1) % QUALITY_COUNT;
        }

        mu_end_window(ctx);
    }

    ui_end(COL_BACKGROUND);
}

static void sand_frame(uint32_t dt_ms, const input_t *input)
{
    /* SCREEN_MENU is checked first, but a FAILED start_sim() sets screen to
     * SCREEN_RUNNING before returning specifically so it falls through to
     * the `failed` check below rather than getting caught here - see that
     * comment in start_sim(). Do not reorder these first two checks, or
     * change what SCREEN_PALETTE does relative to them, without keeping
     * that path intact.
     *
     * The palette check sits AFTER `failed`, deliberately: a failed start
     * must still reach the "no memory for the grid" screen even though
     * screen is SCREEN_RUNNING at that point, and SCREEN_PALETTE is never
     * entered on that path (see open_palette()'s only caller, below) - but
     * putting the check any earlier would invite exactly that mistake the
     * next time a state is added here. */
    if (screen == SCREEN_MENU) {
        draw_menu(input);
        return;
    }

    if (failed) {
        gfx_clear(gfx_rgb(0x1A0C0C));
        gfx_text(20, GFX_HEIGHT / 2, "no memory for the grid", gfx_rgb(0xFF5C5C));
        return;
    }

    if (screen == SCREEN_PALETTE) {
        handle_palette_input(input);
        return;
    }

    /* input->boot.held opens the panel from here, and closes over the rest
     * of this RUNNING frame doing so: open_palette() flips `screen` to
     * SCREEN_PALETTE immediately, so returning right after it means
     * read_gravity_input()/run_sim_steps()/handle_pour_input()/
     * handle_brush_input()/draw_dirty_rows() below are never reached on the
     * very frame the panel opens, exactly as they are skipped on every
     * later frame by the SCREEN_PALETTE check above - the sim is paused
     * from the same frame the panel becomes visible, not one frame later.
     *
     * The release that ends this hold fires no `released` edge at all -
     * see button_fsm.h's contract - so next frame's handle_palette_input()
     * cannot mistake the finger lifting for a tap that should close the
     * panel it just opened. No guard needed here for that; it falls out of
     * the FSM's own guarantee. */
    if (input->boot.held) {
        open_palette();
        return;
    }

    int gx, gy, flow, jostle, rotation;
    imu_sample_t sample = { 0 };
    read_gravity_input(dt_ms, &sample, &gx, &gy, &flow, &jostle, &rotation);

    handle_brush_input(input);

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
