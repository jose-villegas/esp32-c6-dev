/*
 * Portable suite: the falling-sand automaton - frame-budget performance
 * against the shared benchmark scenes, plus the app-level allocation
 * selfcheck and a couple of full-grid acid-bubble tests.
 *
 * DEVICE_BUILD-only, almost entirely: wall-clock frame-budget assertions
 * are meaningless on a host whose CPU speed bears no relation to the
 * device's, so nearly every test below only compiles into the on-device
 * selftest image, not the host runner - see each test's own #ifdef
 * DEVICE_BUILD guard and RUN_TEST line.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h}; the scene builders these frame-budget
 * tests measure live in suite_sand_scenes.{c,h} - see those headers.
 */
#include <math.h>   /* not every file in the split still needs atan2()/M_PI,
                     * but every file inherited suite_sand.c's own include
                     * block rather than being pruned by hand, to keep the
                     * split itself mechanical and low-risk */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
/* Not every libc defines this in <math.h> without a feature-test macro this
 * file has no other reason to set (MinGW's, notably, on the host build) -
 * cheaper to supply it directly than to widen this file's own feature-test
 * exposure for one constant. */
#define M_PI 3.14159265358979323846
#endif

#include "unity.h"
#include "suites.h"

#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"
#include "suite_sand_scenes.h"
#include "material_palette.h"
#include "tilt.h"      /* TILT_TAU_*_MS - the turn below follows the real
                       * filter shape rather than a straight line */


#ifdef DEVICE_BUILD
#include <stdlib.h>
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"
#include "row_runs.h"
#include "../../gfx/gfx.h"
#define REAL_BLOCK_COLS ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define REAL_BLOCK_ROWS ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
#include "suite_sand_split.h"

/* The worst case: every cell on the screen moving at once. Cross-build
 * risk: the same code has measured a 3.2-3.9 ms swing purely from the
 * ESP32-C6's flash cache aligning differently as unrelated code shifts
 * layout - loosen this budget rather than treat one flaky run as a
 * regression if that reappears. */
#define FULL_STEP_BUDGET_US 5800
/* Every frame budget in this file targets measured * 0.9, rounded - a
 * fixed 10% demand, not a ceiling matching whatever the code costs today;
 * the measured number beside each assertion is the anchor, the target is a
 * tenth under it. Some budgets read higher than an older number because the
 * older one predates now-added features (temperature, viscosity, drag,
 * percolation) - holding it frozen would conflate a feature's real cost
 * with a regression. This one: measured 6434 us -> target 5800. */

static void test_a_full_size_step_fits_in_the_frame_budget(void)
{
    uint8_t *big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(big,
        "the real grid must fit in what the framebuffer leaves behind");

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 99u);

    /* Half full, and deliberately not settled: a grid of falling grains is the
     * expensive case, because every one of them attempts a move. A settled
     * pile is cheaper and would flatter the measurement. */
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(&real, x, y, SAND_FIRST_SHADE);
            }
        }
    }
    const int grains = sand_count(&real);

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "sand_step on %dx%d with %d grains: %lld us",
             REAL_W, REAL_H, grains, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real),
        "the full-size grid must conserve grains too");

    free(big);

    TEST_ASSERT_LESS_THAN_MESSAGE(FULL_STEP_BUDGET_US, (int)per_step,
        "the simulation no longer fits in its share of the frame");
}

#ifdef SAND_HOST_PROBE
void sand_host_probe_run_full_step_control(void)
{
    test_a_full_size_step_fits_in_the_frame_budget();
}
#endif

static void build_water_scene(sand_t *real, uint8_t *big, uint8_t *blocks)
{
    sand_init(real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(real, blocks);

    /* Half a screen of water, dropped in as an uneven slab so it is genuinely
     * flowing rather than already settled - the expensive case. */
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

static int64_t water_scene_us_per_step(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    build_water_scene(&real, big, blocks);

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    free(big);
    free(blocks);
    return per_step;
}

static void test_a_screen_of_water_fits_in_the_frame_budget(void)
{
    /* Measured separately from sand, because water takes an entirely different
     * path through the step - and the one part of it that is not local, the
     * search across the flow, runs per cell. Something has to watch that. */
    const int64_t per_step = water_scene_us_per_step();

    ESP_LOGI("device_tests", "water flowing on %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    /* Water gets a larger budget than the full-step case: it moves an amount
     * rather than a cell, and takes a second sweep across the flow (the only
     * reason a tilted pool levels at all). This is the transient cost of a
     * screen-wide collapse - water at rest is 45 us; if this cost becomes
     * sustained, argue the budget down instead of up. Re-pegged 2026-09-11,
     * perf-scoped: measured 12060 -> target 10800, from 16043 -> 14400. */
    TEST_ASSERT_LESS_THAN_MESSAGE(10800, (int)per_step,
        "a screen-wide collapse of water must still land inside a frame or "
        "two - the search across the flow is the thing to suspect");
}

#ifdef DEVICE_BUILD
/* Elapsed microseconds cannot tell a stall from an instruction, so "cycles
 * per grid load" has been read as a memory signature without evidence. These
 * registers count fetches and misses directly. The grid is heap SRAM and
 * never reaches this cache; code and flash-resident const do. Scheduler ticks
 * inside the window count too, biasing misses UP - a small reading is the
 * trustworthy direction. */
static void cache_counters_begin(void)
{
    REG_WRITE(EXTMEM_L1_CACHE_ACS_CNT_CTRL_REG,
              EXTMEM_L1_IBUS_CNT_CLR | EXTMEM_L1_DBUS_CNT_CLR);
    REG_WRITE(EXTMEM_L1_CACHE_ACS_CNT_CTRL_REG,
              EXTMEM_L1_IBUS_CNT_ENA | EXTMEM_L1_DBUS_CNT_ENA);
}

static void cache_counters_report(const char *what, int steps,
                                  uint32_t cycles)
{
    const uint32_t ihit  = REG_READ(EXTMEM_L1_IBUS_ACS_HIT_CNT_REG);
    const uint32_t imiss = REG_READ(EXTMEM_L1_IBUS_ACS_MISS_CNT_REG);
    const uint32_t dhit  = REG_READ(EXTMEM_L1_DBUS_ACS_HIT_CNT_REG);
    const uint32_t dmiss = REG_READ(EXTMEM_L1_DBUS_ACS_MISS_CNT_REG);
    REG_WRITE(EXTMEM_L1_CACHE_ACS_CNT_CTRL_REG, 0);

    ESP_LOGI("device_tests",
             "cache %s: %lu cycles/step, ibus %lu hit %lu miss, "
             "dbus %lu hit %lu miss (per step)",
             what, (unsigned long)(cycles / (uint32_t)steps),
             (unsigned long)(ihit / (uint32_t)steps),
             (unsigned long)(imiss / (uint32_t)steps),
             (unsigned long)(dhit / (uint32_t)steps),
             (unsigned long)(dmiss / (uint32_t)steps));
}

static void test_the_cache_counters_over_a_water_step(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    build_water_scene(&real, big, blocks);

    const int steps = 20;
    cache_counters_begin();
    const uint32_t c0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
    cache_counters_report("water", steps, cycles);

    free(big);
    free(blocks);
}

static void test_the_cache_counters_over_a_settled_sand_step(void)
{
    uint8_t *big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL(big);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 99u);
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(&real, x, y, SAND_FIRST_SHADE);
            }
        }
    }

    const int steps = 10;
    cache_counters_begin();
    const uint32_t c0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - c0;
    cache_counters_report("falling sand", steps, cycles);

    free(big);
}

static void build_fire_scene(sand_t *real, uint8_t *big, uint8_t *blocks)
{
    sand_init(real, big, REAL_W, REAL_H, 19u);
    sand_enable_sleeping(real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(real, x, y, FIRE);
        }
    }
}

/* FEWER WARMUP STEPS THAN WATER, deliberately: fire BURNS OUT. Ten steps of
 * warmup would time a board that has already decayed to smoke and empty, which
 * is not the expensive case anyone is trying to fix. Two keeps it alight. */
#define FIRE_WARMUP_STEPS 2
#define FIRE_REPEATS      2
#endif /* DEVICE_BUILD */


#ifdef SAND_HOST_PROBE
/* Host-only timing probe (see the full-step control's own wrapper above,
 * and main/apps/sand/tools/perf_probe/). */
void sand_host_probe_run_water(void)
{
    test_a_screen_of_water_fits_in_the_frame_budget();
}
#endif

#endif /* DEVICE_BUILD */

#ifdef DEVICE_BUILD
static void test_the_gas_random_walk_against_the_exhaustive_mover(void)
{
    int64_t best[2] = { -1, -1 };

    for (int arm = 0; arm < 2; arm++) {
        for (int r = 0; r < FIRE_REPEATS; r++) {
            uint8_t *big    = malloc(REAL_W * REAL_H);
            uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
            TEST_ASSERT_NOT_NULL(big);
            TEST_ASSERT_NOT_NULL(blocks);

            sand_t real;
            build_fire_scene(&real, big, blocks);
            for (int i = 0; i < FIRE_WARMUP_STEPS; i++) {
                sand_step(&real, 0, 1000, 0);
            }

            sand_set_gas_walk(&real, arm == 1);
            const int64_t start = esp_timer_get_time();
            sand_step(&real, 0, 1000, 0);
            const int64_t took = esp_timer_get_time() - start;

            free(big);
            free(blocks);
            if (best[arm] < 0 || took < best[arm]) {
                best[arm] = took;
            }
        }
    }

    ESP_LOGI("device_tests", "gas mover, fire scene: exhaustive %lld us",
             (long long)best[0]);
    ESP_LOGI("device_tests", "gas mover, fire scene: random walk %lld us",
             (long long)best[1]);
    if (best[0] > 0) {
        ESP_LOGI("device_tests",
                 "gas mover, fire scene: walk is %lld%% of the exhaustive cost",
                 (long long)((best[1] * 100) / best[0]));
    }
}

static void test_a_screen_of_settled_sand_costs_almost_nothing(void)
{
    /* The user-visible complaint this answers: adding lots of sand dropped the
     * framerate, even though most of it was just sitting there. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 5u);
    sand_enable_sleeping(&real, blocks);

    /* Every cell full, so nothing can move anywhere. */
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    sand_step(&real, 0, 1, 0);          /* one step to notice it is settled */

    const int64_t start = esp_timer_get_time();
    const int steps = 50;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "settled %dx%d grid: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    const int grains = sand_count(&real);
    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(REAL_W * REAL_H, grains,
        "and nothing may have moved");
    /* Re-pegged at measured * 0.9 from the first capture after the sweep
     * stopped building a per-row context for a block row it was going to
     * skip whole (esp32c6-lgc): 58 us, where the same board cost 269. */
    TEST_ASSERT_LESS_THAN_MESSAGE(52, (int)per_step,
        "sand that is not moving must cost almost nothing - if this fails, "
        "rows are being examined that had no reason to be");
}

static void test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget(void)
{
    /* Worst case pouring: all blocks wake at once. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 13u);
    sand_enable_sleeping(&real, blocks);

    /* A big pour: the middle half of the screen's width, filled from the
     * floor up to half the screen's height - wide enough to span many
     * block-columns, deliberately not the whole grid. */
    for (int y = REAL_H / 2; y < REAL_H; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    const int grains = sand_count(&real);

    /* Let it fully settle first - every block should go to sleep, the
     * same state a real pile reaches between pours. */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Flip - straight up instead of straight down. */
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gravity flip on a %d-grain pile, %dx%d: %lld us "
                             "per step", grains, REAL_W, REAL_H,
             (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real),
        "flipping gravity must conserve grains too");

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(5900, (int)per_step,
        "reversing gravity on a settled pile must still fit in a frame or "
        "two - this is the real worst case pouring and tilting produces");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the other liquid-free control (see the
 * full-step control's own wrapper for the pattern). */
void sand_host_probe_run_settled_flip_control(void)
{
    test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget();
}
#endif

/* Mass invariant for liquid scenes; water cell variant holds 1..15, diffusion
 * model adjusts amounts without changing cell count. */
static int settled_pool_total_mass(const sand_t *s, int w, int h)
{
    int total = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const cell_t c = sand_at(s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

/* A 90-degree turn is the expensive case a gravity reversal is not:
 * reversing drops the body in place, while turning sideways makes the pool
 * re-level across the full grid width, which is what the cross-flow search
 * costs the most for. Swept one step per degree because the expensive
 * frames are the mid-re-level ones. The worst step is logged alongside the
 * asserted mean, since a mean alone can hide a spike. */
static void test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    /* About 40% of the grid, full width, resting on the floor - the user's
     * own "fill the screen to about 40% with water in portrait". */
    for (int y = (REAL_H * 3) / 5; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    const int mass = settled_pool_total_mass(&real, REAL_W, REAL_H);

    /* Settle until every block sleeps - "with the water settled" is half the
     * reported condition, and a pool that is still moving would time
     * something else entirely. */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* THE TURN. */
    const int steps = 90;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 1; i <= steps; i++) {
        const int gx = (1000 * i) / steps;
        const int gy = 1000 - gx;
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, gx, gy, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "portrait->landscape turn on a settled %d-mass "
                             "pool, %dx%d: %lld us per step, worst single "
                             "step %lld us", mass, REAL_W, REAL_H,
             (long long)per_step, (long long)worst);

    const int mass_after = settled_pool_total_mass(&real, REAL_W, REAL_H);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(mass, mass_after,
        "turning the board must move water, not create or destroy it - the "
        "cell COUNT changes as the pool re-levels, the mass must not");

    /* MEASURED 18,981 us per step on device, 2026-09-11, perf-scoped.
     * Budget is that x 0.9 rounded DOWN to 17,000. The 27,100 it replaces
     * came from 30,134 measured 2026-09-10; cross-flow stopped walking
     * rows and spans it cannot draw from between the two. */

    /* THE 14000 THIS REPLACES WAS NEVER A BUDGET - it was borrowed from
     * the water screen so the row would compile, and said so. It also
     * misled a reader into reporting a 167% regression that never
     * happened, by dividing it by 0.9 as if it were pegged. */

    /* WORTH KNOWING BEFORE OPTIMISING THIS ROW: the impulse flight pass
     * never runs here at all - s->impulse_count is 0 for all 390 steps,
     * host-counted 2026-09-06 - and a host pass map puts ~48% of the cost
     * in cross-flow, ~1% reactions, ~1.5% gas. */
    TEST_ASSERT_LESS_THAN_MESSAGE(17000, (int)per_step,
        "turning the board a quarter turn with a settled pool on it must "
        "still fit in a frame or two - the pool re-levels across the whole "
        "grid width, so the cross-flow search is the thing to suspect, and "
        "a host pass map agrees at ~48%. A reduction target at measured x "
        "0.9, so failing means the work is not done yet");
}

/* Tilt shape uses exponential moving average with tau interpolating between
 * TILT_TAU_MOVING_MS and TILT_TAU_STILL_MS. Moving tau prioritizes demanding
 * case with largest gravity delta. Returns mean and worst single step. */
static int64_t time_a_quarter_turn(sand_t *real, int steps, int64_t *worst_out)
{
    const int dt_ms  = 24;
    const int tau_ms = TILT_TAU_MOVING_MS;

    int32_t gx_q8 = 0;
    int32_t gy_q8 = 1000 * 256;

    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        gx_q8 += (int32_t)(((int64_t)(1000 * 256 - gx_q8) * dt_ms) / (tau_ms + dt_ms));
        gy_q8 += (int32_t)(((int64_t)(0 - gy_q8) * dt_ms) / (tau_ms + dt_ms));

        const int64_t t0 = esp_timer_get_time();
        sand_step(real, gx_q8 / 256, gy_q8 / 256, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    *worst_out = worst;
    return per_step;
}

/* paint_row_n() is `static inline` inside app_sand.c and unreachable from
 * here, so no row in this suite exercises the app's paint path - the
 * shading could land measuring "nothing" because nothing was looking. This
 * times the per-cell work the shading adds, over the real grid, walked the
 * way paint_row_n() walks it: the scan and the wave, not the framebuffer
 * writes nor the extra paint_row() calls the gust's wake tick causes.
 * Prints; asserts no budget, since half the cost is out of reach. */
static void test_the_wood_leaf_shading_on_a_grove(void)
{
    uint8_t *big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL(big);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 3u);
    build_tree_grove_scene(&real);

    int8_t top5[5][2];
    int down = 0;
    material_wood_leaf_top5(0, 1000, &down, top5);

    int wood = 0, leaf = 0, near_leaf = 0, rows_lit = 0;
    unsigned sink = 0;

    const int64_t start = esp_timer_get_time();
    for (int rep = 0; rep < 20; rep++) {
        for (int y = 0; y < REAL_H; y++) {
            const uint8_t *row = big + (size_t)y * REAL_W;
            const uint8_t *above = (y > 0) ? row - REAL_W : NULL;
            const uint8_t *below = (y < REAL_H - 1) ? row + REAL_W : NULL;
            int lit = 0;
            for (int x = 0; x < REAL_W; x++) {
                const unsigned hash = material_grain_hash(x, y);
                const bool is_leaf = row[x] == MATX(MATX_LEAF);
                const bool tinted = is_leaf
                    || (row[x] == CELL_MAKE(MAT_WOOD, 0)
                        && material_wood_near_leaf(above, row, below, x,
                                                   REAL_W, top5, hash, 5u));
                if (tinted) {
                    sink += material_wood_leaf_wave(rep * 40u, x, REAL_W, hash);
                    lit = 1;
                }
                if (rep == 0) {
                    if (is_leaf) { leaf++; }
                    else if (row[x] == CELL_MAKE(MAT_WOOD, 0)) {
                        wood++;
                        if (tinted) { near_leaf++; }
                    }
                }
            }
            if (rep == 0) { rows_lit += lit; }
        }
    }
    const int64_t per_pass = (esp_timer_get_time() - start) / 20;

    const int64_t c0 = esp_timer_get_time();
    for (int rep = 0; rep < 20; rep++) {
        for (int y = 0; y < REAL_H; y++) {
            const uint8_t *row = big + (size_t)y * REAL_W;
            for (int x = 0; x < REAL_W; x++) {
                sink += material_grain_hash(x, y);
                sink += (row[x] == MATX(MATX_LEAF))
                     || (row[x] == CELL_MAKE(MAT_WOOD, 0));
            }
        }
    }
    const int64_t control_pass = (esp_timer_get_time() - c0) / 20;

    ESP_LOGI("device_tests",
             "wood/leaf shading on a grove: %lld us per full-grid pass, "
             "control %lld us, so the shading is %lld us "
             "(wood %d, of which %d beside a leaf; leaf %d; %d of %d rows "
             "carry foliage and so wake every gust tick) [%u]",
             (long long)per_pass, (long long)control_pass,
             (long long)(per_pass - control_pass),
             wood, near_leaf, leaf, rows_lit, REAL_H, sink & 1u);

    free(big);
}

/* Reported from play: the frame drops when water is poured onto the bed;
 * pacing is stable once plants are merely drinking from wet dirt. Same bed
 * and settle, timed over the steps immediately AFTER a fresh pour against a
 * quiet window, so the pair brackets what a player sees - the DIFFERENCE
 * between the two rows is the point, a number from either alone describes
 * half the experience. */
static void test_pouring_water_onto_a_plant_bed_costs_more_than_steady_growth(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(&real);
    for (int i = 0; i < PLANT_BED_SETTLE_STEPS; i++) {
        if (i == PLANT_BED_RAIN_A || i == PLANT_BED_RAIN_B) {
            plant_bed_rain(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 20;

    /* Steady first, from the same board the pour will start from - measuring
     * the pour first would leave the steady rows a wetter bed than the one
     * the other row times. */
    int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t steady = (esp_timer_get_time() - start) / steps;

    plant_bed_rain(&real);
    start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t poured = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests",
             "plant bed pour: steady %lld us, first %d steps after a pour "
             "%lld us (%lld us more, %lld%%)", (long long)steady, steps,
             (long long)poured, (long long)(poured - steady),
             steady > 0 ? (long long)(((poured - steady) * 100) / steady) : 0);

    free(big);
    free(blocks);
}

static void test_a_growing_plant_bed_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(&real);

    for (int i = 0; i < PLANT_BED_SETTLE_STEPS; i++) {
        if (i == PLANT_BED_RAIN_A || i == PLANT_BED_RAIN_B) {
            plant_bed_rain(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 20;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "growing plant bed, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RED ON PURPOSE, reduction target, not regression guard. Soak/dry is 28%
     * of this step. */
    TEST_ASSERT_LESS_THAN_MESSAGE(65800, (int)per_step,
        "a bed of growing plants costs three frames a step - a reduction "
        "target at measured x 0.9, so failing means the work is not done yet");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the growing plant bed (see the full-step control's
 * own wrapper for the pattern). */
void sand_host_probe_run_plant_bed(void)
{
    test_a_growing_plant_bed_fits_in_the_frame_budget();
}
#endif

static void test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);

    build_campfire_scene(&real);

    /* Let the sand settle and the fire catch, so the timed steps are a
     * burning campfire rather than a scene still falling into place. */
    for (int i = 0; i < 30; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 20;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "campfire on a sand bed, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* MEASURED 56,963 us per step on device, 2026-09-09
     * (capture_ref_5cf243b_20260909_054... ). Budget is that x 0.9 = 51,266,
     * rounded DOWN to 51,200 so the target is never looser than the
     * convention. */
    TEST_ASSERT_LESS_THAN_MESSAGE(51200, (int)per_step,
        "a small fire on a settled sand bed is the shape the app is usually "
        "in - a reduction target at measured x 0.9, so failing means the work "
        "is not done yet");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the campfire scene (see the full-step control's
 * own wrapper for the pattern). */
void sand_host_probe_run_campfire(void)
{
    test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget();
}
#endif

/* A tilted board is a different path, not a rotation of the same one:
 * equalise_gas() takes its spread direction from ring_dir(i_stable + 2), and
 * gas_run_t's carry runs only where py == 0. Packed bounds the worst case
 * and is the only shape that fires the row skip; the half-screen scene below
 * is the realistic counterpart, and the pair is the point. */
static void test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    build_smoke_and_steam_scene(&real);
    const int total = REAL_W * REAL_H;

    int64_t worst = 0;
    const int64_t per_step = time_a_quarter_turn(&real, 24, &worst);

    ESP_LOGI("device_tests", "quarter turn on a PACKED screen of gas, %dx%d: "
                             "%lld us per step, worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    /* Read before the frees, asserted after - Unity longjmps out of a failing
     * assert, so an assert ahead of free() would leak ~41 KB on this device's
     * no-PSRAM heap. */
    const int count = sand_count(&real);

    free(big);
    free(blocks);

    /* Same condensation caveat as the smoke-and-steam row above, and more
     * of it: a turning board keeps stirring steam into fresh 2x2 patches,
     * so the loss is larger here and varies run to run. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - total / 8, count,
        "turning the board must not empty it - steam condensing into water "
        "loses three cells a patch, but a packed screen that has shed an "
        "eighth of itself is not the scene this row means to time");

    TEST_ASSERT_LESS_THAN_MESSAGE(128800, (int)per_step,
        "a quarter turn on a fully packed screen of gas is the worst case the "
        "gas passes can be handed - a reduction target at measured x 0.9, so "
        "failing means the work is not done yet");
}

static void test_turning_a_half_screen_of_gas_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    /* 40% of the grid, full width, against the ceiling - where gas ends up. */
    for (int y = 0; y < (REAL_H * 2) / 5; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_GAS, 0));
        }
    }

    /* Settle first: the turn should start from a body at rest, not from a
     * field still finding its own shape. */
    for (int i = 0; i < 60; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int before = sand_count(&real);

    int64_t worst = 0;
    const int64_t per_step = time_a_quarter_turn(&real, 24, &worst);

    ESP_LOGI("device_tests", "quarter turn on a HALF screen of gas, %dx%d: "
                             "%lld us per step, worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    const int after = sand_count(&real);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, after,
        "turning the board must move gas, not create or destroy it - decay is "
        "off by default, so the cell count is conserved across the turn");

    TEST_ASSERT_LESS_THAN_MESSAGE(46100, (int)per_step,
        "a quarter turn on a settled half screen of gas is the realistic "
        "tilted case - a reduction target at measured x 0.9, so failing "
        "means the work is not done yet");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe (see the full-step control's own wrapper for the
 * pattern). */
void sand_host_probe_run_settled_pool_to_landscape(void)
{
    test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget();
}
#endif

static void test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    const int sand_x1  = (REAL_W * 3) / 10;          /* ~30% from the left */
    const int water_x0 = REAL_W - (REAL_W * 3) / 10; /* ~30% from the right */

    for (int y = REAL_H / 2; y < REAL_H; y++) {
        for (int x = 0; x < sand_x1; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
        for (int x = water_x0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    const int mid_w = water_x0 - sand_x1;
    for (int y = 0; y < REAL_H; y++) {
        const int off = (y * (mid_w - 1)) / (REAL_H - 1);
        const int xa = sand_x1 + off;
        const int xb = water_x0 - 1 - off;
        const int xa2 = (xa + 1 < water_x0) ? xa + 1 : xa;
        const int xb2 = (xb - 1 >= sand_x1) ? xb - 1 : xb;
        sand_set(&real, xa,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&real, xa2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&real, xb,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&real, xb2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }

    /* Let it fully settle first - same starting state a real pour-then-
     * pause reaches, stone included (it was never moving, but the pass
     * still has to notice that). */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Flip - straight up instead of straight down. */
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gravity flip on a mixed sand/water/stone-X "
                             "scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    /* No grain-conservation check. Water's model can spread mass across
     * cells, so sand_count() legitimately changes; test_a_screen_of_water_
     * fits_in_the_frame_budget skips this same check for the same reason.
     * Asserting it here once leaked ~41 KB - the failure's longjmp skipped
     * the frees below it. */

    free(big);
    free(blocks);

    /* A deliberate reduction target from the day it was written (12000
     * against a then-measured 15144), never headroom. Re-pegged
     * 2026-09-11, perf-scoped: measured 9311 -> target 8300, from the
     * 12999 -> 11700 that had stopped asking for anything. */
    TEST_ASSERT_LESS_THAN_MESSAGE(8300, (int)per_step,
        "reversing gravity over a mixed sand/water/stone scene should come "
        "down to this - a target to optimize toward, not yet the reality");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe (see the full-step control's own wrapper for the
 * pattern). */
void sand_host_probe_run_mixed_flip(void)
{
    test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget();
}
#endif

/* Board banded with every material, reactive pairs touch, gravity inverted.
 * Catches combination costs. THE ASSERTION BELOW IS NOT A BUDGET. Replace
 * with real figure from `run_device_tests.sh`. */
static void test_a_gravity_flip_on_every_material_at_once_stays_sane(void)
{
    uint8_t   *big      = malloc(REAL_W * REAL_H);
    uint8_t   *blocks   = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t *impulses = malloc((size_t)ALL_PAIRS_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    /* Without this, sand_explode() has nowhere to write and the gunpowder
     * patches below can never detonate - see ALL_PAIRS_IMPULSE_MAX. */
    sand_enable_impulses(&real, impulses, ALL_PAIRS_IMPULSE_MAX);


    /* build_all_pairs_scene() (suite_sand_scenes.c) also plants the
     * deliberate gunpowder patches - the tiling alone scatters gunpowder
     * as one cell in nineteen, never enough to form the fuse's 2x2. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, ALL_PAIRS_SPAWN_COUNT,
        "the pattern below needs at least two materials to interleave");

    build_all_pairs_scene(&real);

    /* Let it get going - long enough for the reactions to be under way and
     * the liquids to have found their levels, so the flip lands on a live
     * scene rather than a freshly painted one. */
    for (int i = 0; i < 120; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gravity flip with every material at once, "
                             "%dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);
    free(impulses);

    /* THE 87800 BUDGET IS INVALIDATED, NOT CARRIED FORWARD: it was
     * measured against the fourteen-material scene, and this scene is
     * now bigger. */

    /* MEASURED 90,713 us per step, 2026-09-10 - this scene's first clean
     * capture, which is what the 200000 sanity ceiling before it was
     * waiting for. Budget is that x 0.9 rounded DOWN to 81,600. */
    TEST_ASSERT_LESS_THAN_MESSAGE(81600, (int)per_step,
        "flipping gravity on every material at once, including the "
        "extended statics and gunpowder, is held to 10% below its first "
        "measured number as a reduction target - failing means the work "
        "is not done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the every-material flip scene (see the
 * full-step control's own wrapper for the pattern). */
void sand_host_probe_run_every_material_flip(void)
{
    test_a_gravity_flip_on_every_material_at_once_stays_sane();
}
#endif

static void test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }
    sand_set(&real, 0, 0, FIRE);
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    sand_step(&real, 0, 1000, 0);
    const int64_t elapsed = esp_timer_get_time() - start;

    ESP_LOGI("device_tests", "fire cascading through a full %dx%d screen of "
                             "gas: %lld us for the one step",
             REAL_W, REAL_H, (long long)elapsed);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(&real),
        "setup: cells must only ever convert material, never appear or "
        "vanish, across gas igniting into fire");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE,
        CELL_MATERIAL(sand_at(&real, REAL_W - 1, REAL_H - 1)),
        "setup: the cascade must have reached the far corner - the whole "
        "grid must have ignited in this one step, or this is not "
        "actually measuring the worst case it claims to");

    free(big);
    free(blocks);

    /* A DELIBERATELY SYNTHETIC WORST CASE: not held to plain-material
     * budgets. Failing by design, not moving goalposts. */
    TEST_ASSERT_LESS_THAN_MESSAGE(222700, (int)elapsed,
        "a full-screen cascade must stay in the same ballpark as measured "
        "- a jump here means something got much more expensive, not that "
        "this specific number is a real-time requirement");
}

static void test_a_full_screen_of_fire_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 19u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, FIRE);
        }
    }
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "full %dx%d screen already fire, steady "
                             "state: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(&real),
        "setup: a fully packed screen of same-density fire cannot "
        "displace, ignite, or smother anything - the count must not "
        "drift");

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(87700, (int)per_step,
        "steady-state cost of a full screen of fire must stay in the "
        "same ballpark as measured - not a real-time promise, but a "
        "real regression guard");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the full_fire scene (see the full-step control's
 * own wrapper for the pattern). */
void sand_host_probe_run_full_fire(void)
{
    test_a_full_screen_of_fire_fits_in_the_frame_budget();
}
#endif

/* Four liquids of different density painted upside down
 * (build_four_liquid_scene(), shared with test_the_four_liquid_scene_
 * keeps_reacting_after_settling) so lava, acid, water and oil migrate past
 * each other the whole window instead of settling into inert bands. Also
 * the only benchmark here, besides the gravity-flip test above, running at
 * sand_set_mobility(SAND_MOBILITY_PER_MATERIAL) - the setting app_sand.c
 * itself uses - so this holds the app's own liquid path to any real
 * ceiling. */
static void test_four_liquids_reacting_at_once_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_four_liquid_scene(&real);

    /* Settle first - the same "let it get going" step as the every-material
     * flip test above, so the measured window lands on a live scene. */
    for (int i = 0; i < 10; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "four liquids reacting at once, %dx%d: %lld "
                             "us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RE-PEGGED 2026-09-11, perf-scoped: 86,920 us measured, inside the
     * 89,200 it carried, so that number had stopped being a target.
     * x 0.9 rounded DOWN -> 78,200. */
    TEST_ASSERT_LESS_THAN_MESSAGE(78200, (int)per_step,
        "four liquids reacting under the app's own per-material mobility "
        "is held to 10% below its last measured number, as a reduction "
        "target - failing means the work is not done, not that something "
        "broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the four-liquids scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_four_liquids(void)
{
    test_four_liquids_reacting_at_once_fits_in_the_frame_budget();
}
#endif

static void test_the_lava_stress_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_lava_stress_scene(&real);

    for (int i = 0; i < 30; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "lava stress scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RE-PEGGED 2026-09-11, perf-scoped: 106,354 us measured, inside the
     * 109,000 it carried. x 0.9 rounded DOWN -> 95,700. */
    TEST_ASSERT_LESS_THAN_MESSAGE(95700, (int)per_step,
        "the lava stress scene is held to 10% below its last measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the lava stress scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_lava_stress(void)
{
    test_the_lava_stress_scene_fits_in_the_frame_budget();
}
#endif

static void test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    build_smoke_and_steam_scene(&real);
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "screen of smoke and steam, %dx%d: %lld us "
                             "per step",
             REAL_W, REAL_H, (long long)per_step);

    /* Read before the frees below, asserted after - the same fix
     * test_a_gravity_flip_on_every_material_at_once_stays_sane documents:
     * Unity longjmps out of a failing assert, so an assert ahead of
     * free() would skip it and leak ~41 KB on this device's no-PSRAM
     * heap. */
    const int count = sand_count(&real);

    free(big);
    free(blocks);

    /* host twin forces off with sand_set_condenses() due to budget pegged
     * with condensation running. Screen did not quietly empty into unmeasured
     * state. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - total / 16, count,
        "setup: a screen of smoke and steam must still be essentially full "
        "at the end of the window - steam condensing into water loses three "
        "cells a patch, but losing an appreciable fraction of the board "
        "means it decayed into something else");
    /* RE-PEGGED 2026-09-10: 115,178 us measured, inside the 127000 it
     * carried. x 0.9 rounded DOWN -> 103,600. */
    TEST_ASSERT_LESS_THAN_MESSAGE(103600, (int)per_step,
        "a full screen of smoke and steam is held to 10% below its last "
        "measured number, as a reduction target - failing means the work "
        "is not done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the smoke+steam scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_smoke_and_steam(void)
{
    test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget();
}
#endif

/* 480 glass compartments (build_thermal_shock_scene(), shared with
 * test_the_thermal_shock_scene_shatters_in_both_directions). No settling
 * steps: every ring starts strictly between the two shock thresholds and
 * touching from step 1, so the lattice is already at its most active the
 * moment it's painted. Clean measurement 98738 us -> target 89000
 * (file-wide 0.9 rule, see FULL_STEP_BUDGET_US's comment). */
static void test_the_thermal_shock_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    /* Step count is fixed at 10 by the host guard beside this test (its own
     * comment covers the cullet timeline); the ceiling is chosen against
     * the device's 5-second task watchdog at that fixed count - raising the
     * count without minding the ceiling needs re-doing the bet. */

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_thermal_shock_scene(&real);

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "thermal shock lattice, %dx%d: %lld us per "
                             "step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(89000, (int)per_step,
        "the thermal shock lattice is held to 10% below its first "
        "measured number, as a reduction target - failing means the work "
        "is not done, not that something broke");
}

static void test_the_boiler_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 43u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_boiler_scene(&real);

    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 30;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "boiler scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RE-PEGGED 2026-09-11, perf-scoped: 28,125 us measured, inside the
     * 28,500 it carried. x 0.9 rounded DOWN -> 25,300. */
    TEST_ASSERT_LESS_THAN_MESSAGE(25300, (int)per_step,
        "the boiler scene is held to 10% below its last measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the boiler (see the full-step control's own
 * wrapper for the pattern). */
void sand_host_probe_run_boiler(void)
{
    test_the_boiler_scene_fits_in_the_frame_budget();
}
#endif

/* Sand and dirt poured in equal amounts, water dropped over both until
 * it settles (build_wet_earth_scene(), shared with test_the_wet_earth_
 * scene_keeps_percolating_across_the_window). First benchmark to put
 * sustained load through sand_step_reactions() via moisture rather than
 * fire/heat (see the builder's own comment on may_have_moisture). 35
 * settle steps then 30 measured, matching the host test's own window (see
 * that test's comment for why 35). */
static void test_the_wet_earth_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    /* Measured 100367 us/30 steps -> target 80000 (measured * 0.8, NOT this
     * file's usual * 0.9 - an explicit instruction for this benchmark, not
     * an inconsistency to fix). */

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 53u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_wet_earth_scene(&real);

    for (int i = 0; i < 35; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 30;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "wet earth scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(80000, (int)per_step,
        "wet earth is held to measured x 0.8 - a deliberately tighter "
        "reduction target than the rest of the file's x 0.9, set by "
        "explicit instruction - so failing means the work is not done, "
        "not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the wet earth scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_wet_earth(void)
{
    test_the_wet_earth_scene_fits_in_the_frame_budget();
}
#endif

/* The water-over-lava scene from this file's own section above, run as a
 * frame-budget test. TWENTY STEPS, NO SETTLING - matching test_the_water_
 * over_lava_scene_reaches_the_quench_cooloff_and_burst_paths_it_claims
 * exactly, so what this times is a scene already proven to reach quench,
 * cool_off_chain() and the burst gate. No settling step: the scene is
 * already at its busiest the instant it's painted (the whole seam touching
 * for the first time). */
static void test_the_water_over_lava_scene_fits_in_the_frame_budget(void)
{
    uint8_t   *big      = malloc((size_t)REAL_W * REAL_H);
    uint8_t   *blocks   = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t *impulses = malloc((size_t)WATER_LAVA_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 59u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, WATER_LAVA_IMPULSE_MAX);

    build_water_over_lava_scene(&real);

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "water over lava scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);
    free(impulses);

    /* MEASURED 199,311 us per step, 2026-09-10 - this row's first real
     * device number, replacing the provisional ceiling it carried. Budget
     * is that x 0.9 rounded DOWN to 179,300. */
    TEST_ASSERT_LESS_THAN_MESSAGE(179300, (int)per_step,
        "water poured onto lava is held to 10% below its first measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the water-over-lava scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_water_over_lava(void)
{
    test_the_water_over_lava_scene_fits_in_the_frame_budget();
}
#endif

/* The gunpowder basin scene (build_gunpowder_basin_scene(),
 * suite_sand_scenes.c), shared with the coverage test that proves the
 * chain-detonation really spans several bursts and reaches fuel
 * outside the vessel. Closes half of bd esp32c6-4d9. */

/* NINETY STEPS, NO SETTLING - matching the coverage test exactly, so
 * this times the same run already proved to reach every path it
 * claims to. See GUNPOWDER_BASIN_MEASURED_STEPS's own comment
 * (suite_sand_scenes.c) for the timeline that window came from. */

/* MEASURED 31,399 us per step on device, 2026-09-06, first clean run of
 * this row (capture_ref_gunpowder-basin-benchmark_20260906_213221.md).
 * Budget is that x 0.9 = 28,259, rounded DOWN to 28,200 so the target is
 * never looser than the convention. */

/* SO THIS ROW FAILS BY DESIGN, like every other budget in this section:
 * a reduction target, not a regression guard. Re-peg only from a fresh
 * capture, never to make it green. */
static void test_the_gunpowder_basin_scene_fits_in_the_frame_budget(void)
{
    uint8_t   *big      = malloc((size_t)REAL_W * REAL_H);
    uint8_t   *blocks   = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t *impulses = malloc((size_t)GUNPOWDER_BASIN_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 61u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, GUNPOWDER_BASIN_IMPULSE_MAX);

    build_gunpowder_basin_scene(&real);

    const int64_t start = esp_timer_get_time();
    const int steps = GUNPOWDER_BASIN_MEASURED_STEPS;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gunpowder basin scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_LESS_THAN_MESSAGE(28200, (int)per_step,
        "a chain detonation in a brush-drawn stone vessel, with the "
        "aftermath reaching fuel outside it, should cost less per step "
        "than the 31,399us first measured on 2026-09-06 - this is a "
        "reduction target at measured x 0.9, so failing means the work "
        "is not done yet, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the gunpowder basin scene (see the
 * full-step control's own wrapper for the pattern). */
void sand_host_probe_run_gunpowder_basin(void)
{
    test_the_gunpowder_basin_scene_fits_in_the_frame_budget();
}
#endif

/* --- the interaction round's three scenes -------------------------------
 *
 * Picked from a 380-pairing arena rather than from the shape of the board.
 * Each builder (suite_sand_scenes.c) carries the measurement that earned it
 * a row, and the coverage test beside it proves the scene does that inside
 * the window timed here. */

/* Measured 83,173 / 16,077 / 63,371 us per step, perf-scoped, pegged at that
 * x 0.9 rounded DOWN - so all three ship RED, a reduction target rather than
 * a guard, as every row here was first set. The host ranked all three right
 * and priced none: 137x, 177x, 176x against the 179-214x its comparable rows
 * predicted. */
#define PLANT_RUIN_BUDGET_US     74800
#define FILLING_BASIN_BUDGET_US  14400
#define SNOWFALL_BUDGET_US       57000

/* 84,706 us a step, perf-scoped, pegged at that x 0.9 rounded down like the
 * three above - the third dearest scene in the suite, behind water over
 * lava and a packed screen of gas. */
#define PLANT_POUR_BUDGET_US     76200

/* 60 us, pegged the same way, and the number worth writing down: the same
 * board cost 28,362 before a landed plant stopped arming the reaction pass
 * (see may_have_faller/faller_may_move in sand.h). What is left is the
 * sweep's own block scan, which the settled-sand row measures too. */
#define PLANT_IDLE_BUDGET_US     54

/* THE ONE ROW HERE WITH NO DEVICE CAPTURE BEHIND IT: another round held the
 * board. Ranked, not priced - 138 us on the host against the growing bed's
 * 420 for the same board, applied to that row's device figure, then the
 * file-wide x 0.9. Replace it with a capture rather than trusting it. */
#define MATURE_TREE_BUDGET_US    21600

/* A grown plant bed with acid eating down to its roots on one side of a wall
 * and lava burning its canopy on the other (build_plant_ruin_scene(), shared
 * with test_the_plant_ruin_scene_eats_roots_and_burns_a_canopy). The acid
 * leads the lava by PLANT_RUIN_ACID_LEAD_STEPS because the two do not peak
 * together - see that constant. */
static void test_the_plant_ruin_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_plant_ruin_scene(&real);
    for (int i = 0; i < PLANT_BED_SETTLE_STEPS; i++) {
        if (i == PLANT_BED_RAIN_A || i == PLANT_BED_RAIN_B) {
            plant_bed_rain(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }
    for (int i = 0; i < PLANT_RUIN_ACID_LEAD_STEPS; i++) {
        if (i % PLANT_RUIN_ACID_EVERY == 0) {
            plant_ruin_acid_pour(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }
    plant_ruin_lava_pour(&real);

    const int steps = PLANT_RUIN_MEASURED_STEPS;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (i % PLANT_RUIN_ACID_EVERY == 0) {
            plant_ruin_acid_pour(&real);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "plant ruin scene, %dx%d: %lld us per step, "
                             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    /* THE INTERACTION IS THE FINDING: the same bed, grown the same way, is
     * 68,076 us a step while it is merely drinking rain and 83,173 once acid
     * and lava arrive - 22% for the pours alone. */
    TEST_ASSERT_LESS_THAN_MESSAGE(PLANT_RUIN_BUDGET_US, (int)per_step,
        "the plant family meeting acid and lava is held to 10% below its "
        "first measured number, as a reduction target - failing means the "
        "work is not done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the plant ruin scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_plant_ruin(void)
{
    test_the_plant_ruin_scene_fits_in_the_frame_budget();
}
#endif

/* Water running down a ramp into a pool (build_filling_basin_scene(), shared
 * with test_the_filling_basin_scene_runs_from_the_lip_to_the_pool) - the
 * companion to the free-falling slab above, on the same board and with a
 * comparable body of water, but settling rather than dropping into vacuum.
 * The slab row is deliberately left exactly as it was: PRs #174 and #175
 * quote its numbers, and redefining it would invalidate that history. */
static void test_the_filling_basin_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_filling_basin_scene(&real);
    for (int i = 0; i < FILLING_BASIN_SETTLE_STEPS; i++) {
        if (i % FILLING_BASIN_POUR_EVERY == 0) {
            filling_basin_pour(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = FILLING_BASIN_MEASURED_STEPS;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (i % FILLING_BASIN_POUR_EVERY == 0) {
            filling_basin_pour(&real);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "filling basin scene, %dx%d: %lld us per step, "
                             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    /* WHAT THE PAIR SAYS, and it is the reason this row exists: the slab row
     * above measured 12,060 us a step in the same capture, this one 16,077.
     * A third more for the same board of water, purely for settling rather
     * than dropping into vacuum - so the row the water work is tuned on is
     * the cheaper of the two cases by 33%. */
    TEST_ASSERT_LESS_THAN_MESSAGE(FILLING_BASIN_BUDGET_US, (int)per_step,
        "water running into a pool is held to 10% below its first measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the filling basin scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_filling_basin(void)
{
    test_the_filling_basin_scene_fits_in_the_frame_budget();
}
#endif

/* Snow falling onto a bank that has already crusted, over sand and dirt
 * (build_snowfall_scene(), shared with test_the_snowfall_scene_holds_a_
 * crusting_bank_and_a_live_fall). Forced crust - see the builder's own
 * declaration for why a scene left at the shipped rate holds no ice at all
 * inside any window this file times. */
static void test_the_snowfall_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_set_crust(&real, CRUST_ROLL_MAX);

    build_snowfall_scene(&real);
    for (int i = 0; i < SNOWFALL_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = SNOWFALL_MEASURED_STEPS;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (i % SNOWFALL_DRIFT_EVERY == 0) {
            snowfall_drift(&real);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "snowfall scene, %dx%d: %lld us per step, "
                             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    /* 63,371 us a step from a material that had no scene at all: about what
     * a growing plant bed costs, and dearer than a campfire. */
    TEST_ASSERT_LESS_THAN_MESSAGE(SNOWFALL_BUDGET_US, (int)per_step,
        "snow on earth is held to 10% below its first measured number, as "
        "a reduction target - failing means the work is not done, not that "
        "something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the snowfall scene (see the full-step control's
 * own wrapper for the pattern). */
void sand_host_probe_run_snowfall(void)
{
    test_the_snowfall_scene_fits_in_the_frame_budget();
}
#endif

/* The plant brush poured onto damp earth (build_plant_pour_scene()), which no
 * other row reaches: every plant scene here grows a garden, and a grown tree
 * is anchored, so its support walk returns on the first neighbour.
 *
 * Timed from the first stamp rather than after a settle - a settled heap is
 * the plant bed row over again. */
static void test_pouring_the_plant_brush_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_pour_scene(&real);

    for (int i = 0; i < PLANT_POUR_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = PLANT_POUR_MEASURED_STEPS;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        plant_pour_stamp(&real, i);
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "plant pour, %dx%d: %lld us per step, "
                             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(PLANT_POUR_BUDGET_US, (int)per_step,
        "pouring plants is held to 10% below its first measured number, as "
        "a reduction target - failing means the work is not done, not that "
        "something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the plant pour (see the full-step control's own
 * wrapper for the pattern). */
void sand_host_probe_run_plant_pour(void)
{
    test_pouring_the_plant_brush_fits_in_the_frame_budget();
}
#endif

/* The same heap once it has stopped: the state a poured garden spends almost
 * all of its life in, and the one no other row measures. Every plant here is
 * landed or anchored, so the reaction pass has nothing it can do and the
 * number is whatever it costs to find that out. */
static void test_a_settled_plant_garden_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_dry_plant_heap_scene(&real);

    for (int i = 0; i < PLANT_POUR_MEASURED_STEPS; i++) {
        plant_pour_stamp(&real, i);
        sand_step(&real, 0, 1000, 0);
    }
    for (int i = 0; i < PLANT_IDLE_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 200;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "settled plant garden, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(PLANT_IDLE_BUDGET_US, (int)per_step,
        "a garden that has stopped moving is held to 10% below its measured "
        "number, as a reduction target - failing means the work is not done, "
        "not that something broke");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the settled garden (see the full-step control's
 * own wrapper for the pattern). */
void sand_host_probe_run_plant_idle(void)
{
    test_a_settled_plant_garden_fits_in_the_frame_budget();
}
#endif

/* The maintainer's own case: a tree grown from seed on damp earth, with wood,
 * leaves and a root system, left until it has both stopped growing and drunk
 * the ground dry. Every other plant row here is chosen for something still
 * happening in it; this one is chosen for nothing happening, because that is
 * what a garden does for all but the first few hundred steps of its life. */
static void test_a_finished_tree_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(&real);

    for (int i = 0; i < MATURE_TREE_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 200;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "finished tree, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(MATURE_TREE_BUDGET_US, (int)per_step,
        "a tree that has stopped growing is held to a host-RANKED number, "
        "not a measured one - see MATURE_TREE_BUDGET_US, which wants a "
        "device capture behind it before either outcome means much");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the finished tree (see the full-step control's own
 * wrapper for the pattern). */
void sand_host_probe_run_mature_tree(void)
{
    test_a_finished_tree_fits_in_the_frame_budget();
}
#endif

/* --- gfx_present() cost against real sand scenes ------------------------
 *
 * Every frame-budget test above times sand_step() alone, with no drawing
 * involved - nothing has measured what gfx_present() actually costs against
 * the dirty pattern a real sand scene leaves (see suite_gfx.c for
 * synthetic-mark numbers only). These tests close that gap: build a real
 * scene, step it, reproduce app_sand.c's own marking policy, then time
 * gfx_present() on the result. */

/* REAL_W*REAL_CELL_PX == GFX_WIDTH and REAL_H*REAL_CELL_PX == GFX_HEIGHT -
 * REAL_W/REAL_H are the grid size at cell=2, the finest ("ULTRA") quality
 * tier in app_sand.c's qualities[] table, which is what the pixel math in
 * mirror_app_sand_marking()'s gfx_mark_dirty() calls has to agree with. */
#define REAL_CELL_PX 2
/* REPRODUCING, NOT CALLING: draw_dirty_rows()/draw_one_row()/paint_row()
 * (app_sand.c) are static, inlined at their one call site - sharing a hot
 * per-call function across a translation-unit boundary previously cost a
 * measured 26% regression elsewhere (Performance-Tuning-Attempts.md).
 * Duplicates draw_dirty_rows()'s ~15-line policy instead (same row_runs
 * calls, same order, same dirty gate); paints no pixels, since
 * gfx_present()'s cost depends only on marked regions, never colour. */

static void mirror_app_sand_marking(const uint8_t *cells, int w, int h,
                                    uint8_t *dirty_rows, uint16_t *row_x0,
                                    uint16_t *row_x1, uint8_t *row_n)
{
    for (int cy = 0; cy < h; cy++) {
        if (!dirty_rows[cy]) {
            continue;
        }
        dirty_rows[cy] = 0;

        const uint8_t *row = &cells[(size_t)cy * w];

        int run_x0[ROW_MAX_RUNS], run_x1[ROW_MAX_RUNS];
        const int n = row_runs_find(row, w, SAND_EMPTY, run_x0, run_x1);

        uint16_t cur_x0[ROW_MAX_RUNS], cur_x1[ROW_MAX_RUNS];
        int cur_n;
        if (n < 0) {
            int x0, x1;
            row_runs_span_fallback(row, w, SAND_EMPTY, &x0, &x1);
            cur_x0[0] = (uint16_t)x0;
            cur_x1[0] = (uint16_t)x1;
            cur_n = 1;
        } else {
            for (int i = 0; i < n; i++) {
                cur_x0[i] = (uint16_t)run_x0[i];
                cur_x1[i] = (uint16_t)run_x1[i];
            }
            cur_n = n;
        }

        uint16_t *rprev_x0 = &row_x0[cy * ROW_MAX_RUNS];
        uint16_t *rprev_x1 = &row_x1[cy * ROW_MAX_RUNS];
        const int rprev_n = row_n[cy];

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, rprev_x0,
                                              rprev_x1, rprev_n, send_x0,
                                              send_x1);

        for (int i = 0; i < send_n; i++) {
            gfx_mark_dirty(send_x0[i] * REAL_CELL_PX, cy * REAL_CELL_PX,
                          (send_x1[i] - send_x0[i]) * REAL_CELL_PX,
                          REAL_CELL_PX);
        }

        for (int i = 0; i < cur_n; i++) {
            rprev_x0[i] = cur_x0[i];
            rprev_x1[i] = cur_x1[i];
        }
        row_n[cy] = (uint8_t)cur_n;
    }
}

static void seed_row_runs_full_width_for_gfx_test(uint16_t *row_x0,
                                                   uint16_t *row_x1,
                                                   uint8_t *row_n, int w,
                                                   int h)
{
    for (int i = 0; i < h; i++) {
        row_x0[i * ROW_MAX_RUNS] = 0;
        row_x1[i * ROW_MAX_RUNS] = (uint16_t)w;
        row_n[i] = 1;
    }
}

/* The settle frames are whole frames, so gfx's dirty state and row_runs'
 * "previous" reach what a running app sees before the timed window starts,
 * instead of measuring an inflated first frame.
 *
 * This is the PIPELINED price: queued bands drain together, so seven bands
 * in a real frame come to 18,147 us, not 7 x 3,405 = 23,835. suite_gfx.c's
 * ratio tests measure the un-pipelined price; the two do not convert by a
 * band count. */
static int64_t run_present_against_scene(sand_t *s, const uint8_t *cells,
                                          int w, int h, uint8_t *dirty_rows,
                                          uint16_t *row_x0, uint16_t *row_x1,
                                          uint8_t *row_n, int gx, int gy,
                                          int gz, int settle_steps,
                                          int measured_steps, int *full_bands,
                                          int *gathered, int *partial_bands,
                                          int64_t *sim_us_out,
                                          int64_t *mark_us_out,
                                          int64_t *present_us_out)
{
    for (int i = 0; i < settle_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1,
                                row_n);
        gfx_present();
    }

    gfx_reset_strip_send_counts();

    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    for (int i = 0; i < measured_steps; i++) {
        const int64_t t0 = esp_timer_get_time();
        sand_step(s, gx, gy, gz);
        const int64_t t1 = esp_timer_get_time();
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1,
                                row_n);
        const int64_t t2 = esp_timer_get_time();
        gfx_present();
        const int64_t t3 = esp_timer_get_time();

        sim_us     += t1 - t0;
        mark_us    += t2 - t1;
        present_us += t3 - t2;
    }

    gfx_get_strip_send_counts(full_bands, gathered, partial_bands);

    /* Per-step MEANS, same as the return value below - three phases of the
     * same measured window, so they share one averaging convention. */
    if (sim_us_out != NULL) {
        *sim_us_out = sim_us / measured_steps;
    }
    if (mark_us_out != NULL) {
        *mark_us_out = mark_us / measured_steps;
    }
    if (present_us_out != NULL) {
        *present_us_out = present_us / measured_steps;
    }

    return present_us / measured_steps;
}

/* DENSE, CONTIGUOUS shape. Checkerboard exceeds ROW_MAX_RUNS (2).
 * row_runs_find() fails, row_runs_span_fallback() reports wide span.
 * gfx_present() handles. Scene shared with
 * test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene. Allocate
 * `big`, `dirty_rows`, `row_x0`, `row_x1`, `row_n` for sand_init(), tracking,
 * seeding. */
static void build_falling_sand_present_scene(sand_t *real, uint8_t *big,
                                              uint8_t *dirty_rows,
                                              uint16_t *row_x0,
                                              uint16_t *row_x1, uint8_t *row_n)
{
    sand_init(real, big, REAL_W, REAL_H, 99u);
    sand_track_dirty_rows(real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W,
                                          REAL_H);

    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(real, x, y, SAND_FIRST_SHADE);
            }
        }
    }
}

static void test_present_cost_against_a_falling_sand_scene(void)
{
    uint8_t  *big       = malloc(REAL_W * REAL_H);
    uint8_t  *dirty_rows = malloc(REAL_H);
    uint16_t *row_x0    = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t *row_x1    = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t  *row_n     = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    build_falling_sand_present_scene(&real, big, dirty_rows, row_x0, row_x1,
                                     row_n);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    const int measured_steps = 20;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W,
        REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1, 0, 5, measured_steps,
        &full_bands, &gathered, &partial_bands, NULL, NULL, NULL);

    ESP_LOGI("device_tests", "present cost, falling sand checkerboard, "
                             "%dx%d: mean %lld us/frame over %d frames "
                             "(%d full-band, %d gathered, %d partial-band "
                             "strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands,
             gathered, partial_bands);

    free(big);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    /* Present() is ~94% irreducible bus time (gfx.h;
     * test_full_present_cost_splits_into_bus_time_and_overhead) - the only
     * movable thing is HOW MANY strips get sent, shown by the strip-send
     * counts beside the timing. Target: measured 9961 * 0.97 -> 9650, NOT the
     * 0.9 sand_step() rows use: a 10% target on a 6%-reducible cost is
     * permanently unreachable, and 3% already asks for half the movable part.
     * Bound by different hardware (bus, not flash layout) - do not correct
     * this to 0.9. */
    TEST_ASSERT_LESS_THAN_MESSAGE(9650, (int)mean_us,
        "present() against a moving falling-sand scene got more expensive "
        "- check the full-band vs gathered counts in the log line above "
        "before suspecting the panel");
}

/* Present tests run the sim outside their own timer. Neither measures the
 * frame SUM, needed before justification. PRINTS, no frame budget argued yet.
 * Missing the real pixel writes - a LOWER BOUND only. */
static void test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene(void)
{
    uint8_t  *big       = malloc(REAL_W * REAL_H);
    uint8_t  *dirty_rows = malloc(REAL_H);
    uint16_t *row_x0    = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t *row_x1    = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t  *row_n     = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    build_falling_sand_present_scene(&real, big, dirty_rows, row_x0, row_x1,
                                     row_n);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int measured_steps = 20;
    run_present_against_scene(&real, big, REAL_W, REAL_H, dirty_rows, row_x0,
        row_x1, row_n, 0, 1, 0, 5, measured_steps, &full_bands, &gathered,
        &partial_bands, &sim_us, &mark_us, &present_us);

    free(big);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    const int64_t total_us = sim_us + mark_us + present_us;
    const int present_pct = total_us > 0
        ? (int)((present_us * 100) / total_us) : 0;

    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: sim %lld us/frame",
             (long long)sim_us);
    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: mark %lld us/frame",
             (long long)mark_us);
    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: present %lld us/frame",
             (long long)present_us);
    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: total %lld us/frame",
             (long long)total_us);
    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: present is %d%% of "
             "the total",
             present_pct);
}

static void test_present_cost_against_the_lava_stress_scene(void)
{
    uint8_t  *big        = malloc(REAL_W * REAL_H);
    uint8_t  *blocks     = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t  *dirty_rows = malloc(REAL_H);
    uint16_t *row_x0     = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t *row_x1     = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t  *row_n      = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_track_dirty_rows(&real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W,
                                          REAL_H);

    build_lava_stress_scene(&real);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int measured_steps = 20;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W,
        REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1000, 0, 30,
        measured_steps, &full_bands, &gathered, &partial_bands, &sim_us, &mark_us,
        &present_us);

    /* THE WHOLE FRAME, not just the bus. bd esp32c6-e6c: every other
     * row here times sand_step() with no drawing, and the present rows
     * time the bus alone, so nothing measured the frame a user actually
     * sees. The helper already separates these three - this row was
     * discarding them. */
    ESP_LOGI("device_tests", "frame time, lava stress: sim %lld us/frame",
             (long long)sim_us);
    ESP_LOGI("device_tests", "frame time, lava stress: mark %lld us/frame",
             (long long)mark_us);
    ESP_LOGI("device_tests", "frame time, lava stress: present %lld us/frame",
             (long long)present_us);
    ESP_LOGI("device_tests", "frame time, lava stress: total %lld us/frame",
             (long long)(sim_us + mark_us + present_us));

    ESP_LOGI("device_tests", "present cost, lava stress scene, %dx%d: mean "
                             "%lld us/frame over %d frames (%d full-band, "
                             "%d gathered, %d partial-band strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands,
             gathered, partial_bands);

    free(big);
    free(blocks);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    TEST_ASSERT_LESS_THAN_MESSAGE(12200, (int)mean_us,
        "present() against the lava stress scene got more expensive - "
        "check the full-band vs gathered counts in the log line above "
        "before suspecting the panel");
}

static void test_present_cost_against_the_thermal_shock_scene(void)
{
    uint8_t  *big        = malloc(REAL_W * REAL_H);
    uint8_t  *blocks     = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t  *dirty_rows = malloc(REAL_H);
    uint16_t *row_x0     = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t *row_x1     = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t  *row_n      = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_track_dirty_rows(&real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W,
                                          REAL_H);

    build_thermal_shock_scene(&real);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int measured_steps = 10;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W,
        REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1000, 0, 0,
        measured_steps, &full_bands, &gathered, &partial_bands, &sim_us, &mark_us,
        &present_us);

    /* THE WHOLE FRAME, not just the bus. bd esp32c6-e6c: every other
     * row here times sand_step() with no drawing, and the present rows
     * time the bus alone, so nothing measured the frame a user actually
     * sees. The helper already separates these three - this row was
     * discarding them. */
    ESP_LOGI("device_tests", "frame time, thermal shock: sim %lld us/frame",
             (long long)sim_us);
    ESP_LOGI("device_tests", "frame time, thermal shock: mark %lld us/frame",
             (long long)mark_us);
    ESP_LOGI("device_tests", "frame time, thermal shock: present %lld us/frame",
             (long long)present_us);
    ESP_LOGI("device_tests", "frame time, thermal shock: total %lld us/frame",
             (long long)(sim_us + mark_us + present_us));

    ESP_LOGI("device_tests", "present cost, thermal shock lattice, %dx%d: "
                             "mean %lld us/frame over %d frames (%d "
                             "full-band, %d gathered, %d partial-band "
                             "strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands,
             gathered, partial_bands);

    free(big);
    free(blocks);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    /* 70 of 70 strip-sends full and zero gathered is correct, not a target:
     * this lattice dirties every strip every frame, and an oracle marking
     * only the cells whose byte changed sends the identical 164,864 pixels.
     * Pixels sent is the number to watch.
     *
     * Budget is measured x 0.97 rather than the sand rows' 0.9 because only
     * ~6% of a present is not bus time. A failure most likely means the
     * scene dirties MORE pixels; do not go looking for a slower present. */
    TEST_ASSERT_LESS_THAN_MESSAGE(17450, (int)mean_us,
        "present() against the thermal shock lattice got more expensive "
        "than a full-screen send every frame, which is already what it "
        "costs - check the strip-send counts in the log line above");
}
#endif /* DEVICE_BUILD */

#define BUBBLE_W 41
#define BUBBLE_H 30

/* acid_bubble() (sand_reactions.c) replaced splash_displace()'s old
 * "landed hard on already-occupied liquid" trigger for acid: a real-scene
 * reproduction found landing events concentrating against whichever wall
 * cross-flow reached first - an emergent, self-reinforcing bias with no
 * single buggy line behind it. acid_bubble()'s flat, independent, per-cell
 * roll has no such feedback loop. */
static void test_acid_bubbles_do_not_favour_one_wall(void)
{
    /* NO POUR NEEDED: acid_bubble() checks every acid cell the REACTIONS
     * pass visits, every step, for open space against gravity, so a flat,
     * static pool's own exposed surface alone keeps it rolling. */
    enum { POOL_TOP = 15 };
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t *bubble_cells = malloc((size_t)BUBBLE_W * BUBBLE_H);
    impulse_t *bubble_buf = malloc(512 * sizeof *bubble_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(bubble_cells,
        "acid-bubble pool grid must fit in what the framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(bubble_buf,
        "acid-bubble impulse queue must fit in what the framebuffer leaves");
    sand_init(&fx.bubble_sim, bubble_cells, BUBBLE_W, BUBBLE_H, 3u);
    sand_enable_impulses(&fx.bubble_sim, bubble_buf, 512);

    /* NOT sleeping-enabled here, unlike test_acid_bubbles_still_bubble_
     * once_the_block_is_asleep below: this test's job is the SPATIAL claim
     * (no wall favoured), that test's is SLEEPING. FULL GRID WIDTH, NO
     * MARGIN: a pool with room to spread lowers its own surface over
     * hundreds of steps (same mass, wider footprint), so a fixed "above
     * POOL_TOP" check would end up looking above where the surface once
     * sat, not where it is. */
    for (int y = POOL_TOP; y < BUBBLE_H; y++) {
        for (int x = 0; x < BUBBLE_W; x++) {
            sand_set(&fx.bubble_sim, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }

    int left_pops = 0, right_pops = 0;
    const int mid = BUBBLE_W / 2;
    for (int i = 0; i < 300; i++) {
        sand_step(&fx.bubble_sim, 0, 1000, 0);
        for (int y = 0; y < POOL_TOP; y++) {
            for (int x = 0; x < BUBBLE_W; x++) {
                if (CELL_MATERIAL(sand_at(&fx.bubble_sim, x, y)) == MAT_ACID) {
                    if (x < mid) {
                        left_pops++;
                    } else if (x > mid) {
                        right_pops++;
                    }
                }
            }
        }
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(bubble_cells);
    free(bubble_buf);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, left_pops + right_pops,
        "acid_bubble() must actually pop grains above an exposed surface "
        "over time - none appeared at all");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, left_pops,
        "bubbles must reach the left half of the surface, not just the "
        "right - see this test's own top comment for the exact regression "
        "this guards against");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, right_pops,
        "bubbles must reach the right half of the surface, not just the "
        "left - see this test's own top comment for the exact regression "
        "this guards against");
}

#define SLEEPY_BLOCK_COLS ((BUBBLE_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define SLEEPY_BLOCK_ROWS ((BUBBLE_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
static uint8_t sleepy_bubble_blocks[SLEEPY_BLOCK_COLS * SLEEPY_BLOCK_ROWS];

/* Bubbling must survive block sleeping. step_one_row() (sand.c) skips a
 * settled block entirely, so anything hung off move_liquid_grain() stops
 * the moment a puddle goes calm - exactly when bubbling is meant to prove
 * it is still alive. acid_bubble() lives in the reactions pass, which is
 * not block-gated, for the same reason dissolving and cooling are not.
 * This pool is settled to a confirmed sand_block_settled() before a single
 * bubble is allowed to count. */
static void test_acid_bubbles_still_fire_once_the_block_is_asleep(void)
{
    enum { POOL_TOP = 15 };
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. sleepy_bubble_blocks stays static -
     * it is a tiny sleep-state bitmap, not one of the buffers that
     * starved the device heap. */
    uint8_t *sleepy_bubble_cells = malloc((size_t)BUBBLE_W * BUBBLE_H);
    impulse_t *sleepy_bubble_buf = malloc(512 * sizeof *sleepy_bubble_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(sleepy_bubble_cells,
        "sleepy acid-bubble pool grid must fit in what the framebuffer "
        "leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(sleepy_bubble_buf,
        "sleepy acid-bubble impulse queue must fit in what the "
        "framebuffer leaves");
    sand_init(&fx.sleepy_bubble_sim, sleepy_bubble_cells, BUBBLE_W, BUBBLE_H, 3u);
    sand_enable_sleeping(&fx.sleepy_bubble_sim, sleepy_bubble_blocks);
    sand_enable_impulses(&fx.sleepy_bubble_sim, sleepy_bubble_buf, 512);

    for (int y = POOL_TOP; y < BUBBLE_H; y++) {
        for (int x = 0; x < BUBBLE_W; x++) {
            sand_set(&fx.sleepy_bubble_sim, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
    /* GLASS LID to ensure "must fall asleep" setup check is independent of
     * SAND_ACID_BUBBLE_CHANCE. acid_bubble() rolls only for open space
     * against gravity. Lid prevents acid cell exposure, ensuring pool settles
     * by physics alone. Removed once asleep confirmed. */
    for (int x = 0; x < BUBBLE_W; x++) {
        sand_set(&fx.sleepy_bubble_sim, x, POOL_TOP - 1, GLASS);
    }

    bool asleep = false;
    for (int i = 0; i < 40 && !asleep; i++) {
        sand_step(&fx.sleepy_bubble_sim, 0, 1000, 0);
        asleep = true;
        for (int bx = 0; bx < SLEEPY_BLOCK_COLS && asleep; bx++) {
            for (int by = 0; by < SLEEPY_BLOCK_ROWS && asleep; by++) {
                if (!sand_block_settled(&fx.sleepy_bubble_sim, bx, by)) {
                    asleep = false;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(asleep,
        "setup: the pool must actually fall asleep within 40 quiet steps, "
        "or this test is not exercising the sleeping path it exists to "
        "check at all");

    for (int x = 0; x < BUBBLE_W; x++) {
        sand_erase(&fx.sleepy_bubble_sim, x, POOL_TOP - 1, 0);
    }

    int pops = 0;
    for (int i = 0; i < 300 && pops == 0; i++) {
        sand_step(&fx.sleepy_bubble_sim, 0, 1000, 0);
        for (int y = 0; y < POOL_TOP && pops == 0; y++) {
            for (int x = 0; x < BUBBLE_W; x++) {
                if (CELL_MATERIAL(sand_at(&fx.sleepy_bubble_sim, x, y)) == MAT_ACID) {
                    pops++;
                    break;
                }
            }
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of sleepy_bubble_cells/sleepy_bubble_buf are done
     * by this point. */
    free(sleepy_bubble_cells);
    free(sleepy_bubble_buf);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, pops,
        "acid_bubble() must keep firing even after its block has gone to "
        "sleep - see this test's own top comment for the exact bug this "
        "guards against (a real, calm puddle on device that never bubbled "
        "at all)");
}

/* --- suite -------------------------------------------------------------- */

#ifdef DEVICE_BUILD
/* Defined in app_sand.c, the hardware-facing entry point, which is not part
 * of the host build - hence DEVICE_BUILD around both this declaration and
 * the test. Declared here rather than in a header because one function does
 * not earn an app_sand.h and nothing else calls it. */
bool sand_app_alloc_selfcheck(size_t *out_largest_free, bool *out_impulses_ok);

/* Runs inside the suite on purpose, not before it. The question worth asking
 * is whether the app can be entered on a heap this suite has already worked
 * over, because that is the state someone actually finds the board in after
 * an autorun image finishes. */
static void test_the_sand_app_can_still_allocate_everything_it_needs(void)
{
    size_t largest_free = 0;
    bool   impulses_ok  = false;
    const bool ok = sand_app_alloc_selfcheck(&largest_free, &impulses_ok);

    ESP_LOGI("device_tests",
             "app alloc selfcheck: essential=%s impulses=%s largest_free=%u",
             ok ? "ok" : "FAILED", impulses_ok ? "ok" : "failed",
             (unsigned)largest_free);

    /* impulses are REPORTED, not asserted: the app treats a missing impulse
     * buffer as losing the blast mechanic rather than losing the app, so
     * failing the suite over it would overstate the damage. */
    TEST_ASSERT_TRUE_MESSAGE(ok,
        "the sand app could not allocate the grid and buffers a fresh entry "
        "needs - the simulation every frame-budget row in this file measures "
        "is one this device can no longer actually run. Read largest_free in "
        "the line above: the grid alone needs a contiguous 41,216 bytes, and "
        "this suite has just churned dozens of allocations that size");
}
#endif /* DEVICE_BUILD */

void run_sand_perf_suite(void)
{
    RUN_TEST(test_acid_bubbles_do_not_favour_one_wall);
    RUN_TEST(test_acid_bubbles_still_fire_once_the_block_is_asleep);

















#ifdef DEVICE_BUILD
    RUN_TEST(test_the_sand_app_can_still_allocate_everything_it_needs);
    RUN_TEST(test_a_full_size_step_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_settled_sand_costs_almost_nothing);
    RUN_TEST(test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget);
    RUN_TEST(test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_water_fits_in_the_frame_budget);
    RUN_TEST(test_the_cache_counters_over_a_water_step);
    RUN_TEST(test_the_cache_counters_over_a_settled_sand_step);
    /* Ungated: the two gas movers compare through sand_set_gas_walk(), an
     * ordinary API, so this runs in every diagnostics build. */
    RUN_TEST(test_the_gas_random_walk_against_the_exhaustive_mover);
    RUN_TEST(test_a_gravity_flip_on_every_material_at_once_stays_sane);
    RUN_TEST(test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_a_full_screen_of_fire_fits_in_the_frame_budget);
    RUN_TEST(test_four_liquids_reacting_at_once_fits_in_the_frame_budget);
    RUN_TEST(test_the_lava_stress_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_water_onto_a_plant_bed_costs_more_than_steady_growth);
    RUN_TEST(test_a_growing_plant_bed_fits_in_the_frame_budget);
    RUN_TEST(test_the_wood_leaf_shading_on_a_grove);
    RUN_TEST(test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_half_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_the_thermal_shock_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_boiler_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_wet_earth_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_water_over_lava_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_gunpowder_basin_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_plant_ruin_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_filling_basin_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_snowfall_scene_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_the_plant_brush_fits_in_the_frame_budget);
    RUN_TEST(test_a_settled_plant_garden_fits_in_the_frame_budget);
    RUN_TEST(test_a_finished_tree_fits_in_the_frame_budget);

    RUN_TEST(test_present_cost_against_a_falling_sand_scene);
    RUN_TEST(test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene);
    RUN_TEST(test_present_cost_against_the_lava_stress_scene);
    RUN_TEST(test_present_cost_against_the_thermal_shock_scene);
#endif
}

SUITE_REGISTER(run_sand_perf_suite);
