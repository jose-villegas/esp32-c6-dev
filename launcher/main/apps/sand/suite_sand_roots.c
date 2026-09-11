/*
 * Portable suite: the falling-sand automaton - roots, plus a handful of
 * extended-material/palette/metal-shine/gunpowder-tone regression tests
 * that had drifted under the same "--- roots ---" banner in suite_sand.c
 * over time. Left together rather than re-split further by topic: this
 * file is already a comfortable size, and hunting down where each
 * drifted test's OWN section went would be a second refactor, not this
 * one.
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

#include "material_palette.h"
#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/* See reaction_t.roots and docs/sand/Sand-Simulation.md's tree-feeding
 * section. */

/* see test_a_rooted_collar_survives_the_bed_shifting_away's own scene 1 */
static void test_a_watered_plant_roots_into_the_soil_it_drinks_from(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }

    int rooted = 0;
    for (int cycle = 0; cycle < 200 && !rooted; cycle++) {
        for (int y = 0; y < H - 2; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, SAND_EMPTY);
            }
        }
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, H - 2)) == MAT_DIRT) {
                sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
            }
        }
        sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

        for (int i = 0; i < 40 && !rooted; i++) {
            sand_step(&s, 0, 1000, 0);
            for (int y = 0; y < H && !rooted; y++) {
                for (int x = 0; x < W; x++) {
                    if (sand_at(&s, x, y) == MATX(MATX_ROOT)) {
                        rooted = 1;
                    }
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(rooted,
        "a plant growing on watered soil must eventually weld a root "
        "into the ground it drinks from - without this, a tree only "
        "ever rests on the soil it grew from rather than being embedded "
        "in it");
}

/* The comment points to the behavior and reasoning defined in `anchored()`
 * and `is_kin()` within `sand_reactions.c`, particularly regarding how roots
 * and other non-kin elements interact with kin bodies in terms of anchoring
 * and structural support. */
static void test_a_trunk_standing_on_its_own_root_is_anchored(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    sand_set(&s, cx, H - 3, MATX(MATX_ROOT));
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WOOD, 0));
    sand_set(&s, cx, H - 5, CELL_MAKE(MAT_WOOD, 0));
    /* Diagonally off the top of it, touching nothing else - the same
     * shape test_a_limb_hangs_on_to_a_wooden_trunk uses above, so a
     * limb that only ever looked anchored by luck of a squarer test
     * geometry cannot pass this one by accident either. */
    sand_set(&s, cx + 1, H - 6, MATX(MATX_PLANT));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_ROOT), sand_at(&s, cx, H - 3),
        "the root itself must not have moved - it holds still regardless "
        "of what is or is not beneath it");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, cx + 1, H - 6),
        "a limb growing off a trunk that stands on nothing but a root "
        "must stay exactly where it grew - if the root were mistaken "
        "for more of the tree's own body instead of something the tree "
        "rests ON, this whole structure would read as unsupported and "
        "come down");
}

/* test_a_root_column_reaches_below_the_collar */

/* The cross-reference points to the test that verifies the mechanism by which
 * roots find water and how it relates to the growth process of a tree. */
#define LIFT_TEST_W 8
#define LIFT_TEST_H 24

/* That split matters. Only the top cell of the stem is MATX_PLANT; others are
 * MAT_WOOD. Soft cells grow independently, branching or thickening freely.
 * Wood does not grow. Returns if the tip grows at least one cell further,
 * measured across the grid. */
static bool lift_boundary_grows(int stem, int roots)
{
    uint8_t *grid = malloc((size_t)LIFT_TEST_W * LIFT_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(grid,
        "lift-boundary grid must fit in what the framebuffer leaves");

    sand_t t;
    sand_init(&t, grid, LIFT_TEST_W, LIFT_TEST_H, 12345u);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    const int cx      = LIFT_TEST_W / 2;
    const int floor_y = LIFT_TEST_H - 1;
    const int bed_top = floor_y - 3;

    for (int x = 0; x < LIFT_TEST_W; x++) {
        sand_set(&t, x, floor_y, STONE);
        for (int y = bed_top; y < floor_y; y++) {
            sand_set(&t, x, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
    }
    for (int y = bed_top - roots; y < bed_top; y++) {
        sand_set(&t, cx, y, MATX(MATX_ROOT));
    }
    const int stem_top = bed_top - roots - stem;
    for (int y = stem_top + 1; y < bed_top - roots; y++) {
        sand_set(&t, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&t, cx, stem_top, MATX(MATX_PLANT)); /* the one grower */

    int total_before = 0;
    for (int y = 0; y < floor_y; y++) {
        for (int x = 0; x < LIFT_TEST_W; x++) {
            const cell_t c = sand_at(&t, x, y);
            if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
                total_before++;
            }
        }
    }

    bool grew = false;
    for (int i = 0; i < 400 && !grew; i++) {
        sand_step(&t, 0, 1000, 0);
        int total_now = 0;
        for (int y = 0; y < floor_y; y++) {
            for (int x = 0; x < LIFT_TEST_W; x++) {
                const cell_t c = sand_at(&t, x, y);
                if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
                    total_now++;
                }
            }
        }
        if (total_now > total_before) {
            grew = true;
        }
    }
    free(grid);
    return grew;
}

static void test_a_root_column_does_not_spend_the_trees_lift(void)
{
    /* TWO CONTROLS FIRST, or the boundary below proves nothing. Without both
     * of these holding, a change to either side of that boundary could not be
     * blamed on roots specifically. */
    TEST_ASSERT_TRUE_MESSAGE(lift_boundary_grows(10, 0),
        "control: a plain ten-cell stem must still be able to grow one "
        "more cell - if this fails, the boundary below proves nothing "
        "about roots specifically");
    TEST_ASSERT_FALSE_MESSAGE(lift_boundary_grows(11, 0),
        "control: an eleven-cell stem must NOT grow further - TREE_LIFT "
        "is 10, and this is the cap actually engaging; if this passes, "
        "growth was never bounded here and the comparison below proves "
        "nothing either");

    /* see `find_water()`'s own comment */
    TEST_ASSERT_TRUE_MESSAGE(lift_boundary_grows(10, 3),
        "a ten-cell stem standing on 3 cells of root must still be able "
        "to grow one more cell, the same as standing straight on soil - "
        "a root must cost the tree no TREE_LIFT at all");
}

/* A root with dirt piled back on top of it must not cut the tree off
 * from the water below it (find_water()'s own soil-walk transparency). */
#define BURIED_ROOT_TEST_W 8
#define BURIED_ROOT_TEST_H 16
/* A DEEP wet reserve (6 rows), not a single row: since PART 2 of the
 * roots feature, a root on its only reachable water is itself a second
 * consumer of that cell (step_one_rooting_cell()) - a single-row reserve
 * races the root's own slow eating against the tree's growth roll,
 * deterministically losing for this suite's fixed seed, a different
 * failure from the transparency bug under test. Six rows is far more than
 * the root can plausibly eat through before growth succeeds once. */
#define BURIED_ROOT_WET_ROWS 6

static void test_a_buried_root_does_not_cut_off_the_water_below_it(void)
{
    uint8_t *grid = malloc((size_t)BURIED_ROOT_TEST_W * BURIED_ROOT_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(grid,
        "buried-root grid must fit in what the framebuffer leaves");

    sand_t t;
    sand_init(&t, grid, BURIED_ROOT_TEST_W, BURIED_ROOT_TEST_H, 12345u);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    const int cx      = BURIED_ROOT_TEST_W / 2;
    const int floor_y = BURIED_ROOT_TEST_H - 1;
    const int wet_top = floor_y - BURIED_ROOT_WET_ROWS; /* the real water */
    const int root_y  = wet_top - 1;                    /* buried */
    const int dry_y    = root_y - 1;                    /* piled back on later */
    const int plant_y  = dry_y - 1;

    for (int x = 0; x < BURIED_ROOT_TEST_W; x++) {
        sand_set(&t, x, floor_y, STONE);
        for (int y = wet_top; y < floor_y; y++) {
            sand_set(&t, x, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_set(&t, x, dry_y, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    sand_set(&t, cx, root_y, MATX(MATX_ROOT));
    sand_set(&t, cx, plant_y, MATX(MATX_PLANT));

    int before = 0;
    for (int y = 0; y < BURIED_ROOT_TEST_H; y++) {
        const cell_t c = sand_at(&t, cx, y);
        if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
            before++;
        }
    }

    int grew = 0;
    for (int i = 0; i < 2000 && !grew; i++) {
        sand_step(&t, 0, 1000, 0);
        int now = 0;
        for (int y = 0; y < BURIED_ROOT_TEST_H; y++) {
            const cell_t c = sand_at(&t, cx, y);
            if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
                now++;
            }
        }
        if (now > before) {
            grew = 1;
        }
    }
    free(grid);
    TEST_ASSERT_TRUE_MESSAGE(grew,
        "a plant standing over dry dirt, a buried root and wet dirt in "
        "that order must still be able to grow - the buried root has to "
        "be transparent to the soil walk, or dirt piled back on top of "
        "it would cut the tree off from the water below its own root");
}

/* see find_water()'s own bug SHAPE over the whole run: nothing above the
 * collar, deepens and spreads */
#define REACH_TEST_W 16
#define REACH_TEST_H 12

static void test_a_root_column_reaches_below_the_collar(void)
{
    uint8_t *grid = malloc((size_t)REACH_TEST_W * REACH_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(grid,
        "root-reach grid must fit in what the framebuffer leaves");

    sand_t t;
    sand_init(&t, grid, REACH_TEST_W, REACH_TEST_H, 12345u);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    const int cx        = REACH_TEST_W / 2;
    const int floor_y    = REACH_TEST_H - 1;
    const int collar_y   = floor_y - 8; /* eight rows of saturated dirt
                                          * below the collar, and the
                                          * full width beside it, so the
                                          * system has real room to grow
                                          * both ways */

    for (int x = 0; x < REACH_TEST_W; x++) {
        sand_set(&t, x, floor_y, STONE);
        for (int y = collar_y; y < floor_y; y++) {
            sand_set(&t, x, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
    }
    sand_set(&t, cx, collar_y - 1, CELL_MAKE(MAT_WOOD, 0)); /* shelters
                                                              * the seed
                                                              * root so
                                                              * this test
                                                              * is not
                                                              * confounded
                                                              * by rot */
    sand_set(&t, cx, collar_y, MATX(MATX_ROOT)); /* the collar, already
                                                   * rooted - PART 2 needs
                                                   * no growing plant at
                                                   * all to run, only a
                                                   * root cell with a
                                                   * moist neighbour */

    for (int i = 0; i < 3000; i++) {
        sand_step(&t, 0, 1000, 0);
    }

    int above_collar = 0, max_depth = 0, max_half_width = 0;
    for (int y = 0; y < REACH_TEST_H; y++) {
        for (int x = 0; x < REACH_TEST_W; x++) {
            if (sand_at(&t, x, y) != MATX(MATX_ROOT)) {
                continue;
            }
            if (y < collar_y) {
                above_collar = 1;
            }
            const int depth = y - collar_y;
            if (depth > max_depth) {
                max_depth = depth;
            }
            const int hw = (x > cx) ? (x - cx) : (cx - x);
            if (hw > max_half_width) {
                max_half_width = hw;
            }
        }
    }
    free(grid);

    TEST_ASSERT_FALSE_MESSAGE(above_collar,
        "a root must never appear above the collar row - gravity still "
        "applies to where a tree's own footing can be, even though the "
        "eating rule itself has no direction weights");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, max_depth,
        "over this run the system must still reach at least two rows "
        "below the collar - a root system that never deepens is not one, "
        "however wide it spreads");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, max_half_width,
        "and it must spread at least two columns to either side - a "
        "single straight column is the shape this feature moved away "
        "from, not the one it is aiming for");
}

/* see MATX_ROOT's own row */
static void test_lava_burns_a_root_out_of_the_ground(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, cx, H - 2, MATX(MATX_ROOT));
    /* Diagonal to the lava, so the wood is out of its four-neighbour
     * reach - it plays no part in this test beyond matching the scene the
     * fire test just below uses. */
    sand_set(&s, cx - 1, H - 2, CELL_MAKE(MAT_WOOD, 0));
    sand_set(&s, cx + 1, H - 2, STONE);
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));

    int gone = 0;
    for (int i = 0; i < 400 && !gone; i++) {
        sand_step(&s, 0, 1000, 0);
        gone = (sand_at(&s, cx, H - 2) != MATX(MATX_ROOT));
    }
    TEST_ASSERT_TRUE_MESSAGE(gone,
        "lava sitting on a root must burn it out - a root ignores flame, "
        "but molten rock is the one heat that reaches it");
}

static void test_fire_leaves_a_root_alone(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, cx, H - 2, MATX(MATX_ROOT));
    /* Diagonal to the flame, not cardinal, so the wood itself is never
     * the thing that catches - it plays no other part in this test. */
    sand_set(&s, cx - 1, H - 2, CELL_MAKE(MAT_WOOD, 0));

    for (int i = 0; i < 400; i++) {
        /* Held against it - fire is a gas and drifts off in a step or two,
         * so a single cell of it would test one lick, not sustained heat. */
        sand_set(&s, cx, H - 3, CELL_MAKE(MAT_FIRE, MATERIAL_VARIANTS - 1));
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_ROOT), sand_at(&s, cx, H - 2),
            "a root must ignore fire - flammability 0 and heat_chance 0 are "
            "both deliberate, and `melts` must not have opened a side door "
            "for a gas");
    }
}

/* see `roots_to` on the LEAF row THE ISOLATION IS THE WHOLE TEST
 * step_one_drinking_cell() looks at the four cardinal neighbours only */
static void test_a_canopy_waters_the_soil_through_its_own_roots(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0)); /* bone dry */
        sand_set(&s, x, H - 3, MATX(MATX_ROOT));           /* no way round */
    }
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WOOD, 0));
    sand_set(&s, cx, H - 5, MATX(MATX_LEAF));

    /* The box: the water can only ever touch the leaf. */
    sand_set(&s, cx - 2, H - 5, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);

    int wettest = 0;
    for (int i = 0; i < 2000 && wettest == 0; i++) {
        /* Held against the leaf, the way a watering can is - drinking
         * spends the water it takes, so a single cell of it would answer
         * "did one drink happen" rather than "can a canopy water a
         * rooted tree". */
        sand_set(&s, cx - 1, H - 5, CELL_MAKE(MAT_WATER, MASS_MAX));
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT && CELL_MOISTURE(c) > wettest) {
                wettest = CELL_MOISTURE(c);
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, wettest,
        "a leaf held against water must be able to put that water into the "
        "soil under its own tree even when the only route down runs "
        "through root - the soil here is dry, is touching no water of its "
        "own, and the leaf is the only cell the water touches, so the one "
        "way it can get wet is the walk crossing the roots");
}

/* Two scenes: collar erased at step ZERO if no roots, mimicking forced-off
 * roots effect. */
static void test_a_rooted_collar_survives_the_bed_shifting_away(void)
{
    const int cx = W / 2;

    /* see test_a_watered_plant_roots_into_the_soil_it_drinks_from's own top
     * comment */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }

    int rooted = 0;
    for (int cycle = 0; cycle < 200 && !rooted; cycle++) {
        for (int y = 0; y < H - 2; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, SAND_EMPTY);
            }
        }
        if (CELL_MATERIAL(sand_at(&s, cx, H - 2)) == MAT_DIRT) {
            sand_set(&s, cx, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_set(&s, cx, H - 3, MATX(MATX_PLANT));

        for (int i = 0; i < 40 && !rooted; i++) {
            sand_step(&s, 0, 1000, 0);
            rooted = (sand_at(&s, cx, H - 2) == MATX(MATX_ROOT));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(rooted,
        "setup failure, not the claim under test: the collar must have "
        "rooted before the rest of this test means anything");

    int before = 0;
    for (int y = 0; y < H; y++) {
        const cell_t c = sand_at(&s, cx, y);
        if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD ||
            c == MATX(MATX_ROOT)) {
            before++;
        }
    }
    int grew_with_root = 0;
    for (int i = 0; i < 1500 && !grew_with_root; i++) {
        sand_step(&s, 0, 1000, 0);
        int now = 0;
        for (int y = 0; y < H; y++) {
            const cell_t c = sand_at(&s, cx, y);
            if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD ||
                c == MATX(MATX_ROOT)) {
                now++;
            }
        }
        if (now > before) {
            grew_with_root = 1;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(grew_with_root,
        "a tree whose collar has already rooted must keep growing - the "
        "root holds still, so nothing about the bed shifting can carry "
        "it away out from under the tree");

    /* Scene 2: the collar erased before a root could ever have formed -
     * the deterministic stand-in for `roots` never having existed. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));
    /* find_water()'s logic for handling gravity-ward cells */
    sand_set(&s, cx - 1, H - 2, SAND_EMPTY);
    sand_set(&s, cx, H - 2, SAND_EMPTY); /* the bed shifting, right now */
    sand_set(&s, cx + 1, H - 2, SAND_EMPTY);

    /* By COUNT across the whole grid, not by watching one column. The seed
     * FALLS one row if all cells under it are gone. Growth MAKES cells;
     * falling relocates them. */
    int grew_without_root = 0;
    for (int i = 0; i < 1500 && !grew_without_root; i++) {
        sand_step(&s, 0, 1000, 0);
        if (count_cells_of(MAT_EXTENDED) + count_cells_of(MAT_WOOD) > 1) {
            grew_without_root = 1;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(grew_without_root,
        "a tree whose collar is erased before any root exists must stay "
        "stuck - this is the bug the whole feature exists to fix, "
        "reproduced here to prove the fix is actually load-bearing");
}

/* A root is inert: it does not fall, does not grow, is not a bud or
 * sprout site, and never appears on a board with no plant on it. */
static void test_a_root_is_inert(void)
{
    fixture();
    sand_clear(&s);

    /* Mid-air over nothing, and over stone that is not directly beneath
     * it either - if a root fell, moved, or spawned anything at all,
     * this would catch it. */
    sand_set(&s, W / 2, 2, MATX(MATX_ROOT));
    for (int i = 0; i < 300; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATX(MATX_ROOT), sand_at(&s, W / 2, 2),
        "a root must not move - it holds still and holds on, the whole "
        "of its own reaction row");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "and it must not have grown, budded, sprouted, or produced "
        "anything else - an inert cell sitting alone for 300 steps "
        "should still be alone");

    /* And on a board with water, wet soil, and no plant anywhere - the
     * only way a root can ever be created - none ever appears. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        sand_set(&s, x, 0, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    for (int i = 0; i < 500; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_EXTENDED),
        "a board with wet soil and standing water but no plant or wood "
        "anywhere must never produce a root - roots are grown, not "
        "spontaneous");
}

static void test_root_conversion_never_creates_moisture(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int total = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_DIRT) {
                total += CELL_MOISTURE(c);
            }
        }
    }
    const int initial = total;

    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);

        int now = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) == MAT_DIRT) {
                    now += CELL_MOISTURE(c);
                }
            }
        }
        char why[224];
        snprintf(why, sizeof why,
                 "step %d: total soil moisture rose to %d from a starting "
                 "total of %d - nothing on this board pours water in, so "
                 "growth, budding and root conversion together must only "
                 "ever spend moisture, never create it", i, now, initial);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(initial, now, why);
    }
}

/* step_one_rooting_cell()'s conversion accounts for the fall */
static void test_a_root_eats_a_moist_neighbour_and_only_spends_its_own_moisture(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* Dirt is KIND_POWDER (material.c) */
    sand_set(&s, cx - 1, cy + 2, STONE);
    sand_set(&s, cx, cy + 2, STONE);
    sand_set(&s, cx + 1, cy + 2, STONE);
    sand_set(&s, cx, cy - 1, CELL_MAKE(MAT_WOOD, 0)); /* shelter */
    sand_set(&s, cx, cy, MATX(MATX_ROOT));
    sand_set(&s, cx, cy + 1, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX)); /* the
                                                                          * one
                                                                          * candidate
                                                                          */

    int ate = 0;
    for (int i = 0; i < 3000 && !ate; i++) {
        sand_step(&s, 0, 1000, 0);
        ate = (sand_at(&s, cx, cy + 1) == MATX(MATX_ROOT));
    }
    TEST_ASSERT_TRUE_MESSAGE(ate,
        "setup failure, not the claim under test: the root's one moist "
        "neighbour never got eaten at all, so there is nothing to check "
        "the moisture accounting of");

    int total = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_DIRT) {
                total += CELL_MOISTURE(c);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total,
        "the one dirt cell on this board held the only moisture there "
        "was; once step_one_rooting_cell() converts it, that moisture "
        "must be simply GONE - spent as the price of the conversion, not "
        "carried anywhere else on the board there is nothing else to "
        "carry it to");
}

/* CELL_IS_EMPTY() */
static void test_a_root_never_eats_dry_dirt_sand_or_empty_space(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* see previous test */
    for (int x = cx - 2; x <= cx + 2; x++) {
        sand_set(&s, x, cy + 1, STONE);
    }
    sand_set(&s, cx, cy - 1, CELL_MAKE(MAT_WOOD, 0));         /* shelter, up */
    sand_set(&s, cx, cy, MATX(MATX_ROOT));
    /* Three candidates, one per guard in step_one_rooting_cell()'s
     * neighbour scan - all beside the root rather than below it, since
     * the row below is now the floor. */
    sand_set(&s, cx - 1, cy, CELL_SOIL(MAT_DIRT, 1, 0)); /* dry, left */
    sand_set(&s, cx + 1, cy, SAND_FIRST_SHADE);          /* sand, right */
    /* (cx - 1, cy - 1) stays SAND_EMPTY from sand_clear() above - empty,
     * diagonally up-left, one of the root's eight neighbours - named
     * explicitly here rather than left implicit, so the scene reads as
     * three deliberate candidates rather than two plus whatever the grid
     * happened to start as. */

    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CELL_SOIL(MAT_DIRT, 1, 0),
        sand_at(&s, cx - 1, cy),
        "dry dirt must never be eaten - CELL_MOISTURE(n) != 0 is not "
        "optional, it is the whole reason the conversion does not need "
        "to spend anything separately");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_FIRST_SHADE,
        sand_at(&s, cx + 1, cy),
        "sand must never be eaten, wet or not - reaction_of(n)->dries == "
        "0 for sand, and that is the guard that keeps this rule to soil "
        "specifically");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, cx - 1, cy - 1)),
        "empty space must never become a root out of nowhere");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "and nothing else on the board should have changed at all - one "
        "root in, one root out");
}

/* see test_a_root_column_does_not_spend_the_trees_lift's own comment */
static bool
surface_rule_lets_growth_through(int satellite_roots)
{
    sand_t t;
    uint8_t grid[8 * 8];
    sand_init(&t, grid, 8, 8, 12345u);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    const int cx = 4, cy = 4;
    /* see test_a_root_never_eats_dry_dirt_sand_or_empty_space's own comment */
    for (int x = cx - 2; x <= cx + 2; x++) {
        sand_set(&t, x, cy + 2, STONE);
    }
    sand_set(&t, cx, cy, MATX(MATX_ROOT));
    sand_set(&t, cx, cy + 1, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX)); /* the
                                                                          * one
                                                                          * candidate,
                                                                          * down
                                                                          */
    /* Satellites fill row cy - 1 ONLY - up-left, up, up-right - and nowhere
     * else */
    static const int sat_dx[3] = {-1, 0, 1};
    for (int i = 0; i < satellite_roots; i++) {
        sand_set(&t, cx + sat_dx[i], cy - 1, MATX(MATX_ROOT));
    }

    bool grew = false;
    for (int i = 0; i < 2000 && !grew; i++) {
        sand_step(&t, 0, 1000, 0);
        grew = (sand_at(&t, cx, cy + 1) == MATX(MATX_ROOT));
    }
    return grew;
}

/* A tip keeps going the way it was going - away from its parent - and
 * down. Reported from the device: the uniform pick let moisture's own
 * sideways spread near the surface decide everything, so deeper roots
 * only happened at the angles the geometry favoured. Now a candidate that
 * continues away from the cell's root neighbours weighs +2 and one that
 * reaches gravity-ward +1, over a base of 1 (step_one_rooting_cell()).
 *
 * STATISTICAL, over many seeds, because a weighted roll is a tendency and
 * not a rule. The scene is a tip with its parent directly ABOVE it in a
 * moist bed, so "away from parent" and "down" point the same way: the
 * three cells below the tip weigh 4 each, the two beside it 1 each -
 * 12 against 2. Only the first conversion adjacent to the tip is
 * classified, per seed.
 *
 * THE PARENT IS SILENCED, and that is the whole difficulty of this scene.
 * A live parent competes for the tip's two side cells (they touch both),
 * and its own away-vector points UP, so it reaches them with no penalty
 * at all: measured with the parent free to grow, the weights only moved
 * "below" from 49% to 59%, which is a real skew drowned in a confound.
 * Three roots above the parent give it four root neighbours, past
 * ROOT_SURFACE_MAX, so it never rolls; the three extras can only reach
 * rows above the tip, never the cells classified here. What is left is
 * the tip's own pick and nothing else.
 *
 * Watched red with both weights stubbed to zero - the tip's uniform pick
 * over five candidates, three of them below, lands "below" 3 in 5. The
 * bar is 75%: well above 60%, well below the 86% the weights predict. */
static void test_a_root_tip_grows_on_away_from_its_parent_and_down(void)
{
    const int cx = W / 2, ty = H - 4;
    int below = 0, beside = 0;

    for (uint32_t seed = 1; seed <= 120u; seed++) {
        sand_init(&s, cells, W, H, seed);
        sand_clear(&s);
        sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
            for (int y = H - 7; y <= H - 2; y++) {
                sand_set(&s, x, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
            }
        }
        sand_set(&s, cx - 1, ty - 2, MATX(MATX_ROOT)); /* three above the parent: */
        sand_set(&s, cx, ty - 2, MATX(MATX_ROOT));     /* with the tip that is four */
        sand_set(&s, cx + 1, ty - 2, MATX(MATX_ROOT)); /* neighbours, so it never rolls */
        sand_set(&s, cx, ty - 1, MATX(MATX_ROOT));     /* the parent, above */
        sand_set(&s, cx, ty, MATX(MATX_ROOT));         /* the tip */

        int verdict = 0;
        for (int i = 0; i < 600 && verdict == 0; i++) {
            sand_step(&s, 0, 1000, 0);
            for (int dx = -1; dx <= 1 && verdict == 0; dx++) {
                if (sand_at(&s, cx + dx, ty + 1) == MATX(MATX_ROOT)) {
                    verdict = 1; /* below the tip */
                }
            }
            if (verdict == 0 && (sand_at(&s, cx - 1, ty) == MATX(MATX_ROOT) ||
                                 sand_at(&s, cx + 1, ty) == MATX(MATX_ROOT))) {
                verdict = 2; /* beside it */
            }
        }
        if (verdict == 1) below++;
        if (verdict == 2) beside++;
    }
    TEST_ASSERT_TRUE_MESSAGE(below + beside >= 100,
        "setup failure, not the claim under test: the tip must actually "
        "grow in most seeds for the ratio to mean anything");
    TEST_ASSERT_TRUE_MESSAGE(below * 100 >= 75 * (below + beside),
        "a tip with its parent above it must mostly grow ON and DOWN, not "
        "sideways - the weighted pick has to actually skew the roll, or the "
        "system stays as sideways as the moisture that feeds it");
}

/* The reasoning lives in the behavior and classification of the first root
 * under a trunk as detailed in `step_one_rooting_cell()`. */
static void test_the_first_root_under_a_trunk_heads_down(void)
{
    const int cx = W / 2, ry = H - 4;
    int below = 0, beside = 0;

    for (uint32_t seed = 1; seed <= 120u; seed++) {
        sand_init(&s, cells, W, H, seed);
        sand_clear(&s);
        sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
            for (int y = H - 7; y <= H - 2; y++) {
                sand_set(&s, x, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
            }
        }
        sand_set(&s, cx, ry - 1, CELL_MAKE(MAT_WOOD, 0)); /* the trunk */
        sand_set(&s, cx, ry, MATX(MATX_ROOT));            /* its first root */

        int verdict = 0;
        for (int i = 0; i < 600 && verdict == 0; i++) {
            sand_step(&s, 0, 1000, 0);
            for (int dx = -1; dx <= 1 && verdict == 0; dx++) {
                if (sand_at(&s, cx + dx, ry + 1) == MATX(MATX_ROOT)) {
                    verdict = 1;
                }
            }
            if (verdict == 0 && (sand_at(&s, cx - 1, ry) == MATX(MATX_ROOT) ||
                                 sand_at(&s, cx + 1, ry) == MATX(MATX_ROOT))) {
                verdict = 2;
            }
        }
        if (verdict == 1) below++;
        if (verdict == 2) beside++;
    }
    TEST_ASSERT_TRUE_MESSAGE(below + beside >= 100,
        "setup failure, not the claim under test: the collar root must "
        "actually grow in most seeds for the ratio to mean anything");
    TEST_ASSERT_TRUE_MESSAGE(below * 100 >= 75 * (below + beside),
        "the first root under a trunk must mostly head DOWN - the trunk it "
        "grew from has to count as the parent it grows away from, or the "
        "seed of every root system starts with a coin-toss along the wet "
        "surface");
}

/* see `step_one_conducting_cell()`'s own comment */
static void test_a_root_carries_a_level_of_water_down_through_itself(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, ry = H - 3;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    sand_set(&s, cx, ry, MATX(MATX_ROOT));
    sand_set(&s, cx - 1, ry, CELL_MAKE(MAT_WOOD, 0));          /* shelter; blocks the source's slide */
    sand_set(&s, cx, ry - 1, CELL_SOIL(MAT_DIRT, 1, 5));       /* the source, ABOVE the root */
    sand_set(&s, cx, ry + 1, CELL_SOIL(MAT_DIRT, 1, 0));       /* the sink, beneath, dry */

    int got = 0;
    for (int i = 0; i < 400 && !got; i++) {
        sand_step(&s, 0, 1000, 0);
        got = CELL_MOISTURE(sand_at(&s, cx, ry + 1));
    }
    const int source_now = CELL_MOISTURE(sand_at(&s, cx, ry - 1));
    TEST_ASSERT_TRUE_MESSAGE(got > 0,
        "a root with wet soil above it and dry soil beneath it must carry "
        "water down through itself into the dry cell - that is the whole "
        "reason roots conduct");
    TEST_ASSERT_TRUE_MESSAGE(got <= 5 - source_now,
        "conduction MOVES water, it never makes it: the sink may gain at "
        "most what the source lost (drying may take more from the source, "
        "never less)");
}

/* An INVARIANT guard, and honest about what that means: this test asserts
 * things that never happen, so stubbing the conduit's chance to zero
 * cannot turn it red - it passes with the mechanism off exactly as with
 * it on. It was watched red the only way such a test can be, by briefly
 * inverting the source/sink roles in step_one_conducting_cell() during
 * development, and it stays here to catch that inversion coming back. */
static void test_conduction_never_pushes_water_up_or_into_anything_but_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, ry = H - 3;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    sand_set(&s, cx, ry, MATX(MATX_ROOT));
    sand_set(&s, cx, ry + 1, CELL_SOIL(MAT_DIRT, 1, 6));      /* wet, BENEATH - only ever a sink */
    sand_set(&s, cx, ry - 1, CELL_SOIL(MAT_DIRT, 1, 0));      /* dry, above */
    sand_set(&s, cx - 1, ry, CELL_SOIL(MAT_DIRT, 1, 0));      /* dry, beside */
    sand_set(&s, cx + 1, ry, CELL_MAKE(MAT_WOOD, 0));          /* not soil, and the shelter */

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_MOISTURE(sand_at(&s, cx, ry - 1)),
            "water beneath a root must never be carried UP");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_MOISTURE(sand_at(&s, cx - 1, ry)),
            "water beneath a root must never be carried SIDEWAYS");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(CELL_MAKE(MAT_WOOD, 0), sand_at(&s, cx + 1, ry),
            "conduction touches soil only - wood is not a sink and not a "
            "source");
    }
}

/* No "brings water deeper than bare soil" test here: on this 8x8 grid,
 * percolation alone already floods a six-row bed, leaving no depth for a
 * conduit to add, and a conducting column eats the cell it wetted,
 * undercounting moisture-based measurement. The depth claim is proven on
 * the 60x70 harness instead: mean deepest root 4.6 rows with conduction
 * off, 15.0 with it on, over ten seeds (ROOT_CONDUCT_CHANCE's comment,
 * sand_reactions.c; docs/sand/Sand-Simulation.md). */

static void test_a_thickly_rooted_cell_stops_growing(void)
{
    TEST_ASSERT_TRUE_MESSAGE(surface_rule_lets_growth_through(2),
        "control: a root with only 2 root neighbours (at or under "
        "ROOT_SURFACE_MAX) must still be able to grow into an eligible "
        "candidate - if this fails, the boundary below proves nothing "
        "about the surface rule specifically");
    TEST_ASSERT_FALSE_MESSAGE(surface_rule_lets_growth_through(3),
        "a root with 3 root neighbours (over ROOT_SURFACE_MAX) must not "
        "roll to grow at all, even with an eligible candidate right "
        "there - this is the rule that keeps the system a filigree "
        "instead of a solid block");
}

/* ROOTS FOLLOW WATER, WITH NO DIRECTION WEIGHTS OF THEIR OWN
 * (step_one_rooting_cell()'s own top comment, sand_reactions.c): a bed
 * wet on only one side of a root grows root on that side and never the
 * dry one, purely because the moisture check is the only thing steering
 * it - nothing in the scan itself prefers left over right or down over
 * up. */
static void test_roots_grow_toward_the_wet_side_only(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* Both dirt candidates are KIND_POWDER - a floor under the whole
     * candidate row, or neither survives long enough to be eaten OR to
     * stay put and prove it was not. Two cells wider than the candidates
     * themselves - see test_a_root_never_eats_dry_dirt_sand_or_empty_
     * space's own comment on why a floor flush with its edge cells is
     * not actually a floor; a powder resting right at the edge still has
     * an open diagonal to slide off into. */
    for (int x = cx - 2; x <= cx + 2; x++) {
        sand_set(&s, x, cy + 1, STONE);
    }
    sand_set(&s, cx, cy - 1, CELL_MAKE(MAT_WOOD, 0));    /* shelter, up */
    sand_set(&s, cx, cy, MATX(MATX_ROOT));
    sand_set(&s, cx - 1, cy, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX)); /* wet,
                                                                          * left
                                                                          */
    sand_set(&s, cx + 1, cy, CELL_SOIL(MAT_DIRT, 1, 0));                /* dry,
                                                                          * right
                                                                          */

    int wet_side_grew = 0;
    for (int i = 0; i < 3000 && !wet_side_grew; i++) {
        sand_step(&s, 0, 1000, 0);
        wet_side_grew = (sand_at(&s, cx - 1, cy) == MATX(MATX_ROOT));
        TEST_ASSERT_FALSE_MESSAGE(sand_at(&s, cx + 1, cy) == MATX(MATX_ROOT),
            "the dry side must never become root - there is no water "
            "there for the conversion to spend");
    }
    TEST_ASSERT_TRUE_MESSAGE(wet_side_grew,
        "setup failure, not the claim under test: the wet side never "
        "grew either, so there is nothing to say about which side the "
        "system preferred");
}

/* THE RUNAWAY SCENE, scaled for the host suite - see the Roots section
 * of docs/sand/Sand-Simulation.md for the full six-seed, 20,000-step
 * version this is a fast stand-in for. A root pre-planted (so the rare
 * one-time collar seed cannot confound the reading - see
 * spend_soil_moisture()'s own comment), its collar rewatered to
 * SOIL_MOISTURE_MAX every single step - about as generous as this
 * feature ever sees - and the claim is not "it stays small", it is that
 * the count REACHES A FIXED POINT: two counts taken apart in time, late
 * enough that growth has had its chance, must be equal. A system still
 * climbing between them is the shape of the runaway this whole feature
 * exists to avoid.
 *
 * WHAT THIS DOES NOT ISOLATE, watched red and found wanting: bumping
 * ROOT_SURFACE_MAX itself far past its real value does NOT turn this
 * test red - a board this small (20x14) runs out of reachable moist dirt
 * within 3000 steps regardless of the surface rule, so the count still
 * plateaus, just later and higher. This test is a regression guard on
 * SATURATION happening at all, not a proof of which of the three bounds
 * (moisture, surface rule, roll) is doing the work at this scale -
 * test_a_thickly_rooted_cell_stops_growing pins ROOT_SURFACE_MAX
 * specifically, and the real evidence that it is the surface rule
 * carrying the full-size scene (not merely a smaller board's own limits)
 * is the six-seed, 20,000-step measurement recorded in ROOT_SURFACE_MAX's
 * own comment (sand_reactions.c), where a bed sixty cells wide gives the
 * system far more room than 3,000 steps could plausibly exhaust. */
#define RUNAWAY_TEST_W 20
#define RUNAWAY_TEST_H 14

static void test_a_continuously_watered_root_system_still_saturates(void)
{
    uint8_t *grid = malloc((size_t)RUNAWAY_TEST_W * RUNAWAY_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(grid,
        "runaway grid must fit in what the framebuffer leaves");

    sand_t t;
    sand_init(&t, grid, RUNAWAY_TEST_W, RUNAWAY_TEST_H, 12345u);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    const int cx      = RUNAWAY_TEST_W / 2;
    const int floor_y = RUNAWAY_TEST_H - 1;
    const int bed_top = floor_y - 8;

    for (int x = 0; x < RUNAWAY_TEST_W; x++) {
        sand_set(&t, x, floor_y, STONE);
        for (int y = bed_top; y < floor_y; y++) {
            sand_set(&t, x, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
    }
    sand_set(&t, cx, bed_top - 1, CELL_MAKE(MAT_WOOD, 0));
    /* One row INTO the bed, not ON bed_top - the rewater loop below
     * overwrites bed_top's own 13 cells every single step, which would
     * erase a root planted there before step 0 even finished (measured:
     * this exact mistake, in the scratch harness this scene is modelled
     * on, produced a flat 0 for the entire run). */
    sand_set(&t, cx, bed_top + 1, MATX(MATX_ROOT));

    int mid_count = -1, final_count = -1;
    for (int i = 0; i < 3000; i++) {
        for (int dx = -6; dx <= 6; dx++) {
            const int x = cx + dx;
            if (x < 0 || x >= RUNAWAY_TEST_W) {
                continue;
            }
            sand_set(&t, x, bed_top, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&t, 0, 1000, 0);
        if (i == 1500) {
            mid_count = count_cells_of(MAT_EXTENDED);
        }
    }
    final_count = count_cells_of(MAT_EXTENDED);
    free(grid);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, mid_count,
        "setup failure, not the claim under test: the system must have "
        "grown past its single pre-placed root by the halfway point, or "
        "there is nothing here to say saturated");
    TEST_ASSERT_EQUAL_INT_MESSAGE(mid_count, final_count,
        "a system this continuously watered must still reach a fixed "
        "point rather than keep climbing - ROOT_SURFACE_MAX is what is "
        "supposed to hold it there (sand_reactions.c's own comment on "
        "the constant records the same comparison at 20,000 steps)");
}


/* Plant, leaf, ice, root and metal all take their texture from the
 * position hash, and everything else extended does not. Metal alone is
 * HATCHED rather than SPECKLED, for its travelling shine - so it gets
 * its own pattern check instead of sharing the other four's. */

/* The woven diagonal line that used to sit under that shine read as a
 * printed grid rather than metal, and was dropped; the shine itself
 * stayed. */

/* An extended material's variant IS which one it is, so neither can carry
 * a shade and the position hash is the only variation available - the same
 * tool stone and wood use, and right here for the same reason it was wrong
 * for dirt: neither a wall of ice, a grown tree, nor a smelted metal bar
 * moves.
 *
 * The negative half matters as much. The switch is on the low nibble, and
 * a mistake there does not fail to build - it paints some other extended
 * material in leaf green, which is the sort of thing nobody notices until
 * a fourteenth material arrives and comes out looking like a hedge. */
/* A root darkens by STRUCTURE: `depth` carries the count of root
 * neighbours (material_root_neighbours()), and the shade steps toward a
 * wood-like brown as it climbs - checked per channel, not as a luminance,
 * so a hue drift cannot pass as "darker". Wood and leaf read `depth` too,
 * but as a wave fraction, not a neighbour count - see their own tests. */
static unsigned r5(gfx_color_t c) { const unsigned n = (unsigned)((c >> 8) | (c << 8)) & 0xFFFFu; return (n >> 11) & 31u; }
static unsigned g6(gfx_color_t c) { const unsigned n = (unsigned)((c >> 8) | (c << 8)) & 0xFFFFu; return (n >> 5) & 63u; }
static unsigned b5(gfx_color_t c) { const unsigned n = (unsigned)((c >> 8) | (c << 8)) & 0xFFFFu; return n & 31u; }

static void test_a_root_darkens_as_more_root_grows_around_it(void)
{
    gfx_color_t col[3];
    gfx_color_t by_count[6];
    for (unsigned n = 0; n < 6u; n++) {
        material_colours(MATX(MATX_ROOT), 3u, 0u, n, col);
        by_count[n] = col[0];
    }
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(by_count[0], by_count[1],
        "a tip (one root neighbour) wears the same fresh colour as a lone seed");
    for (unsigned n = 1; n < 5u; n++) {
        TEST_ASSERT_TRUE_MESSAGE(r5(by_count[n + 1]) <= r5(by_count[n]) &&
                                 g6(by_count[n + 1]) <= g6(by_count[n]) &&
                                 b5(by_count[n + 1]) <= b5(by_count[n]),
            "each extra root neighbour may only darken a root, never lighten it");
    }
    TEST_ASSERT_TRUE_MESSAGE(r5(by_count[4]) < r5(by_count[1]) &&
                             g6(by_count[4]) < g6(by_count[1]) &&
                             b5(by_count[4]) < b5(by_count[1]),
        "a root touched on four sides must be visibly darker than a tip, in "
        "every channel - the gradient has to actually exist");
}

/* Leaf reads `depth` as the same wave fraction wood does, not a neighbour
 * count and not ignored either - leaf is now part of the same gust the
 * wood beside it catches (material_colours(), MATX_LEAF case). */
static void test_leaf_tints_toward_the_wave_highlight(void)
{
    gfx_color_t at_rest[3], mid[3], peak[3], other_hash_peak[3];

    /* hash 0's own base grain sits at LEAF_DARK, below the highlight's own
     * green, so blending toward it must raise green monotonically. */
    material_colours(MATX(MATX_LEAF), 0u, 0u, 0u, at_rest);
    material_colours(MATX(MATX_LEAF), 0u, 0u, 129u, mid);
    material_colours(MATX(MATX_LEAF), 0u, 0u, 256u, peak);
    material_colours(MATX(MATX_LEAF), 5u, 0u, 256u, other_hash_peak);

    TEST_ASSERT_TRUE_MESSAGE(g6(mid[0]) > g6(at_rest[0]) && g6(mid[0]) < g6(peak[0]),
        "the midpoint must sit strictly between the base grain and the peak");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(peak[0], other_hash_peak[0],
        "at full depth every hash converges on the exact same highlight colour");
}

/* The count itself, on three synthetic rows with the top one missing the
 * way paint_row_n() hands a NULL `above` on the grid's first row. */
static void test_root_neighbours_are_counted_across_three_rows(void)
{
    const cell_t R = MATX(MATX_ROOT), D = CELL_SOIL(MAT_DIRT, 0, 3), Wd = CELL_MAKE(MAT_WOOD, 0);
    const uint8_t above[5] = { R,  R,  R,  D,  D  };
    const uint8_t row[5]   = { D,  R,  R,  Wd, R  };
    const uint8_t below[5] = { D,  D,  R,  R,  D  };
    /* (2): above-left R, above R, left R, below R, below-right R = 5;
     * above-right D, right Wd, below-left D do not count. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(5u, material_root_neighbours(above, row, below, 2, 5),
        "counts root on all eight sides and nothing else - dirt and wood "
        "are not root");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3u, material_root_neighbours(NULL, row, below, 2, 5),
        "a NULL above row (the grid's top edge) contributes nothing - the "
        "two roots above (2) drop out, left/below/below-right remain");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, material_root_neighbours(above, row, below, 4, 5),
        "the right-hand grid edge is not read past");
}

/* material_wood_leaf_top5() must exclude exactly the 3 directions most
 * aligned with gravity, keeping the other 5 - see material_wood_near_leaf(). */
static bool top5_contains(const int8_t top5[5][2], int dx, int dy)
{
    for (int i = 0; i < 5; i++) {
        if (top5[i][0] == dx && top5[i][1] == dy) {
            return true;
        }
    }
    return false;
}

static void test_wood_leaf_top5_excludes_the_three_most_downward_directions(void)
{
    int8_t top5[5][2];
    int last_down = 0;

    /* Straight down: excludes down, down-left, down-right; keeps up,
     * up-left, up-right, left, right. */
    material_wood_leaf_top5(0, 100, &last_down, top5);
    TEST_ASSERT_FALSE_MESSAGE(top5_contains(top5, 0, 1), "down must not be kept");
    TEST_ASSERT_FALSE_MESSAGE(top5_contains(top5, -1, 1), "down-left must not be kept");
    TEST_ASSERT_FALSE_MESSAGE(top5_contains(top5, 1, 1), "down-right must not be kept");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, 0, -1), "up must be kept");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, -1, 0), "left must be kept");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, 1, 0), "right must be kept");
}

static void test_wood_leaf_top5_rotates_with_gravity(void)
{
    int8_t top5[5][2];
    int last_down = 0;

    /* (60, 80) is not tied with a neighbouring ring direction: down-right
     * wins outright, so it and its own two ring neighbours (down, right)
     * drop out instead of test 1's straight-down set. Down-left survives -
     * only 3 of 8 are ever excluded. */
    material_wood_leaf_top5(60, 80, &last_down, top5);
    TEST_ASSERT_FALSE_MESSAGE(top5_contains(top5, 0, 1), "down must not be kept");
    TEST_ASSERT_FALSE_MESSAGE(top5_contains(top5, 1, 1), "down-right must not be kept");
    TEST_ASSERT_FALSE_MESSAGE(top5_contains(top5, 1, 0), "right must not be kept");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, 1, -1), "up-right must be kept");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, -1, 1), "down-left must still be kept");
}

/* Hysteresis: recomputing "most downward" fresh every frame flipped right
 * at the tie between two ring directions, popping every wood cell whose
 * top5 set had just changed. */
static void test_wood_leaf_top5_resists_a_small_lead_near_the_boundary(void)
{
    int8_t top5[5][2];
    int last_down = 2; /* settled on "right" as of last frame */

    /* Down-right's dot (110) edges right's (100) by only 10, well under
     * the hysteresis margin for this gravity's magnitude (~25) - must not
     * flip just because it is technically now ahead. */
    material_wood_leaf_top5(100, 10, &last_down, top5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, last_down, "a small lead must not flip the sticky choice");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, 0, 1), "down stays kept - the choice did not move to down-right");
}

static void test_wood_leaf_top5_still_flips_on_a_clear_change(void)
{
    int8_t top5[5][2];
    int last_down = 2; /* settled on "right" as of last frame */

    /* Straight down beats "right" by the whole magnitude, far past the
     * margin - a real tilt must still move the sticky choice. */
    material_wood_leaf_top5(0, 100, &last_down, top5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, last_down, "a clear win must still flip the sticky choice");
    TEST_ASSERT_TRUE_MESSAGE(top5_contains(top5, 1, 0), "right is now kept - the choice moved to straight down");
}

/* material_wood_near_leaf() with `slots` 1 checks only the ONE slot `hash`
 * picks, not all five - the mechanism that turns a stable per-cell hash
 * into a stand-in for a one-time random assignment, at the cost of one
 * read instead of up to eight. */
static void test_wood_near_leaf_checks_its_assigned_slot(void)
{
    static const int8_t top5[5][2] = { {0, -1}, {-1, -1}, {1, -1}, {-1, 0}, {1, 0} };
    const cell_t L = MATX(MATX_LEAF), D = CELL_SOIL(MAT_DIRT, 0, 3), Wd = CELL_MAKE(MAT_WOOD, 0);
    const uint8_t above[5] = { D, D, L, D, D }; /* leaf sits directly "up" (slot 0) */
    const uint8_t row[5]   = { D, D, Wd, D, D };
    const uint8_t below[5] = { D, D, D, D, D };

    TEST_ASSERT_TRUE_MESSAGE(material_wood_near_leaf(above, row, below, 2, 5, top5, 0u, 1u),
        "hash 0 picks slot 0 (up), where the leaf actually is");
    TEST_ASSERT_FALSE_MESSAGE(material_wood_near_leaf(above, row, below, 2, 5, top5, 1u, 1u),
        "hash 1 picks slot 1 (up-left), empty - the leaf at slot 0 must not "
        "leak through a different slot's check");
    TEST_ASSERT_FALSE_MESSAGE(material_wood_near_leaf(NULL, row, below, 2, 5, top5, 0u, 1u),
        "a NULL above row (the grid's top edge) must not crash or false-positive");
}

/* `slots` widens how many of the 5 are checked, starting from the same
 * hash-picked slot and wrapping - not a different single slot, an actual
 * wider window, which is what makes coverage a tunable dial. */
static void test_wood_near_leaf_widens_coverage_with_more_slots(void)
{
    static const int8_t top5[5][2] = { {0, -1}, {-1, -1}, {1, -1}, {-1, 0}, {1, 0} };
    const cell_t L = MATX(MATX_LEAF), D = CELL_SOIL(MAT_DIRT, 0, 3), Wd = CELL_MAKE(MAT_WOOD, 0);
    const uint8_t above[5] = { D, D, L, D, D }; /* leaf at slot 0 ("up") */
    const uint8_t row[5]   = { D, D, Wd, D, D };
    const uint8_t below[5] = { D, D, D, D, D };

    TEST_ASSERT_FALSE_MESSAGE(material_wood_near_leaf(above, row, below, 2, 5, top5, 4u, 1u),
        "checking only slot 4 (right) finds nothing - the leaf is at slot 0");
    TEST_ASSERT_TRUE_MESSAGE(material_wood_near_leaf(above, row, below, 2, 5, top5, 4u, 2u),
        "checking 2 slots from the same start wraps around to slot 0 too");
}

/* `depth` carries the wave's fraction (0-255) plus one for MAT_WOOD here,
 * not a neighbour count (that's root's own use, see
 * test_a_root_darkens_as_more_root_grows_around_it above) - see the
 * MAT_WOOD case in material_colours(). */
static void test_unlit_wood_tints_green_only_beside_a_leaf(void)
{
    const cell_t wood = CELL_MAKE(MAT_WOOD, 0);
    gfx_color_t away[3], beside[3];

    const material_pattern_t pat_away = material_colours(wood, 3u, 0u, 0u, away);
    const material_pattern_t pat_beside = material_colours(wood, 3u, 0u, 129u, beside);

    TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_SPECKLED, pat_away, "plain wood keeps its grained pattern");
    TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_FLAT, pat_beside, "the live blend has no per-cell grain");
    TEST_ASSERT_NOT_EQUAL_HEX16_MESSAGE(away[0], beside[0],
        "a leaf neighbour must actually change unlit wood's colour");
}

/* The blend is a live LERP8 between the two named anchors, exact at both
 * ends (depth 1 = fraction 0 = WOOD_LEAF_TINT_LO, depth 256 = fraction 255
 * = WOOD_LEAF_TINT_HI) and monotonic in between - see the MAT_WOOD case in
 * material_colours(). */
static void test_wood_leaf_tint_blends_smoothly_between_its_anchors(void)
{
    const cell_t wood = CELL_MAKE(MAT_WOOD, 0);
    gfx_color_t lo[3], mid[3], hi[3];

    material_colours(wood, 3u, 0u, 1u, lo);
    material_colours(wood, 3u, 0u, 129u, mid);
    material_colours(wood, 3u, 0u, 256u, hi);

    TEST_ASSERT_TRUE_MESSAGE(g6(mid[0]) > g6(lo[0]) && g6(mid[0]) < g6(hi[0]),
        "the midpoint must sit strictly between the two anchors, not equal "
        "either one - a live blend, not a snap to the nearer end");
    TEST_ASSERT_TRUE_MESSAGE(g6(hi[0]) > g6(lo[0]), "the high anchor must read greener than the low one");
}

/* material_wood_leaf_wave() is the pure function behind the animation -
 * app_sand.c is not host-portable, so this is the only place its shape gets
 * checked (see docs/sand/Shading-and-Colour.md, "How to test a shading
 * change"). */
static void test_wood_leaf_wave_rises_then_falls_smoothly(void)
{
    /* pos 0, hash 0: no spatial or salt shift, isolating the wave's shape
     * in time. Known from material_palette.c: PERIOD 600, RISE 60,
     * FALL 140 - a short, thin gust against a longer quiet gap. hash 0's
     * own roll (see test_wood_leaf_wave_sometimes_sits_a_gust_out) happens
     * to activate cycle 0, so this still isolates the shape cleanly. */
    const unsigned at_start = material_wood_leaf_wave(0u, 0, 100, 0u);
    const unsigned at_rise_mid = material_wood_leaf_wave(30u, 0, 100, 0u);
    const unsigned at_peak = material_wood_leaf_wave(60u, 0, 100, 0u);
    const unsigned at_fall_mid = material_wood_leaf_wave(130u, 0, 100, 0u);
    const unsigned at_settled = material_wood_leaf_wave(200u, 0, 100, 0u);
    const unsigned at_quiet = material_wood_leaf_wave(400u, 0, 100, 0u);
    const unsigned at_wrap = material_wood_leaf_wave(600u, 0, 100, 0u);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, at_start, "the gust starts at baseline");
    TEST_ASSERT_TRUE_MESSAGE(at_rise_mid > at_start && at_rise_mid < at_peak,
        "rising smoothly toward the peak, not jumping straight there");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(255u, at_peak, "the peak sits at the end of the rise");
    TEST_ASSERT_TRUE_MESSAGE(at_fall_mid < at_peak && at_fall_mid > at_settled,
        "falling smoothly back down, not snapping");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, at_settled, "back to baseline once the fall finishes");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, at_quiet, "the quiet gap between gusts stays flat, not a slow ramp");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(at_start, at_wrap, "one full period returns to the same baseline");
}

/* Position along the wind axis shifts the wave, the mechanism behind the
 * "wind sweeping a grove" look: without it every tree would pulse together. */
static void test_wood_leaf_wave_shifts_with_position(void)
{
    /* 60ms puts pos 0 exactly at its peak; pos 10 of a 100-wide span
     * shifts by 10*SCREEN_SPAN_MS(4000)/100 = 400ms, landing it in the
     * quiet gap instead - proving the travelling band is narrow. */
    const unsigned at_pos0 = material_wood_leaf_wave(60u, 0, 100, 0u);
    const unsigned at_pos10 = material_wood_leaf_wave(60u, 10, 100, 0u);

    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(at_pos0, at_pos10,
        "different screen columns must sit at different points on the wave "
        "at the same instant, or every trunk would pulse together");
}

/* `hash` salts each cell's own phase within the shared sweep - the same
 * shape as glass's `(hash & 0xFF) + glass_phase`, so leaves catching the
 * same gust still light up at slightly different moments within it. */
static void test_wood_leaf_wave_salts_by_hash(void)
{
    const unsigned at_hash0 = material_wood_leaf_wave(60u, 0, 100, 0u);
    const unsigned at_hash100 = material_wood_leaf_wave(60u, 0, 100, 100u);

    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE(at_hash0, at_hash100,
        "two cells at the same time and position must still differ by hash "
        "alone, or every leaf would shade identically");
}

/* Every eligible cell lighting up on every gust read as one shine sweeping
 * through, not real wind. Known from material_palette.c: hash 0 activates
 * cycle 0's peak, is rolled out on cycle 1, and activates again on cycle 2 -
 * the same cell must skip some gusts, and which ones must vary over time. */
static void test_wood_leaf_wave_sometimes_sits_a_gust_out(void)
{
    const unsigned cycle0_peak = material_wood_leaf_wave(60u, 0, 100, 0u);
    const unsigned cycle1_peak = material_wood_leaf_wave(660u, 0, 100, 0u);
    const unsigned cycle2_peak = material_wood_leaf_wave(1260u, 0, 100, 0u);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(255u, cycle0_peak, "cycle 0's gust must activate");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, cycle1_peak, "cycle 1's gust must be rolled out");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(255u, cycle2_peak, "cycle 2's gust must activate again");
}

/* The wind axis must stay perpendicular to gravity - a grid axis (raw `cx`)
 * does not rotate with the device, so a landscape hold swept the wrong way
 * (top to bottom) until this replaced it. Checked by dot product rather
 * than exact components, since only the "stays level" property matters. */
static void test_wood_leaf_wind_axis_stays_perpendicular_to_gravity(void)
{
    static const int gxs[] = { 0, 100, -100, 60, -30 };
    static const int gys[] = { 0, 0, 40, -80, -70 };

    for (size_t i = 0; i < sizeof gxs / sizeof gxs[0]; i++) {
        int ux_q8 = 0, uy_q8 = 0;
        material_wood_leaf_wind_axis(gxs[i], gys[i], &ux_q8, &uy_q8);
        const long dot = (long)gxs[i] * ux_q8 + (long)gys[i] * uy_q8;
        char why[80];
        snprintf(why, sizeof why, "gravity (%d,%d), dot %ld", gxs[i], gys[i], dot);
        /* Exactly 0 for the true rotation; a few units of Q8 truncation
         * error survive integer division, but a swapped/un-negated
         * component would be off by orders of magnitude more than this. */
        TEST_ASSERT_TRUE_MESSAGE(dot > -400 && dot < 400, why);
    }

    int ux_q8 = 0, uy_q8 = 0;
    material_wood_leaf_wind_axis(0, 0, &ux_q8, &uy_q8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(256, ux_q8, "zero gravity defaults to sweeping along grid-x");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, uy_q8, "zero gravity defaults to sweeping along grid-x");
}

static void test_the_right_extended_materials_are_grained(void)
{
    gfx_color_t col[3] = { 0, 0, 0 };

    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        const cell_t c = MATX(k);
        const bool speckled = (k == MATX_PLANT || k == MATX_LEAF || k == MATX_ICE || k == MATX_ROOT);
        const bool hatched = (k == MATX_METAL);

        int distinct = 0;
        gfx_color_t seen[8];
        for (unsigned hash = 0; hash < 8u; hash++) {
            /* depth 0 for leaf: any nonzero depth now blends leaf's grain
             * toward the wave's highlight (material_colours(), MATX_LEAF
             * case), and 255 sits so far into that blend that RGB565
             * quantisation collapses all 8 hashes toward the same pixel -
             * a real property of the blend, not what THIS test checks. */
            const material_pattern_t pat = material_colours(c, hash, 0u,
                                                            k == MATX_LEAF ? 0u : 255u,
                                                            col);
            char why[96];
            snprintf(why, sizeof why, "extended material %d", k);
            TEST_ASSERT_EQUAL_MESSAGE(
                hatched ? MATERIAL_HATCHED
                        : (speckled ? MATERIAL_SPECKLED : MATERIAL_FLAT),
                pat, why);

            bool known = false;
            for (int i = 0; i < distinct; i++) {
                known = known || (seen[i] == col[0]);
            }
            if (!known) {
                seen[distinct++] = col[0];
            }
        }

        if (speckled || hatched) {
            TEST_ASSERT_GREATER_THAN_MESSAGE(4, distinct,
                "a grained material must actually use its grain - a table "
                "of eight identical colours is a flat fill with extra steps");
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, distinct,
                "and one without a grain must not vary with position - it "
                "would be the palette entry of some other material leaking "
                "through the wrong branch");
        }
    }

    /* Gunpowder's eight bytes (0xF8-0xFF) share MAT_EXTENDED's low nibble
     * with the statics above but are a different material entirely (see
     * GUNPOWDER_BASE, material.h) - drawn MATERIAL_FLAT like an ordinary
     * ungrained material, the variant carrying the shade instead of a
     * hash-driven grain. */
    for (int v = 0; v < 8; v++) {
        const cell_t c = GUNPOWDER_CELL(v);

        int distinct = 0;
        gfx_color_t seen[8];
        for (unsigned hash = 0; hash < 8u; hash++) {
            const material_pattern_t pat = material_colours(c, hash, 0u, 255u, col);
            char why[64];
            snprintf(why, sizeof why, "gunpowder code %d", v);
            TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_FLAT, pat, why);

            bool known = false;
            for (int i = 0; i < distinct; i++) {
                known = known || (seen[i] == col[0]);
            }
            if (!known) {
                seen[distinct++] = col[0];
            }
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, distinct,
            "gunpowder is ungrained - its colour must not vary with "
            "position, only with its own code (tone or moisture level)");
    }
}

/* Metal's shine does NOT vary from cell to cell: light landing on the
 * surface, not the surface itself, and a highlight that wobbled per cell
 * would look chewed rather than reflective. */

/* Its line no longer exists as a separate colour - out[1] just mirrors
 * out[0] now that the woven diagonal is gone - so that half of the old
 * check is replaced with confirming the mirror instead. */
static void test_metal_shine_does_not_vary_between_cells(void)
{
    gfx_color_t a[3], b[3];
    material_colours(MATX(MATX_METAL), 0u, 0u, 255u, a);
    material_colours(MATX(MATX_METAL), 5u, 0u, 255u, b);

    TEST_ASSERT_EQUAL_MESSAGE(a[1], a[0],
        "metal's line colour must mirror its body - there is no separate "
        "line any more");
    TEST_ASSERT_EQUAL_MESSAGE(a[2], b[2],
        "metal's shine colour must be identical in every cell");
}

/* The three airborne materials agree with themselves about weight, speed
 * and lifetime.
 *
 * Steam is the lightest and quickest; a heavy flammable gas should pool
 * and wait. For a long time gas outlived both - gas faded in about 120
 * steps, steam in 160, smoke in 240 - so the heaviest, slowest thing in
 * the air was also the first to disappear, and a pocket of gas could not
 * be built with.
 *
 * STEAM'S OWN DECAY HAS MOVED SEVERAL TIMES SINCE, ALWAYS DOWNWARD (24,
 * then 18, then 16 - briefly landing exactly on smoke's own figure and
 * matching it on purpose - then 12, on a later request for steam to last
 * at least 30% longer still). None of those moves ever required smoke to
 * be equal or slower - the assertion below only ever forbade steam fading
 * FASTER than smoke, so it has never needed to change alongside steam's
 * figure; the ordering that actually matters is gas outlasting both of
 * the lighter, quicker-fading ones.
 *
 * Asserted on the TABLE rather than by watching cells fade. Three
 * populations decaying past each other is a slow and noisy way to check a
 * fact that is written down in one place, and this is the same reasoning
 * as the acid test above: nonzero is not a claim about the right value,
 * only that somebody chose one and that the three still agree. */
static void test_the_air_agrees_about_weight_speed_and_lifetime(void)
{
    /* Lighter rises faster. */
    TEST_ASSERT_LESS_THAN_MESSAGE(material_by_id((material_id_t)MAT_SMOKE)->density,
        material_by_id((material_id_t)MAT_STEAM)->density, "steam must be lighter than smoke");
    TEST_ASSERT_LESS_THAN_MESSAGE(material_by_id((material_id_t)MAT_GAS)->density,
        material_by_id((material_id_t)MAT_SMOKE)->density, "smoke must be lighter than gas");

    TEST_ASSERT_GREATER_THAN_MESSAGE(material_by_id((material_id_t)MAT_SMOKE)->mobility,
        material_by_id((material_id_t)MAT_STEAM)->mobility, "steam must move faster than smoke");
    TEST_ASSERT_GREATER_THAN_MESSAGE(material_by_id((material_id_t)MAT_GAS)->mobility,
        material_by_id((material_id_t)MAT_SMOKE)->mobility, "smoke must move faster than gas");

    /* And the lighter it is, the sooner it is gone: decay is a chance to
     * tick DOWN, so a bigger figure is a shorter life. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(material_by_id((material_id_t)MAT_SMOKE)->decay,
        material_by_id((material_id_t)MAT_STEAM)->decay,
        "steam must not fade FASTER than smoke - equal or slower is fine, "
        "steam's own decay has moved either way over time, just not "
        "reversed past smoke's entirely");
    TEST_ASSERT_GREATER_THAN_MESSAGE(material_by_id((material_id_t)MAT_GAS)->decay,
        material_by_id((material_id_t)MAT_SMOKE)->decay,
        "and smoke sooner than gas - the heaviest, slowest thing in the "
        "air must be the last to go, or a pocket of it cannot be built "
        "with");
}


/* Steam melts ice. Ordinary gas does not.
 *
 * Reported from a scene anyone would build: lava in a pan, water poured on
 * to make a boiler, a sheet of ice above it - and the steam rising into
 * the ice did nothing at all.
 *
 * The gap was that convection only knew how to warm a material whose
 * variant IS a temperature. Glass and stone bank heat and climb a level at
 * a time; ice cannot, and structurally never will, because it is an
 * extended material whose low nibble is which material it is. There is no
 * room left in the cell to hold a temperature. So hot gas walked straight
 * past it, while a flame touching the same cell melted it at once -
 * try_heat_transform() has always had a second, memoryless branch for
 * exactly this, and convection had simply never learned it.
 *
 * The negative half is what pins the gas's own gate: plain gas has no
 * `warms` at all, and must leave ice alone however much of it there is. */
static void test_steam_melts_ice_and_plain_gas_does_not(void)
{
    const int cx = W / 2, cy = H / 2;

    for (int pass = 0; pass < 2; pass++) {
        const material_id_t air = pass ? MAT_GAS : MAT_STEAM;

        fixture();
        sand_clear(&s);
        sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
        sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

        sand_set(&s, cx, cy, MATX(MATX_ICE));
        /* A bystanding grain of sand, on a floor so it stays put. Wood,
         * not stone: stone banks heat, and this board must have nothing
         * on it that holds a temperature, or the reachability half of
         * the test below would be answered by the floor. */
        sand_set(&s, cx + 3, cy + 1, CELL_MAKE(MAT_WOOD, 0));
        sand_set(&s, cx + 3, cy, CELL_MAKE(MAT_SAND, 4));

        int melted = 0;
        for (int i = 0; i < 3000 && !melted; i++) {
            /* Held in a bath of it, replenished, so the question is only
             * whether contact does anything - not whether a rising gas
             * happens to still be there. */
            for (int y = cy - 1; y <= cy + 1; y++) {
                for (int x = cx - 1; x <= cx + 4; x++) {
                    if (CELL_IS_EMPTY(sand_at(&s, x, y))) {
                        sand_set(&s, x, y,
                                 CELL_MAKE(air, MATERIAL_VARIANTS - 1));
                    }
                }
            }
            sand_step(&s, 0, 1000, 0);
            melted = (sand_at(&s, cx, cy) != MATX(MATX_ICE));
        }

        /* And the branch is a THAW, not a melt: sand's heats_to is
         * glass, so a version of this that asked only "can it be heated
         * into something" had a smoke cloud slowly vitrifying every dune
         * it drifted over. Warm air thaws cold things; it does not fire
         * a kiln. */
        int glass = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_GLASS) {
                    glass++;
                }
            }
        }
        TEST_ASSERT_EQUAL_MESSAGE(0, glass,
            "warm air must not turn sand into glass - convection thaws "
            "what is cold, it does not fire a kiln");

        if (air == MAT_STEAM) {
            TEST_ASSERT_TRUE_MESSAGE(melted,
                "steam must melt ice - it is water at a hundred degrees "
                "and ice is water at zero, and a boiler under a sheet of "
                "it did nothing at all");
        } else {
            TEST_ASSERT_FALSE_MESSAGE(melted,
                "but plain gas must not - it carries no heat, and if "
                "convection skipped its own `warms` gate then every gas "
                "on the board would be a thaw");
        }
    }
}



/* Two pours apart in time come out as two different shades.
 *
 * This is how a pile gets layers, and the whole of what it costs is
 * choosing a different number in the one draw that was already being
 * made. A grain's shade was always picked at spawn and always lived in
 * the cell; it was simply spread across the entire band, so every pour
 * looked like every other and a pile was uniform speckle from top to
 * bottom. Centring the draw on a slowly drifting band instead means a
 * brushful is nearly one shade, the next brushful is another, and the
 * first pile stays legible after the second is poured on top of it.
 *
 * Worth stating what this is NOT, because that was built first and
 * measured: it does not look at cells, it does not know which of them are
 * at a surface, and it adds no pass, no flag and no per-cell test. A
 * version that crusted exposed grains in place did all of those and cost
 * 4.4 microseconds a step against 1.0 on a settled board. This one
 * measures 1.0 - the same as having nothing at all.
 *
 * The narrowness matters as much as the difference. A pour spread over
 * the whole band again would put every shade in every layer and there
 * would be no line anywhere. */
static void test_two_pours_apart_in_time_lay_down_different_shades(void)
{
    int lo[2] = { 99, 99 }, hi[2] = { -1, -1 };

    fixture();
    sand_clear(&s);

    for (int pour = 0; pour < 2; pour++) {
        sand_spawn(&s, W / 2, H / 2, 2, MAT_SAND);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) != MAT_SAND) {
                    continue;
                }
                const int v = CELL_VARIANT(c);
                if (v < lo[pour]) { lo[pour] = v; }
                if (v > hi[pour]) { hi[pour] = v; }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(hi[pour] >= 0, "the pour must have landed");
        TEST_ASSERT_LESS_THAN_MESSAGE(SAND_DUNE_SHADES, hi[pour],
            "and must stay inside the DUNE band - the four shades above it "
            "are cullet, and poured sand must never claim to have been a "
            "window");
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(2, hi[pour] - lo[pour],
            "one pour must be NARROW in shade - a brushful spread across "
            "the whole band again would put every shade in every layer, "
            "and there would be no line anywhere to see");

        sand_clear(&s);
        /* Long enough for the band to drift exactly one place along. */
        for (int i = 0; i < 64; i++) {
            sand_step(&s, 0, 1000, 0);
        }
    }

    const int gap = (lo[0] + hi[0]) / 2 - (lo[1] + hi[1]) / 2;
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(3, gap < 0 ? -gap : gap,
        "and two pours a couple of seconds apart must be visibly different "
        "shades - that difference IS the layer, and without it a pile is "
        "one flat speckle however many times it was poured");
}


/* A grain carries its shade wherever it goes.
 *
 * This is the invariant the whole of the layering rests on, and nothing
 * asserted it. The shade is chosen once, at spawn, and layers only mean
 * anything because a grain that falls, slides, avalanches and gets buried
 * arrives with the shade it started with - so a buried surface is still
 * the shade it was when it was a surface.
 *
 * It holds today because the sweep MOVES cells rather than making new
 * ones: the byte travels, and the shade is in the byte. That is easy to
 * break by accident. Anything that re-rolls a shade when a grain settles,
 * or picks one from where the grain has landed, would leave every other
 * test passing and quietly turn every pile back into uniform speckle.
 *
 * Checked as a MULTISET rather than cell by cell, because where each
 * grain ends up is the sweep's business and not this test's. What matters
 * is that the same shades are still on the board, in the same numbers. */
static void test_a_moving_grain_keeps_the_shade_it_was_poured_with(void)
{
    int before[MATERIAL_VARIANTS] = { 0 }, after[MATERIAL_VARIANTS] = { 0 };

    fixture();
    sand_clear(&s);

    /* A floor, and a step for the grains to slide off - so they fall, land,
     * pile up and topple sideways rather than just dropping straight. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 0; x < W / 2; x++) {
        sand_set(&s, x, H - 2, STONE);
    }
    sand_spawn(&s, W / 2, 1, 2, MAT_SAND);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_SAND) {
                before[CELL_VARIANT(c)]++;
            }
        }
    }
    int poured = 0;
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        poured += before[v];
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, poured, "the pour must have landed");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_SAND) {
                after[CELL_VARIANT(c)]++;
            }
        }
    }
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        TEST_ASSERT_EQUAL_MESSAGE(before[v], after[v],
            "a grain must arrive with the shade it was poured with - the "
            "sweep moves cells, so the shade rides in the byte, and if "
            "anything re-rolled it on the way then a buried surface would "
            "no longer be the shade it was when it was a surface");
    }
}


/* Sand that turns to soil arrives WET, and a wet cell carries no tone of
 * its own (material.h's state-split comment) - so the grain's shade has
 * nowhere to go. Wet soil's variation instead comes from the moisture
 * gradient percolation lays down (step_one_soaking_cell()'s soaks_to
 * branch, sand_reactions.c). Whichever end of the dune band a grain came
 * from, it converts to the same moisture - the one unit `soaks` just
 * took - since shade plays no part in the conversion. */
static void test_wet_sand_becomes_soil_wet_with_no_tone_of_its_own(void)
{
    const uint8_t dark_shade = 1;                       /* low half  */
    const uint8_t pale_shade = SAND_DUNE_SHADES - 1;    /* high half */

    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 1, H - 2, CELL_MAKE(MAT_SAND, dark_shade));
    sand_set(&s, 5, H - 2, CELL_MAKE(MAT_SAND, pale_shade));

    for (int i = 0; i < 3000; i++) {
        /* Keep both of them standing in water. */
        for (int k = 0; k < 2; k++) {
            const int x = k ? 5 : 1;
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    const cell_t from_dark = sand_at(&s, 1, H - 2);
    const cell_t from_pale = sand_at(&s, 5, H - 2);
    TEST_ASSERT_EQUAL_MESSAGE(MAT_DIRT, CELL_MATERIAL(from_dark),
        "the dark grain must have soaked into soil by now");
    TEST_ASSERT_EQUAL_MESSAGE(MAT_DIRT, CELL_MATERIAL(from_pale),
        "and so must the pale one");

    TEST_ASSERT_TRUE_MESSAGE(CELL_MOISTURE(from_dark) != 0,
        "soil made from a soaking grain must arrive WET, not dry with a "
        "tone borrowed from the grain's own shade");
    TEST_ASSERT_TRUE_MESSAGE(CELL_MOISTURE(from_pale) != 0,
        "and so must the pale one");
    TEST_ASSERT_EQUAL_MESSAGE(CELL_VARIANT(from_dark), CELL_VARIANT(from_pale),
        "with no tone left to carry over, two grains from opposite ends "
        "of the dune band must land on the exact same soil variant once "
        "both have soaked - shade no longer has anything to say about "
        "it");
}

/* Every material has a colour, and every extended material has one too.
 *
 * The palette is one flat array of 256 entries indexed by the whole cell
 * byte, and C zero-fills whatever an initialiser does not reach. So a
 * material whose block is missing does not fail to build - it renders
 * BLACK, which looks like a styling choice rather than a bug.
 *
 * That has happened twice. Both times a block was added or removed
 * somewhere in the middle and every block after it shifted by sixteen: the
 * first time the extended range landed on the wrong id, the second time
 * folding ember out left ice reading from the zero-filled tail. Reported
 * as "ice look is pretty bad, just black", which is exactly what an unset
 * palette entry looks like.
 *
 * The blocks carry explicit `[MAT_X * MATERIAL_VARIANTS] =` designators
 * now, so removing a material cannot shift the ones after it. This checks
 * the result rather than the mechanism, because the failure is silent
 * either way. */
static void test_every_material_has_a_palette_block(void)
{
    const gfx_color_t *pal = material_palette();

    for (int m = 0; m < MAT_COUNT; m++) {
        int set = 0;
        for (int v = 0; v < MATERIAL_VARIANTS; v++) {
            if (pal[m * MATERIAL_VARIANTS + v] != 0) {
                set++;
            }
        }
        /* 192, not 144: the device build's -Werror=format-truncation
         * proves 144 can clip the tail of this message for a long
         * material name, and a clipped diagnostic is exactly the kind
         * of silent failure this test exists to make loud. */
        char why[192];
        snprintf(why, sizeof why,
                 "%s (id %d) has %d of %d palette entries set - a block "
                 "that is missing or misaligned renders black, and black "
                 "is not an error anyone sees as one",
                 material_by_id((material_id_t)m)->name, m, set,
                 MATERIAL_VARIANTS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS, set, why);
    }

    /* And the extended STATICS, whose entries are one per material rather
     * than a block each - the same failure, one level down. */
    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        char why[128];
        snprintf(why, sizeof why,
                 "extended material %d (cell 0x%02X) has no colour", k,
                 (unsigned)MATX(k));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, pal[MATX(k)], why);
    }

    /* And GUNPOWDER's half of the same nibble - see GUNPOWDER_BASE
     * (material.h). Phase 1 leaves these on the shared magenta placeholder
     * (material.c's palette tail already reaches 0xFF), which is enough to
     * satisfy "not black"; Phase 2 gives them real colours. */
    for (int v = 0; v < 8; v++) {
        char why[128];
        snprintf(why, sizeof why,
                 "gunpowder variant %d (cell 0x%02X) has no colour", v,
                 (unsigned)GUNPOWDER_CELL(v));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, pal[GUNPOWDER_CELL(v)], why);
    }
}

/* Ice is the colour it is meant to be, not merely some colour.
 *
 * The check above catches a block that is missing. This catches one that
 * is present but WRONG - reading a neighbouring material's entry, which is
 * what a shifted palette produces and what the count test cannot see. */
static void test_ice_is_its_own_colour(void)
{
    const gfx_color_t *pal = material_palette();
    const gfx_color_t ice = pal[MATX(MATX_ICE)];

    TEST_ASSERT_EQUAL_MESSAGE(GFX_RGB(0xB6E4F2), ice,
        "ice must be the pale blue its own palette entry names - anything "
        "else means the entry is being read from somewhere other than "
        "where it was written");

    for (int m = 0; m < MAT_COUNT; m++) {
        for (int v = 0; v < MATERIAL_VARIANTS; v++) {
            if (pal[m * MATERIAL_VARIANTS + v] == ice) {
                char why[128];
                snprintf(why, sizeof why,
                         "ice shares a colour with %s variant %d",
                         material_by_id((material_id_t)m)->name, v);
                TEST_FAIL_MESSAGE(why);
            }
        }
    }
}

/* An extended material keeps its identity through a paint.
 *
 * Its low nibble IS which material it is, so anything that treats the
 * variant as a shade to randomise - which is what happens to every
 * ordinary static material - would silently repaint it as a different
 * extended material. That is the one way this scheme can go wrong
 * quietly. */
static void test_an_extended_material_survives_being_painted(void)
{
    fixture();
    sand_clear(&s);

    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        sand_clear(&s);
        sand_spawn_cell(&s, W / 2, H / 2, 0, MATX(k));
        const cell_t got = sand_at(&s, W / 2, H / 2);

        char why[96];
        snprintf(why, sizeof why,
                 "extended material %d came back as %d", k, CELL_VARIANT(got));
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_EXTENDED, CELL_MATERIAL(got), why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k, CELL_VARIANT(got), why);
    }

    /* GUNPOWDER'S identity is being gunpowder AT ALL - unlike a static's
     * low nibble, its low THREE bits are a TONE (random_gunpowder(),
     * sand.c), not a second material to preserve byte-exact, so a paint
     * may pick any of the three dry tones without that being the bug this
     * test exists to catch. What must never happen is a gunpowder spec
     * coming back as an extended STATIC or an ordinary material - see
     * test_painted_gunpowder_starts_dry_in_one_of_three_tones for the tone
     * distribution itself. */
    for (int v = 0; v < 8; v++) {
        sand_clear(&s);
        sand_spawn_cell(&s, W / 2, H / 2, 0, GUNPOWDER_CELL(v));
        const cell_t got = sand_at(&s, W / 2, H / 2);

        char why[96];
        snprintf(why, sizeof why,
                 "gunpowder spec 0x%02X came back as 0x%02X",
                 (unsigned)GUNPOWDER_CELL(v), got);
        TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(got), why);
    }
}

/* They share one physics row, and that is the deal.
 *
 * material_of() is read per cell per step by the sweep, so it must not
 * decode anything. Every extended material therefore moves - or rather
 * does not move - identically. Asserting it keeps someone from quietly
 * adding a row that expects otherwise. */
static void test_every_extended_material_shares_one_physics_row(void)
{
    const material_t *first = material_of(MATX(0));

    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        char why[96];
        snprintf(why, sizeof why, "extended material %d", k);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(first, material_of(MATX(k)), why);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_STATIC, first->kind,
        "the shared row has to be an inert solid - anything that moves "
        "needs its own physics, which is exactly what the extended range "
        "cannot give it");

    /* GUNPOWDER shares ITS OWN one row across all eight of its codes - the
     * same deal as the statics above, one level up: material_of() still
     * decodes nothing finer than "which half of nibble 15" (MATERIAL_ROWS,
     * material.h), so gunpowder's eight tones move identically to each
     * other too. That row is a SEPARATE row from the statics' - the whole
     * point of the split - and it is the one KIND_POWDER extended-range
     * physics gets. */
    const material_t *powder = material_of(GUNPOWDER_CELL(0));
    for (int v = 0; v < 8; v++) {
        char why[96];
        snprintf(why, sizeof why, "gunpowder variant %d", v);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(powder, material_of(GUNPOWDER_CELL(v)),
                                      why);
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(first, powder,
        "gunpowder must NOT read the statics' shared row - that is the "
        "entire reason the hot table grew a second row for nibble 15");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_POWDER, powder->kind,
        "gunpowder is the one extended-range material that moves");
}

/* Same fact as the test above, asserted again on purpose - this one exists
 * for a different reader. The rule deciding which materials may be
 * emitters (see sand_add_emitter() in sand.h) is KIND_POWDER/LIQUID/GAS
 * may, KIND_STATIC may not - a static source would bury itself on its
 * first step and jam forever. That rule has to ask material_of(c)->kind.
 *
 * Since the hot table split (MATERIAL_ROWS, material.h), material_of() DOES
 * tell gunpowder apart from an extended static - they are different rows
 * now. Ice, Plant, Leaf, Metal and Root still answer the emitter question
 * through ONE shared row, and that row still has to stay KIND_STATIC for
 * the derivation below to hold for them; gunpowder answers through its OWN
 * row, which is KIND_POWDER on purpose - it is the one extended-range
 * material meant to emit.
 *
 * This cannot be a _Static_assert: materials[] is `extern const`, so its
 * contents are not a constant expression the preprocessor or compiler can
 * see. A host test that fails loudly is the next best thing - and it
 * needs to fail loudly right here, not wherever the emitter-eligibility
 * code eventually lands, since that code will have no way to know this
 * assumption exists. */
static void test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_STATIC,
        material_by_id(MAT_EXTENDED)->kind,
        "the emitter-eligibility rule (KIND_POWDER/LIQUID/GAS may emit, "
        "KIND_STATIC may not) reads this row via material_of(), which "
        "cannot tell one STATIC from another - if this ever stops being "
        "KIND_STATIC, that rule must be revisited PER extended material "
        "rather than left to derive an answer from a row shared by all "
        "eight");
    TEST_ASSERT_FALSE_MESSAGE(material_can_emit(MATX(MATX_ICE)),
        "a static must not be emitter-eligible");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_POWDER,
        material_of(GUNPOWDER_CELL(0))->kind,
        "gunpowder is the one extended-range material meant to emit, and "
        "its own row - not the statics' - is what material_can_emit() "
        "actually reads for it now that the two are separate rows");
    TEST_ASSERT_TRUE_MESSAGE(material_can_emit(GUNPOWDER_CELL(0)),
        "gunpowder must be emitter-eligible, unlike every other "
        "extended-range byte");
}

/* But they get their own reactions, which is the point of the range. */
static void test_extended_materials_get_their_own_reactions(void)
{
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, reaction_of(MATX(MATX_ICE))->chills,
        "ice must chill - an extended material with no reactions of its "
        "own would just be a coloured block");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reaction_of(MATX(1))->chills,
        "and an extended material that has not been defined must not "
        "inherit the reactions of one that has - they are separate rows, "
        "not one shared row like the physics");
}

/* Every ordinary material's row was written ONCE (material.c's TWIN_ROW
 * macro) and has to land in BOTH halves of its row pair - variant 0 hashes
 * to MATERIAL_ROW(id), variant 15 to MATERIAL_ROW(id) + 1 (cell >> 3 turns
 * on the top bit of the low nibble - see MATERIAL_ROWS, material.h) - and
 * material_of() must return the identical row either way, or a grain of
 * the same material could quietly behave differently depending on which
 * half of its shade band it happened to be painted into. */
static void test_every_ordinary_material_has_identical_twin_rows(void)
{
    for (int id = 0; id < MAT_EXTENDED; id++) {
        const material_t *lo = material_of(CELL_MAKE(id, 0));
        const material_t *hi = material_of(CELL_MAKE(id, 15));
        char why[64];
        snprintf(why, sizeof why, "material id %d (%s)",
                 id, material_by_id((material_id_t)id)->name);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(lo, hi, sizeof(*lo), why);
    }
}

/* NIBBLE 15's TWO ROWS, checked directly against the byte ranges that
 * define them (GUNPOWDER_BASE, material.h): every byte 0xF0-0xF7 must
 * read as an extended STATIC and nothing else, every byte 0xF8-0xFF must
 * read as GUNPOWDER and nothing else, and the two must never agree with
 * each other about which is which - that partition is the entire point
 * of splitting the hot table by `cell >> 3` instead of `cell >> 4`. */
static void test_the_extended_half_rows_are_static_and_powder(void)
{
    for (int v = 0; v < 8; v++) {
        const cell_t stat = MATX(v);
        char why[48];
        snprintf(why, sizeof why, "static code %d", v);
        TEST_ASSERT_TRUE_MESSAGE(cell_is_extended(stat), why);
        TEST_ASSERT_FALSE_MESSAGE(cell_is_gunpowder(stat), why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_STATIC, material_of(stat)->kind, why);
    }
    for (int v = 0; v < 8; v++) {
        const cell_t pow = GUNPOWDER_CELL(v);
        char why[48];
        snprintf(why, sizeof why, "gunpowder code %d", v);
        TEST_ASSERT_FALSE_MESSAGE(cell_is_extended(pow), why);
        TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(pow), why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_POWDER, material_of(pow)->kind, why);
    }
}

/* THE MACROS AND THE TABLE-DRIVEN HELPERS MUST AGREE, for every one of
 * dirt's sixteen possible bytes - this is what makes it safe to leave
 * CELL_MOISTURE()/CELL_WITH_MOISTURE()/CELL_SOIL() in place for the many
 * existing tests that already use them while the ENGINE (sand_reactions.c,
 * sand_priv.h, sand.c) reads through moisture_of()/with_moisture()/
 * soil_cell() instead (material.h). If the two ever disagreed, dirt's own
 * behaviour would fork depending on which path happened to touch a cell. */
static void test_dirt_moisture_macros_and_codec_helpers_agree_on_every_byte(void)
{
    const reaction_t *r = &reactions[MAT_DIRT];
    const cell_t base = CELL_MAKE(MAT_DIRT, 0);

    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        const cell_t c = CELL_MAKE(MAT_DIRT, v);
        char why[64];
        snprintf(why, sizeof why, "dirt variant %d", v);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)CELL_MOISTURE(c),
            moisture_of(c, r), why);
    }

    for (int m = 1; m <= SOIL_MOISTURE_MAX; m++) {
        char why[64];
        snprintf(why, sizeof why, "moisture level %d", m);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(CELL_WITH_MOISTURE(base, (uint8_t)m),
            with_moisture(base, (uint8_t)m, r), why);
    }

    for (int tone = 0; tone < SOIL_DRY_TONES; tone++) {
        char why[64];
        snprintf(why, sizeof why, "dry tone %d", tone);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(CELL_SOIL(MAT_DIRT, tone, 0),
            soil_cell(base, (uint8_t)tone, 0, r), why);
    }
    for (int m = 1; m <= SOIL_MOISTURE_MAX; m++) {
        char why[64];
        snprintf(why, sizeof why, "soil built wet at moisture %d", m);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(CELL_SOIL(MAT_DIRT, 0, m),
            soil_cell(base, 0, (uint8_t)m, r), why);
    }
}

/* PAINTED GUNPOWDER: dry TONE, never moisture - the same claim
 * test_new_dirt_starts_dry_in_a_random_tone makes for dirt, extended to
 * gunpowder's narrower three-tone codec and its own picker
 * (random_gunpowder(), sand.c) instead of random_cell(). Several pours,
 * the pour clock jumped between them the way
 * test_consecutive_dirt_pours_land_on_different_bands does, because one
 * pour's own +/-1 jitter around a single band is not guaranteed to visit
 * every one of only three tones by itself. */
static void test_painted_gunpowder_starts_dry_in_one_of_three_tones(void)
{
    fixture();
    sand_clear(&s);

    const uint8_t tones = reaction_of(GUNPOWDER_BASE)->tones;
    bool seen[8];
    memset(seen, 0, sizeof seen);
    int distinct = 0;

    for (int i = 0; i < W * H / 2; i++) {
        s.pour_phase = (uint32_t)i << 6; /* one POUR_BAND_SHIFT tick per pour -
                                          * see sand.c, not exposed here */
        sand_spawn_cell(&s, i % W, i / W, 0, GUNPOWDER_CELL(0));
        const cell_t c = sand_at(&s, i % W, i / W);

        TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
            "a gunpowder spec must come back as gunpowder");

        const uint8_t code = (uint8_t)(c & 0x07);
        char why[64];
        snprintf(why, sizeof why, "gunpowder code %d", code);
        /* code >= tones would be a MOISTURE level - see material.h's
         * moisture codec comment - and a freshly poured grain arriving
         * already wet is exactly the bug random_cell()'s own dirt branch
         * exists to avoid, now on gunpowder's picker instead. */
        TEST_ASSERT_TRUE_MESSAGE(code < tones, why);
        if (!seen[code]) {
            seen[code] = true;
            distinct++;
        }
    }

    char why[128];
    snprintf(why, sizeof why,
             "only %d of %d dry tones appeared over %d pours - "
             "random_gunpowder() should eventually visit all of them",
             distinct, tones, W * H / 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(tones, distinct, why);
}

void run_sand_roots_suite(void)
{
    RUN_TEST(test_a_watered_plant_roots_into_the_soil_it_drinks_from);
    RUN_TEST(test_a_trunk_standing_on_its_own_root_is_anchored);
    RUN_TEST(test_a_root_column_does_not_spend_the_trees_lift);
    RUN_TEST(test_a_buried_root_does_not_cut_off_the_water_below_it);
    RUN_TEST(test_a_root_column_reaches_below_the_collar);
    RUN_TEST(test_lava_burns_a_root_out_of_the_ground);
    RUN_TEST(test_fire_leaves_a_root_alone);
    RUN_TEST(test_a_canopy_waters_the_soil_through_its_own_roots);
    RUN_TEST(test_a_rooted_collar_survives_the_bed_shifting_away);
    RUN_TEST(test_a_root_is_inert);
    RUN_TEST(test_root_conversion_never_creates_moisture);
    RUN_TEST(test_a_root_eats_a_moist_neighbour_and_only_spends_its_own_moisture);
    RUN_TEST(test_a_root_never_eats_dry_dirt_sand_or_empty_space);
    RUN_TEST(test_a_root_tip_grows_on_away_from_its_parent_and_down);
    RUN_TEST(test_the_first_root_under_a_trunk_heads_down);
    RUN_TEST(test_a_root_carries_a_level_of_water_down_through_itself);
    RUN_TEST(test_conduction_never_pushes_water_up_or_into_anything_but_soil);
    RUN_TEST(test_a_thickly_rooted_cell_stops_growing);
    RUN_TEST(test_roots_grow_toward_the_wet_side_only);
    RUN_TEST(test_a_continuously_watered_root_system_still_saturates);
    RUN_TEST(test_a_root_darkens_as_more_root_grows_around_it);
    RUN_TEST(test_leaf_tints_toward_the_wave_highlight);
    RUN_TEST(test_root_neighbours_are_counted_across_three_rows);
    RUN_TEST(test_wood_leaf_top5_excludes_the_three_most_downward_directions);
    RUN_TEST(test_wood_leaf_top5_rotates_with_gravity);
    RUN_TEST(test_wood_leaf_top5_resists_a_small_lead_near_the_boundary);
    RUN_TEST(test_wood_leaf_top5_still_flips_on_a_clear_change);
    RUN_TEST(test_wood_near_leaf_checks_its_assigned_slot);
    RUN_TEST(test_wood_near_leaf_widens_coverage_with_more_slots);
    RUN_TEST(test_unlit_wood_tints_green_only_beside_a_leaf);
    RUN_TEST(test_wood_leaf_tint_blends_smoothly_between_its_anchors);
    RUN_TEST(test_wood_leaf_wave_rises_then_falls_smoothly);
    RUN_TEST(test_wood_leaf_wave_shifts_with_position);
    RUN_TEST(test_wood_leaf_wave_salts_by_hash);
    RUN_TEST(test_wood_leaf_wave_sometimes_sits_a_gust_out);
    RUN_TEST(test_wood_leaf_wind_axis_stays_perpendicular_to_gravity);
    RUN_TEST(test_the_right_extended_materials_are_grained);
    RUN_TEST(test_metal_shine_does_not_vary_between_cells);
    RUN_TEST(test_the_air_agrees_about_weight_speed_and_lifetime);
    RUN_TEST(test_steam_melts_ice_and_plain_gas_does_not);
    RUN_TEST(test_two_pours_apart_in_time_lay_down_different_shades);
    RUN_TEST(test_a_moving_grain_keeps_the_shade_it_was_poured_with);
    RUN_TEST(test_wet_sand_becomes_soil_wet_with_no_tone_of_its_own);
    RUN_TEST(test_every_material_has_a_palette_block);
    RUN_TEST(test_ice_is_its_own_colour);
    RUN_TEST(test_an_extended_material_survives_being_painted);
    RUN_TEST(test_every_extended_material_shares_one_physics_row);
    RUN_TEST(test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on);
    RUN_TEST(test_extended_materials_get_their_own_reactions);
    RUN_TEST(test_every_ordinary_material_has_identical_twin_rows);
    RUN_TEST(test_the_extended_half_rows_are_static_and_powder);
    RUN_TEST(test_dirt_moisture_macros_and_codec_helpers_agree_on_every_byte);
    RUN_TEST(test_painted_gunpowder_starts_dry_in_one_of_three_tones);
}

SUITE_REGISTER(run_sand_roots_suite);
