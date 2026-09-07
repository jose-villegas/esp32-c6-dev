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

#define COL_BACKGROUND 0x0A0C14

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

static int quality = QUALITY_DEFAULT;

static int cell, grid_w, grid_h, block_cols, block_rows;

#define CELL_MIN    2                          /* finest quality; sets every allocation size */
#define GRID_W_MAX  (GFX_WIDTH  / CELL_MIN)
#define GRID_H_MAX  (GFX_HEIGHT / CELL_MIN)

#define BLOCK_COLS_MAX ((GRID_W_MAX + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define BLOCK_ROWS_MAX ((GRID_H_MAX + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

#define MENU_BTN_W    300
#define MENU_BTN_H    UI_ROW_HEIGHT
#define MENU_BTN_GAP  20

#define POUR_RADIUS_PX   10      /* was 5 cells at 2 px */

#define POUR_HZ       60
#define POUR_STEP_MS  (1000 / POUR_HZ)

#define ERASE_RADIUS_PX  16      /* was 8 cells at 2 px */

#define ERASE_EMITTER_RADIUS_PX  32









#define DETONATE_RADIUS_PX  50

#define APP_IMPULSE_MAX  2048














#define SAND_IMPULSE_BUDGET_BYTES  12288

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

/* What the finger puts down. Selected from the palette panel (BOOT's release
 * edge opens it - see sand_ui.c's open_palette(), and sand_frame()'s own
 * comment on why that is the release and not the press; a tap on a tile
 * selects it - see draw_palette()'s own mu_button() loop and sand_ui.c's
 * sand_ui_tile_clicked()) rather than cycled one button press at a time.
 * Cycling was the palette's stand-in before the panel existed, and it aged
 * badly for the obvious reason: reaching the Nth material cost N presses, and
 * every material added since - eight of them - pushed everything after it
 * that much further away. The panel costs one press to open and one tap to
 * choose, whatever this list grows to. PWR still cycles PAINT/ERASE/DETONATE
 * directly, unchanged from before the panel existed - see sand_ui.c's
 * handle_brush_input() and sand_mode_t's own comment in sand_ui.h for why
 * DETONATE rides along on this cycle rather than living in the palette. A
 * plain press is the cheaper action for all three: a HOLD costs
 * BUTTON_HOLD_US (600 ms) of waiting before it even registers, every single
 * time, and paying that tax on a control used this often would make erasing
 * feel sluggish next to the immediacy pouring already has. A dedicated
 * button's press has none of that cost. MAT_WOOD but not MAT_STEAM: every
 * entry here costs a tile in the palette panel - see BRUSH_COUNT and the
 * _Static_assert on PALETTE_FITS below - so only materials someone actually
 * paints belong. Wood is something you build a fire out of; steam is a
 * byproduct you watch happen. Burning wood is not listed either, because it
 * is a STATE of wood rather than a material - see reaction_t.burn_decay, and
 * docs/Sand/Adding-a-Material.md for this as a worked example. Whole CELLS
 * rather than material ids, because an extended material cannot be named by
 * an id - for the static half its whole low nibble is its identity (MATX() in
 * material.h); gunpowder only goes as far as bit 3 of the low nibble
 * (GUNPOWDER_CELL()), the bottom three bits being a code rather than part of
 * what names it. An ordinary material is written CELL_MAKE(id, 0) and its
 * variant is chosen the usual way when it is painted. */
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

_Static_assert(PALETTE_FITS(BRUSH_COUNT),
               "the palette panel for BRUSH_COUNT brushes is taller than the "
               "screen at some orientation - see palette_cols()/PALETTE_TILE "
               "in palette.h");

/* brush_mode_t per brush - sand_init() does not reset this, so a brush can
 * still show Water as its source with no tap present. */
static uint8_t brush_mode[BRUSH_COUNT];

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

#define SHAKE_DEADZONE 40

#define SIM_HZ            60
#define SIM_STEP_MS       (1000 / SIM_HZ)

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

static uint16_t   *row_run_x0;
static uint16_t   *row_run_x1;
static uint8_t    *row_run_n;
static sand_t      sim;
static tilt_t      tilt;
static bool        failed;
static uint32_t    label_left_ms;    /* countdown for the mode label */

static bool        input_ready;

static int         palette_drawn_quarter;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Rolling averages, purely for the log line - a release build has nobody
 * watching the serial console to read them, so it carries none of this. */
static uint32_t frames;
static int64_t  step_us_total;
static int64_t  draw_us_total;
static int64_t  rows_redrawn_total;
static int64_t  steps_total;

static int64_t  pour_step_us_total, pour_draw_us_total;
static uint32_t pour_frames;
static int64_t  idle_step_us_total, idle_draw_us_total;
static uint32_t idle_frames;
static int64_t  split_log_at_us;

static int64_t  pour_awake_total, idle_awake_total;

/* Measure occupied cells in blocks; confirms step_one_row() cost per row, not
 * unit. */
static int64_t  pour_awake_cells_total, idle_awake_cells_total;
#endif
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
    ui.screen = SAND_UI_MENU;

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

static void mark_sand_fully_dirty(void)
{
    seed_row_runs_full_width();
    memset(dirty_rows, 1, (size_t)grid_h);
    gfx_mark_all_dirty();
}

#if CONFIG_LAUNCHER_SELFTEST
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
     * INSTEAD AND A DEVICE FLASH MADE IT WORSE. A live serial capture that
     * showed impulse_buf's malloc failing with "largest free block is 14592"
     * at three different quality settings was misread once as "impulse_buf
     * never gets a fair shot because grid/dirty_rows/ sleep_blocks fragment
     * the heap ahead of it" - grid's own request (GRID_W_MAX * GRID_H_MAX) is
     * actually a FIXED 41,216 bytes regardless of quality (the varying
     * numbers in that capture were the ACTIVE grid_w*grid_h subset in use,
     * not the allocation size), and it succeeded cleanly in all three
     * captures. So the real picture those three captures agree on is: this
     * heap reliably has one contiguous run big enough for grid's 41,216
     * bytes, and roughly 14,592 bytes left over after grid and the small
     * buffers land - which is a single largest-block ordering fact, not a
     * fragmentation problem this app's own allocation order was causing.
     * Moving impulse_buf's smaller (12,288-byte, see APP_IMPULSE_MAX) request
     * to go FIRST was tried anyway, on the chance that ordering still
     * mattered - and a device flash of that build produced "no memory for the
     * grid" instead, a WORSE failure than impulse_buf alone failing:
     * impulse_buf grabbed a piece of the one heap region big enough for it,
     * and grid's subsequent 41,216-byte request then found nowhere left to
     * land, tripping the mandatory-buffer fallback below (`ui.screen =
     * SAND_UI_RUNNING` with the grid's own "could not allocate" message).
     * Reordering does not create more contiguous space anywhere in the heap -
     * it only decides who gets first pick of what already exists - and on
     * THIS device grid is the one allocation that needs the single largest
     * contiguous run, so it has to be the one that picks first. This ordering
     * (grid and the other mandatory buffers before impulse_buf) is the one
     * three real device captures confirm actually works; do not move
     * impulse_buf ahead of grid again without a fresh device capture that
     * shows it helping, because the only capture that ever tried has already
     * shown it hurting. */
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
    if (grid == NULL || dirty_rows == NULL || sleep_blocks == NULL ||
        row_run_x0 == NULL || row_run_x1 == NULL || row_run_n == NULL) {
        ESP_LOGE(TAG, "Could not allocate a %d x %d grid (%d bytes); "
                      "largest free block is %u",
                 GRID_W_MAX, GRID_H_MAX, GRID_W_MAX * GRID_H_MAX,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        failed = true;
        ui.screen = SAND_UI_RUNNING;
        return;
    }

    seed_row_runs_full_width();

    sand_init(&sim, grid, grid_w, grid_h, (uint32_t)esp_timer_get_time());
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);

    sand_track_dirty_rows(&sim, dirty_rows);

    sand_enable_sleeping(&sim, sleep_blocks);

    /* DETONATE scaffolding - see sand_ui.h. Enabled unconditionally to catch
     * allocation failures early. */
    sand_enable_impulses(&sim, impulse_buf, APP_IMPULSE_MAX);
    tilt_reset(&tilt, IMU_COUNTS_PER_G);

    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - falling back to fixed downward gravity");
    }

    ESP_LOGI(TAG, "%d x %d grid, %d bytes, %d px cells",
             grid_w, grid_h, grid_w * grid_h, cell);

    gfx_clear(material_palette()[SAND_EMPTY]);
    gfx_mark_all_dirty();

    ui.screen = SAND_UI_RUNNING;
}

static void sand_exit(void)
{
    /* Grid is kept between visits (the app's largest allocation) so
     * re-entry cannot fail to heap fragmentation from whatever ran while
     * this app was closed. */
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











#define SHINE_PERIOD   64      /* power of two - see the mask below */
#define SHINE_STEP_MS  40
#define SHINE_STEP_PX   2

static int      shine_offset;
static uint32_t shine_elapsed_ms;

static int shine_ux_q8 = 181;
static int shine_uy_q8 = 181;

static uint8_t row_has_shine[GRID_H_MAX];

static uint8_t row_has_cullet[GRID_H_MAX];

static uint8_t row_has_glass[GRID_H_MAX];

#define FOAM_BLOB_SHIFT 3

#define FOAM_PHASE_MS 90

static uint32_t foam_elapsed_ms;

#define CULLET_PHASE_MS 250

static uint32_t cullet_elapsed_ms;

#define GLASS_PHASE_SHIFT 7

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









static unsigned local_depth_scale_q8;
static bool local_depth_vertical_dominant;
static bool local_depth_v_reverse;
static bool local_depth_h_reverse;

static unsigned local_depth_ax;
static unsigned local_depth_ay;

static bool local_depth_vertical_dominant_prev;
static bool local_depth_v_reverse_prev;
static bool local_depth_h_reverse_prev;






static uint8_t local_depth_row_a[GRID_W_MAX];
static uint8_t local_depth_row_b[GRID_W_MAX];
static uint8_t *local_depth_cur_row = local_depth_row_a;
static uint8_t *local_depth_prev_row = local_depth_row_b;



/* THE DEFECT, exactly. Every cross-row read in paint_row_n() below treats
 * local_depth_prev_row[qx] as "the count belonging to the cell one step
 * back along the ray, in row cy - vdir". That is only true when the row
 * painted immediately before this one WAS row cy - vdir. */





/* With this guard: 83, and none of them in the body of the pool (see the
 * residual note below). */






#define LOCAL_DEPTH_NO_ROW (-2)
static int local_depth_prev_cy = LOCAL_DEPTH_NO_ROW;











/* BOTH REGIMES SHARE THE ARRAY, NOT JUST THE TYPE, because a REGIME FLIP
 * (see update_local_depth_gravity() below) always resets it wholesale
 * alongside local_depth_row_a[]/local_depth_row_b[] - the two regimes
 * never read a value the OTHER one wrote, by construction, so there is no
 * cross-regime confusion for either convention to guard against. */
static uint8_t local_depth_top_row[GRID_W_MAX];

/* local_depth_cur_row[]/local_depth_prev_row[] hold a plain CELL COUNT - see
 * LOCAL DEPTH's own top comment, "THE COUNT MUST STAY A RAW COUNT", for why
 * an eighths-of-a-cell (or any pre-scaled) accumulator was tried for an
 * earlier shape of this mechanism and rejected; that lesson transfers
 * unchanged. This is that count's saturation point. UNLIKE THE TWO-WALK
 * DESIGN'S OWN LOCAL_DEPTH_COUNT_CEILING (34, raised above
 * MATERIAL_LIQUID_DEPTH_BAND's own 24), THIS ONE NEEDS NO RAISE - and that is
 * a genuine, load-bearing difference from this walk's own scale, not an
 * oversight carried over. The two-walk design's own weight was `component /
 * len`, ALWAYS <= 256 (minimised at the 45-degree tie, around 183 of 256), so
 * a count clamped at the plain band (24) could project to BELOW the band at
 * every angle except perfect axis alignment - "breathing" - and the ceiling
 * had to be raised past the band so a saturated count still reached it after
 * being shrunk. THIS walk's own scale is `len / dominant_axis`, ALWAYS >= 256
 * (equal to 256 only at perfect axis alignment, growing at every other angle)
 * - so a count clamped at the plain band ALREADY projects to AT LEAST the
 * band at every angle, and the combiner's own explicit clamp to
 * MATERIAL_LIQUID_DEPTH_BAND (see "THE WALK ITSELF" below) does the rest.
 * Verified directly, host-side, against a fully saturated cell swept across
 * the same 0-90 degree range test_a_
 * saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle uses:
 * projected depth clamps to a flat MATERIAL_LIQUID_DEPTH_BAND at every
 * sample, with the ceiling equal to the band and no raise at all - the same
 * flatness the raised ceiling existed to buy back, without needing to buy it
 * back, because this walk's own scale points the opposite way. */
#define LOCAL_DEPTH_COUNT_CEILING MATERIAL_LIQUID_DEPTH_BAND

static void update_local_depth_gravity(int gx, int gy)
{
    const int ax = im_abs(gx), ay = im_abs(gy);

    const int len = im_len(gx, gy);
    const bool new_vertical_dominant = (ay >= ax);
    const unsigned dom_axis = new_vertical_dominant ? (unsigned)ay : (unsigned)ax;
    local_depth_scale_q8 = (dom_axis != 0u)
        ? (256u * (unsigned)len) / dom_axis : 256u;
    local_depth_ax = (unsigned)ax;
    local_depth_ay = (unsigned)ay;

    const bool new_v_reverse = (gy < 0);
    const bool new_h_reverse = (gx < 0);















    /* THE GATE IS EXACT ARITHMETIC, NOT A DEADBAND - deliberately, because
     * this mechanism already removed one tuned dead zone and should not
     * quietly grow another. Each condition below is a statement about
     * whether the flipped flag can change a number THIS grid's walk
     * actually computes, derived from the Bresenham arithmetic itself. */



    /* A REGIME FLIP is never gated - it always changes what the array
     * slot means, whatever the magnitudes are. */

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
        local_depth_prev_cy = LOCAL_DEPTH_NO_ROW;
        local_depth_vertical_dominant_prev = new_vertical_dominant;
        local_depth_v_reverse_prev = new_v_reverse;
        local_depth_h_reverse_prev = new_h_reverse;
    }

    local_depth_vertical_dominant = new_vertical_dominant;
    local_depth_v_reverse = new_v_reverse;
    local_depth_h_reverse = new_h_reverse;
}















/* A pool's INTERIOR - the bulk of its rows, the part this array already
 * marked before the widening - is unaffected: a row with any interior
 * cell was already gated in, rim or not. The widening can only ADD the
 * handful of edge-only rows the old condition used to skip; it cannot
 * double the marked-row count the way gating on "any liquid" from scratch
 * would if the array previously gated on nothing at all. */
#define LOCAL_DEPTH_WAKE_MS 120

static uint32_t local_depth_wake_elapsed_ms;

static uint8_t row_has_liquid[GRID_H_MAX];

static inline void paint_row_n(gfx_color_t *fb, const gfx_color_t *pal,
                               int cy, const uint8_t *row, int n)
{
    gfx_color_t *out = fb + (cy * n) * GFX_WIDTH;
    row_has_shine[cy] = 0;
    row_has_liquid[cy] = 0;
    row_has_cullet[cy] = 0;
    row_has_glass[cy] = 0;

    const uint8_t *above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t *below = (cy < grid_h - 1) ? row + grid_w : NULL;

    const uint8_t *toward_surface = local_depth_v_reverse ? below : above;

    /* local_depth_prev_row[] checks if `toward_surface` points at it, hoisted
     * out of the cx loop for an 841-to-83 improvement. Only the
     * hold-then-commit debounce reads it, as same-material climbs trust a
     * stale count, and BOUNDARY's carry is nonsense for other rows. */
    const int local_depth_vdir = local_depth_v_reverse ? -1 : 1;
    const bool local_depth_chain_ok =
        (local_depth_prev_cy == cy - local_depth_vdir);

    const int hdir = local_depth_h_reverse ? -1 : 1;
    const int cx_first = local_depth_h_reverse ? grid_w - 1 : 0;
    const int cx_step  = local_depth_h_reverse ? -1 : 1;

    /* THE ROW OFFSET, WITHOUT AN ACCUMULATOR - the vertical-dominant regime's
     * own per-row horizontal drift ("step" in LOCAL DEPTH's own top comment).
     * Every column in this row shares the SAME step (gravity does not change
     * from one column to the next), so this is computed ONCE per row-call -
     * but it must NOT be a running Bresenham accumulator carried across
     * separate paint_row_n() calls the way a naive port of a reference model
     * would do it: paint_row_n() is only ever called for DIRTY rows (LOCAL
     * DEPTH's own top comment, "STALE READINGS...ARE ACCEPTED"), so a
     * row-to-row accumulator would silently skip every row in between and
     * drift out of sync with whichever row is actually being painted. `cum(n)
     * = floor(n * minor / dominant)` is the exact total Bresenham drift after
     * `n` row-steps starting from a clean 0 - verified by hand against a
     * plain step-by-step Bresenham march before this was written this way,
     * both give identical sequences - so THIS row's own step is simply
     * `cum(n) - cum(n - 1)` for whatever `n` (this row's own distance, in the
     * scan's own direction, from the surface-most row) this particular row
     * happens to be, computable directly from `cy` alone with no memory of
     * which rows were painted before it. One divide, but once per PAINTED row
     * here, not once per cell - see update_local_depth_gravity()'s own
     * comment for the budget this spends against. Meaningless (and left 0)
     * when horizontal-dominant, or when gravity has no direction at all
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

    int local_depth_herr = 0;
    const int ysign = local_depth_v_reverse ? 1 : -1;

    const unsigned cullet_first = MAT_SAND * MATERIAL_VARIANTS + SAND_CULLET_BASE;

    for (int cx_i = 0; cx_i < grid_w; cx_i++) {
        const int cx = cx_first + cx_i * cx_step;

        unsigned mask =
            ((cx > 0          && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT  : 0u) |
            ((cx < grid_w - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u) |
            ((above != NULL   && CELL_IS_EMPTY(above[cx]))   ? MATERIAL_EDGE_UP    : 0u) |
            ((below != NULL   && CELL_IS_EMPTY(below[cx]))   ? MATERIAL_EDGE_DOWN  : 0u);

        if ((mask & MATERIAL_EDGE_CARDINAL) != 0 &&
            CELL_MATERIAL(row[cx]) == MAT_WATER) {
            mask |=
                ((cx > 0          && above != NULL && CELL_IS_EMPTY(above[cx - 1])) ? MATERIAL_EDGE_UP_LEFT    : 0u) |
                ((cx < grid_w - 1 && above != NULL && CELL_IS_EMPTY(above[cx + 1])) ? MATERIAL_EDGE_UP_RIGHT   : 0u) |
                ((cx > 0          && below != NULL && CELL_IS_EMPTY(below[cx - 1])) ? MATERIAL_EDGE_DOWN_LEFT  : 0u) |
                ((cx < grid_w - 1 && below != NULL && CELL_IS_EMPTY(below[cx + 1])) ? MATERIAL_EDGE_DOWN_RIGHT : 0u);
        }

        const bool cell_is_water = CELL_MATERIAL(row[cx]) == MAT_WATER;
        const unsigned hash = cell_is_water
            ? material_grain_hash(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)
            : material_grain_hash(cx, cy);

        int step = 0;
        if (!local_depth_vertical_dominant) {
            local_depth_herr += (int)local_depth_ay;
            if (local_depth_ax > 0u && local_depth_herr >= (int)local_depth_ax) {
                local_depth_herr -= (int)local_depth_ax;
                step = ysign;
            }
        }

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

        unsigned count;
        if (!here_liquid) {
            count = 0u;
        } else if (same_material) {
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
                count = 0u;
            } else {
                const unsigned carry =
                    (cross_row && !local_depth_chain_ok) ? 0u : src_count;
                count = carry < LOCAL_DEPTH_COUNT_CEILING
                    ? carry + 1u : LOCAL_DEPTH_COUNT_CEILING;
                local_depth_top_row[cx] = local_depth_vertical_dominant
                    ? (uint8_t)cy : 0u;
            }
        }
        local_depth_cur_row[cx] = (uint8_t)count;

        const unsigned depth_raw = (count * local_depth_scale_q8) >> 8;
        const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

        const unsigned depth = (row[cx] == MATX(MATX_ROOT))
            ? material_root_neighbours(above, row, below, cx, grid_w)
            : depth_liquid;

        if (here_liquid) {
            row_has_liquid[cy] = 1;
        }

        if ((unsigned)(row[cx] - cullet_first) < SAND_CULLET_SHADES) {
            row_has_cullet[cy] = 1;
        }

        if (CELL_MATERIAL(row[cx]) == MAT_GLASS) {
            row_has_glass[cy] = 1;
        }

        gfx_color_t col[3];
        const material_pattern_t pat =
            material_colours(row[cx], hash, mask, depth, col);
        gfx_color_t *p = out + cx * n;

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

        const int shine_base_q8 = (cx * n) * shine_ux_q8 + (cy * n) * shine_uy_q8;

        for (int dy = 0; dy < n; dy++) {
            for (int dx = 0; dx < n; dx++) {


                const int shine_q8 = shine_base_q8 + dx * shine_ux_q8 + dy * shine_uy_q8;
                const int along = ((shine_q8 >> 8) + shine_offset)
                                  & (SHINE_PERIOD - 1);

                p[dy * GFX_WIDTH + dx] = (along < n) ? col[2] : col[0];
            }
        }
    }

    uint8_t *local_depth_tmp = local_depth_cur_row;
    local_depth_cur_row = local_depth_prev_row;
    local_depth_prev_row = local_depth_tmp;

    local_depth_prev_cy = cy;
}

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

static unsigned cullet_phase_index;

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

    if (shine_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_shine[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (local_depth_woke) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_liquid[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (cullet_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_cullet[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (glass_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_has_glass[cy]) {
                dirty_rows[cy] = 1;
            }
        }
    }

    const gfx_color_t *pal = material_palette();

#if CONFIG_LAUNCHER_DEVELOPMENT
    int redrawn = 0;
#endif

    /* Ordinarily ascending - but see
     * local_depth_row_a[]/local_depth_row_b[]'s own comment in paint_row_n()
     * for why a gravity-UP frame walks this loop in the OPPOSITE order
     * instead: local_depth_cur_row[]/local_ depth_prev_row[]'s own pointer
     * swap means "the row painted before this one" is only meaningful if rows
     * are actually visited surface-first, or a dirty row far from the surface
     * reads whatever a DIFFERENT, farther row left behind the last time IT
     * was nearer the front of the sweep, not last frame's own value for the
     * row actually being painted. UNLIKE the two-walk design (where only the
     * vertical array needed this and the horizontal one never did), BOTH
     * regimes of the single ray walk depend on row order now - see LOCAL
     * DEPTH's own top comment for why one row-order rule (driven by gy's sign
     * alone) turns out to serve both the vertical-dominant regime's own
     * per-row chain AND the horizontal-dominant regime's own occasional
     * cross-row reads. Just "does gravity point up" - `local_depth_v_reverse`
     * alone - with no "and which regime is active this frame" to ask
     * alongside it: the same rule the two-walk design would have needed one
     * flag for, this one needs for a different, cleaner reason (see
     * paint_row_n()'s own "THE ROW OFFSET, WITHOUT AN ACCUMULATOR").
     * Reversing here, rather than juggling which array slot means "toward the
     * surface" inside the depth bookkeeping itself, is safe because nothing
     * else in this loop depends on row order - dirty_rows[cy],
     * row_run_x0/x1/n, row_has_shine[cy], row_has_liquid[cy] and
     * row_has_cullet[cy] are all indexed by cy directly, and gfx_mark_dirty()
     * below only ever unions a bounding box, which does not care what order
     * the boxes arrive in either. */
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

#define EMITTER_MARKER_COLOR 0xFF3EC8

#define EMITTER_MARKER_PX  12

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

        const int mid_x = ex * cell + cell / 2;
        const int mid_y = ey * cell + cell / 2;
        const int px = mid_x - EMITTER_MARKER_PX / 2;
        const int py = mid_y - EMITTER_MARKER_PX / 2;
        gfx_fill_rect(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX, marker);
        gfx_mark_dirty(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX);
    }
}

static gfx_color_t brush_color(cell_t c)
{
    if (cell_is_gunpowder(c)) {
        return material_palette()[GUNPOWDER_CELL(2)];
    }
    return material_palette()[
        cell_is_extended(c) ? c : CELL_MAKE(CELL_MATERIAL(c), 13)];
}

static int gravity_quarter_turn(int gx, int gy)
{
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ay >= ax) {
        return (gy >= 0) ? 0 : 2;      /* down is down : board upside down */
    }
    return (gx >= 0) ? 3 : 1;          /* down is to the right : to the left */
}

/* Draws the mode label against whichever edge is currently UP. Up is the
 * opposite of gravity, so the label follows the device rather than the
 * screen: turn the board on its side and the label moves to what is now the
 * top, and turns with it so it still reads the right way up. Anything else
 * looks like a bug the moment the device is not held upright. Snapped to one
 * of four, because the font can only be turned in quarters - a diagonal tilt
 * picks whichever quarter it is nearest. STILL ITS OWN gravity_quarter_turn()
 * CALL - NOT display_shell_quarter() This looks like an inconsistency next to
 * draw_palette(), which now just inherits the shell's orientation instead of
 * computing one. It is not: the two draw through different paths.
 * draw_palette() goes through microui, which every other described UI in this
 * shell also goes through, and which is exactly what ui_set_transform()
 * reaches - that is the whole reason the shell owning the transform is enough
 * to turn the palette. draw_mode_label() instead calls gfx_text_turned()
 * straight onto the app's own canvas, bypassing microui and the UI transform
 * entirely, the same way the sand grid itself is painted. A transform set on
 * the UI layer has no effect on either. So this is exactly the
 * canvas-versus-chrome line: the palette is chrome (drawn through microui,
 * like the launcher and every boot menu), the label is canvas (drawn straight
 * onto this app's own framebuffer region, like the sand grid itself), and
 * only chrome follows the shell's transform for free. The label keeps turning
 * itself because nothing else will. */
static void draw_mode_label(int gx, int gy)
{
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

#define PALETTE_GROUT   4

#define PALETTE_BEZEL   3

/* Eligibility/spawn corner badge: 18px outer square on 92px PALETTE_TILE
 * tile, 2px border, 2px margin. */
#define PALETTE_BADGE_SIZE    18
#define PALETTE_BADGE_INSET    2
#define PALETTE_BADGE_MARGIN   2

#define PALETTE_BADGE_BORDER_COLOR  0x141414
#define PALETTE_BADGE_FILL_COLOR    0xF2F2F2

static mu_Color mu_color_hex(uint32_t rgb)
{
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF),
                    (int)(rgb & 0xFF), 255);
}









static void draw_palette(const input_t *input)
{
    mu_Context *ctx = ui_context();

    ui_begin(input);

    ui_set_text_style(UI_TEXT_OUTLINED);

    ui_set_button_style(UI_BUTTON_BEZEL);

    const mu_Color saved_button_color = ctx->style->colors[MU_COLOR_BUTTON];
    const mu_Color saved_text_color   = ctx->style->colors[MU_COLOR_TEXT];

    /* Black used for each tile name by mu_button() and UI_TEXT_OUTLINED.
     * ui_text_halo() derives a light halo at render time, ensuring dark text
     * reads against any swatch. Unlike the face below, this colour is set
     * once here rather than per tile. */
    ctx->style->colors[MU_COLOR_TEXT] = mu_color(0, 0, 0, 255);

    const int cols = palette_cols(ui_width());

    if (ui_begin_screen(ctx, "Sand Palette",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        for (int i = 0; i < BRUSH_COUNT; i++) {
            int x, y, w, h;
            palette_tile_rect(i, BRUSH_COUNT, cols, ui_width(), ui_height(),
                              &x, &y, &w, &h);

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

            /* The selection cue. ui_style's own bezel already inverts a
             * button's lit/shadowed edge pair, but only while microui's
             * hover/focus says a finger is on it THIS frame (see
             * styled_draw_frame() in ui.c) - there is no "selected" state for
             * a plain mu_button() to draw, since selection is this app's own
             * idea, not microui's. So the selected tile gets a second,
             * purpose-drawn cue on top: ui_bezel_spans() again, called for
             * its SUNKEN edge pair only (spans[1..4] - span[0] is the flat
             * face, already painted by mu_button() above, so it is skipped
             * rather than redrawn on top of itself). Those edges are mixed
             * toward white and toward black off THIS TILE'S OWN face colour
             * (see UI_BEZEL_HIGHLIGHT/UI_BEZEL_SHADOW in ui_style.h), not a
             * fixed colour - which is what keeps the ring visible at both
             * ends of the swatch range: on Snow, a near-white face, it is the
             * mixed-toward-black edge that reads; on Stone, a near-black
             * face, it is the mixed-toward-white edge that reads. A fixed
             * light or dark ring would vanish on one of those two the same
             * way the old white selection ring did, and the same way the
             * badge's own face-derived fill did before it was changed to a
             * fixed pair for the same reason - see
             * PALETTE_BADGE_BORDER_COLOR/ PALETTE_BADGE_FILL_COLOR's own
             * comment on that history. This ring gets to derive from the face
             * instead of needing a fixed pair of its own, because unlike the
             * badge it never has to sit ON a face of unknown lightness - it
             * sits at the tile's own edge, always paired with the one face it
             * was mixed from, so there is no separate swatch it has to stay
             * legible against. */
            if (i == ui.brush) {
                ui_span_t spans[UI_BEZEL_MAX_SPANS];
                const int n = ui_bezel_spans(mu_rect(ix, iy, iw, ih), face,
                                             true, spans, UI_BEZEL_MAX_SPANS);
                for (int s = 1; s < n; s++) {
                    mu_draw_rect(ctx, spans[s].rect, spans[s].color);
                }
            }

            /* The badge: zero or more rects (and an icon) drawn after the
             * bezel, at the tile's top-right corner inside it. Three states,
             * not one: eligible, BRUSH_POUR an empty box - a near-black
             * border (PALETTE_BADGE_BORDER_COLOR) around a near-white fill
             * (PALETTE_BADGE_ FILL_COLOR). The slot exists; nothing is armed.
             * eligible, BRUSH_SPAWN the same box, with a near-black check
             * mark drawn inside it ( MU_ICON_CHECK - draw_command() routes
             * that to icons.h's icon_check(), the same artwork the
             * diagnostics app's checkboxes use). The tap is armed. not
             * eligible nothing, as before - material_can_emit() is false for
             * every KIND_STATIC material (stone, glass, the STATIC half of
             * the extended range - ice, plant, leaf, metal, root). Gunpowder
             * is the one extended-range brush this rule does NOT hide: it is
             * KIND_POWDER, so it gets a badge like any ordinary powder. The
             * absence or presence of a badge is what makes that eligibility
             * rule visible on the panel instead of a fact someone has to be
             * told separately. The border and fill are a FIXED pair, not
             * derived from the face the way the bezel above is - see
             * PALETTE_BADGE_BORDER_ COLOR/PALETTE_BADGE_FILL_COLOR's own
             * comment for why: the badge used to derive from the face the
             * same way the bezel still does, and it read fine on most
             * swatches, but Snow's face is itself near-white, so mixing
             * toward white for the armed fill landed almost exactly on the
             * face colour - armed and unarmed became indistinguishable on the
             * one tile where telling them apart matters most. A mark that has
             * to read on every swatch from snow's near-white to stone's
             * near-black cannot itself be made of the swatch it sits on. Laid
             * out relative to the tile's own logical (ix, iy) corner, like
             * everything else in this loop - see this function's own top
             * comment on why that is enough to turn with the board: the
             * badge's rects go through draw_command() exactly like the bezel
             * spans above, so a quarter-turn transform carries this along
             * with the tile it sits on without this loop naming a turn
             * anywhere. */
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

    ui_end(UI_NO_BACKGROUND);
}

/*---------------------------------------------------------------------------
 * Frame
 *-------------------------------------------------------------------------*/

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

    tilt_update(&tilt, GRAVITY_SCREEN_X(sample), GRAVITY_SCREEN_Y(sample),
                sample->az, *rotation, dt_ms);

    *gx   = tilt_x(&tilt);
    *gy   = tilt_y(&tilt);
    *flow = tilt_strength(&tilt);

    const int shake = tilt_shake(&tilt);
    *jostle = shake > SHAKE_DEADZONE ? shake : 0;
}


static void handle_pour_input(const input_t *input, uint32_t dt_ms)
{
    if (ui.mode == SAND_MODE_DETONATE) {
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

    if (ui.mode == SAND_MODE_PAINT && ui.modes[ui.brush] == BRUSH_SPAWN) {
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

static void draw_menu(const input_t *input)
{
    mu_Context *ctx = ui_context();

    ui_begin(input);

    if (ui_begin_screen(ctx, "Sand Menu",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        const int total_h = 2 * MENU_BTN_H + MENU_BTN_GAP;
        const int top      = (ui_height() - total_h) / 2;

        mu_layout_set_next(ctx,
                           ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top),
                           0);
        if (mu_button(ctx, "START")) {
            start_sim();
        }

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
    if (ui.screen == SAND_UI_MENU) {
        draw_menu(input);
        return;
    }

    if (failed) {
        gfx_clear(gfx_rgb(0x1A0C0C));
        gfx_text(20, GFX_HEIGHT / 2, "no memory for the grid", gfx_rgb(0xFF5C5C));
        return;
    }

    const unsigned actions = sand_ui_step(&ui, input);

    if (actions & SAND_UI_CLOSE_PALETTE) {
        if (actions & SAND_UI_SHOW_LABEL) {
            label_left_ms = LABEL_MS;
        }

        /* A text style is ambient context for this whole shell, not per-frame
         * state private to the panel - ui_set_text_style() stays in force for
         * every UI drawn after it until something changes it again (see
         * ui.h's own comment on why, and ui_set_button_style()'s neighbouring
         * comment for the contrast with a style that DOES reset itself).
         * Restoring UI_TEXT_PLAIN here, the moment the panel is torn down, is
         * what stops the outline from leaking into the launcher or the sand
         * boot menu the next time either draws a frame - leaving this out is
         * the obvious failure: the whole shell would come up haloed after the
         * palette had ever been opened once. THE TRANSFORM ITSELF IS NOT
         * RESTORED HERE ANY MORE This used to also reset
         * ui_set_transform(ui_transform_identity()), for what was at the time
         * the same reason as the text style: draw_palette() left the
         * transform turned, and something had to put it back before the
         * launcher or the sand boot menu drew again. That reasoning no longer
         * applies, because the transform is no longer this app's to leave
         * turned OR to put back. main.c now owns it for the whole shell (see
         * display.h and main.c's display sampling) and sets it from the
         * board's actual orientation on its own schedule, independent of
         * whether this panel happens to be open. If this app reset it to
         * identity here, it would fight the shell the moment the board was
         * genuinely held sideways: the launcher would snap upright the
         * instant the palette closed, and stay upright - wrong - until the
         * shell's own next sample corrected it, on a board that never stopped
         * being sideways. An app must not touch the shell's transform at all;
         * it only ever inherits whatever main.c has already set. */
        ui_set_text_style(UI_TEXT_PLAIN);

        sim_accumulator_q8 = 0;
        pour_accumulator_ms = 0;
        mark_sand_fully_dirty();
        return;
    }

    if (actions & SAND_UI_OPEN_PALETTE) {
        label_left_ms = 0;
    }

    if (ui.screen == SAND_UI_PALETTE) {
        const int quarter = display_shell_quarter();

        if (actions & SAND_UI_OPEN_PALETTE) {
            ui_invalidate();

            palette_drawn_quarter = quarter;
        } else if (quarter != palette_drawn_quarter) {
            /* The shell's orientation moved on since this panel was last
             * painted - the board was turned while the palette stayed open.
             * draw_palette() below paints UI_NO_BACKGROUND (see its own top
             * comment: the frozen sand showing through the grout between
             * tiles is the intended look, not a bug an opaque fill would be a
             * lazy way to paper over), so nothing ever erases a tile's OLD
             * footprint on its own. The panel is a square region placed by
             * ui_transform_quarter_turn(), which lands it somewhere different
             * on the physical screen at each quarter turn, so whatever the
             * previous footprint covered that the new one does not is still
             * sitting in the framebuffer as a ghost tile until something
             * repaints it. The sand simulation itself never rotates -
             * draw_dirty_rows() always paints in physical canvas coordinates,
             * transform or no transform - so "repaint" here just means
             * putting the frozen sand back the way it already looks, across
             * the whole canvas rather than working out exactly which pixels
             * the old footprint touched. mark_sand_fully_dirty() is the
             * identical three-step reseed SAND_UI_CLOSE_PALETTE uses above
             * for the same underlying reason (something that was covering the
             * sand moved) - see its own comment. Unlike that path, this one
             * calls draw_dirty_rows() itself, right here: there is still a
             * panel to draw on top afterward this same frame, rather than a
             * return that leaves the redraw for the next ordinary frame to
             * pick up. draw_emitter_markers() follows for the same reason the
             * ordinary running frame below always pairs it with
             * draw_dirty_rows(): the markers are drawn straight over the
             * grid's own pixels, not stored in it, so a full repaint of the
             * grid alone would erase them without this. Nothing here calls
             * advance_shine(), advance_local_depth_wake(), advance_cullet()
             * or advance_glass_phase(), or passes any of their results along
             * - the simulation is paused while the panel is open, so this
             * repaint happens only on an actual orientation change, never
             * once per frame; ticking any of the four wakes on a static
             * canvas would be paying an animation cost for a picture that
             * already looks right. */
            mark_sand_fully_dirty();
            draw_dirty_rows(false, false, false, false);
            draw_emitter_markers();
            palette_drawn_quarter = quarter;
        }

        draw_palette(input);
        return;
    }

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

    if (label_left_ms > 0) {
        label_left_ms = (dt_ms >= label_left_ms) ? 0 : (label_left_ms - dt_ms);
        memset(dirty_rows, 1, (size_t)grid_h);
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }

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

    material_set_gravity(gx, gy);

    material_shine_direction(gx, gy, &shine_ux_q8, &shine_uy_q8);

    update_local_depth_gravity(gx, gy);

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

static void sand_diagnostic_json(char *out, size_t len)
{
    snprintf(out, len, "{\"tilt_x\":%d,\"tilt_y\":%d}",
             tilt_x(&tilt), tilt_y(&tilt));
}

const app_t app_sand = {
    .name           = "Falling Sand",
    .summary        = "Tilt to steer, touch to pour",
    .enter          = sand_enter,
    .frame          = sand_frame,
    .exit           = sand_exit,
    .diagnostic_json = sand_diagnostic_json,
};

APP_REGISTER(app_sand);
