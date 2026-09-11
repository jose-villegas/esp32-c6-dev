/*
 * Portable suite: the falling-sand automaton - shared benchmark scenes -
 * four-liquid, lava-stress, smoke-and-steam, thermal-shock, boiler, and wet-
 * earth.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
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

/* --- on the real grid, on the real chip --------------------------------- */

/* REAL_W/REAL_H moved to suite_sand_common.h - reused throughout the
 * scenes/perf/blast portion of the split, well beyond this section. */

/* EMPTY_SHARE_PERCENT moved to suite_sand_scenes.h - the frame-budget
 * equivalent of the test below, in suite_sand_perf.c, needs it too. */

/* Which material lands on cell (x, y) in the all-pairs tiling.
 *
 * Bands cover far less than they look like they do: stacking materials in
 * horizontal strips puts only the vertically-adjacent pairs in contact, and
 * measured, just 20 of the 66 possible pairs ever met. */
int all_pairs_material_at(int x, int y, int first, int n_mats)
{
    const int stride = (y % (n_mats - 1)) + 1;
    return first + ((x * stride + y) % n_mats);
}

/* Maps a tiling index to the cell spec sand_spawn_cell() should place:
 * ordinary materials first, then the extended statics MATX_ICE..MATX_ROOT
 * (not the three spare, unclaimed MATERIAL_EXTENDED_COUNT slots), then
 * gunpowder. One copy, shared by the timed device scene and the host
 * coverage test, so the two cannot tile different sets. */
cell_t all_pairs_spawn_cell(int index)
{
    if (index < ALL_PAIRS_ORDINARY_COUNT) {
        /* Skipping one id keeps the 19 indices contiguous, so the tiling
         * sees a gap-free 0..n-1 and its stride argument still holds. */
        material_id_t m = (material_id_t)(MAT_EMPTY + 1 + index);
        if (m >= ALL_PAIRS_SKIPPED_ORDINARY) {
            m = (material_id_t)(m + 1);
        }
        return CELL_MAKE(m, 0);
    }
    index -= ALL_PAIRS_ORDINARY_COUNT;
    if (index < ALL_PAIRS_EXTENDED_COUNT) {
        return MATX(index);
    }
    return GUNPOWDER_CELL(0);
}

/* The tiling scatters gunpowder as one isolated cell in nineteen, which
 * can never form the fuse's fully-lit 2x2, so Gunpowder.explodes never
 * fires here. A fire cell is planted beside each patch rather than left
 * to the tiling's luck - build_gunpowder_basin_scene proved that pairing
 * reliable at scale. A handful, fixed and deterministic, not a field. */
typedef struct {
    int x, y;
} all_pairs_patch_t;

#define ALL_PAIRS_PATCH_SIZE 3

static const all_pairs_patch_t all_pairs_gunpowder_patches[] = {
    {40, 100},
    {120, 180},
};
#define ALL_PAIRS_PATCH_COUNT \
    (int)(sizeof(all_pairs_gunpowder_patches) / sizeof(all_pairs_gunpowder_patches[0]))

/* Paints the tiling, then overwrites the patches above - both device timing
 * and the host coverage test below call this one function, so they can
 * never see two different grids. */
void build_all_pairs_scene(sand_t *s)
{
    const int first  = 0;
    const int n_mats = ALL_PAIRS_SPAWN_COUNT;
    const int top    = (REAL_H * EMPTY_SHARE_PERCENT) / 100;

    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int idx = all_pairs_material_at(x, y, first, n_mats);
            /* sand_spawn_cell() with radius 0 rather than sand_set(): see
             * the device test's own comment (suite_sand_perf.c) for why. */
            sand_spawn_cell(s, x, y, 0, all_pairs_spawn_cell(idx));
        }
    }

    for (int p = 0; p < ALL_PAIRS_PATCH_COUNT; p++) {
        const int px = all_pairs_gunpowder_patches[p].x;
        const int py = all_pairs_gunpowder_patches[p].y;
        for (int dy = 0; dy < ALL_PAIRS_PATCH_SIZE; dy++) {
            for (int dx = 0; dx < ALL_PAIRS_PATCH_SIZE; dx++) {
                sand_set(s, px + dx, py + dy, GUNPOWDER_CELL(0));
            }
        }
        sand_set(s, px + ALL_PAIRS_PATCH_SIZE, py, FIRE);
    }
}

/* The exact inverse of all_pairs_spawn_cell() above: buckets a real cell
 * back into the tiling's own species index. Reading the grid this way,
 * rather than re-deriving what the tiling formula WOULD have painted, is
 * what lets the coverage test below see the patches overwriting cells -
 * the formula has no idea they exist. */
static int all_pairs_species_of(cell_t c)
{
    if (cell_is_gunpowder(c)) {
        return ALL_PAIRS_SPAWN_COUNT - 1;
    }
    if (cell_is_extended(c)) {
        return ALL_PAIRS_ORDINARY_COUNT + (c & 0x07);
    }
    const int m = CELL_MATERIAL(c);
    return (m < ALL_PAIRS_SKIPPED_ORDINARY) ? m - 1 : m - 2;
}

/* Every pair of materials really is adjacent somewhere in that scene - a
 * property of the ACTUAL BUILT GRID, not the tiling formula: this test
 * used to compute adjacency from all_pairs_material_at() directly, which
 * cannot see a patch overwriting a cell and would keep reporting full
 * coverage even if one destroyed the only place two materials touched.
 * Host-side: coverage needs no clock. */
static void test_the_mixed_scene_puts_every_material_pair_in_contact(void)
{
    const int n_mats = ALL_PAIRS_SPAWN_COUNT;
    const int top    = (REAL_H * EMPTY_SHARE_PERCENT) / 100;
    const int want   = (n_mats * (n_mats - 1)) / 2;

    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&s, blocks);

    build_all_pairs_scene(&s);

    /* One bit per unordered pair, sized to the tiling's own index range -
     * no longer MATERIAL_MAX, since an index past
     * ALL_PAIRS_ORDINARY_COUNT names an extended static or gunpowder, not
     * a material_id_t value. */
    static bool seen[ALL_PAIRS_SPAWN_COUNT][ALL_PAIRS_SPAWN_COUNT];
    memset(seen, 0, sizeof seen);

    int found = 0;
    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = all_pairs_species_of(sand_at(&s, x, y));
            const int nb[2] = {
                x + 1 < REAL_W ? all_pairs_species_of(sand_at(&s, x + 1, y)) : m,
                y + 1 < REAL_H ? all_pairs_species_of(sand_at(&s, x, y + 1)) : m,
            };
            for (int k = 0; k < 2; k++) {
                const int a = m < nb[k] ? m : nb[k];
                const int b = m < nb[k] ? nb[k] : m;
                if (a != b && !seen[a][b]) {
                    seen[a][b] = true;
                    found++;
                }
            }
        }
    }

    free(big);
    free(blocks);

    char why[200];
    snprintf(why, sizeof why,
             "the mixed-material scene must put all %d pairs of %d "
             "materials in contact - it reached %d, so some reaction it "
             "claims to exercise never fires in it",
             want, n_mats, found);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, found, why);
}

/* --- three more scenes, built once and shared with a device benchmark --- */

/* A benchmark must be proven to run the reactions it claims to measure, and
 * "proven" means a host test that builds the SAME scene through the SAME
 * builder and checks the reactions really fired, not a comment asserting
 * they do. Three device rounds once went into optimising a function the
 * failing benchmark never called. */

/* Four liquids of different density, painted upside down. In their own
 * settled order - lava at the bottom, oil on top - each layer finds its
 * level within a few steps and the interfaces that were doing the reacting
 * stop touching. Inverted, every layer has to migrate through every other
 * to reach where density wants it, so the interfaces stay in contact and
 * reacting for the whole measured window. */
void build_four_liquid_scene(sand_t *s)
{
    const int top = REAL_H / 6;                 /* headroom above the pour */
    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int band = ((y - top) * 4) / (REAL_H - top);
            const material_id_t m = (band == 0) ? MAT_LAVA
                                   : (band == 1) ? MAT_ACID
                                   : (band == 2) ? MAT_WATER : MAT_OIL;
            sand_set(s, x, y, CELL_MAKE(m, MASS_MAX));
        }
    }
}

/* The property the scene above exists for: that inverting the density order
 * really does keep the reactions running instead of merely moving where they
 * happen. Runs with the app's own per-material scatter, decay and mobility
 * rather than the defaults, because app_sand.c does too. */
static void test_the_four_liquid_scene_keeps_reacting_after_settling(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_four_liquid_scene(&s);

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int stone = 0, steam = 0, fire = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_STONE)      stone++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_FIRE)  fire++;
        }
    }

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(50, stone,
        "lava quenched by water should still be leaving a good showing of "
        "stone at the end of the window - if it isn't, the scene has gone "
        "quiet and the device test beside it is measuring almost nothing");
    /* Measured well clear of this floor: steam runs over 500 here. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(50, steam,
        "water boiled and fire quenched should still be leaving some "
        "showing of steam at the end of the window - if it isn't, the "
        "scene has gone quiet and the device test beside it is measuring "
        "almost nothing");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(50, fire,
        "oil ignited by lava should still be leaving a good showing of "
        "fire at the end of the window - if it isn't, the scene has gone "
        "quiet and the device test beside it is measuring almost nothing");
}

/* A lava reservoir on the floor, a water slab on the roof, and between them
 * repeating six-cell columns of sand, wood and oil with every fourth
 * column left empty as a chute. Lava is the reaction-richest material in
 * the simulation, so one scene covers six reactions instead of six tests.
 *
 * The chute is load-bearing: without it the roof water perches on the
 * columns and takes most of a minute to reach the lava, so the quench and
 * boil never fire inside the measured window. */
void build_lava_stress_scene(sand_t *s)
{
    /* floor: a lava reservoir */
    for (int y = (REAL_H * 3) / 4; y < REAL_H; y++)
        for (int x = 0; x < REAL_W; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));

    /* middle: repeating columns six cells wide - sand, wood, oil, then a
     * gap - deliberately, not an oversight, see the comment above. */
    for (int y = REAL_H / 3; y < (REAL_H * 3) / 4; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int col = (x / 6) % 4;
            if (col == 0)      sand_set(s, x, y, SAND_FIRST_SHADE);
            else if (col == 1) sand_set(s, x, y, CELL_MAKE(MAT_WOOD, 0));
            else if (col == 2) sand_set(s, x, y, CELL_MAKE(MAT_OIL, MASS_MAX));
            /* col == 3 is the chute - left empty on purpose */
        }
    }

    /* roof: a water slab */
    for (int y = 0; y < REAL_H / 6; y++)
        for (int x = 0; x < REAL_W; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
}

/* All six reactions the scene above exists to cover really do fire in it,
 * checked the same way test_the_four_liquid_scene_keeps_reacting_after_-
 * settling checks its own scene: build it through the same function the
 * device test uses, step it the same number of times, and count. */
static void test_the_lava_stress_scene_reaches_every_reaction_it_claims(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_lava_stress_scene(&s);

    for (int i = 0; i < 30; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int glass = 0, fire = 0, steam = 0, stone = 0, extended = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            const int m = CELL_MATERIAL(c);
            if (m == MAT_GLASS)      glass++;
            else if (m == MAT_FIRE)  fire++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_STONE) stone++;
            if (cell_is_extended(c)) extended++;
        }
    }

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(400, glass,
        "sand converting under sustained heat should have left a good "
        "showing of glass");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(400, fire,
        "wood and oil igniting against the lava should have left a good "
        "showing of fire");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(20, steam,
        "water reaching the lava through the chute should have boiled some "
        "of it to steam - a low count here means the chute let the water "
        "perch instead of falling through");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5, stone,
        "water reaching the lava through the chute should have quenched "
        "some of it to stone - a low count here means the chute let the "
        "water perch instead of falling through");

    /* This scene has both ingredients a plant needs - wood, and sand that
     * could take up water and become soil - yet never grows one: the roof
     * water flashes to steam before it can wet the sand. That is an
     * accident of tuning, and the device test beside this one pegs its
     * frame budget to a hardware capture of this same scene, so growth
     * starting inside the measured window would quietly stop that number
     * describing what the test claims to measure. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended,
        "the lava stress scene should not be growing any plants - if it "
        "is, the device test's frame budget is no longer measuring the "
        "scene it claims to");
}

/* An edge-to-edge checkerboard of smoke and steam, with one spark of fire.
 * Deliberately synthetic: no scene a user can paint packs the whole grid
 * with gas, but the reactions pass has to survive one that does. Smoke and
 * steam warm what they touch where fire and plain gas do not, so this is
 * the scene where the pass's per-cell neighbour work for gases runs.
 *
 * Left at the DEFAULT scatter, decay and mobility: at per-material decay
 * the gases fade away within the measured window. */
void build_smoke_and_steam_scene(sand_t *s)
{
    for (int y = 0; y < REAL_H; y++)
        for (int x = 0; x < REAL_W; x++)
            sand_set(s, x, y, ((x + y) & 1) ? CELL_MAKE(MAT_SMOKE, 8)
                                             : CELL_MAKE(MAT_STEAM, 8));
    sand_set(s, REAL_W / 2, REAL_H - 1, CELL_MAKE(MAT_FIRE, 8));
}

/* The scene above really is still a gas screen at the end of the measured
 * window, not one that quietly emptied itself into something else - and
 * cells are conserved throughout, the same setup check the two full-screen
 * fire tests below make of their own scenes. */
static void test_the_smoke_and_steam_scene_stays_a_gas_screen(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&s, blocks);
    /* This scene's whole point is a screen where every cell is one gas
     * or the other, conserved - and reaction_t.condenses genuinely is
     * not conserving: a 2x2 patch of steam collapsing into one water
     * cell is a real, deliberate loss of three cells. Forced off so
     * that real, working feature does not read as this test's cells
     * "quietly emptying into something else". */
    sand_set_condenses(&s, 0);

    build_smoke_and_steam_scene(&s);
    const int total = REAL_W * REAL_H;

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int smoke = 0, steam = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_SMOKE)      smoke++;
            else if (m == MAT_STEAM) steam++;
        }
    }
    const int count = sand_count(&s);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, count,
        "cells must only ever convert material, never appear or vanish, "
        "across a screen of smoke and steam");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5000, smoke,
        "the scene should still be mostly smoke and steam at the end of "
        "the window - a low count means this has decayed into something "
        "the device test beside it no longer measures");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5000, steam,
        "the scene should still be mostly smoke and steam at the end of "
        "the window - a low count means this has decayed into something "
        "the device test beside it no longer measures");
}

/* A lattice of 20x24 glass-walled compartments, each a ring of glass around
 * a payload with the shatter trigger just outside it.
 *
 * Every ring is painted strictly between SAND_SHOCK_COLD and
 * SAND_SHOCK_HEAT, so no compartment is born already qualifying for a
 * crack, and the spread of ring temperatures staggers when they cross.
 *
 * Lava is a payload and never a trigger: a trigger sits in a bare
 * one-cell-wide U with nothing under it, so a liquid there drains away
 * before it tests anything. */
void build_thermal_shock_scene(sand_t *s)
{
    for (int tr = 0; tr < 24; tr++) {
        for (int tc = 0; tc < 20; tc++) {
            const int ox = tc * 9, oy = tr * 9;
            const bool family_c = (tc < 10);
            const int ring_temp = 2 + (tc % 3);   /* {2,3,4} */

            /* glass ring: perimeter of lx in [2,7], ly in [2,7] */
            for (int ly = 2; ly <= 7; ly++) {
                for (int lx = 2; lx <= 7; lx++) {
                    if (lx != 2 && lx != 7 && ly != 2 && ly != 7) {
                        continue;
                    }
                    sand_set(s, ox + lx, oy + ly,
                             CELL_MAKE(MAT_GLASS, (uint8_t)ring_temp));
                }
            }

            /* the trigger, outside the box: a U under and beside it */
            const cell_t trigger = family_c ? CELL_MAKE(MAT_WOOD, MASS_MAX)
                                            : MATX(MATX_ICE);
            for (int ly = 2; ly <= 8; ly++) {
                sand_set(s, ox + 1, oy + ly, trigger);
                sand_set(s, ox + 8, oy + ly, trigger);
            }
            for (int lx = 2; lx <= 7; lx++) {
                sand_set(s, ox + lx, oy + 8, trigger);
            }

            /* the payload, inside */
            const bool low = (tr & 1) != 0;
            const cell_t payload = family_c
                ? (low ? MATX(MATX_ICE) : CELL_MAKE(MAT_SNOW, MASS_MAX))
                : (low ? CELL_MAKE(MAT_WOOD, MASS_MAX)
                       : CELL_MAKE(MAT_LAVA, MASS_MAX));
            const int ly0 = low ? 5 : 3;
            for (int ly = ly0; ly <= ly0 + 1; ly++) {
                for (int lx = 3; lx <= 6; lx++) {
                    sand_set(s, ox + lx, oy + ly, payload);
                }
            }
        }
    }
}

/* A bitset, not a byte per cell: this is the only test in the file needing
 * a second full-grid buffer alongside `big`, and the device has about 68 KB
 * of heap left once the display framebuffer is carved out of it - two
 * 41,216-byte grids do not fit there, 5,152 bytes do. */
#define EVER_CULLET_BYTES \
    (((size_t)REAL_W * (size_t)REAL_H + 7) / 8)

static inline bool ever_cullet_get(const uint8_t *mask, size_t idx)
{
    return (mask[idx >> 3] >> (idx & 7)) & 1u;
}

/* Sets the bit for `idx` and reports whether it was actually clear
 * beforehand, so callers can count new cullet without a second pass over
 * the mask. */
static inline bool ever_cullet_set(uint8_t *mask, size_t idx)
{
    const uint8_t bit = (uint8_t)(1u << (idx & 7));
    const bool was_clear = (mask[idx >> 3] & bit) == 0;
    mask[idx >> 3] |= bit;
    return was_clear;
}

/* Cold arriving at hot glass (step_one_cold_cell()) and heat arriving at
 * cold glass (try_heat_transform()) are separate code paths that have broken
 * independently. The counters below stand in for each direction: they count
 * its precondition, which takes no roll once it holds and so is a fact about
 * the board rather than a probability. A standing precondition is no promise
 * those panes break next step, which is why the assertions grade how many
 * STEPS it stands, not a count. */
static void test_the_thermal_shock_scene_shatters_in_both_directions(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t *ever_cullet = malloc(EVER_CULLET_BYTES);
    const bool have_all = (big != NULL && blocks != NULL &&
                            ever_cullet != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(ever_cullet);
        TEST_FAIL_MESSAGE("need a grid, a block map and a one-bit-per-cell "
                           "cullet mask for the thermal shock scene, and "
                           "at least one of the three failed to allocate");
    }
    memset(ever_cullet, 0, EVER_CULLET_BYTES);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_thermal_shock_scene(&s);
    const int painted = sand_count(&s);

    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };

    int d1_steps_nonzero = 0, d2_steps_nonzero = 0;
    int sticky_total_before = 0;
    int third_gain[3] = { 0, 0, 0 };

    /* Ten steps, graded by charging each step's new cullet to one third of
     * the window - the shortest window whose LAST third still earns a real
     * share. Measured, ten splits 54.1 / 28.9 / 17.0 percent; twelve,
     * fifteen and twenty push the tail to 11.8, 11.5 and 13.6, and eight
     * leaves it at 11.0. The step count and the /3 below are one decision. */
    for (int step = 1; step <= 10; step++) {
        sand_step(&s, 0, 1000, 0);

        int d1 = 0, d2 = 0;
        for (int y = 0; y < REAL_H; y++) {
            for (int x = 0; x < REAL_W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) != MAT_GLASS) {
                    continue;
                }
                const int v = CELL_VARIANT(c);
                bool near_chiller = false, near_burner = false;
                for (int d = 0; d < 4; d++) {
                    const int nx = x + dx[d], ny = y + dy[d];
                    if ((unsigned)nx >= (unsigned)REAL_W ||
                        (unsigned)ny >= (unsigned)REAL_H) {
                        continue;
                    }
                    const cell_t n = sand_at(&s, nx, ny);
                    if (CELL_IS_EMPTY(n)) {
                        continue;
                    }
                    if (reaction_of(n)->chills != 0)  near_chiller = true;
                    if (cell_is_burning(n))           near_burner  = true;
                }
                if (v >= SAND_SHOCK_HEAT && near_chiller) d1++;
                if (v <= SAND_SHOCK_COLD && near_burner)  d2++;
            }
        }
        if (d1 > 0) d1_steps_nonzero++;
        if (d2 > 0) d2_steps_nonzero++;

        /* Cullet is not inert - sand.heats_to is MAT_GLASS, so a fallen
         * shard near a hot payload re-fuses and can crack again - and a
         * live per-step delta goes negative the moment it does, hiding the
         * churn this scene exists to show. A mask that only ever grows
         * keeps "new cullet this third" non-negative. */
        int sticky_total = sticky_total_before;
        for (int y = 0; y < REAL_H; y++) {
            for (int x = 0; x < REAL_W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) == MAT_SAND &&
                    CELL_VARIANT(c) >= SAND_CULLET_BASE) {
                    if (ever_cullet_set(ever_cullet,
                                         (size_t)y * REAL_W + (size_t)x)) {
                        sticky_total++;
                    }
                }
            }
        }
        int third = step - 1;
        third /= 3;
        if (third > 2) third = 2;
        third_gain[third] += sticky_total - sticky_total_before;
        sticky_total_before = sticky_total;
    }

    int cullet_left = 0, cullet_right = 0;
    int water = 0, steam = 0, fire = 0, matx_plant = 0, heat_holders = 0;
    int lava_left = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            const int m = CELL_MATERIAL(c);
            const bool left_half = x < REAL_W / 2;
            if (m == MAT_SAND && CELL_VARIANT(c) >= SAND_CULLET_BASE) {
                if (left_half) cullet_left++; else cullet_right++;
            }
            if      (m == MAT_WATER) water++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_FIRE)  fire++;
            else if (m == MAT_LAVA && left_half) lava_left++;
            if (cell_is_extended(c) && CELL_VARIANT(c) == MATX_PLANT) {
                matx_plant++;
            }
            if (!CELL_IS_EMPTY(c) && reaction_of(c)->heat_ramp != 0) {
                heat_holders++;
            }
        }
    }

    /* Distinct TILES (of the 240 per half) that have gained at least one
     * cullet cell over the window - a coarser, per-compartment measure
     * that a handful of very active tiles cannot satisfy on their own,
     * unlike the raw cell counts above. */
    int distinct_left = 0, distinct_right = 0;
    for (int tr = 0; tr < 24; tr++) {
        for (int tc = 0; tc < 20; tc++) {
            bool has_cullet = false;
            for (int ly = 0; ly < 9 && !has_cullet; ly++) {
                for (int lx = 0; lx < 9 && !has_cullet; lx++) {
                    const int x = tc * 9 + lx, y = tr * 9 + ly;
                    if (x >= REAL_W || y >= REAL_H) {
                        continue;
                    }
                    if (ever_cullet_get(ever_cullet,
                                         (size_t)y * REAL_W + (size_t)x)) {
                        has_cullet = true;
                    }
                }
            }
            if (has_cullet) {
                if (tc < 10) distinct_left++; else distinct_right++;
            }
        }
    }

    const int sand_count_now = sand_count(&s);
    const bool temperature_flag  = s.may_have_temperature;
    const bool heat_holder_flag  = s.may_have_heat_holder;

    free(big);
    free(blocks);
    free(ever_cullet);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, d1_steps_nonzero,
        "the cold-onto-hot direction (step_one_cold_cell()'s shock "
        "precondition) must really be firing across most of the window - "
        "if this is low, family H's ice trigger and family C's own cold "
        "payload have stopped reaching hot glass and this scene is no "
        "longer exercising the direction it claims to");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, d2_steps_nonzero,
        "the hot-onto-cold direction (try_heat_transform()'s shock "
        "precondition) must really be firing across most of the window - "
        "kept as a SEPARATE assertion from the one above for the same "
        "reason test_heat_arriving_at_frosted_glass_cracks_it is kept "
        "separate from its mirror: the two directions are different code "
        "and break independently");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, cullet_left,
        "the left half of the lattice must be producing cullet in "
        "quantity, not just in one corner of it");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, cullet_right,
        "the right half of the lattice must be producing cullet in "
        "quantity, not just in one corner of it");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(100, distinct_left,
        "shattering must be spread across many compartments in the left "
        "half, not concentrated in a few tiles that happen to be "
        "unusually active");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(100, distinct_right,
        "shattering must be spread across many compartments in the right "
        "half, not concentrated in a few tiles that happen to be "
        "unusually active");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, third_gain[0],
        "the first third of the window must already be producing new "
        "cullet - see the sticky-mask comment above for why this counts "
        "distinct cells that have EVER been cullet rather than a live "
        "snapshot, which would undercount once fallen shards start "
        "re-fusing near the payload's heat");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, third_gain[1],
        "the middle third of the window must still be producing new "
        "cullet - shattering has to be staggered across the window "
        "rather than all landing in the first couple of steps");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, third_gain[2],
        "the last third of the window must still be producing new "
        "cullet - if this is low while the first third is not, the "
        "lattice went quiet early and the device benchmark beside this "
        "test is measuring a scene that has already settled");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1000, water,
        "meltwater from ice and snow must still be showing at the end of "
        "the window");
    /* LOWERED FROM 800 when cold gained the ability to conduct through a
     * medium (bd esp32c6-tov): the payload's glass now chills faster and
     * further, so less meltwater reaches it still hot, and this scene settled
     * at 774. That is a real consequence of the feature, not a regression -
     * and this floor exists to catch a scene that has gone QUIET, which 774
     * plainly has not. Kept well below the new figure so it still would. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(700, steam,
        "steam from meltwater meeting a hot payload must still be "
        "showing at the end of the window");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2000, fire,
        "fire escaping broken compartments must still be showing at the "
        "end of the window");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(painted, sand_count_now,
        "cells must never be net-destroyed - this is deliberately a "
        "FLOOR, not a conservation check: burning wood and lava both "
        "flare fresh MAT_FIRE into empty neighbours, so the live count "
        "is expected to grow past what was painted, exactly as it does "
        "in the lava stress and four-liquid scenes above, neither of "
        "which asserts conservation either");

    /* The lava stress scene's plant pin, for the same reason: this scene
     * makes meltwater and has sand about, so wet soil is reachable in
     * principle, and growth inside the window would quietly stop the device
     * benchmark's pegged frame budget describing the scene it claims to.
     * The plain cell_is_extended(c) form cannot be reused - this scene's own
     * payload is MATX(MATX_ICE), itself an extended cell. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, matx_plant,
        "the thermal shock lattice should not be growing any plants - if "
        "it is, the device test's frame budget is no longer measuring "
        "the scene it claims to, and the ice payload means the usual "
        "cell_is_extended() plant pin cannot be reused here as-is");

    /* A bound, not an exact zero: the left half's rings really do melt into
     * lava under their own payload and trigger, and only WHEN is pinned
     * here - timing that rides the same shared RNG stream every reaction on
     * the board draws from, so any new roll elsewhere shifts it. A
     * single-digit residual is that shift and is always a LOCAL
     * glass-to-lava conversion, never material crossing from the right half.
     * The regression this must still catch measured 123 left-half lava
     * cells by step 40. */
    TEST_ASSERT_LESS_THAN_MESSAGE(10, lava_left,
        "family C's rings (the left half) must not have melted into lava "
        "in bulk inside this window - a small residual is an expected "
        "RNG-timing shift (see this assertion's own comment), but this "
        "many means the window, or something else about this scene, "
        "genuinely regressed");

    /* step_one_warming_cell()'s call site is gated on r->warms,
     * may_have_temperature and may_have_heat_holder at once, so only
     * pinning all three together proves the warming path is reachable in
     * this scene rather than skipped by a gate that happens to be shut. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, steam,
        "setup for the warming-gate check below: there must be steam on "
        "the board for the gate to be worth anything");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, heat_holders,
        "setup for the warming-gate check below: there must be cells "
        "that can hold a temperature (glass, here) for the gate to be "
        "worth anything");
    TEST_ASSERT_TRUE_MESSAGE(temperature_flag,
        "may_have_temperature must be armed by this scene, or "
        "step_one_warming_cell()'s call site is never reached at all");
    TEST_ASSERT_TRUE_MESSAGE(heat_holder_flag,
        "may_have_heat_holder must be armed by this scene too - "
        "may_have_temperature alone is not the gate; a previous tuning "
        "round added this second flag specifically because the first one "
        "arms itself the moment anything with a temperature is painted, "
        "which is not the same claim as there being something around "
        "that can actually hold one");
}

/* A small fire on a board that mostly cannot react. The other reaction
 * scenes measure what a reacting cell COSTS; this measures what is paid for
 * cells that cannot react at all: sand_step_reactions() early-outs only on a
 * board-wide flag test, so one lit match makes it walk all 41,216 cells
 * every step. Roughly 2% of this board can do anything.
 *
 * Sand for the bulk, not stone: a settled powder is the case the main
 * sweep's own block skip already handles. */
void build_campfire_scene(sand_t *s)
{
    const int ground_top = (REAL_H * 9) / 20;       /* sand fills ~55% */
    const int pile_w     = 24;
    const int pile_h     = 8;
    const int pile_x0    = (REAL_W - pile_w) / 2;
    const int pile_y1    = ground_top;              /* sits on the sand */
    const int pile_y0    = pile_y1 - pile_h;

    for (int y = ground_top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(s, x, y, SAND);
        }
    }

    for (int y = pile_y0; y < pile_y1; y++) {
        for (int x = pile_x0; x < pile_x0 + pile_w; x++) {
            sand_set(s, x, y, WOOD);
        }
    }

    /* Lit along the top of the pile, not buried in it: a fire needs air, and
     * burying it would measure smothering instead of burning. */
    for (int x = pile_x0; x < pile_x0 + pile_w; x++) {
        sand_set(s, x, pile_y0 - 1, FIRE);
    }
}

/* A heat source left running, against build_thermal_shock_scene()'s burst of
 * damage - the two shapes of thermal load.
 *
 * conduct_heat() attenuates at roughly 0.86 per cell of depth, so the slab's
 * thickness is the THROTTLE on the boil: at 11 rows the rate holds across
 * the window instead of exhausting the basin partway through.
 *
 * Lava never decays and is the steady burner, wood (burn_decay 24) the
 * ember. Enclosing both leaves flare almost nowhere to put fresh fire. */
void build_boiler_scene(sand_t *s)
{
    const int burn_h = 4, slab_h = 11, water_h = 30;
    const int burn_top  = REAL_H - burn_h;          /* 220 */
    const int slab_top  = burn_top - slab_h;        /* 209 */
    const int water_top = slab_top - water_h;       /* 179 */

    /* basin walls, full depth */
    for (int y = water_top; y < REAL_H; y++) {
        for (int x = 0; x < 3; x++) {
            sand_set(s, x, y, STONE);
            sand_set(s, REAL_W - 1 - x, y, STONE);
        }
    }
    /* two burners under one slab */
    for (int y = burn_top; y < REAL_H; y++) {
        for (int x = 3; x <= REAL_W - 4; x++) {
            sand_set(s, x, y, (x < REAL_W / 2) ? CELL_MAKE(MAT_LAVA, MASS_MAX)
                                               : CELL_MAKE(MAT_WOOD, MASS_MAX));
        }
    }
    /* the slab */
    for (int y = slab_top; y < burn_top; y++) {
        for (int x = 3; x <= REAL_W - 4; x++) {
            sand_set(s, x, y, STONE);
        }
    }
    /* the water */
    for (int y = water_top; y < slab_top; y++) {
        for (int x = 3; x <= REAL_W - 4; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* The boiler keeps boiling for the whole window rather than front-loading
 * its output and going quiet. 20 settle steps: at ten the board is still
 * filling with the first flush of steam (295 cells of it), at twenty it
 * boils at a rate that then holds.
 *
 * Four sampled intervals, not one before/after pair, so a basin that boils
 * hard and then tails off is caught. Measured, the quarters lose 27, 34, 26
 * and 25 cells of water; the assertions sit at 12, roughly half the measured
 * minimum. */
static void test_the_boiler_scene_keeps_boiling_across_the_window(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    const bool have_all = (big != NULL && blocks != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        TEST_FAIL_MESSAGE("need a grid and a block map for the boiler "
                           "scene, and at least one of the two failed to "
                           "allocate");
    }

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 43u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    /* Condensation is not one-for-one the way boiling is - a 2x2 patch of
     * steam collapses into a SINGLE water cell, a net loss of three - so
     * left on it would eventually violate the sand_count_now floor below
     * for a reason unrelated to what this test measures. */
    sand_set_condenses(&s, 0);
    /* A burst partway through the window disrupts the slab that is supposed
     * to hold steady for the whole test. This scene measures boiling, not
     * bursting. */
    sand_set_lava_burst(&s, 0);

    build_boiler_scene(&s);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int water_window_start = 0, steam_window_start = 0;
    int water_left_start = 0, water_right_start = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_WATER) {
                water_window_start++;
                if (x < REAL_W / 2) water_left_start++; else water_right_start++;
            } else if (m == MAT_STEAM) {
                steam_window_start++;
            }
        }
    }
    const int count_at_window_start = sand_count(&s);

    int water_at_checkpoint[5];
    water_at_checkpoint[0] = water_window_start;
    const int checkpoints[4] = { 7, 15, 22, 30 };
    int next_checkpoint = 0;

    for (int i = 1; i <= 30; i++) {
        sand_step(&s, 0, 1000, 0);
        if (next_checkpoint < 4 && i == checkpoints[next_checkpoint]) {
            int w = 0;
            for (int y = 0; y < REAL_H; y++) {
                for (int x = 0; x < REAL_W; x++) {
                    if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) w++;
                }
            }
            water_at_checkpoint[next_checkpoint + 1] = w;
            next_checkpoint++;
        }
    }

    int water = 0, steam = 0, stone = 0, burning_wood = 0;
    int stone_off_ambient = 0, extended = 0;
    int water_left = 0, water_right = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            const int m = CELL_MATERIAL(c);
            const bool left_half = x < REAL_W / 2;
            if (m == MAT_WATER) {
                water++;
                if (left_half) water_left++; else water_right++;
            } else if (m == MAT_STEAM) {
                steam++;
            } else if (m == MAT_STONE) {
                stone++;
                if (CELL_VARIANT(c) != SAND_AMBIENT_HEAT) stone_off_ambient++;
            } else if (m == MAT_WOOD && cell_is_burning(c)) {
                burning_wood++;
            }
            if (cell_is_extended(c)) extended++;
        }
    }

    const int sand_count_now = sand_count(&s);
    const bool temperature_flag = s.may_have_temperature;
    const bool heat_holder_flag = s.may_have_heat_holder;

    free(big);
    free(blocks);

    for (int q = 0; q < 4; q++) {
        const int lost = water_at_checkpoint[q] - water_at_checkpoint[q + 1];
        char why[320];
        snprintf(why, sizeof why,
                 "the boiler must still be boiling in quarter %d of the "
                 "measured window (lost %d cells of water there) - a "
                 "quiet quarter means the basin exhausted its heat or "
                 "its water before the window was over, and this is "
                 "meant to be a STEADY state, not a transient", q, lost);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(12, lost, why);
    }

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5000, water,
        "the basin must not be exhausted by the end of the window - "
        "measured, 5158 cells of water are left, 98% of what the window "
        "started with (reaction_t.boils makes water resist conducted-heat "
        "boiling now, see material.c's own row), which is what makes "
        "this a steady state rather than another transient like the "
        "thermal shock lattice above");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(150, steam,
        "steam production must be sustained through to the end of the "
        "window");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(steam_window_start, steam,
        "steam must have grown over the measured window, not merely be "
        "present - a count that matches the window's starting steam "
        "would mean production had already stalled by the time "
        "measurement began");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(200, stone_off_ambient,
        "the slab must be genuinely carrying a temperature by the end of "
        "the window - this is the proof that heat is arriving at the "
        "water by conduction THROUGH the slab, not by some other route");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(300, burning_wood,
        "the second burner must still be alight at the end of the "
        "window, or the \"two heat sources\" claim this scene makes only "
        "holds for part of it");

    /* Both halves boil, not just the lava-fed one - the wood-fed half's
     * ember has to be pulling its own weight too. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(30,
        water_left_start - water_left,
        "the left (lava-fed) half of the basin must have lost a real "
        "amount of water over the window");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(30,
        water_right_start - water_right,
        "the right (wood-fed) half of the basin must have lost a real "
        "amount of water over the window - a low loss here would mean "
        "the ember burner is not pulling its share and the \"two "
        "burners\" claim only holds on one side");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(count_at_window_start, sand_count_now,
        "cells must never be net-destroyed here - a FLOOR, not the exact "
        "conservation test_a_screen_of_smoke_and_steam_fits_in_the_frame_"
        "budget makes of its own scene, because this one has a burner in "
        "it. The stone enclosure (side walls, slab, grid floor) leaves "
        "flare almost nowhere to put fresh fire, and measured the count "
        "holds at its window-start 8280 for twenty steps before the boil "
        "opens gaps above the slab and it climbs to 8293 - see "
        "build_boiler_scene()'s comment. Water boiling to steam is "
        "one-for-one, and condensation (a real, DIFFERENT loss of three "
        "cells per event - see the sand_set_condenses() call above) is "
        "forced off in this scene precisely so it cannot fire here, so a "
        "count BELOW the window's start means cells went missing for a "
        "reason this scene has no business producing, which is a "
        "different and much worse thing than flare adding a few");

    /* Same reasoning as the thermal shock lattice's plant pin above, and
     * the plain cell_is_extended() form works here, unlike there,
     * because this scene paints no extended material at all. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended,
        "the boiler scene should not contain any extended cells - if it "
        "does, either the scene changed to paint one on purpose (update "
        "this test) or something is growing that this benchmark was "
        "never meant to measure");

    /* The same check on step_one_warming_cell()'s three-part call-site
     * gate as the thermal shock lattice's host test above - the branch a
     * previous tuning round added may_have_heat_holder for. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, steam,
        "setup for the warming-gate check below: there must be steam on "
        "the board for the gate to be worth anything");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, stone,
        "setup for the warming-gate check below: there must be cells "
        "that can hold a temperature (stone, here) for the gate to be "
        "worth anything");
    TEST_ASSERT_TRUE_MESSAGE(temperature_flag,
        "may_have_temperature must be armed by this scene, or "
        "step_one_warming_cell()'s call site is never reached at all");
    TEST_ASSERT_TRUE_MESSAGE(heat_holder_flag,
        "may_have_heat_holder must be armed by this scene too - see the "
        "thermal shock lattice's host test above for why this second "
        "flag is not redundant with the first");
}

/* Painted rather than grown: this scene exists for the RENDER path, and the
 * shading asks only whether a cell is unlit wood with a leaf near it.
 *
 * Both costs are here because they pull in opposite directions - trunk wood
 * AWAY from leaves pays the full five-slot scan and finds nothing, the
 * scan's worst case, while canopy wood BESIDE leaves short-circuits early
 * but takes the tint and wakes its row on every gust tick. A canopy alone
 * would flatter the scan, a bare trunk the dirtying. */
void build_tree_grove_scene(sand_t *s)
{
    const int ground = (REAL_H * 4) / 5;

    for (int y = ground; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_DIRT, 0));
        }
    }

    /* Four trees across the width, tall enough that trunk and canopy land in
     * different rows - the dirtying is per row, so a tree squashed into a few
     * rows would understate it. */
    for (int t = 0; t < TREE_GROVE_TREES; t++) {
        const int cx = (REAL_W * (2 * t + 1)) / (2 * TREE_GROVE_TREES);
        const int top = ground - TREE_GROVE_HEIGHT;

        for (int y = top; y < ground; y++) {
            for (int dx = -1; dx <= 1; dx++) {
                sand_set(s, cx + dx, y, CELL_MAKE(MAT_WOOD, 0));
            }
        }

        /* Canopy: a disc of leaf with woody branch cells threaded through it,
         * so wood-beside-leaf is common rather than a thin rim. */
        for (int dy = -TREE_GROVE_CANOPY_R; dy <= TREE_GROVE_CANOPY_R; dy++) {
            for (int dx = -TREE_GROVE_CANOPY_R; dx <= TREE_GROVE_CANOPY_R; dx++) {
                if (dx * dx + dy * dy > TREE_GROVE_CANOPY_R * TREE_GROVE_CANOPY_R) {
                    continue;
                }
                const int x = cx + dx, y = top + dy;
                if ((unsigned)x >= (unsigned)REAL_W || (unsigned)y >= (unsigned)REAL_H) {
                    continue;
                }
                const bool branch = ((dx + dy) & 3) == 0;
                sand_set(s, x, y, branch ? CELL_MAKE(MAT_WOOD, 0)
                                         : MATX(MATX_LEAF));
            }
        }
    }
}

/* The plant code - anchored()'s BFS, find_water(), the root roll - only runs
 * for a cell already standing on damp soil, so every other scene here prices
 * it at zero.
 *
 * Spacing is the yield knob: seeds too close exhaust the same soil and stop
 * spending moisture, while spaced out each has its own damp column and keeps
 * growing. A timed step wants many plants busy at once, not one tall one. */
#define PLANT_BED_SEED_SPACING 8

void build_plant_bed_scene(sand_t *s)
{
    const int bed_top  = (REAL_H * 7) / 10;   /* bottom 30% is ground */
    const int dirt_top = REAL_H - (REAL_H - bed_top) / 2;

    for (int y = bed_top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            /* Dirt is the CAP, not the fill: roots weld into soil, and soil is
             * what holds the moisture they drink, so the surface has to be
             * dirt however the rest of the bed is made. */
            sand_set(s, x, y, y < dirt_top
                                  ? CELL_SOIL(MAT_DIRT, 1, 0)
                                  : CELL_MAKE(MAT_SAND, 0));
        }
    }

    for (int x = PLANT_BED_SEED_SPACING / 2; x < REAL_W;
         x += PLANT_BED_SEED_SPACING) {
        sand_set(s, x, bed_top - 1, MATX(MATX_PLANT));
    }

    plant_bed_rain(s);
}

/* SEPARATE FROM THE BUILDER because one pour is not enough: the bed drinks a
 * fall of rain dry in a few hundred steps, and a plant that runs out of
 * moisture simply stops - which is the same exhaustion suite_sand_roots.c
 * works around by replanting. Callers pour again partway through settling so
 * the timed steps land on a bed that is still growing, not one that finished.
 *
 * Poured ABOVE the seeds rather than onto them: water dropped on a seed
 * buries it before it can grow. */
void plant_bed_rain(sand_t *s)
{
    const int bed_top = (REAL_H * 7) / 10;

    for (int y = bed_top - 8; y < bed_top - 4; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_IS_EMPTY(sand_at(s, x, y))) {
                sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
            }
        }
    }
}

/* Sand and dirt in equal amounts under water, the one scene here that soaks.
 *
 * Damp dirt is what keeps the reactions pass alive: sand_step_reactions()
 * clears may_have_moisture the moment a pass finds nothing to do, and only a
 * cell WRITE re-arms it, never liquid movement - so a board of nothing but
 * water goes silent after its first pass. Callers must also
 * sand_set_soak(), off by default; left off this measures liquid movement
 * and nothing else. */
void build_wet_earth_scene(sand_t *s)
{
    const int earth_top = (REAL_H * 2) / 5;    /* bottom three fifths,
                                                 * 135 rows */
    /* EQUAL CONTACT: a single-cell checkerboard rather than stacked halves
     * or stripes, so water meets both materials in the same proportion at
     * every column and depth instead of one sitting nearer the surface and
     * racing the other's soak rate by geometry. 12,420 cells of each. */
    for (int y = earth_top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const material_id_t m = ((x + y) & 1) ? MAT_SAND : MAT_DIRT;
            sand_set(s, x, y, CELL_MAKE(m, 0));
        }
    }

    /* Flush on the earth, since a ten-row gap measured zero wet dirt cells
     * across four hundred steps - the first pass clears may_have_moisture
     * before contact happens. The center half only, because a full-width
     * slab is already at rest: spreading sideways to the flanks is the
     * settling this scene is named for, full-width contact landing between
     * step 30 and step 31 (three seeds). */
    const int water_h = earth_top / 2;
    const int water_top = earth_top - water_h;
    const int cx0 = REAL_W / 4, cx1 = (REAL_W * 3) / 4;
    for (int y = water_top; y < earth_top; y++) {
        for (int x = cx0; x < cx1; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* Water is graded as summed variant, not cell count: a count only moves when
 * a WHOLE unit is used up, so it steps in noisy jumps, while the mass sum
 * falls by exactly what soaking took every step. */
static void wet_earth_scan(const sand_t *s, int *water_mass, int *dirt_count,
                            int *moisture_sum, int *extended_count)
{
    int wm = 0, dirt = 0, moist = 0, ext = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(s, x, y);
            const int m = CELL_MATERIAL(c);
            if (m == MAT_WATER) {
                wm += CELL_VARIANT(c);
            } else if (m == MAT_DIRT) {
                dirt++;
                moist += CELL_MOISTURE(c);
            }
            if (cell_is_extended(c)) {
                ext++;
            }
        }
    }
    *water_mass = wm;
    *dirt_count = dirt;
    *moisture_sum = moist;
    *extended_count = ext;
}

/* Counters taken MID-FLIGHT: what a scene is BUILT from is not what it
 * CONTAINS once running.
 *
 * 35 settle steps is build_wet_earth_scene()'s full-width-contact milestone
 * plus a margin, and the milestone is asserted directly rather than trusting
 * the step count to have reached it.
 *
 * Measured per quarter (three seeds): 274-373 units of water mass lost,
 * 190-271 moisture gained, and 48/51/31/54 dirt on the fixed seed. Each
 * floor below sits at roughly half the worst measured quarter. */
static void test_the_wet_earth_scene_keeps_percolating_across_the_window(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    const bool have_all = (big != NULL && blocks != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        TEST_FAIL_MESSAGE("need a grid and a block map for the wet earth "
                           "scene, and at least one of the two failed to "
                           "allocate");
    }

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 53u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_wet_earth_scene(&s);

    int painted_sand = 0, painted_dirt = 0, painted_water = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_SAND)       painted_sand++;
            else if (m == MAT_DIRT)  painted_dirt++;
            else if (m == MAT_WATER) painted_water++;
        }
    }

    for (int i = 0; i < 35; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* The milestone the settle allowance is chosen against - see this
     * scene's own comment above. Every column that has water resting
     * directly on earth counts, the same adjacency the mixed scene's own
     * coverage test uses above. */
    int touching_columns = 0;
    for (int x = 0; x < REAL_W; x++) {
        for (int y = 0; y < REAL_H - 1; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) != MAT_WATER) {
                continue;
            }
            const int below = CELL_MATERIAL(sand_at(&s, x, y + 1));
            if (below == MAT_SAND || below == MAT_DIRT) {
                touching_columns++;
                break;
            }
        }
    }

    int water_mass[5], dirt_count[5], moisture_sum[5], extended_unused;
    wet_earth_scan(&s, &water_mass[0], &dirt_count[0], &moisture_sum[0],
                   &extended_unused);

    const int checkpoints[4] = { 7, 15, 22, 30 };
    int next_checkpoint = 0;
    for (int i = 1; i <= 30; i++) {
        sand_step(&s, 0, 1000, 0);
        if (next_checkpoint < 4 && i == checkpoints[next_checkpoint]) {
            wet_earth_scan(&s, &water_mass[next_checkpoint + 1],
                           &dirt_count[next_checkpoint + 1],
                           &moisture_sum[next_checkpoint + 1],
                           &extended_unused);
            next_checkpoint++;
        }
    }

    int extended_final;
    int water_mass_final, dirt_count_final, moisture_sum_final;
    wet_earth_scan(&s, &water_mass_final, &dirt_count_final,
                   &moisture_sum_final, &extended_final);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(painted_dirt, painted_sand,
        "sand and dirt must be painted in exactly equal amounts, or the "
        "\"equal contact\" claim in build_wet_earth_scene()'s comment is "
        "not actually what this scene does");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4000, painted_water,
        "enough water must be painted to percolate through the bed, not "
        "merely wet its surface");
    TEST_ASSERT_EQUAL_INT_MESSAGE(REAL_W, touching_columns,
        "the water must have finished spreading to touch earth across the "
        "full width by the end of the 35-step settle allowance - see "
        "build_wet_earth_scene()'s comment: measured, full-width contact "
        "lands at step 30 or 31, well inside this allowance");

    for (int q = 0; q < 4; q++) {
        const int mass_lost     = water_mass[q] - water_mass[q + 1];
        const int dirt_gained   = dirt_count[q + 1] - dirt_count[q];
        const int moist_gained  = moisture_sum[q + 1] - moisture_sum[q];

        char why[224];
        snprintf(why, sizeof why,
                 "quarter %d of the measured window must still show water "
                 "being consumed by soaking (lost %d units of mass there) "
                 "- a quiet quarter means the percolation this scene "
                 "exists to measure has stalled", q, mass_lost);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(150, mass_lost, why);

        snprintf(why, sizeof why,
                 "quarter %d must still be converting sand to dirt (gained "
                 "%d dirt cells there) - the sand-to-soil claim this scene "
                 "makes only holds if it keeps happening for the whole "
                 "window, not just at the start", q, dirt_gained);
        /* 20, not the original 50 - see this test's own top comment for
         * why PART 2's percolation slowdown moved this specific floor and
         * not the other two. */
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(20, dirt_gained, why);

        snprintf(why, sizeof why,
                 "quarter %d must still be raising dirt's own moisture "
                 "(gained %d units of it there), not merely converting "
                 "fresh sand in at the minimum starting level", q,
                 moist_gained);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(100, moist_gained, why);
    }

    /* Same reasoning as the lava stress, thermal shock and boiler scenes'
     * own plant pins above: the plant materials are under active
     * development, and if growth ever starts happening inside this
     * measured window, the device benchmark beside this test would
     * quietly stop measuring the scene it claims to. This scene paints no
     * wood or seed anywhere, so it should hold at zero more easily than
     * any of the three it borrows the reasoning from. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended_final,
        "the wet earth scene should not contain any extended cells - if "
        "it does, either something is growing that this benchmark was "
        "never meant to measure, or the scene changed to paint one on "
        "purpose (update this test)");
}

/* --- water over lava: a continuous pour onto a sealed pool --------------
 *
 * A pour, not a synthetic max-chance stress scene: production never pins
 * the burst-gate chance, so forcing it would cost more but not be
 * representative. Chains quench, cool_off_chain() (sand_reactions.c) and
 * the burst gate, so a regression in any shows up here.
 *
 * DO NOT compare these numbers to an older capture under this name: a
 * removed vent-spam mechanism (bd esp32c6-0f2) measured a different,
 * costlier scene here. */

/* One full-width seam, not many small sealed pockets: nothing here needs to
 * stay sealed, and a seam this wide puts as many lava cells in simultaneous
 * contact with water as the grid can hold - the worst case for the quench
 * pass. The crust it leaves covers the pool just as completely, which is the
 * worst case for the burst gate. */
#define WATER_LAVA_LAVA_TOP (REAL_H / 2)

/* Same real device impulse budget the vent-spam scene this replaces used
 * (that scene's own comment, git history, has the full account) - the
 * app's own buffer is sized APP_IMPULSE_MAX (2048), and this scene should
 * be fighting the same memory ceiling a real device pour actually has,
 * not a looser one a differently-sized test buffer would hide. */
/* WATER_LAVA_IMPULSE_MAX moved to suite_sand_scenes.h - build_water_over_lava_scene()'s test above and the frame-budget test in suite_sand_perf.c both need it. */

/* sand_set_lava_cooloff()/sand_set_lava_burst() forced to their maximum,
 * the same reasoning the vent-spam scene this replaces gave for forcing
 * sand_set_vent_chance(255) (git history): production leaves both
 * deliberately rare (SAND_LAVA_COOLOFF_CHANCE, SAND_LAVA_BURST_CHANCE,
 * sand.h), and a benchmark that mostly rolls "no" would not be measuring
 * the mechanisms it claims to. Quench itself has no chance to force - a
 * burning liquid touching a quenching one always converts - so only
 * these two need it. */
void build_water_over_lava_scene(sand_t *s)
{
    sand_set_lava_cooloff(s, 255);
    sand_set_lava_burst(s, 255);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = (y < WATER_LAVA_LAVA_TOP)
                                  ? CELL_MAKE(MAT_WATER, MASS_MAX)
                                  : CELL_MAKE(MAT_LAVA, MASS_MAX);
            sand_set(s, x, y, c);
        }
    }
}

/* One independent signal per claimed path, each resting on this scene having
 * no other source for what it counts:
 *
 * - any stone proves quench fired;
 * - stone beyond REAL_W - one seam's worth, all quench alone can reach in a
 *   pass - proves cool_off_chain() carried it past direct contact;
 * - any fire proves the burst path fired, since quench only ever produces
 *   stone (material.c's quench_to) and the core fill is the only other
 *   source. */
static void test_the_water_over_lava_scene_reaches_the_quench_cooloff_and_burst_paths_it_claims(void)
{
    uint8_t *big    = malloc((size_t)REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)WATER_LAVA_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 59u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&s, impulses, WATER_LAVA_IMPULSE_MAX);

    build_water_over_lava_scene(&s);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int stone = 0, fire = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_STONE) stone++;
            else if (m == MAT_FIRE) fire++;
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, stone,
        "water touching lava must convert some of it to stone - if none "
        "appeared, quench itself stopped firing");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(REAL_W, stone,
        "the stone count must exceed one seam's worth (REAL_W) of direct "
        "contact - if it does not, cool_off_chain() stopped carrying the "
        "conversion into lava that was never itself touching water");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fire,
        "no burst means no fire anywhere in this scene - ordinary quench "
        "only ever produces stone, so a zero count here means the burst "
        "gate never fired at all");
}

/* --- gunpowder basin: a brush-drawn vessel of gunpowder, lit once ------
 * Closes half of bd esp32c6-4d9: a lit pile chain-detonating via
 * find_lit_two_by_two()/sand_explode() (sand_reactions.c) has never been
 * profiled on device, unlike the gas-pocket and covered-lava bursts it
 * mirrors. */

/* WALLS MUST BE BRUSH-DRAWN, NOT A CLEAN RECTANGLE like
 * build_dune_in_a_vessel_scene's (suite_sand_dune_blast.c). A 2026-09-03
 * confinement rule found zero eligible cells over 5000 steps on a clean
 * rectangle, 16 on a brush-drawn one - confinement is shape-dependent. */
#define GUNPOWDER_BASIN_INT_X0     67
#define GUNPOWDER_BASIN_INT_W      50
#define GUNPOWDER_BASIN_INT_Y0     150
#define GUNPOWDER_BASIN_INT_H      45
#define GUNPOWDER_BASIN_WALL_STEP  3

/* A stroke's own natural variation in radius, 2-4 - fixed by step index
 * rather than rolled, since this scene has to reproduce byte-identically
 * run to run, the same as every other scene in this file. */
static int gunpowder_basin_brush_radius(int step_index)
{
    static const int radii[3] = { 2, 3, 4 };
    return radii[step_index % 3];
}

/* One run of the U-shaped wall, as overlapping sand_spawn() discs
 * GUNPOWDER_BASIN_WALL_STEP cells apart. `vertical` true draws a
 * vertical run (fixed x, `v` walks y); false draws a horizontal one. */
static void gunpowder_basin_wall_run(sand_t *s, int fixed, int lo, int hi,
                                      bool vertical)
{
    int i = 0;
    for (int v = lo; v <= hi; v += GUNPOWDER_BASIN_WALL_STEP, i++) {
        const int r = gunpowder_basin_brush_radius(i);
        if (vertical) {
            sand_spawn(s, fixed, v, r, MAT_STONE);
        } else {
            sand_spawn(s, v, fixed, r, MAT_STONE);
        }
    }
}

/* FIRE SITS ON THE PILE'S SURFACE, open sky above - the shape
 * test_fire_beside_dry_gunpowder_lights_it proves ignites
 * (suite_sand_gunpowder.c). A sealed pocket measured WORSE: mobility
 * 96/256 leaves it sitting still, rolling every flammable side, more
 * often than it drifts away. */

/* A brush-drawn vessel of dry gunpowder, lit near the top, with water,
 * sand, dirt, oil, wood, acid and metal placed within the blast's reach
 * outside the walls - the aftermath cascade is the point, not just the
 * detonation. Plants excluded - see this scene's own coverage test. */
void build_gunpowder_basin_scene(sand_t *s)
{
    const int ix0 = GUNPOWDER_BASIN_INT_X0;
    const int ix1 = GUNPOWDER_BASIN_INT_X0 + GUNPOWDER_BASIN_INT_W;
    const int iy0 = GUNPOWDER_BASIN_INT_Y0;
    const int iy1 = GUNPOWDER_BASIN_INT_Y0 + GUNPOWDER_BASIN_INT_H;

    /* U-shaped wall - left side, right side, floor - open at the top,
     * the shape a player drags a brush along to build a vessel. */
    gunpowder_basin_wall_run(s, ix0 - 1, iy0 - 4, iy1 + 4, true);
    gunpowder_basin_wall_run(s, ix1,     iy0 - 4, iy1 + 4, true);
    gunpowder_basin_wall_run(s, iy1,     ix0 - 4, ix1 + 4, false);

    const int fx = ix0 + GUNPOWDER_BASIN_INT_W / 2;
    const int fy = iy0;

    /* Fill the interior with dry gunpowder - a real pour floods the
     * whole vessel, notches and bulges alike, which is exactly why the
     * wall discs above are drawn FIRST and this fill is allowed to
     * overwrite whatever bulged into the interior. The fire cell is
     * placed after, not overwritten. */
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            if (x >= fx && x < fx + GUNPOWDER_BASIN_SPARK &&
                y >= fy && y < fy + GUNPOWDER_BASIN_SPARK) {
                continue;
            }
            sand_set(s, x, y, GUNPOWDER_CELL(0));
        }
    }
    for (int y = fy; y < fy + GUNPOWDER_BASIN_SPARK; y++) {
        for (int x = fx; x < fx + GUNPOWDER_BASIN_SPARK; x++) {
            sand_set(s, x, y, FIRE);
        }
    }

    /* A SHELF UNDER EACH OUTSIDE STACK, drawn before they are painted.
     * Without it the stacks stand on nothing: 900 of their 1,800 cells
     * are liquid, so the window's first third times a waterfall landing
     * alongside the detonation it exists to measure. */
    gunpowder_basin_wall_run(s, iy1, ix0 - 26, ix0, false);
    gunpowder_basin_wall_run(s, iy1, ix1, ix1 + 26, false);

    /* Outside, left of the vessel: water, then sand, then dirt, each
     * close enough for a breached wall or a flung ember to reach. */
    for (int y = iy0; y < iy0 + 15; y++)
        for (int x = ix0 - 25; x < ix0 - 5; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    for (int y = iy0 + 15; y < iy0 + 30; y++)
        for (int x = ix0 - 25; x < ix0 - 5; x++)
            sand_set(s, x, y, SAND_FIRST_SHADE);
    for (int y = iy0 + 30; y < iy1; y++)
        for (int x = ix0 - 25; x < ix0 - 5; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_DIRT, 0));

    /* Outside, right of the vessel: oil and wood - fuel for escaping
     * fire to spread into - then acid. */
    for (int y = iy0; y < iy0 + 15; y++)
        for (int x = ix1 + 5; x < ix1 + 25; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_OIL, MASS_MAX));
    for (int y = iy0 + 15; y < iy0 + 30; y++)
        for (int x = ix1 + 5; x < ix1 + 25; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_WOOD, 0));
    for (int y = iy0 + 30; y < iy1; y++)
        for (int x = ix1 + 5; x < ix1 + 25; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));

    /* Above the open mouth: a metal slab, sitting in the path of the
     * updraft of fire and thrown material a blast near the top of the
     * pile sends upward. KIND_STATIC, so resting in open air is not a
     * physics error - metal never falls in this simulation. */
    for (int y = iy0 - 25; y < iy0 - 10; y++)
        for (int x = fx - 10; x < fx + 10; x++)
            sand_set(s, x, y, MATX(MATX_METAL));
}

/* THE MEASURED WINDOW: 90 steps, no settling. Fire touches gunpowder
 * from step one, so the scene is already at its busiest the instant it
 * is painted - the same reasoning the thermal shock lattice and the
 * gas screens give for skipping a settle allowance. */

/* 90 SPLITS INTO HONEST THIRDS. Activity = lit + fire + in-flight
 * impulses per step, the thermal shock lattice's own "new work"
 * reasoning for its cullet metric. Measured: steps 1-30 carry 50.5% of
 * the window's activity, 31-60 carry 34.3%, 61-90 carry 15.2%. */

/* THAT CLEARS the same >=15%-per-third bar the thermal shock lattice's
 * own window was chosen against - see that scene's comment
 * (build_thermal_shock_scene, above) for the precedent this follows. */

/* SMALLER WINDOWS (checked down to 9) balance more evenly on that
 * metric, but end too early to mean anything: by step 50 all 7 of
 * this seed's detonations have fired - fuse_blast_wait stops
 * returning to its post-burst ceiling - so a short window shows only
 * fuse-and-blast, never the aftermath. */

/* 90 IS THE SHORTEST ROUND NUMBER past that point whose last third
 * still clears 15% - and it reaches real aftermath: wood burning goes
 * 0 (step 20) -> 83 (step 90), steam 0 -> 3, oil already down by more
 * than half (300 painted -> 136). */

/* LARGER WINDOWS WERE CHECKED TOO: 105 dips the last third to 14.9%,
 * under the bar, before a later wave of ordinary fire pulls it back up
 * past 120 - not a reason to prefer the larger window, since 90
 * already clears the bar honestly. */
/* GUNPOWDER_BASIN_MEASURED_STEPS moved to suite_sand_scenes.h - the
 * frame-budget test in suite_sand_perf.c needs it too. */

/* This scene really does reach the reactions it claims, checked the
 * way this file's other scene tests are: build it through the same
 * function the device test uses, step it the same number of times,
 * and count. FIVE INDEPENDENT SIGNALS, one per claimed path. */

/* MULTIPLE BURSTS, not one pop, proves the chain-detonation this scene
 * measures is a real CHAIN, not a single pile-wide pop -
 * SAND_GUNPOWDER_BLAST_COOLDOWN (sand_reactions.c) exists to spread
 * bursts out, and this is the check that it is doing so here. */

/* fuse_blast_wait ONLY EVER reads its post-burst ceiling (8) on the
 * exact step a burst fires, so counting the steps where it reads 8
 * counts real, distinct detonations rather than a per-step sample
 * that could miss one entirely (see this scene's own git history). */

/* THE PILE NEARLY CONSUMED proves the chain ran through most of the
 * interior, not just the cells nearest the ignition point. FIRE
 * OUTSIDE THE VESSEL proves the blast reaches past the brush-drawn
 * walls - the reason this scene places fuel outside them at all. */

/* WOOD BURNING AND STEAM prove the aftermath cascade is real: escaping
 * fire reached the wood band, and something reached the water band
 * hard enough to boil some of it. NO PLANTS is the same pin the lava
 * stress, thermal shock, boiler and wet earth scenes each make above. */
static void test_the_gunpowder_basin_scene_reaches_the_reactions_it_claims(void)
{
    uint8_t   *big      = malloc((size_t)REAL_W * REAL_H);
    uint8_t   *blocks   = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                  ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)GUNPOWDER_BASIN_IMPULSE_MAX * sizeof *impulses);
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                           "for the gunpowder basin scene, and at least "
                           "one of the three failed to allocate");
    }

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 61u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&s, impulses, GUNPOWDER_BASIN_IMPULSE_MAX);

    build_gunpowder_basin_scene(&s);

    /* The painted state, before a single step has run - what the
     * builder claims to have placed. */
    int painted_dry = 0, painted_fire = 0, painted_oil = 0, painted_acid = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (cell_is_gunpowder(c)) {
                painted_dry++;
            }
            const int m = CELL_MATERIAL(c);
            if (m == MAT_FIRE) painted_fire++;
            else if (m == MAT_OIL) painted_oil++;
            else if (m == MAT_ACID) painted_acid++;
        }
    }

    /* Every burst resets fuse_blast_wait to its post-burst ceiling (8) -
     * see this test's own top comment for why that makes it a reliable,
     * once-per-burst signal rather than something a per-step sample
     * could miss. */
    int burst_count = 0;
    for (int step = 1; step <= GUNPOWDER_BASIN_MEASURED_STEPS; step++) {
        sand_step(&s, 0, 1000, 0);
        if (s.fuse_blast_wait == 8) {
            burst_count++;
        }
    }

    const int ix0 = GUNPOWDER_BASIN_INT_X0;
    const int ix1 = GUNPOWDER_BASIN_INT_X0 + GUNPOWDER_BASIN_INT_W;
    const int iy0 = GUNPOWDER_BASIN_INT_Y0;
    const int iy1 = GUNPOWDER_BASIN_INT_Y0 + GUNPOWDER_BASIN_INT_H;
    int dry = 0, fire_outside = 0, woodburn = 0, steam = 0, oil = 0, acid = 0;
    int extended_nonmetal = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (cell_is_gunpowder(c) && cell_code(c) != GUNPOWDER_LIT) {
                dry++;
            }
            const int m = CELL_MATERIAL(c);
            const bool inside = (x >= ix0 && x < ix1 && y >= iy0 && y < iy1);
            if (m == MAT_FIRE && !inside) fire_outside++;
            else if (m == MAT_WOOD && cell_is_burning(c)) woodburn++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_OIL) oil++;
            else if (m == MAT_ACID) acid++;
            if (cell_is_extended(c) && CELL_VARIANT(c) != MATX_METAL) {
                extended_nonmetal++;
            }
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    char why[192];
    snprintf(why, sizeof why,
             "the basin's gunpowder must be poured full and lit at "
             "exactly one fixed cell before a single step runs - got "
             "%d dry cells and %d fire cells painted", painted_dry,
             painted_fire);
    TEST_ASSERT_EQUAL_INT_MESSAGE(GUNPOWDER_BASIN_INT_W * GUNPOWDER_BASIN_INT_H
                                      - GUNPOWDER_BASIN_SPARK * GUNPOWDER_BASIN_SPARK,
        painted_dry, why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(GUNPOWDER_BASIN_SPARK * GUNPOWDER_BASIN_SPARK,
                                  painted_fire, why);

    snprintf(why, sizeof why,
             "the pile must chain-detonate across SEVERAL bursts, not "
             "pop once and go quiet - got %d over the measured window",
             burst_count);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(3, burst_count, why);

    snprintf(why, sizeof why,
             "the chain must have consumed nearly the whole interior by "
             "the end of the window - %d dry cells are still unlit "
             "out of %d painted", dry,
             GUNPOWDER_BASIN_INT_W * GUNPOWDER_BASIN_INT_H - 1);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(50, dry, why);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fire_outside,
        "fire must reach past the brush-drawn walls - if none did, "
        "this scene is not exercising the aftermath cascade it claims "
        "to place fuel outside the vessel for");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, woodburn,
        "escaping fire must have reached the wood band outside the "
        "vessel and set some of it alight");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, steam,
        "something must have reached the water band hard enough to "
        "boil at least a little of it to steam");

    snprintf(why, sizeof why,
             "the oil band must show real consumption by fire, not "
             "merely be present - %d cells left of %d painted", oil,
             painted_oil);
    TEST_ASSERT_LESS_THAN_MESSAGE(painted_oil, oil, why);

    snprintf(why, sizeof why,
             "the acid band must show it genuinely took part in "
             "something over the window, not sit inert - %d cells "
             "against %d painted", acid, painted_acid);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(painted_acid, acid, why);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended_nonmetal,
        "the gunpowder basin scene should not be growing any plants - "
        "if it is, the device test's frame budget is no longer "
        "measuring the scene it claims to (the metal slab is the only "
        "extended-static cell this scene paints on purpose)");
}

void run_sand_scenes_suite(void)
{
    RUN_TEST(test_the_mixed_scene_puts_every_material_pair_in_contact);
    RUN_TEST(test_the_four_liquid_scene_keeps_reacting_after_settling);
    RUN_TEST(test_the_lava_stress_scene_reaches_every_reaction_it_claims);
    RUN_TEST(test_the_smoke_and_steam_scene_stays_a_gas_screen);
    RUN_TEST(test_the_thermal_shock_scene_shatters_in_both_directions);
    RUN_TEST(test_the_boiler_scene_keeps_boiling_across_the_window);
    RUN_TEST(test_the_wet_earth_scene_keeps_percolating_across_the_window);
    RUN_TEST(test_the_water_over_lava_scene_reaches_the_quench_cooloff_and_burst_paths_it_claims);
    RUN_TEST(test_the_gunpowder_basin_scene_reaches_the_reactions_it_claims);
}

SUITE_REGISTER(run_sand_scenes_suite);
