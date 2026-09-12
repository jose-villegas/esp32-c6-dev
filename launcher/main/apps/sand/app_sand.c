/*
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "../../app.h"
#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "../../gfx/gfx_font_roles.h"
#include "../../input/imu.h"
#include "../../ui/ui.h"
#include "brush_screen.h"
#include "icons_sand.h"
#include "material_palette.h"
#include "palette.h"
#include "row_runs.h"
#include "sand.h"
#include "sand_swatch.h"
#include "sand_ui.h"
#include "tilt.h"
#include "util/intmath.h" /* im_abs(), im_len() - see
                             * update_local_depth_gravity() below, which
                             * projects gravity's own direction into the
                             * vertical/horizontal weights LOCAL DEPTH's own
                             * comment describes */

static const char* TAG = "sand";

#define COL_BACKGROUND 0x0A0C14

typedef struct {
    const char* name;
    int cell;
} quality_t;

static const quality_t qualities[] = {
    {"ULTRA", 2}, {"HIGH", 3}, {"NORMAL", 4}, {"LOW", 6}, {"VERY LOW", 8},
};
#define QUALITY_COUNT   ((int)(sizeof(qualities) / sizeof(qualities[0])))
#define QUALITY_DEFAULT 2 /* NORMAL */

static int quality = QUALITY_DEFAULT;

static int cell, grid_w, grid_h, block_cols, block_rows;

#define CELL_MIN                  2 /* finest quality; sets every allocation size */
#define GRID_W_MAX                (GFX_WIDTH / CELL_MIN)
#define GRID_H_MAX                (GFX_HEIGHT / CELL_MIN)

#define BLOCK_COLS_MAX            ((GRID_W_MAX + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define BLOCK_ROWS_MAX            ((GRID_H_MAX + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

#define MENU_BTN_W                300
#define MENU_BTN_H                UI_ROW_HEIGHT
#define MENU_BTN_GAP              20

/* Default pour brush radius, in px - seeds sand_ui_t.radius_px; the value
 * actually in force is whatever the brush screen's slider last set (see
 * sand_ui_radius()). */
#define POUR_RADIUS_PX            10

#define POUR_HZ                   60
#define POUR_STEP_MS              (1000 / POUR_HZ)

/* Default erase brush radius, in px - see POUR_RADIUS_PX's own comment. */
#define ERASE_RADIUS_PX           16

#define ERASE_EMITTER_RADIUS_PX   32

/* Default BOOM brush radius, in px - see POUR_RADIUS_PX's own comment. */
#define DETONATE_RADIUS_PX        50

#define APP_IMPULSE_MAX           2048

#define SAND_IMPULSE_BUDGET_BYTES 12288

_Static_assert((unsigned long)APP_IMPULSE_MAX * sizeof(impulse_t) <= SAND_IMPULSE_BUDGET_BYTES,
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

/* Selected from the palette panel, not cycled - a cycle's cost grows with
 * material count, a panel's doesn't. PAINT/ERASE/DETONATE now comes from
 * the brush screen's segmented control, not a PWR cycle, for the same
 * reason: a HOLD's 600ms tax is too slow for a control used this often.
 * Only paintable materials get a tile - burning wood is a STATE, not a
 * material (reaction_t.burn_decay). Whole CELLS, not ids: an extended
 * material isn't nameable by id alone (MATX() in material.h). */
static const cell_t brushes[] = {
    CELL_MAKE(MAT_SAND, 0), CELL_MAKE(MAT_WATER, 0), CELL_MAKE(MAT_STONE, 0), CELL_MAKE(MAT_GAS, 0),
    CELL_MAKE(MAT_FIRE, 0), CELL_MAKE(MAT_WOOD, 0),  CELL_MAKE(MAT_OIL, 0),   CELL_MAKE(MAT_LAVA, 0),
    CELL_MAKE(MAT_ACID, 0), CELL_MAKE(MAT_GLASS, 0), CELL_MAKE(MAT_SNOW, 0),  CELL_MAKE(MAT_DIRT, 0),
    MATX(MATX_ICE),         MATX(MATX_PLANT),        GUNPOWDER_CELL(0), /* dry, tone 0 - see brush_color()'s own comment for
                         * why the panel tile itself paints a different code */
};
#define BRUSH_COUNT ((int)(sizeof(brushes) / sizeof(brushes[0])))

_Static_assert(PALETTE_FITS(BRUSH_COUNT), "the palette panel for BRUSH_COUNT brushes is taller than the "
                                          "screen at some orientation - see palette_cols()/PALETTE_TILE "
                                          "in palette.h");

/* brush_mode_t per brush - sand_init() does not reset this, so a brush can
 * still show Water as its source with no tap present. */
static uint8_t brush_mode[BRUSH_COUNT];

static sand_ui_t ui = {
    .brushes = brushes,
    .modes = brush_mode,
    .brush_count = BRUSH_COUNT,
    /* The three values PAINT/ERASE/DETONATE already used before each mode
     * had a slider of its own, so the brush screen opens on what the app
     * has always done rather than on a fresh set of numbers. */
    .radius_px = {[SAND_MODE_PAINT] = POUR_RADIUS_PX,
                  [SAND_MODE_ERASE] = ERASE_RADIUS_PX,
                  [SAND_MODE_DETONATE] = DETONATE_RADIUS_PX},
};

/* Duration mode label stays after significant change, balancing readability
 * and non-obtrusiveness. */
#define LABEL_MS        1800

#define LABEL_MARGIN    18
#define LABEL_SCALE     2

#define SHAKE_DEADZONE  40

#define SIM_HZ          60
#define SIM_STEP_MS     (1000 / SIM_HZ)

#define SIM_MAX_CATCHUP 2

static uint8_t* grid;
static uint8_t* dirty_rows;    /* GRID_H_MAX bytes: which rows changed -
                                   * only the first grid_h are in use at any
                                   * quality below ULTRA */
static uint8_t* sleep_blocks;  /* BLOCK_COLS_MAX*BLOCK_ROWS_MAX bytes:
                                   * settled blocks to skip - see
                                   * sand_enable_sleeping() */
static impulse_t* impulse_buf; /* APP_IMPULSE_MAX entries: grains in
                                   * flight from DETONATE - see
                                   * sand_enable_impulses(). */

static uint16_t* row_run_x0;
static uint16_t* row_run_x1;
static uint8_t* row_run_n;
static sand_t sim;
static tilt_t tilt;
static bool failed;
static uint32_t label_left_ms; /* countdown for the mode label */

static bool input_ready;

/* The shell's quarter turn as of the last frame either overlay panel
 * (palette or brush) was drawn - both can be left open while the board
 * rotates, and each detects the change against this the same way. */
static int panel_drawn_quarter;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Rolling averages, purely for the log line - a release build has nobody
 * watching the serial console to read them, so it carries none of this. */
static uint32_t frames;
static int64_t step_us_total;
static int64_t draw_us_total;
static int64_t rows_redrawn_total;
static int64_t steps_total;

static int64_t pour_step_us_total, pour_draw_us_total;
static uint32_t pour_frames;
static int64_t idle_step_us_total, idle_draw_us_total;
static uint32_t idle_frames;
static int64_t split_log_at_us;

static int64_t pour_awake_total, idle_awake_total;

/* Measure occupied cells in blocks; confirms step_one_row() cost per row, not
 * unit. */
static int64_t pour_awake_cells_total, idle_awake_cells_total;
#endif
static uint32_t sim_accumulator_q8;
static uint32_t pour_accumulator_ms;

/*
 * Sensor axes to screen axes. How the QMI8658 is soldered relative to the
 * panel is a board layout fact no datasheet carries, so both facts here
 * come from tilting the board: held upright the sensor reads about +1 g on
 * its X axis and roughly zero on Y, so the chip's X runs down the screen and
 * its Y runs across it pointing left, hence the negation.
 */
#define GRAVITY_SCREEN_X(s) (-(s)->ay)
#define GRAVITY_SCREEN_Y(s) ((s)->ax)

/* Setup */

static void
sand_enter(void) {
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
static void
seed_row_runs_full_width(void) {
    for (int i = 0; i < grid_h; i++) {
        row_run_x0[i * ROW_MAX_RUNS] = 0;
        row_run_x1[i * ROW_MAX_RUNS] = (uint16_t)grid_w;
        row_run_n[i] = 1;
    }
}

static void
mark_sand_fully_dirty(void) {
    seed_row_runs_full_width();
    memset(dirty_rows, 1, (size_t)grid_h);
    gfx_mark_all_dirty();
}

#if CONFIG_LAUNCHER_SELFTEST
bool
sand_app_alloc_selfcheck(size_t* out_largest_free, bool* out_impulses_ok) {
    uint8_t* t_dirty = malloc(GRID_H_MAX);
    uint8_t* t_blocks = malloc((size_t)BLOCK_COLS_MAX * BLOCK_ROWS_MAX);
    uint8_t* t_grid = malloc((size_t)GRID_W_MAX * GRID_H_MAX);
    uint16_t* t_x0 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* t_x1 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* t_n = malloc(GRID_H_MAX * sizeof(uint8_t));
    impulse_t* t_imp = malloc((size_t)APP_IMPULSE_MAX * sizeof(impulse_t));

    const bool essential_ok = (t_dirty && t_blocks && t_grid && t_x0 && t_x1 && t_n);
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

static void
start_sim(void) {
    cell = qualities[quality].cell;
    grid_w = GFX_WIDTH / cell;
    grid_h = GFX_HEIGHT / cell;
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
    /* impulse_buf allocates LAST, deliberately: grid needs the single
     * largest contiguous heap run, so it must pick first. Reordering does
     * not create more contiguous space, only decides who gets first pick -
     * moving impulse_buf ahead of grid produced a WORSE failure (no
     * memory for the grid at all). Do not reorder without a fresh device
     * capture showing it helps. */
    if (impulse_buf == NULL) {
        impulse_buf = malloc((size_t)APP_IMPULSE_MAX * sizeof(*impulse_buf));
        /* LOUD, NOT FATAL, unlike the buffers below: sand_enable_impulses
         * (NULL, ...) safely disables just DETONATE, so failing here alone
         * shouldn't strand a player who never wanted it behind a "no
         * memory" screen. Logs largest_free_block, not total free heap -
         * total free heap tells the wrong story here (see
         * SAND_IMPULSE_BUDGET_BYTES); largest block is what actually
         * predicts whether this allocation succeeds. */
        if (impulse_buf == NULL) {
            ESP_LOGE(TAG,
                     "Could not allocate the %d-entry blast buffer "
                     "(%u bytes) - detonate will be a no-op this "
                     "session; largest free block is %u",
                     APP_IMPULSE_MAX, (unsigned)((size_t)APP_IMPULSE_MAX * sizeof(*impulse_buf)),
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
    if (grid == NULL || dirty_rows == NULL || sleep_blocks == NULL || row_run_x0 == NULL || row_run_x1 == NULL
        || row_run_n == NULL) {
        ESP_LOGE(TAG,
                 "Could not allocate a %d x %d grid (%d bytes); "
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

    /* Enabled unconditionally, not just once BOOM is selected, so an
     * allocation failure shows up at start_sim() rather than on the first
     * tap in BOOM mode. */
    sand_enable_impulses(&sim, impulse_buf, APP_IMPULSE_MAX);
    tilt_reset(&tilt, IMU_COUNTS_PER_G);

    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - falling back to fixed downward gravity");
    }

    ESP_LOGI(TAG, "%d x %d grid, %d bytes, %d px cells", grid_w, grid_h, grid_w * grid_h, cell);

    gfx_clear(material_palette()[SAND_EMPTY]);
    gfx_mark_all_dirty();

    ui.screen = SAND_UI_RUNNING;
}

static void
sand_exit(void) {
    /* Grid is kept between visits (the app's largest allocation) so
     * re-entry cannot fail to heap fragmentation from whatever ran while
     * this app was closed. */
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (frames > 0) {
        ESP_LOGI(TAG,
                 "%lu frames, %lld sim steps, step %lld us, draw %lld us, "
                 "%lld of %d rows redrawn per frame",
                 (unsigned long)frames, (long long)steps_total, (long long)(step_us_total / frames),
                 (long long)(draw_us_total / frames), (long long)(rows_redrawn_total / frames), grid_h);
    }
#endif
}

/* Drawing */

#define SHINE_PERIOD  64 /* power of two - see the mask below */
#define SHINE_STEP_MS 40
#define SHINE_STEP_PX 2

static int shine_offset;
static uint32_t shine_elapsed_ms;

static int shine_ux_q8 = 181;
static int shine_uy_q8 = 181;

static int wood_leaf_wind_ux_q8 = 256;
static int wood_leaf_wind_uy_q8;

/* The 5 gravity-relative directions material_wood_near_leaf() checks -
 * see material_wood_leaf_top5(). Recomputed once a frame, not per cell. */
static int8_t wood_leaf_top5[5][2] = {
    {0, -1}, {-1, -1}, {1, -1}, {-1, 0}, {1, 0},
};
static int wood_leaf_top5_down;

/* How many of the 5 are actually checked, out of 5 - higher reads bulkier
 * and greener since more wood cells beside foliage qualify. */
#define WOOD_LEAF_SLOTS_CHECKED       5u

/* A sweep that always travels the same way still reads as one shine, even
 * with gusts dropping out - real wind swings direction. Interval jittered
 * (material_grain_hash of the flip count, not a real RNG) so the swings
 * are not metronomic. */
#define WOOD_LEAF_WIND_FLIP_BASE_MS   1200u
#define WOOD_LEAF_WIND_FLIP_JITTER_MS 1800u

static int wood_leaf_wind_sign = 1;
static uint32_t wood_leaf_wind_flip_elapsed_ms;
static uint32_t wood_leaf_wind_flip_due_ms = WOOD_LEAF_WIND_FLIP_BASE_MS;
static unsigned wood_leaf_wind_flip_count;

/* One bit per row per feature, not five 224-byte GRID_H_MAX arrays each
 * holding a single 0/1 flag - the diagnostics build has no headroom to
 * spend on that (check_static_ram.py). */
#define ROW_FLAG_SHINE     (1u << 0)
#define ROW_FLAG_LIQUID    (1u << 1)
#define ROW_FLAG_CULLET    (1u << 2)
#define ROW_FLAG_GLASS     (1u << 3)
#define ROW_FLAG_WOOD_LEAF (1u << 4)

static uint8_t row_flags[GRID_H_MAX];

#define FOAM_BLOB_SHIFT 3

#define FOAM_PHASE_MS   90

static uint32_t foam_elapsed_ms;

#define CULLET_PHASE_MS 250

static uint32_t cullet_elapsed_ms;

/* time_ms itself runs continuously for a smooth blend, but redraw cadence
 * is separately throttled by WOOD_LEAF_WAKE_MS - dirtying every wood-near-
 * leaf row every frame defeated the dirty-row system for a whole tree, the
 * same reasoning LOCAL_DEPTH_WAKE_MS already applies to liquid depth. */
#define WOOD_LEAF_WAKE_MS 40

static uint32_t wood_leaf_time_ms;
static uint32_t wood_leaf_wake_elapsed_ms;

#define GLASS_PHASE_SHIFT 7

static int glass_last_phase;

/* A LIQUID INTERIOR'S LOCAL DEPTH: follows each puddle's own shape, not a
 * flat screen-position gradient, so an obstacle poking through a pool
 * casts a depth "shadow" along the true gravity direction. Walked along
 * the gravity ray itself (Bresenham), switching per-row/per-column
 * regime at 45 degrees: both regimes measure the same quantity, distance
 * along the ray, so the flip changes only how the count is computed,
 * never what it means, and the two sides agree exactly at the crossing
 * by construction. */

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
static uint8_t* local_depth_cur_row = local_depth_row_a;
static uint8_t* local_depth_prev_row = local_depth_row_b;

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

/* Unlike the two-walk design's own ceiling (34, raised above
 * MATERIAL_LIQUID_DEPTH_BAND's 24), this walk needs no raise: that design's
 * weight (component/len) was always <= 256, so a clamped count could
 * project BELOW the band; this walk's scale (len/dominant_axis) is always
 * >= 256, so a clamped count already projects AT LEAST the band at every
 * angle. Verified host-side against
 * test_a_saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle. */
#define LOCAL_DEPTH_COUNT_CEILING MATERIAL_LIQUID_DEPTH_BAND

static void
update_local_depth_gravity(int gx, int gy) {
    const int ax = im_abs(gx), ay = im_abs(gy);

    const int len = im_len(gx, gy);
    const bool new_vertical_dominant = (ay >= ax);
    const unsigned dom_axis = new_vertical_dominant ? (unsigned)ay : (unsigned)ax;
    local_depth_scale_q8 = (dom_axis != 0u) ? (256u * (unsigned)len) / dom_axis : 256u;
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
    const bool v_reverse_matters =
        (new_v_reverse != local_depth_v_reverse_prev) && (new_vertical_dominant || cross_row_observable);
    const bool h_reverse_matters =
        (new_h_reverse != local_depth_h_reverse_prev) && (!new_vertical_dominant || drift_observable);

    if (new_vertical_dominant != local_depth_vertical_dominant_prev || v_reverse_matters || h_reverse_matters) {
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

/* A pool's INTERIOR - the bulk of its rows - is unaffected: a row with
 * any interior cell is already gated in, rim or not. Widening the gate
 * can only ADD the handful of edge-only rows a tighter condition would
 * skip; it cannot double the marked-row count the way gating on "any
 * liquid" from scratch would if the array gated on nothing at all. */
#define LOCAL_DEPTH_WAKE_MS 120

static uint32_t local_depth_wake_elapsed_ms;

static inline void
paint_row_n(gfx_color_t* fb, const gfx_color_t* pal, int cy, const uint8_t* row, int n) {
    gfx_color_t* out = fb + (cy * n) * GFX_WIDTH;
    row_flags[cy] = 0;

    const uint8_t* above = (cy > 0) ? row - grid_w : NULL;
    const uint8_t* below = (cy < grid_h - 1) ? row + grid_w : NULL;

    const uint8_t* toward_surface = local_depth_v_reverse ? below : above;

    /* local_depth_prev_row[] checks if `toward_surface` points at it, hoisted
     * out of the cx loop for an 841-to-83 improvement. Only the
     * hold-then-commit debounce reads it, as same-material climbs trust a
     * stale count, and BOUNDARY's carry is nonsense for other rows. */
    const int local_depth_vdir = local_depth_v_reverse ? -1 : 1;
    const bool local_depth_chain_ok = (local_depth_prev_cy == cy - local_depth_vdir);

    const int hdir = local_depth_h_reverse ? -1 : 1;
    const int cx_first = local_depth_h_reverse ? grid_w - 1 : 0;
    const int cx_step = local_depth_h_reverse ? -1 : 1;

    /* THE ROW OFFSET, WITHOUT AN ACCUMULATOR: computed from `cy` alone, not
     * a running Bresenham accumulator carried across paint_row_n() calls -
     * that function only runs for DIRTY rows, so an accumulator would
     * silently skip gaps and drift out of sync. `cum(n) - cum(n - 1)`
     * (cum(n) = floor(n*minor/dominant)) gives the same drift a real march
     * would, verified by hand, with no memory of prior rows needed.
     * Meaningless (0) when horizontal-dominant or gravity has no
     * direction. */
    int local_depth_row_step = 0;
    if (local_depth_vertical_dominant && local_depth_ay > 0u) {
        const int vdir = local_depth_vdir;
        const int xsign = local_depth_h_reverse ? 1 : -1;
        const int n = (vdir > 0) ? cy : (grid_h - 1 - cy);
        const int cum_n = (int)(((long)(n) * (long)local_depth_ax) / (long)local_depth_ay);
        const int cum_n1 = (int)(((long)(n + 1) * (long)local_depth_ax) / (long)local_depth_ay);
        local_depth_row_step = xsign * (cum_n1 - cum_n);
    }

    int local_depth_herr = 0;
    const int ysign = local_depth_v_reverse ? 1 : -1;

    const unsigned cullet_first = MAT_SAND * MATERIAL_VARIANTS + SAND_CULLET_BASE;

    for (int cx_i = 0; cx_i < grid_w; cx_i++) {
        const int cx = cx_first + cx_i * cx_step;

        unsigned mask = ((cx > 0 && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT : 0u)
                        | ((cx < grid_w - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u)
                        | ((above != NULL && CELL_IS_EMPTY(above[cx])) ? MATERIAL_EDGE_UP : 0u)
                        | ((below != NULL && CELL_IS_EMPTY(below[cx])) ? MATERIAL_EDGE_DOWN : 0u);

        if ((mask & MATERIAL_EDGE_CARDINAL) != 0 && CELL_MATERIAL(row[cx]) == MAT_WATER) {
            mask |=
                ((cx > 0 && above != NULL && CELL_IS_EMPTY(above[cx - 1])) ? MATERIAL_EDGE_UP_LEFT : 0u)
                | ((cx < grid_w - 1 && above != NULL && CELL_IS_EMPTY(above[cx + 1])) ? MATERIAL_EDGE_UP_RIGHT : 0u)
                | ((cx > 0 && below != NULL && CELL_IS_EMPTY(below[cx - 1])) ? MATERIAL_EDGE_DOWN_LEFT : 0u)
                | ((cx < grid_w - 1 && below != NULL && CELL_IS_EMPTY(below[cx + 1])) ? MATERIAL_EDGE_DOWN_RIGHT : 0u);
        }

        const bool cell_is_water = CELL_MATERIAL(row[cx]) == MAT_WATER;
        const unsigned hash = cell_is_water ? material_grain_hash(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)
                                            : material_grain_hash(cx, cy);

        int step = 0;
        if (!local_depth_vertical_dominant) {
            local_depth_herr += (int)local_depth_ay;
            if (local_depth_ax > 0u && local_depth_herr >= (int)local_depth_ax) {
                local_depth_herr -= (int)local_depth_ax;
                step = ysign;
            }
        }

        const int qx = local_depth_vertical_dominant ? (cx + local_depth_row_step) : (cx - hdir);
        const bool qx_ok = (qx >= 0 && qx < grid_w);
        const bool cross_row = local_depth_vertical_dominant || (step != 0);
        const uint8_t* src_ptr = cross_row ? toward_surface : row;
        const uint8_t* src_arr = cross_row ? local_depth_prev_row : local_depth_cur_row;

        const bool here_liquid = material_of(row[cx])->kind == KIND_LIQUID;
        const bool same_material =
            here_liquid && qx_ok && src_ptr != NULL && (CELL_MATERIAL(src_ptr[qx]) == CELL_MATERIAL(row[cx]));
        const unsigned src_count = qx_ok ? src_arr[qx] : 0u;

        unsigned count;
        if (!here_liquid) {
            count = 0u;
        } else if (same_material) {
            count = src_count < LOCAL_DEPTH_COUNT_CEILING ? src_count + 1u : LOCAL_DEPTH_COUNT_CEILING;
            if (!local_depth_vertical_dominant) {
                local_depth_top_row[cx] = 255u;
            }
        } else {
            const bool committed = local_depth_vertical_dominant ? (local_depth_top_row[cx] == (uint8_t)cy)
                                                                 : (local_depth_top_row[cx] != 255u);
            if (committed) {
                count = 0u;
            } else {
                const unsigned carry = (cross_row && !local_depth_chain_ok) ? 0u : src_count;
                count = carry < LOCAL_DEPTH_COUNT_CEILING ? carry + 1u : LOCAL_DEPTH_COUNT_CEILING;
                local_depth_top_row[cx] = local_depth_vertical_dominant ? (uint8_t)cy : 0u;
            }
        }
        local_depth_cur_row[cx] = (uint8_t)count;

        const unsigned depth_raw = (count * local_depth_scale_q8) >> 8;
        const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

        const bool wood_near_leaf =
            row[cx] == CELL_MAKE(MAT_WOOD, 0)
            && material_wood_near_leaf(above, row, below, cx, grid_w, wood_leaf_top5, hash, WOOD_LEAF_SLOTS_CHECKED);

        /* Projected onto the wind axis (gravity-perpendicular, see
         * material_wood_leaf_wind_axis()), not raw `cx` - a grid column is
         * not a screen-relative direction once the device is rotated.
         * wood_leaf_wind_sign periodically reverses which way the gust
         * appears to travel - see advance_wood_leaf_wind_sign(). */
        const int wood_leaf_wind_pos =
            wood_leaf_wind_sign * ((cx * wood_leaf_wind_ux_q8 + cy * wood_leaf_wind_uy_q8) >> 8);

        /* Every leaf cell rides the same wave unconditionally - no
         * adjacency check needed, unlike wood, since being leaf already
         * means being part of the canopy. */
        const bool leaf_shading = wood_near_leaf || row[cx] == MATX(MATX_LEAF);

        /* +1: the wave's own fraction can legitimately be 0 at its trough,
         * which must still select the tint branch in material_colours(),
         * not fall through to the untinted look an untinted depth of 0
         * would. */
        const unsigned depth =
            (row[cx] == MATX(MATX_ROOT))
                ? material_root_neighbours(above, row, below, cx, grid_w)
                : (leaf_shading ? material_wood_leaf_wave(wood_leaf_time_ms, wood_leaf_wind_pos, grid_w, hash) + 1u
                                : depth_liquid);

        if (here_liquid) {
            row_flags[cy] |= ROW_FLAG_LIQUID;
        }

        if (leaf_shading) {
            row_flags[cy] |= ROW_FLAG_WOOD_LEAF;
        }

        if ((unsigned)(row[cx] - cullet_first) < SAND_CULLET_SHADES) {
            row_flags[cy] |= ROW_FLAG_CULLET;
        }

        if (CELL_MATERIAL(row[cx]) == MAT_GLASS) {
            row_flags[cy] |= ROW_FLAG_GLASS;
        }

        gfx_color_t col[3];
        const material_pattern_t pat = material_colours(row[cx], hash, mask, depth, col);
        gfx_color_t* p = out + cx * n;

        if (pat == MATERIAL_HATCHED) {
            row_flags[cy] |= ROW_FLAG_SHINE;
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
                const int along = ((shine_q8 >> 8) + shine_offset) & (SHINE_PERIOD - 1);

                p[dy * GFX_WIDTH + dx] = (along < n) ? col[2] : col[0];
            }
        }
    }

    uint8_t* local_depth_tmp = local_depth_cur_row;
    local_depth_cur_row = local_depth_prev_row;
    local_depth_prev_row = local_depth_tmp;

    local_depth_prev_cy = cy;
}

static void
paint_row(gfx_color_t* fb, const gfx_color_t* pal, int cy, const uint8_t* row) {
    switch (cell) {
        case 2: paint_row_n(fb, pal, cy, row, 2); break;
        case 3: paint_row_n(fb, pal, cy, row, 3); break;
        case 4: paint_row_n(fb, pal, cy, row, 4); break;
        case 6: paint_row_n(fb, pal, cy, row, 6); break;
        case 8: paint_row_n(fb, pal, cy, row, 8); break;
        /* Unreachable for any cell size in qualities[]; falls back to size 2 to
     * avoid out-of-bounds writes. */
        default: paint_row_n(fb, pal, cy, row, 2); break;
    }
}

static int
draw_one_row(gfx_color_t* fb, const gfx_color_t* pal, int cy, uint16_t* cur_x0, uint16_t* cur_x1) {
    const uint8_t* row = &grid[cy * grid_w];

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
static bool
advance_shine(uint32_t dt_ms) {
    shine_elapsed_ms += dt_ms;
    if (shine_elapsed_ms < SHINE_STEP_MS) {
        return false;
    }
    const uint32_t steps = shine_elapsed_ms / SHINE_STEP_MS;
    shine_elapsed_ms -= steps * SHINE_STEP_MS;
    shine_offset = (int)(((unsigned)shine_offset + steps * SHINE_STEP_PX) & (SHINE_PERIOD - 1));
    return true;
}

static unsigned cullet_phase_index;

static bool
advance_cullet(uint32_t dt_ms) {
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

static bool
advance_wood_leaf_phase(uint32_t dt_ms) {
    wood_leaf_time_ms += dt_ms;
    wood_leaf_wake_elapsed_ms += dt_ms;
    if (wood_leaf_wake_elapsed_ms < WOOD_LEAF_WAKE_MS) {
        return false;
    }
    const uint32_t steps = wood_leaf_wake_elapsed_ms / WOOD_LEAF_WAKE_MS;
    wood_leaf_wake_elapsed_ms -= steps * WOOD_LEAF_WAKE_MS;
    return true;
}

static void
advance_wood_leaf_wind_sign(uint32_t dt_ms) {
    wood_leaf_wind_flip_elapsed_ms += dt_ms;
    if (wood_leaf_wind_flip_elapsed_ms < wood_leaf_wind_flip_due_ms) {
        return;
    }
    wood_leaf_wind_flip_elapsed_ms -= wood_leaf_wind_flip_due_ms;
    wood_leaf_wind_sign = -wood_leaf_wind_sign;
    wood_leaf_wind_flip_count++;
    wood_leaf_wind_flip_due_ms =
        WOOD_LEAF_WIND_FLIP_BASE_MS
        + material_grain_hash((int)wood_leaf_wind_flip_count, 0) % WOOD_LEAF_WIND_FLIP_JITTER_MS;
}

static bool
advance_local_depth_wake(uint32_t dt_ms) {
    local_depth_wake_elapsed_ms += dt_ms;
    if (local_depth_wake_elapsed_ms < LOCAL_DEPTH_WAKE_MS) {
        return false;
    }
    const uint32_t steps = local_depth_wake_elapsed_ms / LOCAL_DEPTH_WAKE_MS;
    local_depth_wake_elapsed_ms -= steps * LOCAL_DEPTH_WAKE_MS;
    return true;
}

static int
gravity_bearing_q16(int gx, int gy) {
    const int64_t ax = gx < 0 ? -(int64_t)gx : (int64_t)gx;
    const int64_t ay = gy < 0 ? -(int64_t)gy : (int64_t)gy;
    const int64_t denom = ax + ay;
    if (denom == 0) {
        return 0; /* flat or free fall: no bearing to report */
    }
    const int64_t p_q16 = ((int64_t)gx << 16) / denom; /* -65536..65536 */
    return (int)(gy < 0 ? (p_q16 - 65536) : (65536 - p_q16));
}

static bool
advance_glass_phase(int gx, int gy) {
    const int phase = gravity_bearing_q16(gx, gy) >> GLASS_PHASE_SHIFT;
    const bool changed = phase != glass_last_phase;
    glass_last_phase = phase;
    material_set_glass_phase(phase);
    return changed;
}

static void
draw_dirty_rows(bool shine_moved, bool local_depth_woke, bool cullet_moved, bool glass_moved, bool wood_leaf_moved) {
    gfx_color_t* fb = gfx_framebuffer();

    if (shine_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_flags[cy] & ROW_FLAG_SHINE) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (local_depth_woke) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_flags[cy] & ROW_FLAG_LIQUID) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (cullet_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_flags[cy] & ROW_FLAG_CULLET) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (glass_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_flags[cy] & ROW_FLAG_GLASS) {
                dirty_rows[cy] = 1;
            }
        }
    }

    if (wood_leaf_moved) {
        for (int cy = 0; cy < grid_h; cy++) {
            if (row_flags[cy] & ROW_FLAG_WOOD_LEAF) {
                dirty_rows[cy] = 1;
            }
        }
    }

    const gfx_color_t* pal = material_palette();

#if CONFIG_LAUNCHER_DEVELOPMENT
    int redrawn = 0;
#endif

    /* Gravity-UP reverses this loop's order: cur_row[]/prev_row[]'s pointer
     * swap only means "row painted before this one" if rows are visited
     * surface-first. Reversing the loop, not the array's meaning, is safe
     * since nothing else here depends on row order - dirty_rows[cy] and
     * friends index by cy directly, and gfx_mark_dirty() only unions a
     * bounding box. */
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

        uint16_t* prev_x0 = &row_run_x0[cy * ROW_MAX_RUNS];
        uint16_t* prev_x1 = &row_run_x1[cy * ROW_MAX_RUNS];
        const int prev_n = row_run_n[cy];

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, prev_x0, prev_x1, prev_n, send_x0, send_x1);

        for (int i = 0; i < send_n; i++) {
            gfx_mark_dirty(send_x0[i] * cell, cy * cell, (send_x1[i] - send_x0[i]) * cell, cell);
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

#define EMITTER_MARKER_PX    12

static void
draw_emitter_markers(void) {
    const gfx_color_t marker = gfx_rgb(EMITTER_MARKER_COLOR);
    const int count = sand_emitter_count(&sim);

    for (int i = 0; i < count; i++) {
        int ex, ey;
        cell_t ecell;
        if (!sand_emitter_at(&sim, i, &ex, &ey, &ecell)) {
            continue; /* not expected - see sand_emitter_count()'s contract */
        }
        (void)ecell; /* the marker's colour is fixed, not the material's */

        const int mid_x = ex * cell + cell / 2;
        const int mid_y = ey * cell + cell / 2;
        const int px = mid_x - EMITTER_MARKER_PX / 2;
        const int py = mid_y - EMITTER_MARKER_PX / 2;
        gfx_fill_rect(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX, marker);
        gfx_mark_dirty(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX);
    }
}

static gfx_color_t
brush_color(cell_t c) {
    if (cell_is_gunpowder(c)) {
        return material_palette()[GUNPOWDER_CELL(2)];
    }
    return material_palette()[cell_is_extended(c) ? c : CELL_MAKE(CELL_MATERIAL(c), 13)];
}

static int
gravity_quarter_turn(int gx, int gy) {
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ay >= ax) {
        return (gy >= 0) ? 0 : 2; /* down is down : board upside down */
    }
    return (gx >= 0) ? 3 : 1; /* down is to the right : to the left */
}

/* Computes its own turn (gravity_quarter_turn()) rather than inheriting the
 * shell's, unlike draw_palette(): that one goes through microui, which the
 * shell's transform reaches; this one calls gfx_text_turned() straight onto
 * the canvas, bypassing microui entirely, the same way the sand grid itself
 * is painted. Canvas draws don't get the shell's transform for free - only
 * chrome does. */
static void
draw_mode_label(int gx, int gy) {
    char text_buf[24];
    const char* text;
    if (ui.mode == SAND_MODE_DETONATE) {
        text = "BOOM";
    } else if (ui.mode == SAND_MODE_ERASE) {
        text = "ERASE";
    } else if (ui.modes[ui.brush] == BRUSH_SPAWN) {
        snprintf(text_buf, sizeof text_buf, "%s SOURCE", material_name(brushes[ui.brush]));
        text = text_buf;
    } else {
        text = material_name(brushes[ui.brush]);
    }
    const int len = (int)strlen(text);
    const int span = len * 8 * LABEL_SCALE;
    const int tall = 8 * LABEL_SCALE;

    const int turn = gravity_quarter_turn(gx, gy);

    int x, y;
    switch (turn) {
        case 0: /* down is down */
            x = (GFX_WIDTH - span) / 2;
            y = LABEL_MARGIN;
            break;
        case 2: /* board upside down */
            x = (GFX_WIDTH + span) / 2 - 8 * LABEL_SCALE;
            y = GFX_HEIGHT - LABEL_MARGIN - tall;
            break;
        case 3: /* down is to the right */
            x = LABEL_MARGIN;
            y = (GFX_HEIGHT + span) / 2 - 8 * LABEL_SCALE;
            break;
        default: /* turn == 1: down is to the left */
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

#define PALETTE_GROUT              4

#define PALETTE_BEZEL              3

/* Eligibility/spawn corner badge: 18px outer square on 92px PALETTE_TILE
 * tile, 2px border, 2px margin. */
#define PALETTE_BADGE_SIZE         18
#define PALETTE_BADGE_INSET        2
#define PALETTE_BADGE_MARGIN       2

#define PALETTE_BADGE_BORDER_COLOR 0x141414
#define PALETTE_BADGE_FILL_COLOR   0xF2F2F2

static mu_Color
mu_color_hex(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

/* APPLY EXACTLY ONCE PER REPAINT OF WHAT IS UNDERNEATH, never per frame.
 * gfx_fill_rect_blend() mixes with the destination it reads, and the sand
 * behind a panel is frozen, so a second application lands on the first's own
 * output and the picture walks toward black a frame at a time. The backdrop
 * is only fresh the frame a panel opens and on a turn taken while it is
 * open; both call this, nothing else may. Cost rules out per frame anyway -
 * this reads all 368x448 pixels. */
#define PANEL_SCRIM_ALPHA 110

static void
dim_backdrop(void) {
    gfx_fill_rect_blend(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x000000), PANEL_SCRIM_ALPHA);
}

static void
draw_palette(const input_t* input) {
    mu_Context* ctx = ui_context();

    ui_begin(input);

    ui_set_text_style(UI_TEXT_OUTLINED);

    ui_set_button_style(UI_BUTTON_BEZEL);

    const mu_Color saved_button_color = ctx->style->colors[MU_COLOR_BUTTON];
    const mu_Color saved_text_color = ctx->style->colors[MU_COLOR_TEXT];

    /* Black used for each tile name by mu_button() and UI_TEXT_OUTLINED.
     * ui_text_halo() derives a light halo at render time, ensuring dark text
     * reads against any swatch. Unlike the face below, this colour is set
     * once here rather than per tile. */
    ctx->style->colors[MU_COLOR_TEXT] = mu_color(0, 0, 0, 255);

    const int cols = palette_cols(ui_width());

    if (ui_begin_screen(ctx, "Sand Palette", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        for (int i = 0; i < BRUSH_COUNT; i++) {
            int x, y, w, h;
            palette_tile_rect(i, BRUSH_COUNT, cols, ui_width(), ui_height(), &x, &y, &w, &h);

            const int ix = x + PALETTE_GROUT;
            const int iy = y + PALETTE_GROUT;
            const int iw = w - 2 * PALETTE_GROUT;
            const int ih = h - 2 * PALETTE_GROUT;

            const mu_Color face = mu_color_hex(gfx_color_rgb888(brush_color(brushes[i])));
            ctx->style->colors[MU_COLOR_BUTTON] = face;

            /* Button placed by mu_layout_set_next() at (ix,iy,iw,ih).
             * mu_button() hit-tests using microui's mouse state, fed via
             * feed_input() in ui.c, mapped through inverse transform. Draws
             * styled frame and centered label, the button's unique id from
             * mu_get_id() in microui.c. Each BRUSH_COUNT brush has a unique
             * name to prevent id collisions. */
            const char* name = material_name(brushes[i]);
            mu_layout_set_next(ctx, mu_rect(ix, iy, iw, ih), 0);
            const int clicked = mu_button(ctx, name);

            /* Hand click to sand_ui_tile_clicked() before drawing selection
             * ring and badge for immediate toggle or selection display in the
             * same frame. Return value unused. */
            if (clicked) {
                sand_ui_tile_clicked(&ui, i);
            }

            /* No "selected" state exists for mu_button() (selection is this
             * app's idea, not microui's), so this draws a second cue: the
             * SUNKEN edge pair, mixed toward white/black off THIS TILE'S
             * face rather than a fixed colour - keeps it visible on both
             * Snow (black-mixed edge reads) and Stone (white-mixed edge
             * reads). Safe here, unlike the badge below: it always sits
             * paired with the one face it was mixed from. */
            if (i == ui.brush) {
                ui_span_t spans[UI_BEZEL_MAX_SPANS];
                const int n = ui_bezel_spans(mu_rect(ix, iy, iw, ih), face, true, spans, UI_BEZEL_MAX_SPANS);
                for (int s = 1; s < n; s++) {
                    mu_draw_rect(ctx, spans[s].rect, spans[s].color);
                }
            }

            /* Badge shows eligibility (material_can_emit(), false for every
             * KIND_STATIC material - gunpowder is the one extended-range
             * exception, being KIND_POWDER). Border/fill are a FIXED pair,
             * not derived from the face like the bezel above - Snow's
             * near-white face would make a derived fill nearly invisible
             * against it, and a mark that must read on every swatch can't
             * itself be made of the swatch. */
            if (material_can_emit(brushes[i])) {
                const mu_Color border = mu_color_hex(PALETTE_BADGE_BORDER_COLOR);
                const mu_Color fill = mu_color_hex(PALETTE_BADGE_FILL_COLOR);
                const int bx = ix + iw - PALETTE_BEZEL - PALETTE_BADGE_MARGIN - PALETTE_BADGE_SIZE;
                const int by = iy + PALETTE_BEZEL + PALETTE_BADGE_MARGIN;
                const mu_Rect badge_rect = mu_rect(bx, by, PALETTE_BADGE_SIZE, PALETTE_BADGE_SIZE);

                mu_draw_rect(ctx, badge_rect, border);
                mu_draw_rect(ctx,
                             mu_rect(bx + PALETTE_BADGE_INSET, by + PALETTE_BADGE_INSET,
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
    ctx->style->colors[MU_COLOR_TEXT] = saved_text_color;

    ui_end(UI_NO_BACKGROUND);
}

/* Padding inside one brush-mode segment, and the gap between its icon and
 * its label - the icon square is whatever is left of the segment's height
 * after both, capped to the segment's width so a narrow canvas can't ask
 * for a wider icon than the segment actually has. */
#define BRUSH_SEG_PAD       8
#define BRUSH_SEG_LABEL_GAP 4

/* Inset from the info button's own edge to its icon - same reasoning as
 * INFO_BTN_SIDE's own comment in brush_screen.c: room so the glyph isn't
 * pressed against the button frame. */
#define BRUSH_INFO_ICON_PAD 12

static const icon_t* const brush_seg_icons[BRUSH_SCREEN_SEGMENT_COUNT] = {
    [BRUSH_SCREEN_SEG_POUR] = &icon_sand_table[ICON_SAND_POUR],
    [BRUSH_SCREEN_SEG_ERASE] = &icon_sand_table[ICON_SAND_ERASE],
    [BRUSH_SCREEN_SEG_BOOM] = &icon_sand_table[ICON_SAND_BOOM],
};

/* One panel frame (ui_style.h's flat section frame) in a fixed colour
 * pair, spans drawn back to front. */
static void
draw_brush_panel(mu_Context* ctx, mu_Rect r) {
    ui_span_t spans[UI_PANEL_MAX_SPANS];
    const int n = ui_panel_spans(r, mu_color_hex(BRUSH_PANEL_FACE_COLOR), mu_color_hex(BRUSH_PANEL_BORDER_COLOR), spans,
                                 UI_PANEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        mu_draw_rect(ctx, spans[i].rect, spans[i].color);
    }
}

/* One bezelled frame (ui_style.h's lit/shadowed control frame) in a given
 * face colour - the info button and the three mode segments use this,
 * each with its own face and `sunken`. The swatch draws its own border
 * below, via draw_brush_swatch(). */
static void
draw_brush_bezel(mu_Context* ctx, mu_Rect r, uint32_t face_rgb, bool sunken) {
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(r, mu_color_hex(face_rgb), sunken, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        mu_draw_rect(ctx, spans[i].rect, spans[i].color);
    }
}

/* Swatch side, in cells per axis. 8 divides SWATCH_SIDE (80px) into an
 * exact 10px cell and keeps the brush screen's whole command list under
 * two thirds of MU_COMMANDLIST_SIZE - see ui.c's command-list high-water
 * log (CONFIG_LAUNCHER_DEVELOPMENT) for the measured figure. */
#define BRUSH_SWATCH_CELLS 8

/* Fills `r` with sand_swatch_cell()'s deterministic pattern for `spec`,
 * then its bezel border on top (span[0] of ui_bezel_spans() is skipped -
 * the grid already fills the face that span would flatten over). */
static void
draw_brush_swatch(mu_Context* ctx, mu_Rect r, cell_t spec) {
    const gfx_color_t* palette = material_palette();

    for (int row = 0; row < BRUSH_SWATCH_CELLS; row++) {
        const int y0 = r.y + row * r.h / BRUSH_SWATCH_CELLS;
        const int y1 = r.y + (row + 1) * r.h / BRUSH_SWATCH_CELLS;
        for (int col = 0; col < BRUSH_SWATCH_CELLS; col++) {
            const int x0 = r.x + col * r.w / BRUSH_SWATCH_CELLS;
            const int x1 = r.x + (col + 1) * r.w / BRUSH_SWATCH_CELLS;
            const cell_t cell = sand_swatch_cell(spec, col, row, BRUSH_SWATCH_CELLS);
            mu_draw_rect(ctx, mu_rect(x0, y0, x1 - x0, y1 - y0), mu_color_hex(gfx_color_rgb888(palette[cell])));
        }
    }

    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n =
        ui_bezel_spans(r, mu_color_hex(gfx_color_rgb888(brush_color(spec))), false, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 1; i < n; i++) {
        mu_draw_rect(ctx, spans[i].rect, spans[i].color);
    }
}

/* `str` at the CURRENT font/scale (whatever ui_set_font_scaled() last set -
 * `scale` must agree, since it's what sizes the text vertically here),
 * vertically centred in `r`, horizontally at `align` (-1 left, 0 centre, 1
 * right). Clipped to `r`, the same guard mu_draw_control_text() gives an
 * ordinary control's label - this screen has no built-in equivalent since
 * it draws its own frames rather than going through mu_button(). */
static void
draw_brush_text(mu_Context* ctx, mu_Rect r, const char* str, mu_Color color, int scale, int align) {
    const int tw = ui_measure_text(str);
    const int th = gfx_font_height(gfx_font_ui(), scale);
    const int x = (align < 0) ? r.x : (align == 0) ? r.x + (r.w - tw) / 2 : r.x + r.w - tw;
    const int y = r.y + (r.h - th) / 2;

    mu_push_clip_rect(ctx, r);
    mu_draw_text(ctx, ctx->style->font, str, -1, mu_vec2(x, y), color);
    mu_pop_clip_rect(ctx);
}

/* Modeled on draw_palette() above - same "caller hit-tests via a real
 * control, sand_ui.c decides what the hit means" split. UI_NO_BACKGROUND
 * for the same reason too: the frozen sand shows through everything the
 * panels do not cover, so the screen reads as sitting ON the sandbox. */
static void
draw_brush_screen(const input_t* input) {
    mu_Context* ctx = ui_context();

    ui_begin(input);

    ui_set_text_style(UI_TEXT_PLAIN);

    brush_screen_layout_t lay;
    brush_screen_layout(ui_width(), ui_height(), &lay);

    if (ui_begin_screen(ctx, "Sand Brush", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        ui_set_font_scaled(gfx_font_ui(), BRUSH_SCREEN_CAPTION_SCALE);

        /* Header: swatch, caption/name, info button (drawn, inert). */
        draw_brush_panel(ctx, lay.header_panel);

        draw_brush_swatch(ctx, lay.swatch, brushes[ui.brush]);

        draw_brush_text(ctx, lay.material_caption, BRUSH_SCREEN_MATERIAL_CAPTION, mu_color_hex(BRUSH_CAPTION_COLOR),
                        BRUSH_SCREEN_CAPTION_SCALE, -1);

        /* 4 is the starting scale, but "Gunpowder" (the longest name any
         * brush carries) doesn't fit it in the name rect at the narrower
         * of the two real canvases - drop a size at a time rather than let
         * draw_brush_text()'s clip cut the tail off a real material name. */
        const char* name = material_name(brushes[ui.brush]);
        int name_scale = 4;
        for (; name_scale > 1; name_scale--) {
            ui_set_font_scaled(gfx_font_ui(), name_scale);
            if (ui_measure_text(name) <= lay.material_name.w) {
                break;
            }
        }
        draw_brush_text(ctx, lay.material_name, name, mu_color_hex(BRUSH_TEXT_COLOR), name_scale, -1);
        ui_set_font_scaled(gfx_font_ui(), BRUSH_SCREEN_CAPTION_SCALE);

        draw_brush_bezel(ctx, lay.info_button, BRUSH_SEG_UNSELECTED_COLOR, false);
        {
            /* No handler: the panel this button opens is separate, later
             * work (see docs/plans/Sand-Brush-Screen-Plan.md). Drawn now
             * because it's in the design; not a bug that tapping it does
             * nothing yet. */
            const mu_Rect icon_r = {
                lay.info_button.x + BRUSH_INFO_ICON_PAD,
                lay.info_button.y + BRUSH_INFO_ICON_PAD,
                lay.info_button.w - 2 * BRUSH_INFO_ICON_PAD,
                lay.info_button.h - 2 * BRUSH_INFO_ICON_PAD,
            };
            ui_draw_icon(ctx, icon_r, &icon_sand_table[ICON_SAND_INFO], icon_sand_rows, mu_color_hex(BRUSH_TEXT_COLOR));
        }

        /* Brush mode: caption, three segments. */
        draw_brush_panel(ctx, lay.mode_panel);
        draw_brush_text(ctx, lay.mode_caption, BRUSH_SCREEN_MODE_CAPTION, mu_color_hex(BRUSH_CAPTION_COLOR),
                        BRUSH_SCREEN_CAPTION_SCALE, -1);

        for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
            const mu_Rect r = lay.segments[i];
            const char* name = brush_screen_segment_label((brush_screen_segment_t)i);

            /* Id from the segment's own name, the same idiom mu_button_ex()
             * uses for a labelled control - the three names differ, so no
             * two segments can collide. */
            const mu_Id id = mu_get_id(ctx, name, (int)strlen(name));
            mu_update_control(ctx, id, r, 0);

            if (ctx->mouse_pressed == MU_MOUSE_LEFT && ctx->focus == id) {
                sand_ui_mode_clicked(&ui, i);
            }

            const bool selected = ((sand_mode_t)i == ui.mode);
            const bool pressed = (ctx->hover == id) || (ctx->focus == id);
            const uint32_t face = selected ? BRUSH_SEG_SELECTED_COLOR : BRUSH_SEG_UNSELECTED_COLOR;
            const mu_Color ink = mu_color_hex(selected ? BRUSH_SEG_SELECTED_INK_COLOR : BRUSH_TEXT_COLOR);

            draw_brush_bezel(ctx, r, face, pressed);

            const int label_h = gfx_font_height(gfx_font_ui(), BRUSH_SCREEN_CAPTION_SCALE);
            int icon_side = r.h - 2 * BRUSH_SEG_PAD - label_h - BRUSH_SEG_LABEL_GAP;
            const int icon_side_max = r.w - 2 * BRUSH_SEG_PAD;
            if (icon_side > icon_side_max) {
                icon_side = icon_side_max;
            }
            const mu_Rect icon_r = {
                r.x + (r.w - icon_side) / 2,
                r.y + BRUSH_SEG_PAD,
                icon_side,
                icon_side,
            };
            ui_draw_icon(ctx, icon_r, brush_seg_icons[i], icon_sand_rows, ink);

            const mu_Rect label_r = {
                r.x + BRUSH_SEG_PAD,
                icon_r.y + icon_side + BRUSH_SEG_LABEL_GAP,
                r.w - 2 * BRUSH_SEG_PAD,
                label_h,
            };
            draw_brush_text(ctx, label_r, name, ink, BRUSH_SCREEN_CAPTION_SCALE, 0);
        }

        /* Brush size: caption/value, slider. */
        draw_brush_panel(ctx, lay.size_panel);

        const char* size_caption = brush_screen_size_caption((brush_screen_segment_t)ui.mode);
        draw_brush_text(ctx, lay.size_caption, size_caption, mu_color_hex(BRUSH_CAPTION_COLOR),
                        BRUSH_SCREEN_CAPTION_SCALE, -1);

        char size_value[8];
        snprintf(size_value, sizeof size_value, "%02u PX", (unsigned)sand_ui_radius(&ui));
        draw_brush_text(ctx, lay.size_value, size_value, mu_color_hex(BRUSH_TEXT_COLOR), BRUSH_SCREEN_CAPTION_SCALE, 1);

        mu_layout_set_next(ctx, lay.slider_track, 0);
        int radius = sand_ui_radius(&ui);
        if (ui_slider_int(ctx, &radius, SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX, 1)) {
            sand_ui_set_radius(&ui, (uint8_t)radius);
        }

        mu_end_window(ctx);
    }

    ui_end(UI_NO_BACKGROUND);
}

/* Frame */

static void
read_gravity_input(uint32_t dt_ms, imu_sample_t* sample, int* gx, int* gy, int* flow, int* jostle, int* rotation) {
    *gx = 0;
    *gy = IMU_COUNTS_PER_G;
    *flow = 256;
    *jostle = 0;
    *rotation = 0;

    if (!imu_ready() || !imu_read(sample)) {
        return;
    }

    *rotation = imu_rotation_level(sample);

    tilt_update(&tilt, GRAVITY_SCREEN_X(sample), GRAVITY_SCREEN_Y(sample), sample->az, *rotation, dt_ms);

    *gx = tilt_x(&tilt);
    *gy = tilt_y(&tilt);
    *flow = tilt_strength(&tilt);

    const int shake = tilt_shake(&tilt);
    *jostle = shake > SHAKE_DEADZONE ? shake : 0;
}

static void
handle_pour_input(const input_t* input, uint32_t dt_ms) {
    if (ui.mode == SAND_MODE_DETONATE) {
        pour_accumulator_ms = 0; /* do not let held time leak into paint/erase */
        if (input->pressed) {
            const int cx = input->x / cell;
            const int cy = input->y / cell;
            sand_explode(&sim, cx, cy, (sand_ui_radius(&ui) + cell / 2) / cell);
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
                ESP_LOGW(TAG, "emitter list full (%d) - tap ignored", SAND_MAX_EMITTERS);
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
            sand_erase(&sim, cx, cy, (sand_ui_radius(&ui) + cell / 2) / cell);
            /* Wider than the sweep above on purpose - see
             * ERASE_EMITTER_RADIUS_PX's own comment for why a point target
             * needs more aiming tolerance than an area sweep does. */
            sand_remove_emitters(&sim, cx, cy, (ERASE_EMITTER_RADIUS_PX + cell / 2) / cell);
        } else {
            sand_spawn_cell(&sim, cx, cy, (sand_ui_radius(&ui) + cell / 2) / cell, brushes[ui.brush]);
        }
    }
}

/* Logs the direction whenever the NEAREST of the eight changes. Quiet when
 * the board is still, and it is what the axis mapping above was verified
 * against. The simulation itself uses the dithered direction, which changes
 * every frame by design and would be useless to log. */
static void
log_direction_change(int gx, int gy, int jostle, const imu_sample_t* sample) {
    static int last_dx = 99, last_dy = 99;
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    if (dx == last_dx && dy == last_dy) {
        return;
    }
    ESP_LOGI(TAG,
             "down is (%+d,%+d)  smoothed (%+6d,%+6d)  "
             "raw (%+6d,%+6d)  shake %d",
             dx, dy, gx, gy, sample->ax, sample->ay, jostle);
    last_dx = dx;
    last_dy = dy;
}

static void
run_sim_steps(int gx, int gy, int jostle, int flow, uint32_t dt_ms) {
    sim_accumulator_q8 += dt_ms * (uint32_t)flow;
    int steps = (int)(sim_accumulator_q8 / (SIM_STEP_MS * 256));
    if (steps > SIM_MAX_CATCHUP) {
        steps = SIM_MAX_CATCHUP;
        sim_accumulator_q8 = 0; /* give up on the backlog */
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
static int
count_occupied_in_block(int bx, int by) {
    const int x0 = bx * SAND_BLOCK_W;
    const int x1 = (x0 + SAND_BLOCK_W < grid_w) ? x0 + SAND_BLOCK_W : grid_w;
    const int y0 = by * SAND_BLOCK_H;
    const int y1 = (y0 + SAND_BLOCK_H < grid_h) ? y0 + SAND_BLOCK_H : grid_h;

    int cells = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = &grid[(size_t)y * grid_w];
        for (int x = x0; x < x1; x++) {
            if (row[x] != SAND_EMPTY) {
                cells++;
            }
        }
    }
    return cells;
}

static void
count_awake(int* out_blocks, int* out_cells) {
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
    *out_cells = cells;
}

static void
track_pour_split(const input_t* input, int64_t step_us, int64_t draw_us, int awake_blocks, int awake_cells,
                 int64_t now) {
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
        const int64_t cells = pour_awake_cells_total / pour_frames;
        ESP_LOGI(TAG,
                 "POURING:     %lu frames, step %lld us, draw %lld us, "
                 "%lld of %d blocks awake, %lld cells/awake block",
                 (unsigned long)pour_frames, (long long)(pour_step_us_total / pour_frames),
                 (long long)(pour_draw_us_total / pour_frames), (long long)blocks, block_cols * block_rows,
                 (long long)(blocks > 0 ? cells / blocks : 0));
    }
    if (idle_frames > 0) {
        const int64_t blocks = idle_awake_total / idle_frames;
        const int64_t cells = idle_awake_cells_total / idle_frames;
        ESP_LOGI(TAG,
                 "NOT POURING: %lu frames, step %lld us, draw %lld us, "
                 "%lld of %d blocks awake, %lld cells/awake block",
                 (unsigned long)idle_frames, (long long)(idle_step_us_total / idle_frames),
                 (long long)(idle_draw_us_total / idle_frames), (long long)blocks, block_cols * block_rows,
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

static void
draw_menu(const input_t* input) {
    mu_Context* ctx = ui_context();

    ui_begin(input);

    if (ui_begin_screen(ctx, "Sand Menu", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        const int total_h = 2 * MENU_BTN_H + MENU_BTN_GAP;
        const int top = (ui_height() - total_h) / 2;

        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top), 0);
        if (mu_button(ctx, "START")) {
            start_sim();
        }

        char label[24];
        snprintf(label, sizeof label, "QUALITY: %s", qualities[quality].name);

        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + MENU_BTN_H + MENU_BTN_GAP),
                           0);
        if (mu_button(ctx, label)) {
            quality = (quality + 1) % QUALITY_COUNT;
        }

        mu_end_window(ctx);
    }

    ui_end(COL_BACKGROUND);
}

static void
sand_frame(uint32_t dt_ms, const input_t* input) {
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

    if (actions & (SAND_UI_CLOSE_PALETTE | SAND_UI_CLOSE_BRUSH)) {
        if (actions & SAND_UI_SHOW_LABEL) {
            label_left_ms = LABEL_MS;
        }

        /* Restores UI_TEXT_PLAIN so the palette's outline style doesn't leak
         * into the next UI drawn (text style stays in force until changed -
         * ui.h); the brush screen only ever used PLAIN, so this is a no-op
         * on that path. Does NOT restore the transform any more: main.c now
         * owns that for the whole shell, sampling real orientation on its
         * own schedule. An app must not touch it - resetting it here would
         * fight the shell the moment the board is actually held sideways. */
        ui_set_text_style(UI_TEXT_PLAIN);

        sim_accumulator_q8 = 0;
        pour_accumulator_ms = 0;
        mark_sand_fully_dirty();
        return;
    }

    if (actions & (SAND_UI_OPEN_PALETTE | SAND_UI_OPEN_BRUSH)) {
        label_left_ms = 0;
    }

    if (ui.screen == SAND_UI_PALETTE) {
        const int quarter = display_shell_quarter();

        if (actions & SAND_UI_OPEN_PALETTE) {
            ui_invalidate();

            dim_backdrop();
            panel_drawn_quarter = quarter;
        } else if (quarter != panel_drawn_quarter) {
            /* Board turned while the palette stayed open: draw_palette()
             * paints UI_NO_BACKGROUND deliberately (frozen sand shows
             * through the grout), so the panel's old footprint - now
             * elsewhere - is left as a ghost until repainted. Full-canvas
             * repaint rather than the exact old footprint, since the sand
             * itself never rotates. draw_emitter_markers() follows since
             * markers aren't stored in the grid. No wake ticks - the sim is
             * paused, so nothing would change anyway. */
            mark_sand_fully_dirty();
            draw_dirty_rows(false, false, false, false, false);
            draw_emitter_markers();
            dim_backdrop();
            panel_drawn_quarter = quarter;
        }

        draw_palette(input);
        return;
    }

    if (ui.screen == SAND_UI_BRUSH) {
        const int quarter = display_shell_quarter();

        if (actions & SAND_UI_OPEN_BRUSH) {
            ui_invalidate();

            dim_backdrop();
            panel_drawn_quarter = quarter;
        } else if (quarter != panel_drawn_quarter) {
            /* Same reasoning as the palette's own turn-handling above: this
             * screen is opaque, but it doesn't cover the corners the sand
             * grid used to occupy at the old orientation, so those still
             * need a forced repaint. */
            mark_sand_fully_dirty();
            draw_dirty_rows(false, false, false, false, false);
            draw_emitter_markers();
            dim_backdrop();
            panel_drawn_quarter = quarter;
        }

        draw_brush_screen(input);
        return;
    }

    int gx, gy, flow, jostle, rotation;
    imu_sample_t sample = {0};
    read_gravity_input(dt_ms, &sample, &gx, &gy, &flow, &jostle, &rotation);

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

    material_wood_leaf_wind_axis(gx, gy, &wood_leaf_wind_ux_q8, &wood_leaf_wind_uy_q8);

    material_wood_leaf_top5(gx, gy, &wood_leaf_top5_down, wood_leaf_top5);

    advance_wood_leaf_wind_sign(dt_ms);

    update_local_depth_gravity(gx, gy);

    foam_elapsed_ms += dt_ms;
    material_set_foam_phase(foam_elapsed_ms / FOAM_PHASE_MS);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t1 = esp_timer_get_time();
    int awake_blocks, awake_cells;
    count_awake(&awake_blocks, &awake_cells);
#endif

    /* Local-depth wake, cullet cycle, shine, and the wood-leaf swing each
     * have their own clock tick and row array. Driven by dt_ms, not frame
     * count. Glass's wake uses gravity_bearing_q16(). */
    draw_dirty_rows(advance_shine(dt_ms), advance_local_depth_wake(dt_ms), advance_cullet(dt_ms),
                    advance_glass_phase(gx, gy), advance_wood_leaf_phase(dt_ms));

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

static void
sand_diagnostic_json(char* out, size_t len) {
    snprintf(out, len, "{\"tilt_x\":%d,\"tilt_y\":%d}", tilt_x(&tilt), tilt_y(&tilt));
}

const app_t app_sand = {
    .name = "Falling Sand",
    .summary = "Tilt to steer, touch to pour",
    .enter = sand_enter,
    .frame = sand_frame,
    .exit = sand_exit,
    .diagnostic_json = sand_diagnostic_json,
};

APP_REGISTER(app_sand);
