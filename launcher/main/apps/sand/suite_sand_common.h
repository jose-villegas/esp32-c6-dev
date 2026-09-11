/*=============================================================================
 * Shared fixtures and assertion helpers for the sand suite files
 * (suite_sand_*.c). Pure test infrastructure, not a suite of its own - no
 * SUITE_REGISTER here, nothing here is a test_* function.
 *
 * suite_sand.c grew past 32,000 lines before being split by topic into the
 * suite_sand_*.c files beside this one; this header is what lets them stay
 * separate translation units while still sharing the one fixture grid, the
 * one material-shorthand macros, and the handful of assertion helpers that
 * almost every test in the split calls.
 *
 * Naming this suite_sand_common.h (not e.g. sand_test_support.h) matters:
 * launcher/main/CMakeLists.txt filters anything matching suite_*.c out of
 * the release build and back in only under CONFIG_LAUNCHER_SELFTEST. A
 * differently-named file would silently link this test scaffolding into
 * release firmware instead.
 *===========================================================================*/
#pragma once

#include "material_palette.h"
#include "sand.h"

/* Big enough for every case here, small enough to write out by hand. */
#define W 8
#define H 8

/* The default fixture grid, initialised by fixture(). By far the most
 * reused fixture in the split - nearly every suite_sand_*.c file touches
 * it, directly or through a helper below. */
extern sand_t   s;
extern uint8_t  cells[W * H];

/* DIRAM on-device is one pool for .data/.bss AND the heap, so every static
 * byte here is a byte the heap never gets - sand_t is 232 B, and some of
 * this split's tests need one contiguous 41,216 B (184x224, real screen
 * size) grid the diagnostics image can't spare from a ~37-38 KiB largest
 * free DMA block (bd esp32c6-e82). Safe to union: only one fixture below is
 * ever live at a time, each test's own fixture helper re-inits it with
 * sand_init() before use, and no function mixes two members of fx, or one
 * of these with s/wide (this split's other two heavily-shared fixtures -
 * big, pool, pour, in suite_sand_motion.c, suite_sand_locality.c and
 * suite_sand_materials.c - are each reused by only one file, so they stay
 * static there instead of moving here).
 *
 * Rule for new tests: use exactly ONE member of fx. A fixture that must
 * stay alive alongside another needs its own static, local to whichever
 * one file uses it. */
typedef union {
    sand_t loc, splash_sim, crater_sim, cascade_test_sim, stir_sim,
           liq_cascade_sim, quench_sim, obst_pool, blend_pool,
           debounce_test, hdebounce_test, depth_test, shallow_pool,
           wake_test_grid, band_test_grid, flash_test_grid, shadow_test_grid,
           fizz_sim, dilute_sim, separated_dilute_sim, oil_dilute_sim,
           dilute_pour_sim, bubble_sim, sleepy_bubble_sim;
} sand_test_fx_t;
extern sand_test_fx_t fx;

/* Dirty-row tracking buffer, reused by dirty_fixture() and by any test
 * elsewhere in the split that also exercises sand_track_dirty_rows(). */
extern uint8_t dirty[H];

#define BLOCK_COLS ((W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define BLOCK_ROWS ((H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
extern uint8_t sleep_blocks[BLOCK_COLS * BLOCK_ROWS];

/* The second most-reused fixture after s/fixture() itself - every test
 * wide enough to need more than the 8x8 default, from water-levelling
 * through boiler/conduction/metal-rod tests, mallocs WIDE_W * WIDE_H bytes
 * into wide_cells and frees them once done, the same technique as every
 * other fixture in the split. */
#define WIDE_W 32
#define WIDE_H 20
extern uint8_t *wide_cells;
extern sand_t   wide;

/* The real screen size. Must match app_sand.c - duplicated rather than
 * shared because sand.h has no business knowing the screen size (see the
 * note at the top of sand.h). Used throughout the scenes/perf/blast
 * portion of the split, well beyond the "on the real grid" section it was
 * first written for. */
#define REAL_W 184
#define REAL_H 224

/* A deliberately over-long grid, for reach-cap tests that cannot share
 * `wide`. CONDUCT_REACH_TEST mirrors sand_reactions.c's own
 * CONDUCT_REACH, which is private to that file; if the two ever drift
 * apart the tests that use this stop proving anything, so keep them
 * together. */
#define CONDUCT_REACH_TEST 32
#define CAP_W (CONDUCT_REACH_TEST + 16)
#define CAP_H 8

/* Material shorthand. Variants are chosen deliberately, not just "8" -
 * see each one's own comment in suite_sand_common.c for what a careless
 * value there broke historically (stone/glass temperature, wood's burn
 * state, and so on). */
#define WATER CELL_MAKE(MAT_WATER, 8)
#define STONE CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT)
#define SAND  CELL_MAKE(MAT_SAND,  8)
#define GAS   CELL_MAKE(MAT_GAS,   8)
#define FIRE  CELL_MAKE(MAT_FIRE,  8)
#define WOOD  CELL_MAKE(MAT_WOOD,  0)
#define STEAM CELL_MAKE(MAT_STEAM, 8)
#define SMOKE CELL_MAKE(MAT_SMOKE, 8)
#define EMBER CELL_MAKE(MAT_WOOD, MATERIAL_VARIANTS - 1)
#define OIL   CELL_MAKE(MAT_OIL,  8)
#define LAVA  CELL_MAKE(MAT_LAVA, 8)
#define GLASS CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT)
#define SNOW  CELL_MAKE(MAT_SNOW,  8)

/* Resets the default fixture (s/cells) via sand_init(). */
void fixture(void);

/* Loads a picture of a grid into s/cells. Rows are given top to bottom, so
 * the text reads the way the screen looks: 'o' a grain, anything else
 * empty. */
void load(const char *rows[], int count);

/* Resets the default fixture and arms dirty-row tracking against it,
 * clearing `dirty`. */
void dirty_fixture(void);

/* Resets the default fixture, enables block-sleeping against sleep_blocks,
 * loads `rows`, then steps `steps` times under (gx, gy). */
void settle_with_sleeping(const char *rows[], int count, int steps,
                           int gx, int gy);

/* Re-runs the current s/cells state fully awake and asserts it is
 * byte-identical to what sleeping left behind - the central property every
 * sleeping test in the split checks. */
void assert_nothing_left_to_do(int gx, int gy);

/* Total AMOUNT of material `m` on grid `g` (w x h) - summed fill level,
 * not cell count. */
long mass_of(const sand_t *g, int w, int h, material_id_t m);

/* Cell count of material `m` on the default fixture. */
int count_of(material_id_t m);

/* Cell count of material `id` on the default fixture - same as count_of(),
 * kept as a separate name because that is what the acid/glass/dirt tests
 * already called it before the split. */
int count_cells_of(uint8_t id);

/* Total fill-level mass of liquid `id` on the default fixture. */
long mass_held_by(uint8_t id);
int liquid_mass_of(uint8_t id);

/* Row of the first cell holding material `id` on the default fixture, or
 * -1 if there is none. */
int first_row_holding(uint8_t id);

/* Resets the default fixture with a stone floor/ceiling spanning the full
 * width and stone walls just outside [x0, x1] - a sealed room for a gas or
 * fire test with no slack cell to drift into. */
void fire_room(int x0, int x1);

/* Resets the default fixture with a sealed stone tank holding standing
 * water, for bubble/gas-through-liquid tests. */
void water_column(void);

/* A sealed GLASS tank on the default fixture, `sand_rows` of sand in the
 * bottom and `acid_rows` of acid above it. Returns the acid mass placed. */
long acid_tank(int sand_rows, int acid_rows);

/* Rec.601 luminance (0-255) of a panel colour (GFX_RGB, byte-swapped
 * RGB565) - shared because both the cullet/tone tests and the soil-tone
 * tests it was written for need the same unpacking math. */
int panel_luminance(gfx_color_t c);
