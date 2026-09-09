/*=============================================================================
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
 *===========================================================================*/
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
#include "tilt.h"      /* TILT_TAU_*_MS - the turn below follows the real
                       * filter shape rather than a straight line */

/* build_water_over_lava_scene()/WATER_LAVA_IMPULSE_MAX and
 * test_the_water_over_lava_scene_reaches_the_quench_cooloff_and_burst_paths_
 * it_claims moved to suite_sand_scenes.{c,h} - the builder and its impulse
 * budget are reused below by test_the_water_over_lava_scene_fits_in_the_
 * frame_budget, the same reason the four-liquid/lava-stress/etc scene
 * builders live there. */

#ifdef DEVICE_BUILD
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "row_runs.h"
#include "../../gfx/gfx.h"
#define REAL_BLOCK_COLS ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define REAL_BLOCK_ROWS ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

/* The worst case: every cell on the screen moving at once.
 *
 * Historically this budget was set from a principle (stay well under the
 * blit's bus-time ceiling, quoted here for a long time as ~9.6 ms and
 * actually ~17 ms - see gfx.h, and the 2026-08-28 decomposition in
 * test_full_present_cost_splits_into_bus_time_and_overhead, which measured
 * 16,998 us of raw blit against 18,147 us of full gfx_present()) rather
 * than the measured number, because
 * it had already been raised twice from over-tight measured budgets - see
 * git history. It also carries a documented cross-build risk: the same code
 * has measured a 3.2-3.9 ms swing purely from the ESP32-C6's 32 KB flash
 * cache aligning differently as unrelated code shifts layout.
 *
 * Tightened anyway, past even the ~10% headroom this file's other
 * budgets use, to 6000 - about 3.3% over the real measured 5802 us,
 * exactly reproducible across fresh captures on the current build (fixed
 * RNG seed - see docs/sand/Architecture.md's "Verifying performance on
 * real hardware"). That thin a margin is a real bet against the
 * documented flash-cache swing above: if a future rebuild reintroduces
 * it and this starts flaking, that is the known, accepted trade-off of
 * tightening this far past the old principle-based number - loosen it
 * again rather than treating one flaky run as a simulation regression.
 *
 * Previously shared with the settled-pile flip test below via one
 * STEP_BUDGET_US constant; split into its own name once the two
 * scenarios needed genuinely different numbers - a checkerboard of
 * falling sand and a full-grid gravity flip are different worst cases
 * and there was never a real reason to hold them to the same ceiling.
 *
 * THE 2026-08-26 RE-BASE, which this and every other frame budget in
 * this file now follows: after the materials wave, the first full
 * capture of the current tree (performance_20260826_150930, reproduced
 * by a second capture to within 4 us on every test) re-measured all
 * thirteen scenes, and every budget was re-set to a UNIFORM REDUCTION
 * TARGET of measured * 0.9, rounded - a deliberate decision to demand
 * 10% improvement across the whole board rather than ratchet any
 * budget up to what the code happens to cost today. Every number
 * stays BELOW its own measured value, so nothing passes by decree;
 * all thirteen fail on the day of the re-base, and each stops failing
 * only when the work is done. Some numbers moved numerically upward
 * from older budgets (water, the fire pair, the every-material flip) -
 * those older budgets were reduction targets pegged to baselines that
 * predate the materials wave, and holding them frozen while the
 * simulation gained temperature, viscosity, drag, percolation and five
 * new scenes would conflate "the feature costs something" with "the
 * code regressed". The measured number beside each assertion is the
 * anchor; the target is a tenth under it.
 *
 * This one: measured 6434 us (was budget 6000, itself 3.3% over an
 * older 5802) -> target 5800. */
#define FULL_STEP_BUDGET_US 5800

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
/* Host-only timing probe (see main/apps/sand/tools/perf_probe/, the
 * canonical host attribution harness - bd esp32c6-o2s). Exposes the actual
 * official test function above under a plain name, so probe_main.c can run
 * it directly on a laptop instead of hand-copying the scene - one of the
 * two liquid-free controls. Never defined by any real build. */
void sand_host_probe_run_full_step_control(void)
{
    test_a_full_size_step_fits_in_the_frame_budget();
}
#endif

/* THE SCENE THE BUDGET TEST BELOW MEASURES, built fresh per call. Shared
 * with the pass decomposition after it, which has to rebuild between
 * configurations: a disabled pass leaves a different grid behind, so
 * reusing one scene would have each configuration measuring a board the
 * previous one shaped. */
/* The board both water_scene_us_per_step() and water_scene_single_step_us()
 * below measure, factored out so the two timing techniques cannot drift into
 * quietly measuring different scenes. `real` must already be default-
 * constructed storage; this does the sand_init()/sand_enable_sleeping() too. */
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

    /* Water gets a budget of its own, and a larger one, because it genuinely
     * does more: it moves an amount rather than a cell, and it takes a second
     * sweep across the flow - which is the only reason a tilted pool levels at
     * all. The figure is what a screen-wide collapse costs, plus room for the
     * cache to move under it.
     *
     * That case is a transient. Water at rest is 45 us, and even mid-pour only
     * the part that is moving costs anything; a whole screen collapsing at
     * once lasts a fraction of a second. Sustained, this would not be
     * acceptable - and if it ever becomes sustained, this number should be
     * argued down rather than up.
     *
     * Tightened 16000 -> 14000 once the tenth attempt's block-liquid skip
     * landed this at 13288 us, reproduced byte-identically across two
     * captures. A deliberately thin margin: nearby builds of the previous
     * mechanism measured up to 13698 us purely from flash layout, so 14000
     * is ~2% over the worst recent observation - the same knowing bet
     * FULL_STEP_BUDGET_US documents. If a rebuild that does not touch the
     * liquid pass flips this, check the liquid-free benchmarks first (they
     * are the layout controls - see Optimization-Playbook.md) and loosen
     * this rather than misread layout noise as a simulation regression.
     *
     * Re-based 2026-08-26: measured 16043 after the materials wave ->
     * target 14400 (measured * 0.9, rounded) - see FULL_STEP_BUDGET_US's
     * comment for the uniform re-base this is part of. Numerically up
     * from 14000, still well below measured: the 13288 the 14000 was
     * pegged to predates viscosity, drag and percolation existing. */
    TEST_ASSERT_LESS_THAN_MESSAGE(14400, (int)per_step,
        "a screen-wide collapse of water must still land inside a frame or "
        "two - the search across the flow is the thing to suspect");
}

#ifdef DEVICE_BUILD
/* A FULL SCREEN OF FIRE, the scene that actually costs the most. Water is
 * 15 ms a step; this one is 254 ms and the gas cascade beside it 347 ms, both
 * over budget, and neither has ever been decomposed. Same construction as
 * test_a_full_screen_of_fire_fits_in_the_frame_budget(), factored so the two
 * cannot drift. */
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
/* THE TWO GAS MOVERS ON THE SAME BOARD. Warmup runs with the walk OFF in both
 * arms, so the timed step sees a byte-identical scene and the only difference
 * is which mover handles it - the same reason the pass decompositions rebuild
 * rather than run twenty steps with a pass disabled.
 *
 * Prints; asserts nothing. The walk is a deliberate behaviour change as well as
 * a cost one, so "is it faster" is only half the question and the other half
 * needs eyes on a screen, not a budget. */
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
    /* Re-based 2026-08-26: measured 260 -> target 235 (measured * 0.9,
     * rounded) - see FULL_STEP_BUDGET_US's comment for the uniform
     * re-base. Down from 300, which had become pure headroom. */
    TEST_ASSERT_LESS_THAN_MESSAGE(235, (int)per_step,
        "sand that is not moving must cost almost nothing - if this fails, "
        "rows are being examined that had no reason to be");
}

static void test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget(void)
{
    /* The real worst case pouring produces, not a synthetic one: a user
     * pours a big pile, it settles and sleeps (as it does in normal use -
     * sleeping is on here, exactly like app_sand.c runs it), and then the
     * device gets tilted hard enough to reverse gravity outright. Every
     * block must wake at once - this is the scenario the whole block-grid
     * design exists to keep affordable, not the synthetic all-cells-full
     * or checkerboard-falling grids the other frame-budget tests use. */
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

    /* Its own number, split off from the plain full-size-step test's
     * FULL_STEP_BUDGET_US above (see that constant's comment for why they
     * used to share one, and for the 2026-08-26 uniform re-base this
     * follows). Re-based: measured 6529 after the materials wave ->
     * target 5900 (measured * 0.9, rounded; was 6500, pegged when this
     * measured 8996 pre-wave). */
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

/* Total liquid MASS on the grid - the invariant a liquid scene actually has,
 * where the sand scenes above use sand_count(). A water cell holds 1..15 in
 * its variant nibble (MASS_MAX, material.h) and the diffusion model moves
 * amounts between cells rather than moving cells, so the CELL COUNT genuinely
 * changes as a pool levels while the mass must not. */
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

/* THE USER'S OWN SCENARIO, AS A FRAME BUDGET - the same turn test_turning_a_
 * settled_pool_to_landscape_does_not_flash_the_whole_body pins visually,
 * timed instead. Filed because the device report was not only "a flip of
 * colours": the other half of it was "tilt flips into straight positions
 * with the water settled seem to cause a huge spike".
 *
 * WHY THIS IS NOT test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_
 * budget ABOVE, which already times a flip on a settled body. It differs in
 * every dimension that decides the cost:
 *
 *   - WATER, NOT SAND. A liquid grain goes through move_liquid_grain() and
 *     the mass-diffusion cross-flow pass, which the water budget above is
 *     more than twice the sand one precisely because of.
 *   - 40% OF THE GRID, full width, against that test's middle-half-width
 *     block from half height - about 25%, and never touching the side walls.
 *   - A 90-DEGREE ROTATION, not a 180-degree reversal. For a liquid that is
 *     the harder case by a long way: reversing gravity mostly drops the body
 *     in place, while turning the board sideways makes the whole pool
 *     re-level ACROSS the full grid width, which is exactly what the
 *     cross-flow search costs the most for. It also crosses the depth walk's
 *     own 45-degree regime line and flips both scan-direction flags, so it
 *     exercises the render side and not only the simulation.
 *
 * THE TURN IS SWEPT, one step per degree of it, rather than stepped in one
 * jump: a real tilt filter ramps, and the expensive frames are the ones
 * where the pool is mid-re-level, not the single frame the vector changes
 * on. Gravity follows a chord from (0, G) to (G, 0) so this needs no
 * trigonometry, the same reason the other tilt scenes here use straight
 * lines and triangle waves.
 *
 * BOTH THE MEAN AND THE WORST SINGLE STEP ARE LOGGED, and deliberately so:
 * a rotation's cost is not flat across the sweep, and a mean alone is
 * exactly the shape of number that can hide a spike - which is what the
 * report was about. The budget is asserted on the mean, matching every other
 * scene here; the worst is there to read in the capture. */
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

    /* MEASURED 41,509 us per step on device, 2026-09-06, this row's first
     * real measurement (capture_ref_main_20260906_185911.md). Budget is
     * that x 0.9 = 37,358, rounded DOWN to 37,300 so the target is never
     * looser than the convention. */

    /* THE 14000 THIS REPLACES WAS NEVER A BUDGET - it was borrowed from
     * the water screen so the row would compile, and said so. It also
     * misled a reader into reporting a 167% regression that never
     * happened, by dividing it by 0.9 as if it were pegged. */

    /* WORTH KNOWING BEFORE OPTIMISING THIS ROW: the impulse flight pass
     * never runs here at all - s->impulse_count is 0 for all 390 steps,
     * host-counted 2026-09-06 - and a host pass map puts ~48% of the cost
     * in cross-flow, ~1% reactions, ~1.5% gas. */
    TEST_ASSERT_LESS_THAN_MESSAGE(37300, (int)per_step,
        "turning the board a quarter turn with a settled pool on it must "
        "still fit in a frame or two - the pool re-levels across the whole "
        "grid width, so the cross-flow search is the thing to suspect, and "
        "a host pass map agrees at ~48%. A reduction target at measured x "
        "0.9, so failing means the work is not done yet");
}

/* THE SHAPE OF A REAL TURN, shared by the two gas tilt scenes below.
 *
 * tilt.c does not sweep gravity linearly - approach() is an exponential
 * moving average, current + (target - current) * dt / (tau + dt), with tau
 * interpolating between TILT_TAU_MOVING_MS and TILT_TAU_STILL_MS. At a ~24 ms
 * frame and the moving tau that is alpha ~ 0.375 per frame, so a real turn is
 * most of the way over in about five frames and settled in twelve.
 *
 * That decides what gets timed. A linear sweep puts its biggest per-step
 * gravity change in the MIDDLE of the turn; the real filter puts it in the
 * first two or three frames, which is where the simulation has the most to
 * do. The moving tau is used rather than the still one because turning the
 * device IS movement, and because it is the demanding case - the largest
 * gravity delta per frame.
 *
 * Returns the mean and writes the worst single step: a rotation's cost is not
 * flat across the turn, and a mean alone is the shape of number that hides a
 * spike. */
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

/* THE GAS PASSES UNDER A TILTED BOARD, which nothing else here measured.
 *
 * WHY NOT test_a_screen_of_smoke_and_steam ABOVE, which uses the same field:
 * that one runs at gravity (0, 1000), so it only ever sees an axis-aligned
 * sweep. equalise_gas() takes its spread direction from ring_dir(i_stable + 2),
 * so an axis-aligned board gives py == 0 and a diagonal one does not, and the
 * two take genuinely different paths - down to whether gas_run_t's carry runs
 * at all, since carry_ok IS py == 0. Four of the eight ring directions are
 * diagonal, half the orientations this device can be held in, and none of them
 * had a scene.
 *
 * THIS ONE IS THE DIAGNOSTIC EXTREME, not a scene anyone plays: a completely
 * packed screen, where no gas can go anywhere because there is nowhere to go.
 * It bounds the worst case and exercises the row-skip path, which needs packed
 * rows to fire at all. The half-screen scene below is its realistic
 * counterpart, and the PAIR is the point - a number from this one alone would
 * flatter any optimisation aimed at packed rows. */
/* HOW MUCH A MOSTLY-INERT BOARD PAYS FOR A SMALL FIRE.
 *
 * Every other reaction scene here fills the board with things that react, so
 * they measure what a reacting cell costs. This one measures the opposite,
 * which is the shape the app is usually in: sand_step_reactions() early-outs
 * only on a board-wide flag test, so one lit match makes it walk every cell
 * every step, decoding each, however small the fire is.
 *
 * The sand bulk is deliberate. A settled powder is exactly what the main
 * sweep's own block skip already handles, so whatever this row costs is the
 * reactions pass and the renderer, not the sweep.
 *
 * What it settled: the full-grid walk a small fire forces is 256 us, 0.4% of
 * this step, against 24343 us in the reaction bodies. That retired bd
 * esp32c6-3pa, which had proposed a per-block skip for the walk - the whole
 * ceiling of that idea turned out to be 0.4%, against a failure mode of a
 * stale bit extinguishing a quiet fire. */
/* THE POUR, WHICH IS THE MOMENT THAT ACTUALLY HURTS.
 *
 * Reported from play: the frame drops when water is poured onto the bed, and
 * pacing is stable once the plants are merely drinking from wet dirt. The
 * steady-state row below measures the second of those, so on its own it
 * measures the moment nobody complains about.
 *
 * Same bed, same settle, but timed over the steps immediately AFTER a fresh
 * pour rather than a quiet window - so the pair brackets what a player sees,
 * the way the packed and half-screen gas scenes do. The whole point is the
 * DIFFERENCE between the two rows; a number from either alone would describe
 * half the experience. Prints; asserts no budget until the pair has been read
 * once. */
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

/* WHAT A GROWING BED COSTS, which no other row here can say.
 *
 * The plant code - anchored()'s support BFS, find_water()'s reach down through
 * the soil, the root roll - only runs for a plant cell standing on damp soil,
 * so every other scene in this file prices it at exactly zero. This one has
 * ~188 plants, 183 leaves and 266 roots alive at once.
 */
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

    /* MEASURED 73,130 us per step on the 230-step schedule (capture_ref_
     * b2bb520) - three whole frames for one bed of plants. Budget is that
     * x 0.9 = 65,817, rounded DOWN to 65,800 so the target is never looser
     * than the convention.
     *
     * RED ON PURPOSE, and it should stay red until the work is done: this is
     * a reduction target, not a regression guard, the same convention the
     * campfire and water rows use. soak/dry is 28% of this step and is where
     * the reduction has to come from. */
    TEST_ASSERT_LESS_THAN_MESSAGE(65800, (int)per_step,
        "a bed of growing plants costs three frames a step - a reduction "
        "target at measured x 0.9, so failing means the work is not done yet");
}

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

    /* MEASURED 143,165 us per step on device, worst single step 148,093
     * (capture_ref_68034bf_20260908_233819.md). Budget is that x 0.9 = 128,848, rounded DOWN to
     * 128,800 so the target is never looser than the convention.
     *
     * SIX FRAMES A STEP, and that is the point of the row: it is the ceiling
     * a completely packed board imposes, not a number anyone plays at. The
     * half-screen row below costs 2.8x less on the same turn - judge changes
     * by that one, and use this to bound the worst case. */
    TEST_ASSERT_LESS_THAN_MESSAGE(128800, (int)per_step,
        "a quarter turn on a fully packed screen of gas is the worst case the "
        "gas passes can be handed - a reduction target at measured x 0.9, so "
        "failing means the work is not done yet");
}

/* THE SAME TURN ON A SCENE SOMEONE COULD ACTUALLY PRODUCE, and the one to
 * trust for whether a change helps real play.
 *
 * Gas rises, so its counterpart to the settled pool's 40% resting on the floor
 * is 40% resting against the CEILING, settled there before the board turns so
 * the turn starts from a body at rest rather than mid-pour. The rest of the
 * grid is open, and that is the difference that matters: a packed screen makes
 * every row skippable and every scan hopeless, while here most rows hold both
 * gas and space, so the passes do their ordinary work. */
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

    /* MEASURED 51,320 us per step on device, worst single step 62,138
     * (capture_ref_68034bf_20260908_233819.md). Budget is that x 0.9 = 46,188, rounded DOWN to
     * 46,100.
     *
     * 2.8x CHEAPER THAN THE PACKED ROW ABOVE on the identical turn, which is
     * why both exist. A change aimed at packed rows will look roughly three
     * times better there than it is worth here, and here is where the board
     * spends its time. */
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
    /* A harder worst case than the single-material flip above: three
     * materials at once, plus a fixed obstacle, so a settled pile isn't the
     * only thing that has to wake and move together. Sand fills ~30% of the
     * width on the left, water ~30% on the right, both poured from the
     * floor to half height so there's headroom to launch into once flipped
     * - same reasoning as the plain flip test. The remaining ~40% in the
     * middle holds a stone X, floor to ceiling: a fixed obstacle both
     * materials have to route around, not just fall/rise past. */
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

    /* The X: two diagonals crossing at mid-height, floor to ceiling across
     * the middle band. Two cells thick, clamped to stay inside the band -
     * a single-pixel diagonal staircase is exactly the shape that let fire
     * leak past a corner earlier this session (see
     * test_fire_is_not_smothered_with_a_gap); thickening it is the same
     * fix applied here up front instead of after finding the same leak
     * twice. */
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

    /* No grain-conservation check here, unlike the plain flip test above -
     * deliberately, not an oversight. sand_count() counts occupied CELLS,
     * and water's fill-level model can legitimately spread its mass across
     * more or fewer cells while conserving total mass; test_a_screen_of_-
     * water_fits_in_the_frame_budget already skips this same check for the
     * same reason. Asserting it here once failed with a real, misleading
     * "Expected 13206 Was 13348" - not a simulation bug, just this
     * invariant not holding once water is in the scene - and because the
     * assertion sat before the frees below, Unity's longjmp on that
     * failure skipped them and leaked ~41 KB, starving every later
     * malloc()-based test on this device's no-PSRAM heap. Left here as the
     * reason, not just removed quietly. */

    free(big);
    free(blocks);

    /* A deliberate reduction target from the day it was written (12000
     * against a then-measured 15144), never headroom. Re-based
     * 2026-08-26: measured 12999 after the materials wave -> target
     * 11700 (measured * 0.9, rounded) - see FULL_STEP_BUDGET_US's
     * comment for the uniform re-base this is part of. */
    TEST_ASSERT_LESS_THAN_MESSAGE(11700, (int)per_step,
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

/* Every material at once, then a gravity flip.
 *
 * The other budget tests each isolate one thing - a settled pile, a
 * screen of water, a fire cascade. This one deliberately does not: the
 * board is banded with a share of EVERY material, arranged so the
 * reactive pairs actually touch (fire against wood and gas, acid against
 * sand, lava against water), and then gravity is inverted. That makes
 * every pass in sand_step() do real work in the same step - the main
 * sweep on powders and liquids, sand_step_liquids()' cross-flow,
 * sand_step_gas()' rise and spread, and sand_step_reactions() dispatching
 * both burning and dissolving cells - which no single-material scene
 * does.
 *
 * It is the scene that catches a cost that only appears in combination:
 * a pass that is cheap alone but interacts badly with another's wake
 * pattern, or a per-cell branch that is well predicted in a uniform
 * scene and mispredicted in a mixed one.
 *
 * THE ASSERTION BELOW IS NOT A BUDGET, and must not be treated as one.
 * Every other figure in this file is a measured number with the
 * measurement written beside it; this one was written without access to
 * the device, so it is a deliberately loose SANITY CEILING - wide enough
 * that it cannot pass as tuned, tight enough to catch something
 * catastrophic like an accidental quadratic. Replace it with a real
 * figure from `run_device_tests.sh`, and say what was measured, the first
 * time anyone runs this on hardware. */
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

    /* The scene is DERIVED from materials[] and laid out by
     * all_pairs_material_at() so that every PAIR of materials touches -
     * not merely every material appearing somewhere. See that function
     * for why bands were not enough, and
     * test_the_mixed_scene_puts_every_material_pair_in_contact, which
     * checks the coverage on the host rather than leaving it a claim in
     * a comment. */

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

    /* Freed BEFORE the assertion, deliberately. Unity longjmps out of a
     * failing assert, so a free() after one never runs - which on this
     * device's no-PSRAM heap leaked ~41 KB and starved every later
     * malloc()-based test. That is a real thing that happened to the
     * mixed-scene test above; the fix belongs in every test shaped like
     * this one, not just the one that got caught. */
    free(big);
    free(blocks);
    free(impulses);

    /* THE 87800 BUDGET IS INVALIDATED, NOT CARRIED FORWARD: it was
     * measured against the fourteen-material scene, and this scene is
     * now bigger. */

    /* 200000 is a loose SANITY CEILING, not a budget - the same
     * placeholder shape this row used before its first measurement.
     * Re-peg from the first clean capture of THIS scene: measured x 0.9,
     * rounded, this section's usual reduction-target rule. */
    TEST_ASSERT_LESS_THAN_MESSAGE(200000, (int)per_step,
        "INVALIDATED, NOT A REAL BUDGET - this scene now covers the "
        "extended statics and gunpowder too, so the 87800 figure it used "
        "to carry no longer describes what it measures; re-peg from the "
        "first clean device capture of this scene at measured x 0.9 and "
        "replace this placeholder ceiling");
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
    /* The worst case sand_step_reactions() can face, not a synthetic one:
     * sand_reactions.c's own top comment explains that this pass's fixed
     * row-major (top-to-bottom, then left-to-right) scan order lets a
     * cascade continue into any neighbour positioned AHEAD of the scan
     * pointer within the same pass - which is both the right neighbour
     * (same row, not yet scanned) AND the neighbour directly below (a
     * row not yet reached at all). A single spark in the top-left corner
     * of a completely gas-filled grid is therefore the single most
     * expensive case there is: the cascade reaches every one of
     * REAL_W*REAL_H cells in ONE step, each paying a decay tick plus up
     * to eight neighbour lookups. */
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

    /* Measured on device at 321339-321342 us (~321 ms), exactly
     * reproducible across three separate captures (this simulation's
     * fixed RNG seeds mean identical runs give identical timings) -
     * nowhere near the plain-material budgets above, and deliberately not
     * held to them:
     * unlike the flip and water tests above, which model a single
     * realistic user gesture (a pour, then a tilt), an edge-to-edge
     * screen of gas is not something the current pour-brush UI can
     * practically produce - this is a deliberately synthetic worst case
     * (see this test's own top comment), not a claim that a real user
     * could trigger a stall this long. If fire+gas ever needs to
     * support a fully-packed screen at interactive rates, that is the
     * "creeping fire" design explicitly deferred in the plan this was
     * built from, not a bug in this v1.
     *
     * Re-based 2026-08-26: what had been a ~9%-over regression guard
     * became a reduction target like everything else in this file (see
     * FULL_STEP_BUDGET_US's comment). Measured 506666 after the
     * materials wave - the reactions pass gained real chemistry and the
     * gas pass is 60% of a saturated step - -> target 456000
     * (measured * 0.9, rounded; was 350000, pegged to a pre-wave
     * 321339).
     *
     * Re-pegged the same evening from the first watchdog-free capture
     * (performance_20260826_183646): the clean number is 412718 - a
     * 94000 us drop that says the 506666 was itself a contaminated row
     * the fifteenth attempt's survey missed, which this single-step
     * test is unusually exposed to (one ~500 ms window per capture
     * against a 5-second dump cadence, and deterministically so).
     * Same uniform rule, measured * 0.9 rounded -> 371500, back to a
     * deliberately failing reduction target rather than the accidental
     * pass the inflated peg produced.
     *
     * AND THEN IT WAS EARNED. The seventeenth attempt's gas sight-scan
     * change brought this to 331,654 us - inside 371500, the first
     * budget this project has closed since the tenth attempt. So the
     * same rule applies again rather than leaving the win as slack:
     * measured * 0.9 rounded -> 298000, failing by design once more.
     * That is deliberate and it is not moving the goalposts. The gas
     * pass had been written off as exhausted five rounds earlier, and
     * then gave up 13.9% to three integers on the stack once someone
     * noticed the cost was sequential rather than spatial - which is
     * the opposite of evidence that this scene has nothing left. When
     * a row genuinely reaches its floor, say so with a measurement the
     * way the thermal-shock present row does, and make it a guard
     * instead. */
    TEST_ASSERT_LESS_THAN_MESSAGE(298000, (int)elapsed,
        "a full-screen cascade must stay in the same ballpark as measured "
        "- a jump here means something got much more expensive, not that "
        "this specific number is a real-time requirement");
}

static void test_a_full_screen_of_fire_fits_in_the_frame_budget(void)
{
    /* The steady-state cost fire's KIND_GAS redesign introduced, not
     * measured by the cascade test above: a full screen that is
     * ALREADY fire pays for BOTH sand_step_gas() (rise+disperse, now
     * that fire shares gas's own pass) AND sand_step_reactions()
     * (decay/extinguish/ignite/smother) on every cell, every step,
     * indefinitely - not just once during ignition. Traced directly:
     * the cascade test's one measured step ignites via
     * sand_step_reactions() alone (sand_step_gas() already ran earlier
     * in that same sand_step() call, before the newly-ignited cells
     * existed), so its own numbers are untouched by this and did not
     * need revisiting - this is genuinely new territory. */
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

    /* Measured on device at 230962 us (~231 ms), identical across two
     * separate captures - same "not a real-time promise" reasoning as
     * the cascade test above: an edge-to-edge screen of fire is not
     * something the pour-brush UI can practically sustain, but this
     * catches a real regression if the steady-state cost balloons past
     * what was actually measured. Originally ~8% headroom over 230962,
     * tightened from an initial, untuned 300000 once the number proved
     * exactly reproducible rather than noisy.
     *
     * Re-based 2026-08-26 (see FULL_STEP_BUDGET_US's comment): measured
     * 295533 after the materials wave -> target 266000 (measured * 0.9,
     * rounded; was 250000). A reduction target now, like the rest of the
     * file.
     *
     * CLOSED, then re-pegged, 2026-08-28. The seventeenth attempt's gas
     * sight-scan change brought this to 255,130 - inside 266000, and the
     * host had predicted 255,400, which is 0.1% out on a change that
     * alters how much work happens rather than merely where the code
     * sits. That is the class of change the eleventh and fourteenth
     * attempts both mispredicted badly, so the accuracy is worth
     * recording rather than assuming next time. Same rule as every
     * other row: measured * 0.9 rounded -> 229500. See the cascade
     * test above for why a closed budget gets re-pegged rather than
     * banked. */
    TEST_ASSERT_LESS_THAN_MESSAGE(229500, (int)per_step,
        "steady-state cost of a full screen of fire must stay in the "
        "same ballpark as measured - not a real-time promise, but a "
        "real regression guard");
}

/* Every material at once above flips gravity on a settled scene; this one
 * never lets the scene settle in the first place. Four liquids of
 * different density are painted upside down (build_four_liquid_scene()
 * above, shared with test_the_four_liquid_scene_keeps_reacting_after_-
 * settling, which is what proves this really does keep reacting rather
 * than just claiming to) so lava, acid, water and oil spend the whole
 * measured window migrating past each other instead of settling into
 * inert bands - see that function's comment for why upside down is what
 * makes that true.
 *
 * This is also the only benchmark in this file, besides the deliberate-
 * reduction-target gravity-flip-on-every-material test above, that runs
 * with sand_set_mobility(SAND_MOBILITY_PER_MATERIAL) - the setting
 * app_sand.c itself actually calls. A liquid's mobility decides how often
 * it refuses to move at all (oil refuses about two moves in three), and
 * until this test existed nothing in this file was holding the app's own
 * liquid path to any budget - the gravity-flip test above runs at that
 * setting too, but it is graded as a reduction target, not a real ceiling.
 * Four liquids at once, at the app's own mobility, is now that benchmark.
 *
 * First device measurement, 2026-08-26: 125430 us (capture
 * performance_20260826_150930, reproduced to within a microsecond by a
 * second capture). The provisional ceiling this shipped with (150000)
 * was retired the same day - not to the ~9-10%-over guard its draft
 * comment prescribed, but to the uniform reduction target the whole
 * file moved to (see FULL_STEP_BUDGET_US's comment): measured * 0.9,
 * rounded -> 113000, failing by construction until the scene gets 10%
 * faster.
 *
 * Re-pegged the same evening from the first CLEAN capture
 * (performance_20260826_183646, taken after the diag image's task
 * watchdog was turned off): the fifteenth attempt proved the 125430
 * was contaminated - a watchdog register dump's console I/O landed
 * inside this test's timing window and was charged to the simulation.
 * Clean measurement 124336 (the dump was worth ~1100 us here) ->
 * target 112000. */
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

    TEST_ASSERT_LESS_THAN_MESSAGE(112000, (int)per_step,
        "four liquids reacting under the app's own per-material mobility "
        "is held to 10% below its first measured number, as a reduction "
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

/* A lava reservoir, a water roof, and columns of sand, wood and oil between
 * them (build_lava_stress_scene() above, shared with
 * test_the_lava_stress_scene_reaches_every_reaction_it_claims, which
 * proves all six reactions this scene exists for really do fire in it).
 * Lava is the reaction-richest material in the simulation, and this scene
 * puts every reaction it takes part in - quenching, boiling, glassing,
 * igniting wood, igniting oil, flaring - in front of the reactions pass at
 * once, across a scene big enough to keep them going for the whole
 * measured window rather than one that burns out in the first few steps.
 *
 * First device measurement, 2026-08-26: 121377 us (capture
 * performance_20260826_150930, reproduced by a second capture to within
 * a microsecond). Provisional ceiling (150000) retired the same day to
 * the file-wide uniform reduction target (see FULL_STEP_BUDGET_US's
 * comment): measured * 0.9, rounded -> 109000. */
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

    TEST_ASSERT_LESS_THAN_MESSAGE(109000, (int)per_step,
        "the lava stress scene is held to 10% below its first measured "
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

/* A full screen of smoke and steam with one spark of fire
 * (build_smoke_and_steam_scene() above, shared with
 * test_the_smoke_and_steam_scene_stays_a_gas_screen, which proves the
 * scene conserves cells and is still a gas screen at the end of the
 * window rather than one that decayed into something else). Smoke and
 * steam are the only materials in this simulation with convection
 * behaviour - they warm what they touch - and no benchmark in this file
 * has ever put either of them on screen in quantity before this one.
 *
 * Ten measured steps, not twenty: the same reason
 * test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget
 * and test_a_full_screen_of_fire_fits_in_the_frame_budget above use ten -
 * a full grid of gas is the most expensive thing this simulation does per
 * step, and a previous round of this project tripped the device's
 * five-second task watchdog with a test that ran too many steps across a
 * full screen. No settling steps either - the scene is the worst case
 * from the moment it is painted.
 *
 * First device measurement, 2026-08-26: 141189 us (capture
 * performance_20260826_150930; the second capture reproduced it at
 * 141187, a 2 us spread). Provisional ceiling (400000) retired the same
 * day to the file-wide uniform reduction target (see
 * FULL_STEP_BUDGET_US's comment): measured * 0.9, rounded -> 127000. */
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

    /* NOT exact conservation - steam condenses. reaction_t.condenses
     * collapses a 2x2 patch of steam into one water cell and clears three,
     * which is a real, deliberate loss; the host twin of this scene forces
     * it off with sand_set_condenses() to be able to count exactly, and
     * this row cannot, because its budget is pegged with condensation
     * running. What is still worth asserting is that the screen did not
     * quietly empty itself into something this row no longer measures. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - total / 16, count,
        "setup: a screen of smoke and steam must still be essentially full "
        "at the end of the window - steam condensing into water loses three "
        "cells a patch, but losing an appreciable fraction of the board "
        "means it decayed into something else");
    TEST_ASSERT_LESS_THAN_MESSAGE(127000, (int)per_step,
        "a full screen of smoke and steam is held to 10% below its first "
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

/* 480 glass compartments (build_thermal_shock_scene() above, shared with
 * test_the_thermal_shock_scene_shatters_in_both_directions, which proves
 * both shock directions really fire across the lattice and that the
 * aftermath - meltwater, steam, escaping fire, falling cullet - is still
 * alive at the end of the window rather than one this scene claims to
 * measure but has actually gone quiet). No settling steps: the lattice is
 * already at its most active the moment it is painted, since every ring
 * starts strictly between the two shock thresholds and every trigger
 * starts touching its ring from step 1.
 *
 * First device measurement, 2026-08-26: 106650 us (capture
 * performance_20260826_150930, reproduced by a second capture).
 * Provisional ceiling (400000) retired the same day to the file-wide
 * uniform reduction target (see FULL_STEP_BUDGET_US's comment):
 * measured * 0.9, rounded -> 96000.
 *
 * Re-pegged the same evening from the first CLEAN capture
 * (performance_20260826_183646, taken after the diag image's task
 * watchdog was turned off): the fifteenth attempt proved the 106650
 * was contaminated - a watchdog register dump's console I/O landed
 * inside this test's timing window and was charged to the simulation,
 * and here the dump was worth fully ~7900 us of the old number. Clean
 * measurement 98738 -> target 89000.
 *
 * The watchdog reasoning here is stronger than it was for the three
 * scenes above: ten steps, and no settling step to spend any of the
 * window on first. Host timing, best-of-5 and interleaved with the other
 * four scenes so nothing is measured while the machine is still warming
 * up, ranks this scene the most expensive of the five - 1199 us/step
 * against 824 for the lava stress scene, 763 for four liquids reacting,
 * 647 for the smoke-and-steam screen and 218 for the boiler. That host
 * number is a RELATIVE signal only, telling us this scene costs more
 * than its siblings on the same machine - it is deliberately NOT
 * extrapolated to a device figure, for the reason given above, and the
 * absolute figures are worth little in any case: host wall-clock on this
 * project drifts up to 40% within a single harness run, which is why
 * they are taken best-of-N with the builds interleaved.
 *
 * The step count is not chosen against those numbers at all. It is fixed
 * at ten by the host guard beside this test - see that test's comment on
 * why the cullet timeline only splits into honest thirds at ten - and
 * the CEILING is then what gets chosen against the watchdog: at the
 * original provisional 400000, ten steps was four seconds against the
 * device's five-second task watchdog; at the re-based 96000 the margin
 * is wide. The two remain one decision - raise the step count without
 * minding the ceiling and the bet needs re-doing. */
static void test_the_thermal_shock_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

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

/* The boiler scaled to the whole grid and run as a sustained steady state
 * (build_boiler_scene() above, shared with test_the_boiler_scene_keeps_-
 * boiling_across_the_window, which proves the basin keeps boiling for
 * the whole measured window, both burners are still contributing at the
 * end of it, and no cells go missing - the enclosed-burner property that
 * host test explains and this one relies on).
 *
 * 20 settle steps, then 30 measured - matching the host test's own
 * window exactly, so what this times is what that one already proved
 * really is a sustained boil rather than a burst that has mostly spent
 * itself by the time the measured window starts.
 *
 * First device measurement, 2026-08-26: 31529 us (capture
 * performance_20260826_150930, reproduced by a second capture).
 * Provisional ceiling (80000) retired the same day to the file-wide
 * uniform reduction target (see FULL_STEP_BUDGET_US's comment):
 * measured * 0.9, rounded -> 28500.
 *
 * The watchdog pairing the provisional ceiling was chosen against (50
 * total steps at 80000 us was four seconds against the five-second task
 * watchdog; an earlier draft's round 100000 was exactly five, which is
 * not a margin) is comfortably looser at the re-based number. Host
 * timing, best-of-5 and interleaved, ranks this scene the CHEAPEST of
 * the five at 218 us/step against 1199 for the thermal shock lattice -
 * which is why it can afford fifty steps where that one is held to
 * ten. */
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

    TEST_ASSERT_LESS_THAN_MESSAGE(28500, (int)per_step,
        "the boiler scene is held to 10% below its first measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}

/* Sand and dirt poured in equal amounts, water dropped over both until it
 * settles (build_wet_earth_scene() above, shared with
 * test_the_wet_earth_scene_keeps_percolating_across_the_window, which
 * proves - with counters taken mid-flight, not on the freshly built scene
 * - that the water is still being consumed by soaking, dirt's own
 * moisture is still rising and sand is still converting to dirt across
 * every quarter of the measured window). This is the first benchmark in
 * the file to put sustained load through sand_step_reactions() by way of
 * moisture rather than fire or heat - see the builder's own comment for
 * why may_have_moisture, unlike the other five content flags the
 * reactions pass gates on, needs a cell already holding moisture to stay
 * armed, and why that makes this scene run the reactions pass every
 * single step rather than the rare pass a plain water scene would.
 *
 * 35 settle steps, then 30 measured - matching the host test's own window
 * exactly (see that test's comment for why 35 rather than the 20-30 the
 * boiler and lava stress scenes use), so what this times is what that one
 * already proved really is sustained percolation rather than a spreading
 * transient that has not yet reached every column.
 *
 * MEASURED 2026-09-01: 100367 us for 30 steps, on the first capture that
 * could allocate this scene at all - three rounds of device captures had
 * measured nothing here, because the fixtures could not get their grids
 * until the static-fixture conversions freed the heap back to 63952 bytes.
 *
 * The provisional 300000 ceiling this replaces was sized with no hardware
 * and turned out to be 3x looser than the truth - the scene came in at
 * +66.5% headroom against it, so it never constrained anything.
 *
 * 80000 is measured * 0.8 rounded, NOT the * 0.9 the file's other thirteen
 * device budgets use. That was an explicit instruction from the person
 * pegging this benchmark, so it should not be "corrected" to * 0.9 for
 * consistency with the rest of the file. Like every other reduction target
 * here it is deliberately below what the scene currently costs: this row
 * is expected to FAIL until the work is done, and the budget is never
 * raised to meet a measurement. */
static void test_the_wet_earth_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

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
        "PROVISIONAL ceiling, not yet re-pegged from a device capture - "
        "see this test's own comment. Once measured this row becomes "
        "measured x 0.8 (a deliberately tighter reduction target than "
        "the rest of the file's x 0.9), not a loosened guard");
}

#ifdef SAND_HOST_PROBE
/* Host-only timing probe - the wet earth scene (see the full-step
 * control's own wrapper for the pattern). */
void sand_host_probe_run_wet_earth(void)
{
    test_the_wet_earth_scene_fits_in_the_frame_budget();
}
#endif

/* The water-over-lava scene from this file's own section above (see that
 * section's own top comment for why it replaces the vent-spam scene that
 * used to sit here), run as a frame-budget test.
 *
 * TWENTY STEPS, NO SETTLING - matching test_the_water_over_lava_scene_
 * reaches_the_quench_cooloff_and_burst_paths_it_claims (above) exactly,
 * so what this times is the same scene that test already proved really
 * does reach quench, cool_off_chain() and the burst gate rather than
 * sitting quiet. No settling step: the scene is already at its busiest
 * the instant it is painted (the whole seam touching for the first time),
 * and waiting would only let the pour burn through more of its own lava
 * before the window closes.
 *
 * THE ASSERTION BELOW IS NOT A BUDGET - nobody has run this scene on a
 * device yet. It is a deliberately loose PROVISIONAL ceiling, not
 * extrapolated from host timings: this file has already been burned once
 * by an extrapolated figure that came out four times too pessimistic
 * against the real device number (see git history, and test_the_lava_
 * stress_scene_fits_in_the_frame_budget's own account of it when IT was
 * new), and a second time by the vent-spam scene this replaces, whose own
 * provisional ceiling was 15.7x looser than its eventual measured figure
 * (git history, e7d3a0a). Replace this with a real figure the first time
 * this runs on a device: measured * 0.9, rounded - the same reduction-
 * target method every other row in this section uses - and say what was
 * measured, not a number picked to keep this row passing. Do NOT carry
 * the old vent-spam figure forward in any form - this is a different
 * scene measuring a different mechanism, not a faster or slower version
 * of the one it replaced. */
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

    TEST_ASSERT_LESS_THAN_MESSAGE(400000, (int)per_step,
        "PROVISIONAL ceiling, not yet measured on a device - see this "
        "test's own comment. Once measured this row becomes measured x "
        "0.9, rounded, the same reduction-target method every other row "
        "in this section uses - not a number chosen to keep this row "
        "passing");
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

/* --- gfx_present() cost against real sand scenes ------------------------
 *
 * Every frame-budget test above times sand_step() alone, with no drawing at
 * all involved - nobody has ever measured what gfx_present() actually costs
 * against the dirty pattern a busy, real sand scene leaves behind (see
 * suite_gfx.c for the only present() numbers that exist, all from synthetic
 * marks a test wrote by hand). The tests below close that gap, by building a
 * real scene, stepping it, reproducing app_sand.c's OWN marking policy
 * against the result, and timing gfx_present() on what that produces.
 *
 * "Reproducing", not calling: app_sand.c's draw_dirty_rows(), draw_one_row()
 * and paint_row() are all static, relying on the compiler inlining them
 * into their one call site. Removing `static` from a hot per-call
 * function to share it across a translation-unit boundary is the exact
 * shape of change a previous tuning round measured a 26% regression from
 * - not these functions, but try_fall_or_scatter()/try_slide() in sand.c -
 * see the seventh attempt in docs/sand/Performance-Tuning-Attempts.md, and
 * the eighth for how long it took to notice a "fix" for it was aimed at
 * code the failing benchmark never even ran. So
 * mirror_app_sand_marking() below duplicates the ~15 lines of policy from
 * draw_dirty_rows()'s `for (int cy...)` loop (app_sand.c, around its own
 * line 884) instead: the same row_runs_find()/row_runs_span_fallback()/
 * row_runs_reconcile() calls, in the same order, gated by the same per-row
 * dirty flag sand_track_dirty_rows() maintains. It does not paint any
 * pixels - gfx_present()'s cost depends only on which regions were marked
 * dirty, never on their colour, and paint_row()/draw_one_row() are the
 * static functions that decide colour, exactly the ones this cannot call
 * without repeating the regression above. */

/* REAL_W*REAL_CELL_PX == GFX_WIDTH and REAL_H*REAL_CELL_PX == GFX_HEIGHT -
 * REAL_W/REAL_H are the grid size at cell=2, the finest ("ULTRA") quality
 * tier in app_sand.c's qualities[] table, which is what the pixel math in
 * mirror_app_sand_marking()'s gfx_mark_dirty() calls has to agree with. */
#define REAL_CELL_PX 2

/* See the section comment above: mirrors app_sand.c's draw_dirty_rows()
 * marking policy against `cells` (the same raw w*h buffer sand_init() was
 * given - app_sand.c's own `grid`), gated by `dirty_rows` (hooked up via
 * sand_track_dirty_rows() at the call site) and reconciled against the
 * per-row "previous" state in row_x0/row_x1/row_n (seeded full-width by
 * seed_row_runs_full_width_for_gfx_test() below, the same lie
 * app_sand.c's seed_row_runs_full_width() starts from and for the same
 * reason: forces the first pass over each row to send full width rather
 * than trusting a "previous" state that was never real). */
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

/* app_sand.c's seed_row_runs_full_width(), duplicated for the same reason
 * mirror_app_sand_marking() above is: seeds every row's "previous run" as
 * one full-width span, so the first marking pass over a freshly-built
 * scene sends each row's true width rather than trusting a "previous"
 * state that was never real. */
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

/* Runs `settle_steps` unmeasured frames - each a real sand_step(),
 * mirror_app_sand_marking() and gfx_present(), not just the simulation
 * step - so gfx's own dirty state and the row_runs "previous" state
 * converge to what an actually-running app would see by the time the
 * timed window starts, instead of measuring the inflated first frame a
 * freshly-seeded full-width "previous" state would otherwise produce.
 * Then times `measured_steps` more of the same, returning the mean
 * gfx_present() cost in us. gfx_reset_strip_send_counts() is called right
 * before the measured window starts, so `full_bands`/`gathered`/
 * `partial_bands` come back as the totals accumulated over exactly those
 * steps, not the settle ones.
 *
 * Because each measured gfx_present() here can carry several full bands
 * at once, this function measures the PIPELINED price: send_full_row()
 * (gfx.c) queues its draw_bitmap without waiting, and gfx_present()
 * drains every queued band together at the end, so later bands' DMA
 * overlaps earlier bands' CPU-side setup. That is why seven bands sent
 * in a real frame come to 18,147 us, not 7 x 3,405 = 23,835 - the sum of
 * seven un-pipelined sends. The un-pipelined price, 3,405 us for one
 * band presented alone with nothing else queued, is what the ratio tests
 * in suite_gfx.c measure instead - test_a_narrow_change_costs_less_than_
 * a_full_band and its neighbors, through test_two_far_corners_cost_less_
 * than_a_full_band. The two numbers are not interchangeable: do not
 * sanity-check one against the other by multiplying by the band count. */
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

/* The plain falling-sand case: the same half-screen checkerboard
 * test_a_full_size_step_fits_in_the_frame_budget above builds, deliberately
 * not settled so every grain attempts to move every step. No sleeping and
 * no per-material scatter/decay/mobility, exactly matching that test's own
 * setup - this is the same worst case, with drawing added on top of it.
 *
 * Chosen as the DENSE, CONTIGUOUS end of the shape spectrum these three
 * present-cost tests are picked to bracket: a checkerboard alternates
 * cell-by-cell, which overflows ROW_MAX_RUNS (2) on essentially every
 * occupied row, so row_runs_find() gives up and row_runs_span_fallback()
 * reports one span covering nearly the whole row width - despite only half
 * of it actually holding a grain. That wide fallback span, not the true
 * occupied-cell count, is what gfx_present() actually has to move. */
/* THE SCENE both this test and test_a_real_frame_is_sim_plus_present_on_a_
 * falling_sand_scene below measure - factored out so the two cannot drift
 * into quietly timing different boards, the same reason build_water_scene()
 * exists above. `big`/`dirty_rows`/`row_x0`/`row_x1`/`row_n` must already be
 * allocated storage; this does the sand_init()/tracking/seeding too. */
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

    /* Pegged from the first device capture (performance_20260828_014644):
     * mean 10,852 us/frame, 76 full-band and 13 gathered strip-sends over
     * 20 frames. 12000 is ~10% over that - a REGRESSION GUARD, deliberately
     * not the measured*0.9 reduction target the thirteen sand budgets in
     * this file use. Those hold sand_step(), which is all reducible work;
     * a present() is 94% irreducible bus time (see gfx.h, and
     * test_full_present_cost_splits_into_bus_time_and_overhead), so the
     * only thing an optimisation could move here is HOW MANY strips get
     * sent - which the strip-send counts beside the timing are there to
     * show. Demanding 10% off a hardware constant would be a target
     * nobody could hit honestly.
     *
     * TIGHTENED 12000 -> 10200 on 2026-08-28, after the sixteenth
     * attempt's partial-band send path took this from 10,852 to 9,900
     * (26 of its 76 whole-band sends became full-width sends at their
     * own height). 10200 is ~3% over the new measurement, and a 3%
     * guard is defensible HERE in a way it would not be on a sand_step()
     * row: those ride the flash-layout lottery, which this project has
     * measured at ~4% and now suspects is quantised into two states,
     * while a present() is bus-bound and does not - measured 9,889 /
     * 9,900 across two captures of different builds, a 0.1% spread.
     * A present row can therefore be held far tighter than a sim row,
     * and that difference is a fact about which hardware each one is
     * bound by, not a matter of taste.
     *
     * RE-PEGGED 10200 -> 9650 on 2026-09-01, and with it this row stops
     * being a guard and becomes a reduction target like the sand rows -
     * measured 9,961 * 0.97, deliberately below the measurement, so it
     * fails until the work is done.
     *
     * 0.97, NOT the 0.9 the sand budgets use, and the difference is the
     * whole point: only ~6% of a present is not bus time, so a 10% target
     * here would be unreachable by any code that could ever be written -
     * a permanently red row teaches nothing. 3% asks for half of the only
     * part that can actually move. Do not "correct" this to 0.9 for
     * consistency with the sand rows; they are bound by different
     * hardware. If the reachable 6% is ever fully won, re-peg from the
     * new measurement rather than cutting deeper on principle. */
    TEST_ASSERT_LESS_THAN_MESSAGE(9650, (int)mean_us,
        "present() against a moving falling-sand scene got more expensive "
        "- check the full-band vs gathered counts in the log line above "
        "before suspecting the panel");
}

/* bd esp32c6-e6c: sand_step() budgets exclude present; present tests run
 * the sim outside their own timer. Neither measures the frame SUM, which
 * bd esp32c6-91i (overlapping present with the next step) needs before it
 * can be justified - water's step (15,168 us) now beats a full present
 * (~17,800 us). PRINTS, no frame budget argued yet. Missing the real pixel
 * writes (draw_dirty_rows()/paint_row() are static, unreachable here) - a
 * LOWER BOUND only. */
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

/* The lava stress scene (build_lava_stress_scene() above, shared with
 * test_the_lava_stress_scene_reaches_every_reaction_it_claims and
 * test_the_lava_stress_scene_fits_in_the_frame_budget) - a lava reservoir
 * floor, a water roof, and full-width columns of sand/wood/oil between
 * them. Same seed, settings and settle/measured split as that sand_step-
 * only benchmark above it, so this is directly comparable to it: what
 * drawing costs on top of the same scene and the same window.
 *
 * Chosen as the OTHER dense/contiguous case rather than the scattered one -
 * every layer in this scene spans the full row width (the floor and roof
 * are solid slabs; even the middle's sand/wood/oil/gap columns are wide
 * enough that a dirty row through them is one or two long runs, not many
 * short ones) - specifically so the scattered pick below has something
 * genuinely different to contrast against, not just a second variation on
 * the checkerboard's own fallback-span shape. */
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
    const int measured_steps = 20;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W,
        REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1000, 0, 30,
        measured_steps, &full_bands, &gathered, &partial_bands, NULL, NULL,
        NULL);

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

    /* Pegged from the first device capture (performance_20260828_014644):
     * mean 13,018 us/frame, 100 full-band and only 4 gathered strip-sends
     * over 20 frames - a denser, more contiguous dirty pattern than the
     * checkerboard's, and it gathers even less often. 14300 is ~10% over,
     * a regression guard for the same reason spelled out in
     * test_present_cost_against_a_falling_sand_scene's comment above.
     *
     * TIGHTENED 14300 -> 12200 on 2026-08-28. The partial-band path took
     * this from 13,018 to 11,885 - the largest share of any scene, 34 of
     * its 100 whole-band sends converted - and 12200 is ~3% over that.
     * See the falling-sand comment above for why 3% is a defensible
     * margin on a present row and would not be on a sand_step() one. */
    TEST_ASSERT_LESS_THAN_MESSAGE(12200, (int)mean_us,
        "present() against the lava stress scene got more expensive - "
        "check the full-band vs gathered counts in the log line above "
        "before suspecting the panel");
}

/* The thermal shock lattice (build_thermal_shock_scene() above, shared
 * with test_the_thermal_shock_scene_shatters_in_both_directions and
 * test_the_thermal_shock_scene_fits_in_the_frame_budget) - 480 separate
 * glass compartments in a 20x24 tile grid, each a small ring with real
 * empty margin between it and its neighbours (see that builder's own
 * comment on why the tiling keeps them from touching at all). Same seed,
 * settings and ten-step measured window as that sand_step-only benchmark
 * (no settle steps there either - the lattice is already at its most
 * active the moment it is painted), so this is directly comparable to it.
 *
 * Chosen as the SCATTERED case: a dirty row through this lattice crosses
 * many narrow, genuinely separate rings with real gaps between them,
 * rather than the lava scene's and the checkerboard's wide, near-full-
 * width spans - the shape gfx_present()'s gather-vs-full-band choice
 * (send_one_row() in gfx.c) exists for in the first place. */
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
    const int measured_steps = 10;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W,
        REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1000, 0, 0,
        measured_steps, &full_bands, &gathered, &partial_bands, NULL, NULL,
        NULL);

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

    /* Pegged from the first device capture (performance_20260828_014644),
     * and this is the row worth reading twice: mean 17,922 us/frame with
     * 70 full-band and ZERO gathered strip-sends over 10 frames. Ten
     * frames x 7 strips is 70, so every strip of every frame went out as
     * a whole band - this scene costs a full-screen send every frame
     * (a full present measures 18,147 us)
     * and the dirty-region tracking has nothing left to give: an ORACLE
     * that marks the exact set of cells whose byte changed this frame,
     * with no cap of any kind, sends the identical 164,864 pixels per
     * frame that the shipped marking does. The zero is not a target - 70
     * of 70 is correct behaviour, because a 480-compartment lattice
     * really does dirty every strip across its full width and full
     * height, every frame. The tracker is at its ceiling here rather
     * than failing. The number worth watching in this scene is PIXELS
     * SENT, not the gathered count, and only a change to what the scene
     * itself draws could move it.
     *
     * TIGHTENED 19700 -> 18700 on 2026-08-28, and this row is the one
     * that must NEVER become a reduction target, however the rest of
     * this file is graded. The oracle above proves the marking is exact
     * and gfx.h proves the bus is saturated, so the only honest budget
     * here is a guard sitting just above a number that cannot legally
     * fall. Measured 18,017 / 18,042 / 18,129 across three captures of
     * three different builds - a 0.6% spread, because a bus-bound row
     * does not ride the layout lottery - so a present row can be held far
     * tighter than a sim row.
     *
     * RE-PEGGED 18700 -> 17450 on 2026-09-01: measured 17,992 * 0.97, so
     * this stops being a guard ~3% ABOVE the measurement and becomes a
     * reduction target ~3% BELOW it, failing until the work is done.
     * 0.97 rather than the sand rows' 0.9 because only ~6% of a present
     * is not bus time - see the same re-peg note on
     * test_present_cost_against_a_falling_sand_scene for the full
     * reasoning, which applies unchanged here.
     *
     * If this fails because something made the scene dirty MORE pixels,
     * that is the regression this row still catches; do not go looking
     * for a slower present. */
    TEST_ASSERT_LESS_THAN_MESSAGE(17450, (int)mean_us,
        "present() against the thermal shock lattice got more expensive "
        "than a full-screen send every frame, which is already what it "
        "costs - check the strip-send counts in the log line above");
}
#endif /* DEVICE_BUILD */

#define BUBBLE_W 41
#define BUBBLE_H 30

/* acid_bubble() (sand_reactions.c) replaced splash_displace()'s old "landed
 * hard on already-occupied liquid" trigger for acid, specifically because
 * that trigger's location was wherever a landing event happened to occur -
 * and a real-scene reproduction (a symmetric pool, poured continuously into
 * its own centre) found those events concentrating hard against whichever
 * wall ordinary cross-flow levelling happened to reach first, an emergent,
 * self-reinforcing bias with no single buggy line behind it (ruled out one
 * at a time: the disc-seeding math, the diagonal-slide try-order, block
 * alignment, the liquid_flip/sweep_flip alternation, exact pool/pour
 * centring all still reproduced it). acid_bubble()'s flat, independent,
 * per-cell roll has no such feedback loop to fall into.
 *
 * NO POUR NEEDED to exercise this - acid_bubble() checks every acid cell
 * the REACTIONS pass visits, every step that pass runs, for open space
 * directly against gravity from it, regardless of whether anything is
 * actively landing. A flat, static, fully-settled pool's own surface row
 * stays exposed forever, so it alone is enough to keep rolling.
 *
 * NOT sleeping-enabled here, deliberately, unlike test_acid_bubbles_still_
 * bubble_once_the_block_is_asleep below - this test's own job is the
 * SPATIAL claim (no wall favoured), which does not need sleeping in the
 * picture at all; that test's job is the SLEEPING claim on its own.
 *
 * FULL GRID WIDTH, NO MARGIN - tried first with open columns either side
 * of the pool (room for it to spread into) and found NO pops ever counted,
 * despite direct tracing showing bubbles firing and moving: cross-flow
 * spreads a pool with room to spread into, which LOWERS its own surface
 * over hundreds of steps (same total mass, wider footprint), so a fixed
 * "above POOL_TOP" check ends up looking above where the surface USED to
 * be, not where it actually is by the time a bubble pops. A pool exactly
 * as wide as the grid has nowhere to spread, so its surface stays put. */
static void test_acid_bubbles_do_not_favour_one_wall(void)
{
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

    for (int y = POOL_TOP; y < BUBBLE_H; y++) {
        for (int x = 0; x < BUBBLE_W; x++) {
            sand_set(&fx.bubble_sim, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }

    /* Checked EVERY step, not just at the end - a popped grain falls back
     * under ordinary gravity within a few steps of landing (finalize_
     * settling() runs after step_impulses(), so the very next step's own
     * sweep pulls it straight back down), so a snapshot taken only after
     * all 300 steps would see nothing but the fully-resettled pool, even
     * on a run where bubbles popped constantly throughout. */
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

/* THE ACTUAL BUG A REAL DEVICE HIT, reported after acid_bubble() first
 * shipped living in move_liquid_grain() (sand_liquid.c): a real, calm
 * puddle of acid on device never bubbled at all, though the exact same
 * mechanism visibly worked in the test above. The difference is
 * sand_enable_sleeping() (see app_sand.c, which does enable it with a
 * real buffer) - move_liquid_grain() only runs for cells the MAIN SWEEP
 * visits, and step_one_row() (sand.c) skips any block marked settled
 * under block-sleeping entirely, never calling move_liquid_grain() for
 * its cells at all. A calm, undisturbed puddle earns that settled mark
 * within a handful of quiet steps - which is exactly what "calm" means -
 * so the trigger stopped firing the moment the puddle stopped visibly
 * moving, precisely when bubbling was supposed to prove it was still
 * "alive". The test above never caught this because it never enables
 * sleeping, so it always visits every cell regardless of settled state -
 * a blind spot in the test, not evidence the mechanism worked on device.
 *
 * FIXED by moving acid_bubble() into sand_reactions.c, called from
 * step_one_reacting_row()'s own `r->dissolves` branch - that pass is not
 * gated by block-sleeping at all (see acid_bubble()'s own comment there),
 * for the same reason dissolving and cooling already were not: they have
 * to keep happening on a board with nothing else moving.
 *
 * THIS TEST is the one that would have caught it: same flat pool as
 * above, but with sand_enable_sleeping() on, and a quiet settle period
 * BEFORE the check loop starts, so the pool's own block is genuinely
 * asleep (confirmed via sand_block_settled(), not merely assumed) before
 * a single bubble is allowed to count. */
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
    /* A GLASS LID over the whole surface while it settles - not load-
     * bearing for the claim itself, but for keeping this test's own
     * "must fall asleep" setup check independent of SAND_ACID_BUBBLE_
     * CHANCE's exact value. acid_bubble() only ever rolls for a cell with
     * open space against gravity from it (see its own comment in
     * sand_reactions.c) - a lid means no acid cell is ever exposed during
     * the settle phase, so nothing can roll a bubble regardless of how
     * high that chance is currently tuned, and the pool settles on
     * physics alone. Removed once asleep is confirmed, below - a covered
     * pool bubbling once uncovered is exactly the same claim the old,
     * chance-sensitive version of this test was after. */
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
    /* Not freed ahead of this assertion, unlike this file's other
     * converted fixtures - sleepy_bubble_sim still points into
     * sleepy_bubble_cells/sleepy_bubble_buf and the lid-lift plus pop
     * check below still need them live. If this setup assertion itself
     * fails, the buffers leak, same as any other test failure in this
     * run. */
    TEST_ASSERT_TRUE_MESSAGE(asleep,
        "setup: the pool must actually fall asleep within 40 quiet steps, "
        "or this test is not exercising the sleeping path it exists to "
        "check at all");

    /* Lift the lid now that the pool is confirmed asleep. Whether erasing
     * it also wakes the block is beside the point either way: acid_
     * bubble() lives in the reactions pass now (sand_reactions.c), which
     * never consults block-sleeping at all - see its own comment - so
     * this test's claim holds regardless of whether the lid's removal
     * happens to wake the block or not. */
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

/* THE CHECK NOTHING ELSE IN THIS FILE MAKES. Every other device row here
 * times sand_step() on a grid this suite allocated for itself, which says
 * nothing about whether the APP can still get one - and on 2026-09-01 it
 * could not: a "no memory for the grid" screen was sitting on the board
 * after a capture, unnoticed by 741 passing tests.
 *
 * Runs inside the suite on purpose, not before it. The question worth
 * asking is whether the app can be entered on a heap this suite has already
 * worked over, because that is the state someone actually finds the board
 * in after an autorun image finishes. */
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
    RUN_TEST(test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_half_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_the_thermal_shock_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_boiler_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_wet_earth_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_water_over_lava_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_gunpowder_basin_scene_fits_in_the_frame_budget);

    RUN_TEST(test_present_cost_against_a_falling_sand_scene);
    RUN_TEST(test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene);
    RUN_TEST(test_present_cost_against_the_lava_stress_scene);
    RUN_TEST(test_present_cost_against_the_thermal_shock_scene);
#endif
}

SUITE_REGISTER(run_sand_perf_suite);
