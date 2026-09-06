/*=============================================================================
 * Portable suite: the falling-sand automaton.
 *
 * Runs on the host and on the board. Nothing here touches hardware - the grid
 * is a byte array and gravity is a pair of ints - which is the point: a rule
 * like "a grain slides off a pile" is far easier to state on a 5x5 grid than
 * to spot by eye on a 184x224 one at 40 fps.
 *
 * Grids are written out as text so a failure is readable:
 *   '.' empty, 'o' a grain.
 *===========================================================================*/

#include <math.h>   /* atan2()/M_PI - shadow_test_bearing()'s own comment
                     * (this section's own gravity-alignment test) explains
                     * why this one measurement uses floating point where
                     * the rest of this file deliberately does not */
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
/* Reaches past sand.h's own public surface on purpose - bd esp32c6-a2j's
 * exhaustive-shape-table test below exercises
 * cover_mask()/covered_at() (ring_dir() too) directly, and a rule that
 * rotates with gravity and is only ever exercised indirectly, through the
 * stochastic burst roll, at whatever gravity direction one scene happens
 * to use, is not really tested. sand_priv.h is pure portable logic
 * (static inline over a plain sand_t, no hardware) like the rest of what
 * this file already links against - the same reason sand_liquid.c and
 * sand_gas.c can include it too - so this is a wider view of the same
 * portable surface, not a step outside it. */
#include "sand_priv.h"
#include "util/intmath.h"   /* im_len() - mirrors app_sand.c's own
                             * update_local_depth_gravity(), which projects
                             * gravity onto each axis with the same helper
                             * rather than re-deriving the weight from
                             * |gx|+|gy| the way the old blend did */

/* s/cells/fx/W/H/fixture()/load() and the rest of the split's shared
 * fixtures and assertion helpers now live in suite_sand_common.{c,h} -
 * see that header for why. */
#include "suite_sand_common.h"

/* --- roots ----------------------------------------------------------------
 *
 * Dirt is a powder and shifts. When the soil directly under a tree's
 * collar slides away, find_water()'s own stem walk finds neither stem nor
 * ground below it and the tree simply stops growing - stranded above
 * water it can no longer reach. As a plant or a trunk spends the soil
 * moisture it grows on, there is a small chance the soil cell it drank
 * through welds into a ROOT instead of staying a grain of dirt -
 * KIND_STATIC, holds still, and cannot be carried away the way loose
 * dirt can. See reaction_t.roots and the top of docs/Sand/Sand-
 * Simulation.md's tree-feeding section. */

/* A watered plant growing on a dirt bed eventually puts a root into the
 * soil under it. The simplest possible claim this feature makes, and the
 * one every other root test in this file assumes already holds.
 *
 * REPLANTED every 40 steps rather than left to grow once. A single seed's
 * own canopy fills the handful of cells its growth can reach and then
 * stops spending moisture at all - measured, a single seed left alone for
 * 30000 steps produced barely a dozen spend events in total, nowhere
 * near enough independent tries at a ~3% root roll (reaction_t.roots).
 * Clearing the canopy and planting a fresh seed on the same spot gives
 * that roll a fresh, independent attempt at the SAME collar every cycle -
 * see test_a_rooted_collar_survives_the_bed_shifting_away's own scene 1,
 * which hits the identical wall and fixes it the same way. */
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

/* A root is not is_kin() of the wood it anchors - and that is exactly
 * right, not an oversight. anchored() only asks whether something
 * GRAVITY-WARD of a kin body is non-kin (sand_reactions.c, is_kin()'s
 * own comment - "three earlier versions... each looked sufficient and
 * each was wrong on the board"), so a root counts as support the same
 * way bare ground would, without needing to be treated as more tree.
 * Verified directly rather than reasoned about, for the same reason
 * anchored()'s own comment gives.
 *
 * The root here rests on NOTHING - empty space all the way down. If
 * anchoring depended on the root itself being supported by something
 * else, or on the root reading as more tree rather than as ground,
 * this would fall; a root is inert regardless of what is under it
 * (extended_reactions[MATX_ROOT] sets no `.falls`), so the only thing
 * keeping the whole structure in place is is_kin() correctly saying a
 * root is NOT more of the plant's own body. */
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

/* DELETED: test_roots_never_go_deeper_than_the_depth_cap used to live
 * here, replanting a seed on the same deep bed for 300 cycles and
 * asserting the resulting root column never ran past ROOT_DEPTH_MAX
 * (4) cells below the collar. That constant is gone - see sand_
 * reactions.c's own RETIRED comment where it used to be defined - along
 * with the mechanism it bounded: PART 1 (the collar seed) now only ever
 * fires once per tree, and PART 2 (step_one_rooting_cell(), local
 * neighbour-eating) is bounded by ROOT_SURFACE_MAX instead, which does
 * not reason about depth from a collar at all. There is no version of
 * this test's claim left to make honest - a root system grown by PART 2
 * is not a single contiguous column counted from a fixed origin, so
 * "how deep is IT" is not a question with one answer the way it used to
 * be. See test_a_root_column_reaches_below_the_collar just below for
 * what still holds instead: roots never appear above the collar, and
 * the system still both deepens and spreads given the chance to. */

/* A tree standing on a pre-placed root column still grows as tall as one
 * standing straight on soil - the test that pins the reason roots are
 * their own material rather than more wood. find_water()'s stem walk
 * must pass through a root without charging it against `lift`, or a
 * handful of root cells would eat a real share of TREE_LIFT for every
 * tree that ever roots at all.
 *
 * NOT measured by growing a tree from a seed and comparing final
 * heights. A single, unreplanted tree plateaus at 7-8 cells long before
 * it gets anywhere near TREE_LIFT = 10 (measured: 6000 steps changed
 * nothing) - hardening and a crowded local neighbourhood stop it cold,
 * the same wall test_a_watered_plant_roots_into_the_soil_it_drinks_from's
 * own top comment describes for a single seed's own spend events. That
 * plateau sits well clear of the cap either way, so comparing final
 * heights this way never actually exercises TREE_LIFT and would pass
 * identically whether or not roots were silently being charged against
 * it.
 *
 * So this builds the stem PRE-GROWN, right at the boundary, and asks
 * one direct question: does the tip's very next growth roll succeed? */
#define LIFT_TEST_W 8
#define LIFT_TEST_H 24

/* `stem` cells stacked directly on `roots` cells of pre-placed root (0
 * for none), which sit on a saturated dirt bed. Only the TOPMOST cell of
 * the stem is MATX_PLANT; every cell below it is already MAT_WOOD.
 *
 * That split matters more than it looks. Every cell of a stem that is
 * still soft plant is independently eligible to roll `grows` - and a
 * cell low down, close to the collar, has a small `lift` of its own and
 * can branch or thicken there regardless of what the actual TIP is
 * capped at. An all-plant stem exercised exactly that: an eleven-cell
 * control meant to prove TREE_LIFT actually blocks something instead
 * grew anyway, from a low cell nowhere near the cap, and proved nothing.
 * Wood does not grow at all (reactions[MAT_WOOD] has no `.grows`), so
 * hardening everything except the tip leaves exactly one cell in the
 * whole scene that can ever add a plant or wood cell - the tip itself,
 * at the lift this test means to test. (Wood can still bud or sprout,
 * but neither is lift-gated and neither one's product - MATX_LEAF, or a
 * root - is counted below, so neither can read as "the tip grew".)
 *
 * Returns whether the tip grows at least one cell further within a
 * short run - measured across the WHOLE grid, not one column, since a
 * successful growth roll can lean or branch sideways from the tip
 * rather than only ever extending straight up. */
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
    /* TWO CONTROLS FIRST, or the boundary below proves nothing. TREE_LIFT
     * is 10 (sand_reactions.c): a ten-cell stem's tip sits at lift 9, the
     * last position still under the cap, so it must still be able to put
     * out an eleventh cell; an eleven-cell stem's tip sits at lift 10 and
     * must NOT be able to put out a twelfth. Without both of these
     * holding, a change to either side of that boundary could not be
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

    /* THE CLAIM: the same ten-cell stem, standing on 3 cells of
     * pre-placed root instead of directly on soil, must grow exactly as
     * readily. If root cells were silently counted against TREE_LIFT
     * (as `*lift` briefly was, from reusing find_water()'s own walk
     * counter for both kinds of cell it crosses), this stem would
     * already read as 3 over the cap and never grow again - the same
     * failure the eleven-cell control above pins, reached from the
     * other direction. */
    TEST_ASSERT_TRUE_MESSAGE(lift_boundary_grows(10, 3),
        "a ten-cell stem standing on 3 cells of root must still be able "
        "to grow one more cell, the same as standing straight on soil - "
        "a root must cost the tree no TREE_LIFT at all");
}

/* A root with dirt piled back on top of it does not cut the tree off
 * from the water below it. Dirt shifts, so a root that formed at a
 * collar can end up buried under fresh soil later - and without the
 * transparency fix in find_water()'s own soil walk, a root would look
 * exactly like the dead end it exists to prevent, reintroducing the bug
 * from the other side.
 *
 * A DEEP wet reserve below the root, not the original single row - since
 * PART 2 of the roots feature, a root sitting directly on its only
 * reachable water is itself a second consumer of that exact cell
 * (step_one_rooting_cell(), sand_reactions.c): given enough steps it
 * will eventually eat the very cell this test's ORIGINAL one-row version
 * depended on and convert it to more root, which the soil walk then
 * crosses too, arriving at stone with nothing left to find - a real
 * race between the root's own slow, serial, one-cell-at-a-time eating
 * and the tree's growth roll, and for the fixed seed this suite always
 * runs with, the root used to win it (measured: FAILED, deterministically,
 * against the single-row version). That is not the transparency bug this
 * test exists to catch - find_water()'s own walk was never touched by
 * PART 2 - it is the new mechanism competing for the one cell of water
 * the old, narrower scene happened to offer. Six rows deep is far more
 * than the root can plausibly eat through (each conversion needs its own
 * independent roll, one cell at a time, only ever on the single newest
 * cell of the column) before the tree's own, faster-firing growth roll
 * succeeds at least once - which is the actual claim under test. */
#define BURIED_ROOT_TEST_W 8
#define BURIED_ROOT_TEST_H 16
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

/* Roots go DOWN, not just sideways along the surface - AND sideways, not
 * just down.
 *
 * RETIRED CLAIM, and why: this test used to assert that the very FIRST
 * conversion after a pre-placed collar root landed directly BELOW it
 * rather than beside it, back when PART 1's collar-welding walk was the
 * entire feature and "does the column go down at all" was a real
 * question - see the old walk's own bug, preserved in git history, where
 * find_water()'s scan found loose dirt diagonally beside a root before
 * it ever crossed the root itself. That bug lived in find_water(), which
 * PART 2 (step_one_rooting_cell(), sand_reactions.c) never touches, and
 * "the very first conversion" is not even a well-formed question for a
 * local rule with no fixed direction preference: every one of a root
 * cell's moist neighbours is an equally valid candidate, so which one
 * gets eaten first is exactly as informative as which one a coin lands
 * on. What is still true, and still worth pinning, is the SHAPE over the
 * whole run: nothing above the collar (gravity did not stop applying),
 * and the system both deepens and spreads given the chance to, since a
 * root that only ever did one of the two would not read as a root
 * system either. */
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

/* Lava reaching a root burns it out; a flame reaching one does nothing.
 *
 * The asymmetry is the point and is deliberate on both sides: fire must
 * not be able to eat a tree's anchor from under it (see MATX_ROOT's own
 * row), but molten rock under a tree should light it from the roots up.
 * A root has no variant to bank a heat ramp in, so the two are told apart
 * at the SOURCE - reaction_t.melts fires only from a burning liquid.
 *
 * Walled in with stone so the lava cannot run off sideways and the only
 * cell it touches is the root beneath it. */
static void test_lava_burns_a_root_out_of_the_ground(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, cx, H - 2, MATX(MATX_ROOT));
    /* Sheltered by a trunk beside it, for the same reason the fire test
     * just below is: an orphaned root ROTS (reaction_t.withers), and the
     * first version of this passed with the melt path deleted outright -
     * the root was simply rotting inside the 400 steps. Diagonal to the
     * lava, so the wood is out of its four-neighbour reach. */
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
        "but molten rock is the one heat that reaches it (and with a trunk "
        "beside it the root cannot have merely rotted, so the melt is the "
        "only door left)");
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
    /* Sheltered by a trunk beside it, or the first version of this test
     * failed for the wrong reason: an orphaned root on bare stone ROTS
     * (reaction_t.withers), and 400 steps at 1 in 256 is plenty for that.
     * Diagonal to the flame, not cardinal, so the wood itself is never
     * the thing that catches. */
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

/* An orphaned root rots; a root under a living tree does not.
 *
 * Roots were permanent litter: no falls, no flammability, only acid ever
 * removed one, and at 7-20 cells per tree a burnt forest would slowly fill
 * its bed with bone-coloured cells nothing could clear - the same trap
 * plant and leaf each fell into once. Withering is the answer for those,
 * so it is the answer here, with the same two exemptions: touching its
 * tree, or able to reach water. */
static void test_an_orphaned_root_in_dry_ground_rots_away(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0)); /* bone dry */
    }
    sand_set(&s, cx, H - 3, MATX(MATX_ROOT)); /* the tree is long gone */

    int gone = 0;
    for (int i = 0; i < 6000 && !gone; i++) {
        sand_step(&s, 0, 1000, 0);
        gone = (sand_at(&s, cx, H - 3) != MATX(MATX_ROOT));
    }
    TEST_ASSERT_TRUE_MESSAGE(gone,
        "a root with no tree above it and no water below it has to rot "
        "away, or every burnt tree leaves its root system in the ground "
        "for ever");
}

/* Both cells of a two-deep column must survive: the top one touches wood,
 * the bottom one touches only root. Without `roots_to` counting as shelter
 * a column rotted from the bottom up beneath a perfectly healthy tree the
 * moment its soil dried. */
static void test_a_root_column_under_a_living_tree_does_not_rot(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0)); /* bone dry */
        sand_set(&s, x, H - 3, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    sand_set(&s, cx, H - 2, MATX(MATX_ROOT)); /* bottom: touches root only */
    sand_set(&s, cx, H - 3, MATX(MATX_ROOT)); /* top: touches wood */
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WOOD, 0));

    for (int i = 0; i < 6000; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_ROOT), sand_at(&s, cx, H - 3),
        "the root touching the trunk is sheltered by it and must stay");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_ROOT), sand_at(&s, cx, H - 2),
        "the root touching only ANOTHER ROOT must stay too - a column under "
        "a living tree must not rot out from the bottom in a drought");
}

/* Watering a CANOPY still reaches the soil once the tree has rooted.
 *
 * A leaf drinks by walking down its own trunk to the ground and putting a
 * level of moisture into the soil it finds there - which is what makes
 * watering the top of a tree water the tree. The walk crosses a root only
 * if the walker's own row names one, so foliage needs `roots_to` on the
 * LEAF row (material.c) even though a leaf can never make a root: it has
 * no `roots`, no soil moisture to spend and nothing to spend it on.
 *
 * Without that field a root is a BARRIER to the water a canopy is trying
 * to deliver, which is backwards - a root should conduct water into the
 * soil, not dam it out.
 *
 * THE ISOLATION IS THE WHOLE TEST, and the first two attempts at it both
 * measured the wrong thing. Wood carries `drinks` 12 of its own AND its
 * row does name `roots_to`, so any trunk cell touching the water delivers
 * moisture whether or not the leaf can - which quietly kept the scene
 * "passing" with the field removed. Here the water is BOXED: stone below
 * it and stone beside it, so it cannot fall or spread, and the only cell
 * cardinally touching it is the leaf. step_one_drinking_cell() looks at
 * the four cardinal neighbours only, so the wood diagonally below the
 * water is not a drinker of it. The root row spans the full width for the
 * same reason - a one-cell root leaves a diagonal escape into loose dirt
 * beside it, and the walk gropes round through that instead, which reads
 * as working. */
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

/* THE REGRESSION TEST FOR THE ACTUAL COMPLAINT: a tree's collar, erased -
 * the bed shifting under it - stops the tree cold if nothing has rooted
 * there yet, and does not once something has.
 *
 * Two scenes rather than one flag, because there is no per-instance way
 * to force `roots` to 0 - the reaction tables are `const` and shared, not
 * something a sand_t carries a knob for the way soak/decay/scatter do.
 * The second scene gets the identical effect a forced-off `roots` would:
 * the collar is erased at step ZERO, before even one growth event has
 * had a chance to roll for a root, so it is guaranteed still plain dirt
 * at the moment it is cleared - indistinguishable from `roots` having
 * never existed at all. (Checked directly during development: with
 * reaction_t.roots forced to 0 in material.c, the first scene fails
 * exactly the way the second one is built to here - no root ever forms,
 * so the collar stays vulnerable and the tree never grows past its
 * seed.) */
static void test_a_rooted_collar_survives_the_bed_shifting_away(void)
{
    const int cx = W / 2;

    /* Scene 1: let a root actually form at the collar, then keep going.
     *
     * REPLANTED every 40 steps - a single seed's own canopy fills up and
     * stops spending moisture long before the collar-seed roll
     * (reaction_t.roots) is likely to land. See
     * test_a_watered_plant_roots_into_the_soil_it_drinks_from's own top
     * comment, which hits the identical wall and fixes it the same way. */
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
    /* All THREE gravity-ward cells, not just the one straight down -
     * find_water()'s own stem walk tries straight down and then either
     * diagonal (it has to, for a branch's own trunk sitting at an
     * angle beneath it), so a shifting bed that only cleared the one
     * cell directly below would still leave the tree two doors it never
     * needed to lose. */
    sand_set(&s, cx - 1, H - 2, SAND_EMPTY);
    sand_set(&s, cx, H - 2, SAND_EMPTY); /* the bed shifting, right now */
    sand_set(&s, cx + 1, H - 2, SAND_EMPTY);

    /* By COUNT across the whole grid, not by watching one column for a
     * cell outside its starting row. The seed itself is no longer
     * anchored once all three cells under it are gone, so it FALLS one
     * row onto the stone floor exactly as any ungrounded plant does
     * (reaction_t.falls) - which is not growth, and a position-based
     * check that treated "moved to a different row" as "grew" caught
     * that fall instead of the thing this test means to catch. Growth
     * MAKES cells; falling only relocates the one there already, so the
     * total count is what actually distinguishes them. */
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

/* The conversion never creates moisture: total soil moisture after is
 * never more than before. A root does not carry water - see reaction_t.
 * roots's own comment on why the small chance matters - it destroys
 * whatever the contact cell still held rather than moving it anywhere,
 * so the running total across the whole board can only ever hold steady
 * or fall, never rise, once the initial bed is placed and nothing is
 * pouring more water in. */
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

/* PART 2's own version of the claim above, isolated rather than read off
 * a whole simulated tree: one root, one moist neighbour, nothing else on
 * the board that could spend or create moisture by any other path. The
 * system-wide test above already proves the total never rises across
 * growth, budding AND root-eating together; this one proves specifically
 * that step_one_rooting_cell()'s own conversion is what accounts for the
 * fall, by giving it exactly one thing to eat and checking that it is
 * gone, not merely reduced, once eaten. */
static void test_a_root_eats_a_moist_neighbour_and_only_spends_its_own_moisture(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* Dirt is KIND_POWDER (material.c) - a lone candidate cell needs
     * direct support or it simply falls away before the root ever gets a
     * turn, which is not this test's claim at all. A stone floor
     * directly under the candidate, and stone either side of it so
     * nothing can slide out from under it either, keeps the pocket to
     * exactly one eligible cell. */
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

/* A root never eats what is not moist dirt. Three candidates in reach,
 * each one testing a different guard in step_one_rooting_cell()'s own
 * neighbour scan: dry dirt (the MOISTURE check), sand (the MATERIAL
 * check - reaction_of(c)->dries == 0), and empty space (CELL_IS_EMPTY()).
 * Run long enough that, at 8 in 256 per step, a guard that had quietly
 * gone missing would show it - not merely long enough that a genuine 3%
 * chance might still happen to miss. */
static void test_a_root_never_eats_dry_dirt_sand_or_empty_space(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* A floor under the whole candidate row, same reason as the previous
     * test - dirt and sand are both KIND_POWDER and fall away from an
     * unsupported cell before there is anything to observe. TWO cells
     * wider than the candidates themselves, not flush with them: a
     * powder resting right at the edge of a floor still has an open
     * diagonal-down past that edge to slide into, which is exactly what
     * a first version of this test did with a 3-wide floor - the sand
     * candidate slid one step diagonally off the end of it on step 0
     * and was gone before the test could observe anything. */
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

/* THE SURFACE RULE ITSELF: a root already touching more than
 * ROOT_SURFACE_MAX (2) other roots does not roll to grow at all, even
 * with an eligible candidate sitting right there. Two scenes, the same
 * shape as every other boundary test in this file (see
 * test_a_root_column_does_not_spend_the_trees_lift's own comment): a
 * control just under the cap that must still grow, and a target just
 * over it that must not, so a failure of either side can be told apart
 * from the other. */
static bool
surface_rule_lets_growth_through(int satellite_roots)
{
    sand_t t;
    uint8_t grid[8 * 8];
    sand_init(&t, grid, 8, 8, 12345u);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    const int cx = 4, cy = 4;
    /* Dirt is KIND_POWDER (material.c) - a floor under it, or it falls
     * away before either scene gets a chance to prove anything. Two
     * cells wider than the candidate itself - see
     * test_a_root_never_eats_dry_dirt_sand_or_empty_space's own comment
     * on why a floor flush with its one occupant is still not a floor a
     * powder cannot slide off the edge of. */
    for (int x = cx - 2; x <= cx + 2; x++) {
        sand_set(&t, x, cy + 2, STONE);
    }
    sand_set(&t, cx, cy, MATX(MATX_ROOT));
    sand_set(&t, cx, cy + 1, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX)); /* the
                                                                          * one
                                                                          * candidate,
                                                                          * down
                                                                          */
    /* Satellites fill row cy - 1 ONLY - up-left, up, up-right, in that
     * order - and nowhere else. That is not an arbitrary choice of
     * geometry: a satellite placed level with the central root (as an
     * earlier version of this scene did, at (cx - 1, cy)) is ITSELF
     * adjacent to the candidate cell, and a satellite with few enough
     * root neighbours of its OWN can roll and eat the candidate directly
     * - entirely bypassing whatever the central root's own surface count
     * says, and confounding the very boundary this test means to pin.
     * Row cy - 1 is two rows from the candidate at cy + 1, so nothing
     * placed there can ever reach it. No explicit wood shelter is needed
     * either: two or more satellite roots already shelter the central
     * one through `roots_to`, the same mutual-shelter path
     * step_one_withering_cell() gives a column of root under a living
     * tree (see test_a_root_column_under_a_living_tree_does_not_rot). */
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

/* The FIRST root under a trunk heads down, not along the surface.
 *
 * A lone collar root has no root neighbours, so with parents alone its
 * away-vector was zero and it was left to the gravity term - and it is
 * the one root whose heading matters most, since the whole system grows
 * from it. Now the trunk it grew from counts as a parent too (`clings_to`
 * in step_one_rooting_cell()'s away scan), so "away from what I grew
 * from" points straight down from under the wood.
 *
 * Same statistical shape as the tip test above, same classification of
 * the first conversion adjacent to the root. The trunk is not a candidate
 * (wood is not dirt), so the root's seven candidates split three below at
 * 1 + AWAY + DOWN, two beside at 1, two up-diagonals at 1: "below" is
 * predicted at 15 in 17. Watched red with the trunk term and DOWN both
 * stubbed to nothing - a uniform pick over the same seven lands "below"
 * 3 in 5 against the same 75% bar. */
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

/* A root is a CONDUIT: it carries a level of water from the wettest soil
 * beside or above it into the driest soil beneath it
 * (step_one_conducting_cell()). Depth was measured to be bounded by water,
 * not by heading - a root can only eat moist soil, and a bed watered from
 * the top dries from the top - so the roots bring the water down with
 * them. Moves only, never makes; gravity-ward only.
 *
 * THE SOURCE SITS DIRECTLY ABOVE THE ROOT, and every cell the source's own
 * percolation could reach is blocked - wood one side, stone the other, the
 * root itself beneath. The first version put the source BESIDE the root,
 * and passed with conduction stubbed to nothing: soil percolates into any
 * of its three gravity-ward cells, and the sink was that source's own
 * diagonal. Ordinary physics was feeding the sink and the test was
 * crediting the conduit. With the source overhead and hemmed in, nothing
 * but a root carrying water THROUGH itself can wet the cell below it.
 * Ordinary drying (`dries`) can still lower the source on its own, which
 * is why the second assertion is "at most what the source lost", never
 * equality. */
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

/* No "brings water deeper than bare soil" test here, deliberately. It was
 * written and it failed for reasons that have nothing to do with the
 * claim: on this 8x8 grid percolation alone floods a six-row bed inside
 * any run long enough to matter, so there is no depth left for a conduit
 * to add - and a column that DOES carry water down then eats the cell it
 * wetted, so measuring moisture in dirt counts the delivery as a loss.
 * The depth claim is established where it can be seen, on the 60x70
 * harness with a 19-row dry bed watered at the collar: mean deepest root
 * 4.6 rows with conduction off, 15.0 with it on, over ten seeds (see
 * ROOT_CONDUCT_CHANCE's own comment in sand_reactions.c, and the Roots
 * section of docs/Sand/Sand-Simulation.md). What the suite pins is the
 * mechanism itself - the two tests above. */

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
 * of docs/Sand/Sand-Simulation.md for the full six-seed, 20,000-step
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
 * HATCHED rather than SPECKLED - it is the only one of the five with a
 * travelling shine on top of its grain (see metal_dither/metal_shine's own
 * comment in material.c) - so it gets its own pattern check instead of
 * sharing the other four's.
 *
 * An extended material's variant IS which one it is, so neither can carry
 * a shade and the position hash is the only variation available - the same
 * tool stone and wood use, and right here for the same reason it was wrong
 * for dirt: neither a wall of ice, a grown tree, nor a smelted metal bar
 * moves.
 *
 * The negative half matters as much. The switch is on the low nibble, and
 * a mistake there does not fail to build - it paints some other extended
 * material in leaf green, which is the sort of thing nobody notices until
 * a fourteenth material arrives and comes out looking like a hedge. */
/* A root darkens by STRUCTURE, not by time: the painter hands
 * material_colours() the count of root neighbours in `depth` for a root
 * cell (material_root_neighbours(), material.h), and the shade steps from
 * the fresh tan toward a wood-like brown as that count climbs. A tip (one
 * neighbour) must wear exactly the fresh colour a lone seed does, and every
 * step older must be darker in every channel - checked per channel after
 * undoing the panel's byte swap, rather than as a luminance, so a hue drift
 * could not pass as "darker". Nothing else may read `depth` this way: a
 * leaf at depth 0 and at depth 4 is the same leaf. */
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

    gfx_color_t leaf_shallow[3], leaf_deep[3];
    material_colours(MATX(MATX_LEAF), 3u, 0u, 0u, leaf_shallow);
    material_colours(MATX(MATX_LEAF), 3u, 0u, 4u, leaf_deep);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(leaf_shallow[0], leaf_deep[0],
        "only a root reads `depth` as a neighbour count - a leaf must ignore it");
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
            const material_pattern_t pat = material_colours(c, hash, 0u,
                                                            255u,
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

/* Metal's body, its lines and their crossings must all differ - the same
 * requirement glass's own painted-the-way-it-should-be check makes, just
 * asserted here instead since metal is extended rather than a top-level
 * MAT_* the other test's loop reaches (see MAT_COUNT <= MAT_EXTENDED in
 * material.h). Equal ones would paint a flat block and the shine would
 * never be seen. */
static void test_metal_hatched_body_lines_and_shine_differ(void)
{
    gfx_color_t col[3] = { 0, 0, 0 };
    material_colours(MATX(MATX_METAL), 0u, 0u, 255u, col);

    TEST_ASSERT_TRUE_MESSAGE(col[0] != col[1] && col[1] != col[2],
        "metal is hatched, so its body, its lines and their crossings "
        "must all differ - equal ones paint a flat block and the shine "
        "vanishes");
}

/* Metal's lines and shine do NOT vary from cell to cell, same reasoning as
 * test_the_shine_does_not_vary_between_cells for glass: they are light
 * landing on the surface, not the surface itself, and a highlight that
 * wobbled per cell would look chewed rather than reflective. Unlike
 * glass's version this has no variant loop to run - metal has none. */
static void test_metal_shine_does_not_vary_between_cells(void)
{
    gfx_color_t a[3], b[3];
    material_colours(MATX(MATX_METAL), 0u, 0u, 255u, a);
    material_colours(MATX(MATX_METAL), 5u, 0u, 255u, b);

    TEST_ASSERT_EQUAL_MESSAGE(a[1], b[1],
        "metal's line colour must be identical in every cell");
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
 * its own (material.h's own comment on soil's state split) - so the
 * grain's shade, which used to become the new soil's tone, now simply
 * has nowhere to go. That is the trade this re-encoding makes: soil got
 * its dry tones back by giving up an independent tone while wet, and wet
 * soil's own variation is the moisture gradient percolation lays down
 * instead of a carried tone - see step_one_soaking_cell()'s own comment
 * on its soaks_to branch (sand_reactions.c).
 *
 * What survives instead is simpler: whichever end of the dune band a
 * grain came from, it converts to the SAME moisture - the one unit
 * `soaks` just took - because the shade plays no part in the conversion
 * at all any more. This used to be
 * test_wet_sand_becomes_soil_in_the_tone_its_shade_implies, pinning the
 * derivation that carried a shade across into a tone; there is no
 * derivation left to pin. */
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
    TEST_ASSERT_NOT_EQUAL_PTR_MESSAGE(first, powder,
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

/* ===================================================================
 * Gunpowder: REVISION 2, the fuse model.
 *
 * Ignition (fire contact, damped by moisture) and the heat path (lava
 * beside it, heat conducted through stone, once any wet stage has
 * steamed the moisture off) both write GUNPOWDER_LIT_CELL - a burning
 * STATE like wood's, not an immediate blast. A lit cell is a heat
 * source: it ignites flammable neighbours, and counts down on its own
 * burn_decay roll. Only at BURN-OUT does step_one_burning_cell() ask
 * find_lit_two_by_two() (sand_reactions.c) whether this cell is one
 * corner of a still-lit 2x2 - if impulses are enabled and it is, that is
 * a blast (sand_explode(), SAND_GUNPOWDER_BLAST_RADIUS); otherwise it is
 * plain fire, the ordinary way a flame guts out. See material.h's own
 * comment on reaction_t.explodes and material.c's GUNPOWDER_REACTION for
 * the full account this section tests against.
 * =================================================================== */

/* Cells whose CELL_MATERIAL reads MAT_EXTENDED are either a static or
 * gunpowder - count_cells_of() (this file, above) cannot tell the two
 * apart, so gunpowder-specific tests get their own counter through
 * cell_is_gunpowder() instead. */
static int count_cells_gunpowder(void)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (cell_is_gunpowder(sand_at(&s, x, y))) {
                n++;
            }
        }
    }
    return n;
}

/* --- encoding and appearance --------------------------------------- */

/* A dry tone travels with the grain exactly the way a sand shade or a
 * dirt tone already does - see test_a_grain_keeps_its_shade_as_it_falls
 * and test_a_dry_dirt_grain_keeps_its_tone_as_it_falls, whose pattern
 * this repeats for gunpowder's own three-tone codec. */
static void test_a_gunpowder_grain_keeps_its_tone_as_it_falls(void)
{
    fixture();
    const cell_t grain = GUNPOWDER_CELL(2);
    sand_set(&s, 3, 0, grain);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(grain, sand_at(&s, 3, 3),
        "a gunpowder grain's dry tone must travel with it as it falls, "
        "the same as sand's shade and dirt's tone already do - see "
        "material_name()'s own comment on why every one of the eight "
        "codes still shares the single name \"Gunpowder\"");
}

/* material_name() decodes the WHOLE nibble through extended_names[] for
 * both halves of MAT_EXTENDED (material.c's own comment on why) - every
 * one of gunpowder's eight codes, dry, damp or lit, must answer the same
 * single name. */
static void test_material_name_says_gunpowder_for_every_code(void)
{
    for (int v = 0; v < 8; v++) {
        char why[48];
        snprintf(why, sizeof why, "gunpowder code %d", v);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("Gunpowder",
            material_name(GUNPOWDER_CELL(v)), why);
    }
}

/* Three dry tones, distinct from one another, and moisture 1..moist_max
 * strictly DARKENING with level, never landing back on a dry tone's
 * exact colour - see material.c's own comment on the GUNPOWDER palette
 * block for the rule this pins. Code 7 (GUNPOWDER_LIT) is deliberately
 * left out of the ramp: it is a burning STATE, not a wetness level - see
 * reaction_t.lit_from's own comment (material.h) for why moisture_of()
 * reads it as dry (0), not saturated, and material.c's own comment on
 * why this suite's final report flags that entry's colour and comment as
 * stale rather than asserting anything about it here. */
static void test_gunpowder_palette_tones_are_distinct_and_moisture_darkens(void)
{
    const gfx_color_t *pal = material_palette();
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);

    for (int a = 0; a < r->tones; a++) {
        for (int b = a + 1; b < r->tones; b++) {
            char why[96];
            snprintf(why, sizeof why,
                "dry tones %d and %d must not share a colour", a, b);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(pal[GUNPOWDER_CELL(a)],
                pal[GUNPOWDER_CELL(b)], why);
        }
    }

    int prev_lum = -1;
    for (int m = 1; m <= r->moist_max; m++) {
        const cell_t c = with_moisture(GUNPOWDER_CELL(0), (uint8_t)m, r);
        const int lum = panel_luminance(pal[c]);
        if (m > 1) {
            char why[96];
            snprintf(why, sizeof why,
                "moisture level %d must read darker than level %d", m, m - 1);
            TEST_ASSERT_LESS_THAN_INT_MESSAGE(prev_lum, lum, why);
        }
        prev_lum = lum;

        for (int tone = 0; tone < r->tones; tone++) {
            char why2[96];
            snprintf(why2, sizeof why2,
                "moisture %d must not repeat dry tone %d's exact colour",
                m, tone);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(pal[GUNPOWDER_CELL(tone)],
                pal[c], why2);
        }
    }
}

/* The whole codec, pinned by its documented shape (material.h's moisture
 * codec comment, REVISION 2's codes table): codes 0-2 are the three dry
 * tones (moisture 0), 3-6 are moisture 1-4, and 7 is GUNPOWDER_LIT, a
 * burning STATE that reads as dry (moisture 0), never as saturated - see
 * reaction_t.lit_from's own comment for why. with_moisture() must round
 * -trip every level moisture_of() can report, including 0 (a no-op call
 * that lands on a dry tone, not a real wet state, but must not crash or
 * mis-decode either). */
static void test_gunpowder_codes_decode_to_the_documented_moisture(void)
{
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    static const uint8_t expected_moisture[8] = { 0, 0, 0, 1, 2, 3, 4, 0 };

    for (int v = 0; v < 8; v++) {
        const cell_t c = GUNPOWDER_CELL(v);
        char why[48];
        snprintf(why, sizeof why, "gunpowder code %d", v);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected_moisture[v],
            moisture_of(c, r), why);
        TEST_ASSERT_EQUAL_MESSAGE(v == GUNPOWDER_LIT, cell_is_burning(c), why);
    }

    for (int m = 0; m <= r->moist_max; m++) {
        char why[48];
        snprintf(why, sizeof why, "moisture level %d", m);
        const cell_t c = with_moisture(GUNPOWDER_CELL(0), (uint8_t)m, r);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)m, moisture_of(c, r), why);
    }
}

/* --- movement -------------------------------------------------------- */

static void test_gunpowder_falls_and_piles_like_a_powder(void)
{
    fixture();
    sand_set(&s, 3, 0, GUNPOWDER_CELL(0));

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(sand_at(&s, 3, H - 1)),
        "a single grain of gunpowder must fall straight down onto the "
        "floor, the same as any other powder");

    /* And under a TILTED gravity, a small heap must migrate toward and
     * pile against whichever wall is down - the same claim
     * test_a_heap_settles_against_whichever_wall_is_down makes for sand. */
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 4; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    const int expected = count_cells_gunpowder();

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 1, 0, 0);
    }

    int touching_wall = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!cell_is_gunpowder(sand_at(&s, x, y))) {
                continue;
            }
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(W / 2, x,
                "every grain must migrate to the side gravity points at");
            if (x == W - 1) {
                touching_wall++;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, count_cells_gunpowder(),
        "piling under a sideways gravity must conserve every grain");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, touching_wall,
        "the heap must actually reach the wall gravity points at, not "
        "stall short of it");
}

/* Density only settles a contest between a POWDER and a LIQUID - a grain
 * displaces a liquid cell whenever it is denser, the ordinary sinking
 * every powder already does. Between two POWDERS AT REST, density never
 * decides anything at all: this simulation deliberately never lets one
 * powder sink through another sitting still (see sand.c's own comment,
 * around the try_fall_or_scatter() family, on why a grain has "no
 * business sinking through another powder" - weight is carried through
 * contact, not resolved by density, unless an IMPULSE is involved). So
 * gunpowder sinks through every liquid it is denser than, but a powder
 * poured onto a bed of gunpowder (or gunpowder poured onto a bed of a
 * powder) simply rests on top of it, whichever way around the pour
 * happened - acid is left out of the liquid half deliberately, since it
 * also DISSOLVES gunpowder (dissolvable 200,
 * test_acid_dissolves_gunpowder already covers that reaction) and would
 * confound a plain sinking check. */
static void test_gunpowder_sinks_through_liquids_and_rests_on_and_under_sand(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    sand_set(&s, 3, 3, GUNPOWDER_CELL(0));
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(sand_at(&s, 3, H - 1)),
        "gunpowder is denser than water, so it must sink all the way "
        "through the pool rather than float on it");

    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, OIL);
        }
    }
    sand_set(&s, 3, 3, GUNPOWDER_CELL(0));
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(sand_at(&s, 3, H - 1)),
        "and through oil, the same reasoning");

    /* Sand poured onto a bed of gunpowder must rest ON that bed, never
     * sink into or beneath it. */
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    sand_set(&s, 3, 3, SAND);
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    bool sand_above_bed = false;
    for (int y = 0; y < 4; y++) {
        if (CELL_MATERIAL(sand_at(&s, 3, y)) == MAT_SAND) {
            sand_above_bed = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sand_above_bed,
        "sand poured onto a bed of gunpowder must rest ON it, not sink "
        "beneath it - powders never displace each other at rest, "
        "regardless of density");

    /* And the reverse pour order: gunpowder onto a bed of sand must
     * rest ON that bed in turn - the same mutual-blocking rule either
     * way around. */
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_set(&s, 3, 3, GUNPOWDER_CELL(0));
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    bool gp_above_bed = false;
    for (int y = 0; y < 4; y++) {
        if (cell_is_gunpowder(sand_at(&s, 3, y))) {
            gp_above_bed = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(gp_above_bed,
        "and gunpowder poured onto a bed of sand must rest ON it too - "
        "the same rule, the other pour order");
}

static void test_gunpowder_is_conserved_under_every_gravity(void)
{
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    const int expected = count_cells_gunpowder();
    TEST_ASSERT_EQUAL_INT(15, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, count_cells_gunpowder(),
                "a step must conserve gunpowder grains in every gravity "
                "direction");
        }
    }
}

/* --- ignition and the heat path -------------------------------------- */

/* NOT fire_room() - that boxes fire in on all four cardinal sides, which
 * works for the GAS neighbour test_fire_ignites_an_adjacent_flammable_
 * neighbour uses (density 10, lighter than fire's own 15, so at least
 * one neighbour always fails smothered()'s "denser" test) but not for
 * gunpowder (density 50): stone on three sides plus gunpowder on the
 * fourth makes every one of fire's cardinal neighbours denser than
 * fire, and smothered() reads that as buried - fire is extinguished on
 * the very first step, before its own ignite walk ever runs, which is
 * exactly what a fully-boxed room measured (confirmed by direct
 * instrumentation while writing this test: the fire cell read back
 * empty at step 0 in every run). Left open on three sides instead, with
 * sand_set_mobility(&s, 0) holding it in place without needing walls
 * to do it - smothered() needs an occupied AND denser neighbour on
 * every side, and an empty one fails that trivially. */
static void test_fire_beside_dry_gunpowder_lights_it(void)
{
    fixture();
    sand_set_mobility(&s, 0);
    /* A floor under both cells (spanning far enough either side to catch
     * the two diagonal-down destinations too) - gunpowder is KIND_POWDER
     * and falls with nothing under it, unlike the GAS neighbour the
     * original fire_room()-boxed test used. mobility 0 only holds GAS
     * still; it says nothing about a powder's own gravity. */
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 3, 4, STONE);
    sand_set(&s, 4, 4, STONE);
    sand_set(&s, 5, 4, STONE);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GUNPOWDER_CELL(0));

    /* Looped, not a single step: flammability 200/256 is likely, not
     * certain, on any one roll - a generous budget against a ~78%/step
     * chance rather than betting the test on this one seed's first draw. */
    cell_t c = sand_at(&s, 4, 3);
    for (int i = 0; i < 20 && cell_is_gunpowder(c) && cell_code(c) != GUNPOWDER_LIT; i++) {
        sand_step(&s, 0, 1000, 0);
        c = sand_at(&s, 4, 3);
    }

    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "fire beside dry gunpowder must light the fuse, not skip past it "
        "to some other material entirely");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(GUNPOWDER_LIT, cell_code(c),
        "a fresh ignition must land on GUNPOWDER_LIT (code 7) - the "
        "burning STATE reaction_t.ignites_to now points at, not plain "
        "MAT_FIRE the way section 2's design once wrote here");
}

static void test_lava_beside_dry_gunpowder_lights_it_through_the_heat_path(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 3, H - 3, STONE);
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set(&s, 4, H - 2, GUNPOWDER_CELL(0));

    bool lit = false;
    for (int i = 0; i < 200 && !lit; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 4, H - 2);
        lit = cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT;
    }

    TEST_ASSERT_TRUE_MESSAGE(lit,
        "heat alone - lava sitting beside dry gunpowder, no flame "
        "required - must light the fuse, the same way it always could "
        "for wood, at heat_chance 24/256 (generous 200-step budget - "
        "P(never once in 200) is astronomically small)");
}

static void test_heat_conducted_through_stone_lights_gunpowder(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "heat-through-stone-lights-gunpowder grid must fit in what the "
        "framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_mobility(&wide, 0);

    const int y = 2;
    const int wall_x0 = 2;
    const int wall_len = 5;
    const int gp_x = wall_x0 + wall_len;

    sand_set(&wide, gp_x - 1, y + 1, STONE);
    sand_set(&wide, gp_x,     y + 1, STONE);
    sand_set(&wide, gp_x + 1, y + 1, STONE);
    sand_set(&wide, 1, y, FIRE);
    for (int i = 0; i < wall_len; i++) {
        sand_set(&wide, wall_x0 + i, y, STONE);
    }
    sand_set(&wide, gp_x, y, GUNPOWDER_CELL(0));

    bool lit = false;
    for (int i = 0; i < 150 && !lit; i++) {
        sand_step(&wide, 0, 1000, 0);
        const cell_t c = sand_at(&wide, gp_x, y);
        lit = cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT;
    }

    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(lit,
        "heat conducted through a stone wall - no flame ever touching "
        "the gunpowder directly - must still light the fuse, the same "
        "conduct_heat() walk that already boils water or smelts dirt "
        "through a wall");
}

/* --- the fuse: trails, blasts, burial, quenching ---------------------- */

/* Ignition spreads cell to cell along a trail exactly like fire spreads
 * through wood - only ONE end is lit here, the rest start dry, so this
 * is a claim about PROPAGATION, not (yet) about the 2x2 blast rule; see
 * test_a_one_wide_lit_trail_never_detonates for that half, pinned
 * separately with the whole trail pre-lit at once. */
static void test_a_lit_gunpowder_trail_burns_along_itself(void)
{
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);   /* sand_init()'s default
                                 * is immortal (decay 0) - the trail must
                                 * actually burn down behind the fuse
                                 * front, not just light up and stay lit */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 1, H - 2, GUNPOWDER_LIT_CELL);
    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, H - 2, GUNPOWDER_CELL(0));
    }

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "trail-propagation impulse queue must fit in what the "
        "framebuffer leaves");
    sand_enable_impulses(&s, buf, W * H);

    bool reached_far_end = false;
    bool all_gone = false;
    for (int i = 0; i < 1000 && !all_gone; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t far = sand_at(&s, 5, H - 2);
        if (!reached_far_end &&
            (!cell_is_gunpowder(far) || cell_code(far) == GUNPOWDER_LIT)) {
            reached_far_end = true;
        }
        all_gone = true;
        for (int x = 1; x <= 5; x++) {
            if (cell_is_gunpowder(sand_at(&s, x, H - 2))) {
                all_gone = false;
            }
        }
    }
    const int impulses = s.impulse_count;
    free(buf);

    TEST_ASSERT_TRUE_MESSAGE(reached_far_end,
        "ignition lit at one end of a 1-wide trail must reach the far "
        "end within a generous budget - a fuse that only ever burns the "
        "cell it started at is not a trail catching, it is one grain "
        "catching");
    TEST_ASSERT_TRUE_MESSAGE(all_gone,
        "the whole trail must eventually burn through - nothing but "
        "fire or empty left, no dry gunpowder surviving untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, impulses,
        "a trail only one cell wide never has a third lit neighbour to "
        "complete a 2x2 with (find_lit_two_by_two(), sand_reactions.c), "
        "so it must never detonate while it burns along");
}

/* The positive case REVISION 2 exists for: four cells, all lit at once,
 * is the smallest lit body where whichever corner burns out FIRST is
 * GUARANTEED to still find its other three corners lit (the rejected
 * first draft asked for a fully-lit 3x3, where eight of nine cells are
 * on the PERIMETER and structurally can never satisfy that - see
 * find_lit_two_by_two()'s own comment, sand_reactions.c, for the device
 * measurement that forced the narrowing). Folded into one test with the
 * lone-cell comparison, the same pairing
 * test_a_confined_gas_pocket_bursts_instead_of_just_catching draws
 * against test_an_open_gas_pocket_still_just_catches_fire. */
static void test_a_lit_two_by_two_of_gunpowder_detonates(void)
{
    /* Scene A: the 2x2, boxed in stone - two rows of fire_room()'s own
     * shape stacked - so nothing so much as slides before the reactions
     * pass gets a turn at it. sand_set_decay(&s, SAND_DECAY_PER_MATERIAL)
     * is not decoration: sand_init()'s own default is decay == 0
     * (immortal - see sand_set_decay()'s own comment, sand.h), exactly
     * what test_a_buried_lit_gunpowder_cell_is_not_smothered and
     * test_water_quenches_lit_gunpowder_to_soaked both WANT, but this
     * test needs the real per-material burn_decay figure so burn-out
     * actually happens at all. */
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 2, STONE);
    sand_set(&s, 2, H - 3, STONE);
    sand_set(&s, 5, H - 3, STONE);
    sand_set(&s, 3, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, GUNPOWDER_LIT_CELL);

    impulse_t *square_buf = malloc((size_t)(W * H) * sizeof *square_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(square_buf,
        "2x2-detonates impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, square_buf, W * H);

    bool square_burned = false;
    for (int i = 0; i < 200 && !square_burned; i++) {
        sand_step(&s, 0, 1000, 0);
        square_burned = !cell_is_gunpowder(sand_at(&s, 3, H - 3)) ||
                        !cell_is_gunpowder(sand_at(&s, 4, H - 3)) ||
                        !cell_is_gunpowder(sand_at(&s, 3, H - 2)) ||
                        !cell_is_gunpowder(sand_at(&s, 4, H - 2));
    }
    const int square_impulses = s.impulse_count;
    free(square_buf);

    TEST_ASSERT_TRUE_MESSAGE(square_burned,
        "setup: at least one corner of the 2x2 must burn out within the "
        "budget");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, square_impulses,
        "a lit 2x2 must detonate at its first burn-out - the corner "
        "that goes first always still has its other three corners lit, "
        "which is exactly what find_lit_two_by_two() asks for");

    /* Scene B: a lone lit cell, same box shape, same impulses enabled -
     * structurally it can never be a corner of a lit 2x2 at all (it has
     * no lit neighbour whatsoever), so it must always resolve to plain
     * fire. */
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 2, STONE);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);

    impulse_t *lone_buf = malloc((size_t)(W * H) * sizeof *lone_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(lone_buf,
        "lone-lit-cell impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, lone_buf, W * H);

    bool lone_resolved = false;
    for (int i = 0; i < 200 && !lone_resolved; i++) {
        sand_step(&s, 0, 1000, 0);
        lone_resolved = !cell_is_gunpowder(sand_at(&s, 3, H - 2));
    }
    const int lone_impulses = s.impulse_count;
    const uint8_t lone_material = CELL_MATERIAL(sand_at(&s, 3, H - 2));
    free(lone_buf);

    TEST_ASSERT_TRUE_MESSAGE(lone_resolved,
        "setup: the lone lit cell must burn out within the budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lone_impulses,
        "a lone lit cell has no lit neighbour at all, so it can never "
        "be a corner of a lit 2x2 and must never queue a blast");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, lone_material,
        "and it must burn out to plain fire - the ordinary "
        "ember-guttering outcome, not vanish or leave something else "
        "behind");
}

/* A stronger, board-wide version of the corner checks
 * test_a_lit_two_by_two_of_gunpowder_detonates already makes:
 * sand_explode()'s unconditional core fill (radius /
 * SAND_EXPLODE_CORE_DIVISOR, sand.c) reaches every cell of a 2x2
 * whichever corner turns out to be its centre, so nothing needs scanning
 * only the four corners by name - the whole board must come up clean of
 * the lit code once the blast has actually happened. */
static void test_a_detonating_two_by_two_leaves_no_lit_gunpowder_behind(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "detonation board-wide grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 11u);
    sand_set_decay(&wide, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&wide, 0);   /* hold the 2x2 in place until burn-out -
                                    * see test_gunpowder_without_impulses_
                                    * burns_to_fire's own use of this */

    impulse_t *buf = malloc((size_t)(WIDE_W * WIDE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "detonation board-wide impulse queue must fit in what the "
        "framebuffer leaves");
    sand_enable_impulses(&wide, buf, WIDE_W * WIDE_H);

    const int cx = WIDE_W / 2, cy = WIDE_H / 2;
    sand_set(&wide, cx,     cy,     GUNPOWDER_LIT_CELL);
    sand_set(&wide, cx + 1, cy,     GUNPOWDER_LIT_CELL);
    sand_set(&wide, cx,     cy + 1, GUNPOWDER_LIT_CELL);
    sand_set(&wide, cx + 1, cy + 1, GUNPOWDER_LIT_CELL);

    bool detonated = false;
    for (int i = 0; i < 200 && !detonated; i++) {
        sand_step(&wide, 0, 1000, 0);
        detonated = wide.impulse_count > 0;
    }

    bool any_lit = false;
    for (int y = 0; y < WIDE_H && !any_lit; y++) {
        for (int x = 0; x < WIDE_W; x++) {
            const cell_t c = sand_at(&wide, x, y);
            if (cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT) {
                any_lit = true;
                break;
            }
        }
    }
    free(buf);
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(detonated,
        "setup: the 2x2 must actually detonate within the budget");
    TEST_ASSERT_FALSE_MESSAGE(any_lit,
        "a detonation must consume every lit gunpowder cell it touches - "
        "sand_explode()'s own core fill (radius / SAND_EXPLODE_CORE_DIVISOR) "
        "unconditionally covers the whole 2x2, so nothing lit may survive "
        "the blast step anywhere on the board");
}

/* SAND_GUNPOWDER_BLAST_COOLDOWN (sand_reactions.c) caps detonations at
 * one per step at its shipped value of 1, board-wide - a big pile goes off one blast at a time
 * rather than however many of its 2x2s happen to qualify together. Two
 * lit 2x2s, more than SAND_GUNPOWDER_BLAST_RADIUS apart (so neither
 * blast can physically reach the other's room - the only thing linking
 * them is the shared s->fuse_blast_wait cooldown), forced to burn
 * out on the SAME step (decay 255 - a near-certain single-step burn-out
 * for every one of the eight lit cells). Scan order (top-to-bottom,
 * left-to-right - this file's own top comment) reaches the first
 * group's qualifying corner before the second's, so the first one
 * detonates and the second, even though it structurally qualifies just
 * as much, finds the cap already spent and falls through to plain fire
 * instead. */
static void test_fuse_blasts_are_capped_at_one_per_step(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "blast-cap grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 5u);
    sand_set_decay(&wide, 255);

    impulse_t *buf = malloc((size_t)(WIDE_W * WIDE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "blast-cap impulse queue must fit in what the framebuffer leaves");
    sand_enable_impulses(&wide, buf, WIDE_W * WIDE_H);

    for (int x = 0; x < WIDE_W; x++) {
        sand_set(&wide, x, 4, STONE);
    }
    /* Group A: columns 2-3. */
    sand_set(&wide, 1, 2, STONE);
    sand_set(&wide, 1, 3, STONE);
    sand_set(&wide, 4, 2, STONE);
    sand_set(&wide, 4, 3, STONE);
    sand_set(&wide, 2, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 2, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 3, GUNPOWDER_LIT_CELL);
    /* Group B: columns 26-27 - 24 cells from group A, past
     * SAND_GUNPOWDER_BLAST_RADIUS (20), so neither blast can physically
     * touch the other's room. */
    sand_set(&wide, 25, 2, STONE);
    sand_set(&wide, 25, 3, STONE);
    sand_set(&wide, 28, 2, STONE);
    sand_set(&wide, 28, 3, STONE);
    sand_set(&wide, 26, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 26, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 3, GUNPOWDER_LIT_CELL);

    sand_step(&wide, 0, 1000, 0);

    const int impulses_after_one_step = wide.impulse_count;
    const bool a_all_gone = !cell_is_gunpowder(sand_at(&wide, 2, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 3, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 2, 3)) &&
                            !cell_is_gunpowder(sand_at(&wide, 3, 3));
    const bool b_all_gone = !cell_is_gunpowder(sand_at(&wide, 26, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 27, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 26, 3)) &&
                            !cell_is_gunpowder(sand_at(&wide, 27, 3));
    /* A real blast's core (radius / SAND_EXPLODE_CORE_DIVISOR) fills
     * unconditionally, reaching the walls at distance 1; a CAPPED
     * burn-out only ever writes fire into its own single cell (or, if it
     * was the corner spend_lit_two_by_two() targeted, into the OTHER
     * cells of ITS OWN 2x2 - never into a wall a cell away), so the wall
     * is exactly what tells a real detonation apart from a capped one. */
    const uint8_t a_wall = CELL_MATERIAL(sand_at(&wide, 1, 2));
    const uint8_t b_wall = CELL_MATERIAL(sand_at(&wide, 25, 2));

    free(buf);
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(a_all_gone,
        "setup: group A must burn out within this one forced step");
    TEST_ASSERT_TRUE_MESSAGE(b_all_gone,
        "setup: group B must burn out within this one forced step too");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, impulses_after_one_step,
        "at least one of the two groups must have detonated");
    TEST_ASSERT_TRUE_MESSAGE(
        (a_wall != MAT_STONE) != (b_wall != MAT_STONE),
        "exactly ONE of the two rooms' walls must have been breached - "
        "the cap must have let exactly one of the two qualifying 2x2s "
        "through this step, not both and not neither");
}

/* And the cooldown is a WAIT, not merely a per-step cap: with
 * sand_set_fuse_cooldown() raised to 3, the second of two independent
 * lit 2x2s must still be waiting a step later, where at the shipped
 * value of 1 it would already have gone off. Same two-room scene as
 * above (24 cells apart, so neither blast can reach the other's room);
 * the only difference is the wait, which is the whole claim.
 *
 * Group B is left UNLIT for the first step and lit afterwards, so its
 * burn-out cannot race group A's inside the same pass - what is under
 * test is whether the board is still refusing on a LATER step, not the
 * within-step ordering test_fuse_blasts_are_capped_at_one_per_step
 * already pins. */
static void test_a_longer_fuse_cooldown_delays_the_next_blast(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "cooldown grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 5u);
    sand_set_decay(&wide, 255);
    sand_set_fuse_cooldown(&wide, 3);

    impulse_t *buf = malloc((size_t)(WIDE_W * WIDE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "cooldown impulse queue must fit in what the framebuffer leaves");
    sand_enable_impulses(&wide, buf, WIDE_W * WIDE_H);

    for (int x = 0; x < WIDE_W; x++) {
        sand_set(&wide, x, 4, STONE);
    }
    sand_set(&wide, 1, 2, STONE);
    sand_set(&wide, 1, 3, STONE);
    sand_set(&wide, 4, 2, STONE);
    sand_set(&wide, 4, 3, STONE);
    sand_set(&wide, 2, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 2, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 25, 2, STONE);
    sand_set(&wide, 25, 3, STONE);
    sand_set(&wide, 28, 2, STONE);
    sand_set(&wide, 28, 3, STONE);

    sand_step(&wide, 0, 1000, 0);   /* group A detonates, wait := 3 */

    const bool a_breached = CELL_MATERIAL(sand_at(&wide, 1, 2)) != MAT_STONE;

    sand_set(&wide, 26, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 26, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 3, GUNPOWDER_LIT_CELL);

    sand_step(&wide, 0, 1000, 0);   /* wait ticks 3 -> 2: still refused */

    const bool b_gone = !cell_is_gunpowder(sand_at(&wide, 26, 2)) &&
                        !cell_is_gunpowder(sand_at(&wide, 27, 2)) &&
                        !cell_is_gunpowder(sand_at(&wide, 26, 3)) &&
                        !cell_is_gunpowder(sand_at(&wide, 27, 3));
    const uint8_t b_wall = CELL_MATERIAL(sand_at(&wide, 25, 2));

    free(buf);
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(a_breached,
        "setup: the first 2x2 must detonate on its own step, or there is "
        "no cooldown running to test");
    TEST_ASSERT_TRUE_MESSAGE(b_gone,
        "setup: the second 2x2 must burn out on the step after, so the "
        "refusal below is the cooldown's doing and not a fuse still lit");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, b_wall,
        "a burn-out inside the cooldown must fall through to plain fire, "
        "leaving its room's wall intact - with no cooldown at all this "
        "this same 2x2 would have breached it");
}

/* Replaces the old board-edge claim (an edge cell CAN now be the corner
 * of an INWARD 2x2, so "the board edge never detonates" is false on its
 * own) with the two claims that actually still hold: off-board counts as
 * not lit (find_lit_two_by_two()'s own comment, sand_reactions.c), and a
 * single LINE, however long, never completes a square. One end of this
 * trail sits AT the board's own edge (x == 0), the whole length pre-lit
 * at once - unlike test_a_lit_gunpowder_trail_burns_along_itself, which
 * is about propagation from one end, this is about the shape itself
 * never qualifying, so every cell starts lit and none of them are ever
 * given a chance to catch each other. */
static void test_a_one_wide_lit_trail_never_detonates(void)
{
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);   /* sand_init()'s default
                                 * is immortal (decay 0) - this test needs
                                 * the whole trail to actually burn out */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 6, H - 2, STONE);
    for (int x = 0; x <= 5; x++) {
        sand_set(&s, x, H - 2, GUNPOWDER_LIT_CELL);
    }

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "edge-trail impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, buf, W * H);

    bool all_gone = false;
    for (int i = 0; i < 300 && !all_gone; i++) {
        sand_step(&s, 0, 1000, 0);
        all_gone = true;
        for (int x = 0; x <= 5; x++) {
            if (cell_is_gunpowder(sand_at(&s, x, H - 2))) {
                all_gone = false;
            }
        }
    }
    const int impulses = s.impulse_count;
    free(buf);

    TEST_ASSERT_TRUE_MESSAGE(all_gone,
        "setup: the whole trail must have burned out within the budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, impulses,
        "a lit cell whose only lit neighbours are off-board or in a "
        "single line - never a second one completing a square - must "
        "never detonate, whether it sits at the board's own edge or "
        "safely inside it");
}

/* Gunpowder carries its own oxidiser, unlike wood or a candle - a fuse
 * buried on every cardinal side by something denser (which WOULD
 * smother an ordinary flame - see smothered(), sand_reactions.c) must
 * keep burning regardless, or nothing inside a real pile could ever
 * reach burn-out at all. Decay forced to 0 (immortal) so this is purely
 * about the smothered() skip, not a race against burn-out timing. */
static void test_a_buried_lit_gunpowder_cell_is_not_smothered(void)
{
    fixture();
    sand_clear(&s);
    sand_set_decay(&s, 0);

    /* The full 3x3 ring, all eight neighbours - not just the four
     * cardinals smothered() itself reads. Walling only the cardinals
     * left the two open diagonals as a slide route: a powder blocked
     * straight down but open diagonally still slides that way (the same
     * scatter behaviour any pile uses to find its angle of repose), and
     * confirmed by direct instrumentation while writing this test, the
     * grain walked diagonally for several steps and off this board
     * entirely rather than staying put to prove anything about
     * smothered() at all. The diagonals play no part in smothered()'s
     * own four-neighbour test - they are here only to pin the grain in
     * place so the four cardinals are the only thing left that could
     * move it. */
    const int x = W / 2, y = H / 2;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            sand_set(&s, x + dx, y + dy, STONE);
        }
    }
    sand_set(&s, x, y, GUNPOWDER_LIT_CELL);

    for (int i = 0; i < 100; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const cell_t c = sand_at(&s, x, y);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT,
        "a lit gunpowder cell walled in by stone on all four cardinal "
        "sides - which would smother an ordinary flame - must keep "
        "burning: reaction_t.explodes != 0 skips smothered() outright "
        "for gunpowder (its own comment, sand_reactions.c)");
}

/* Quenching a lit gunpowder cell must leave it SOAKED, not merely
 * unlit - a fuse doused mid-burn is wet, and landing back on the dry
 * code 0 would let it relight off the very neighbour that just wet it.
 * Decay forced to 0 so this is purely about the quench branch, not a
 * race against burn-out. */
static void test_water_quenches_lit_gunpowder_to_soaked(void)
{
    fixture();
    sand_clear(&s);
    sand_set_decay(&s, 0);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, WATER);

    sand_step(&s, 0, 1000, 0);

    const cell_t c = sand_at(&s, 3, H - 2);
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "quenching a lit gunpowder cell must leave it as gunpowder, not "
        "vanish it the way quenching an ordinary flame does");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(GUNPOWDER_LIT, cell_code(c),
        "the fuse must actually be out - no longer the lit code");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r->moist_max, moisture_of(c, r),
        "a lit gunpowder cell doused by water must land SOAKED "
        "(moist_max), not merely at the unlit dry code 0");
}

/* Without an impulse buffer, sand_explode() is a documented no-op (see
 * its own first line, sand.h) - the same fallback
 * test_an_open_gas_pocket_still_just_catches_fire pins for a confined
 * gas pocket. Reuses the exact 2x2 shape that DOES detonate with
 * impulses enabled (test_a_lit_two_by_two_of_gunpowder_detonates), so
 * the only variable here is the missing buffer. */
static void test_gunpowder_without_impulses_burns_to_fire(void)
{
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);   /* sand_init()'s default
                                 * is immortal (decay 0) - every corner
                                 * has to actually burn out here */
    sand_set_mobility(&s, 0);   /* keep an earlier-resolved corner's
                                 * fresh fire from drifting off before
                                 * the last corner is checked */
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 2, STONE);
    sand_set(&s, 2, H - 3, STONE);
    sand_set(&s, 5, H - 3, STONE);
    sand_set(&s, 3, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, GUNPOWDER_LIT_CELL);
    /* No sand_enable_impulses() - s->impulse_buf stays NULL, the default. */

    /* WHAT EACH CORNER BECOMES AT THE MOMENT IT BURNS OUT, not what sits
     * there once all four have. The four burn-outs are four independent
     * geometric waits (reaction_t.burn_decay), so they are spread over
     * many steps, and the fire the first one leaves is itself a transient
     * that decays - at a long enough fuse it is gone before the last
     * corner resolves, which read as "the corner left nothing" and made
     * this test fail on a pure tuning change. Sampling each cell the step
     * it stops being gunpowder pins the claim the fallback actually makes
     * and is indifferent to how long the fuse burns. */
    uint8_t became[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
    const int cx[4] = {3, 4, 3, 4};
    const int cy[4] = {H - 3, H - 3, H - 2, H - 2};
    bool all_resolved = false;
    for (int i = 0; i < 400 && !all_resolved; i++) {
        sand_step(&s, 0, 1000, 0);
        all_resolved = true;
        for (int k = 0; k < 4; k++) {
            const cell_t c = sand_at(&s, cx[k], cy[k]);
            if (cell_is_gunpowder(c)) {
                all_resolved = false;
            } else if (became[k] == 0xFFu) {
                became[k] = CELL_MATERIAL(c);
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(all_resolved,
        "setup: every corner of the 2x2 must burn out within the budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[0],
        "without an impulse buffer, a corner that would otherwise "
        "detonate must fall through to plain fire instead");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[1],
        "same corner rule for the second cell of the 2x2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[2],
        "same corner rule for the third cell of the 2x2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[3],
        "same corner rule for the fourth cell of the 2x2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
        "s->impulse_count must stay zero - there is no buffer for "
        "sand_explode() to have written into at all");
}

/* --- moisture chemistry ------------------------------------------------ */

/* moist_max (SOAKED) beside lava: the one moisture level where
 * reaction_t.soaked_to's own saturated-to-oil roll can fire
 * (step_one_soaking_cell(), sand_reactions.c) and the one level where
 * SAND_DAMP_IGNITION_SHIFT's damping (f >>= 2*m) actually reaches zero -
 * moisture 2 still lights at 12 in 256 a step by design, so only moist_max
 * is safe to assert "never" at. A soaked fuse must never reach the LIT
 * code while it is still soaked; if it stops being gunpowder before
 * drying below moist_max, oil is the only accepted exit (soaked_to), not
 * fire or anything else. */
static void test_soaked_gunpowder_never_lights_beside_lava(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 3, H - 3, STONE);
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));

    uint8_t last_moisture = r->moist_max;
    bool ignited_while_soaked = false;
    bool resolved = false;
    bool became_oil = false;
    for (int i = 0; i < 4000 && !resolved && !ignited_while_soaked; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 4, H - 2);
        if (!cell_is_gunpowder(c)) {
            became_oil = CELL_MATERIAL(c) == MAT_OIL;
            resolved = true;
            break;
        }
        if (cell_code(c) == GUNPOWDER_LIT) {
            if (last_moisture == r->moist_max) {
                ignited_while_soaked = true;
            }
            break;
        }
        last_moisture = moisture_of(c, r);
    }

    TEST_ASSERT_FALSE_MESSAGE(ignited_while_soaked,
        "a fuse still SOAKED (moist_max) on the step before must never "
        "reach the LIT code - heat has to dry it below saturation first");
    if (resolved) {
        TEST_ASSERT_TRUE_MESSAGE(became_oil,
            "a soaked cell that stops being gunpowder before drying below "
            "moist_max must have turned to oil (soaked_to) - its only "
            "other documented exit at this moisture level, never fire or "
            "anything else");
    }
}

/* moisture 2 - below moist_max, so reaction_t.soaked_to's oil roll
 * (held == moist_max only) can never fire and confound this test's one
 * claim: heat must drive a level of moisture off, visibly as steam,
 * generalising dirt's own wet-earth stage to gunpowder via
 * moisture_of()/with_moisture(). Tolerant of the cell going on to light
 * once it is genuinely dry - that is a different claim, not this one. */
static void test_heat_dries_wet_gunpowder_one_level_with_steam(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 3, H - 3, STONE);
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    const uint8_t start_moisture = 2;
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), start_moisture, r));

    uint8_t last_moisture = start_moisture;
    bool moisture_fell = false;
    bool steam_seen = false;
    for (int i = 0; i < 4000 && !(moisture_fell && steam_seen); i++) {
        sand_step(&s, 0, 1000, 0);
        if (count_cells_of(MAT_STEAM) > 0) {
            steam_seen = true;
        }
        const cell_t c = sand_at(&s, 4, H - 2);
        if (!cell_is_gunpowder(c)) {
            break;
        }
        const uint8_t m = moisture_of(c, r);
        if (m < last_moisture) {
            moisture_fell = true;
        }
        last_moisture = m;
    }

    TEST_ASSERT_TRUE_MESSAGE(moisture_fell,
        "heat must actually drive a level of moisture off within the "
        "budget, the same wet-earth stage dirt already goes through");
    TEST_ASSERT_TRUE_MESSAGE(steam_seen,
        "driving moisture off a heated cell must visibly puff steam, the "
        "same wet-earth branch dirt uses");
}

/* Statistical, over many independent single-cell trials rather than one:
 * SAND_DAMP_IGNITION_SHIFT (sand_reactions.c) damps gunpowder's 200/256
 * flammability to 50/256 at moisture 1, so dry cells must ignite far
 * more often than damp ones in the same single roll. Fresh moisture
 * codes are written directly at setup, not soaked up through
 * simulation, so this test never touches soil_set_moisture()/
 * soil_dry_out() at all. */
#define DAMP_TEST_TRIALS 150

/* One [FIRE][GUNPOWDER][STONE] triple per trial, all in a single row, so
 * every trial is fully isolated from every other: the STONE separator
 * blocks horizontal contact with the next trial's fire or gunpowder, and
 * the only row is floored by stone beneath it, so nothing falls before
 * reactions gets a turn. Earlier draft PACKED trials into a tall column
 * instead (fire/gunpowder pairs stacked with no separator) and measured
 * dry and damp both igniting at ~95-97% - because a freshly-lit
 * gunpowder cell is itself a heat source the SAME scan pass still
 * reaches, and stacked vertically its own ignite walk reached the NEXT
 * trial's still-dry cell directly, cascading down the column independent
 * of that trial's own moisture roll entirely. Isolating every trial is
 * what makes this a test of ONE roll's damping, not of a chain
 * reaction. */
static void ignite_trial_row(sand_t *g, uint8_t *cells, int w, int trials, cell_t gp_byte)
{
    sand_init(g, cells, w, 2, 7u);
    sand_set_mobility(g, 0);   /* keep fire from rising away before
                                * reactions gets a turn at it this same
                                * step - see test_an_open_gas_pocket_
                                * still_just_catches_fire's own use of
                                * this for the same reason */
    sand_set_conduction(g, 0);   /* the one-cell stone separator between
                                * trials is a real conductor at the
                                * table's own rate - left at the default,
                                * conduct_heat() carried a trial's own
                                * fire sideways through it into the NEXT
                                * trial's gunpowder, a second independent
                                * heat_chance roll neither isolated trial
                                * was supposed to get. Confirmed by direct
                                * instrumentation while writing this test:
                                * with conduction on, damp ignited at
                                * ~87% instead of the ~19.5% one damped
                                * roll predicts. Sealed shut here, since
                                * this test is about the DIRECT-contact
                                * ignite roll only. */
    for (int i = 0; i < trials; i++) {
        const int base = i * 3;
        sand_set(g, base + 0, 0, FIRE);
        sand_set(g, base + 1, 0, gp_byte);
        sand_set(g, base + 2, 0, STONE);
    }
    for (int x = 0; x < w; x++) {
        sand_set(g, x, 1, STONE);
    }
}

static void test_damp_gunpowder_ignites_less_readily_than_dry(void)
{
    const int w = DAMP_TEST_TRIALS * 3;
    uint8_t *dry_cells = malloc((size_t)w * 2);
    uint8_t *damp_cells = malloc((size_t)w * 2);
    TEST_ASSERT_NOT_NULL_MESSAGE(dry_cells,
        "dry ignition-rate grid must fit in what the framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(damp_cells,
        "damp ignition-rate grid must fit in what the framebuffer leaves");

    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    const cell_t damp_byte = with_moisture(GUNPOWDER_CELL(0), 1, r);
    sand_t dry_g, damp_g;
    ignite_trial_row(&dry_g, dry_cells, w, DAMP_TEST_TRIALS, GUNPOWDER_CELL(0));
    ignite_trial_row(&damp_g, damp_cells, w, DAMP_TEST_TRIALS, damp_byte);

    /* ONE step only - a short exposure to a single roll, not a budget
     * long enough for the heat path to dry a damp cell off (dries != 0)
     * and then catch it on a LATER, undamped roll, which would erase the
     * very difference this test exists to measure. */
    sand_step(&dry_g, 0, 1000, 0);
    sand_step(&damp_g, 0, 1000, 0);

    int dry_lit = 0, damp_lit = 0;
    for (int i = 0; i < DAMP_TEST_TRIALS; i++) {
        const cell_t dry_c = sand_at(&dry_g, i * 3 + 1, 0);
        if (cell_is_gunpowder(dry_c) && cell_code(dry_c) == GUNPOWDER_LIT) {
            dry_lit++;
        }
        const cell_t damp_c = sand_at(&damp_g, i * 3 + 1, 0);
        if (cell_is_gunpowder(damp_c) && cell_code(damp_c) == GUNPOWDER_LIT) {
            damp_lit++;
        }
    }
    free(dry_cells);
    free(damp_cells);

    char why[288];
    snprintf(why, sizeof why,
             "dry gunpowder (flammability 200/256) must ignite far more "
             "readily in one roll than damp (moisture 1, damped to "
             "50/256 by SAND_DAMP_IGNITION_SHIFT) - dry lit %d of %d "
             "trials, damp lit %d of %d; a 2x margin is generous against "
             "both binomial spreads at n=%d",
             dry_lit, DAMP_TEST_TRIALS, damp_lit, DAMP_TEST_TRIALS,
             DAMP_TEST_TRIALS);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(2 * damp_lit, dry_lit, why);
}

/* Statistical comparison against dirt, over many ISOLATED cells (stone
 * to each side, stone floor beneath) so neither soaking nor drying ever
 * has a same-species neighbour to trade with - no diffusion, no
 * percolation. Dirt is unaffected either way (CELL_MATERIAL(dirt) indexes
 * its own real row). */
#define WET_DRY_TRIALS 40

static void test_water_wets_gunpowder_and_it_dries_out_slowly(void)
{
    const reaction_t *gp_r = reaction_of(GUNPOWDER_BASE);

    /* THE WETTING HALF: an isolated dry cell under a splash of water
     * must take on moisture - the ordinary soaking-up path
     * (step_one_soaking_cell()'s own held < moist_max branch), which
     * calls with_moisture(c, held + 1, r) with the row passed in
     * directly, never re-derived from CELL_MATERIAL(c). */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set(&s, 2, H / 2, STONE);
    sand_set(&s, 4, H / 2, STONE);
    sand_set(&s, 3, H / 2 + 1, STONE); /* a floor: gunpowder is a powder and
                                        * would otherwise fall out from under
                                        * the water before it could soak */
    sand_set(&s, 3, H / 2, GUNPOWDER_CELL(0));
    sand_set(&s, 3, H / 2 - 1, WATER);

    bool wetted = false;
    for (int i = 0; i < 400 && !wetted; i++) {
        sand_step(&s, 0, 1000, 0);
        wetted = moisture_of(sand_at(&s, 3, H / 2), gp_r) != 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(wetted,
        "gunpowder under water must take on moisture, the same as dirt "
        "already does");

    /* THE DRYING HALF: gunpowder dries at half dirt's rate
     * (reaction_t.dries 1 against dirt's 2, both material.c), so after
     * the SAME budget, starting from the SAME moisture 1, more of a
     * gunpowder row must still be wet than a dirt row. */
    uint8_t *gp_grid = malloc((size_t)3 * (size_t)WET_DRY_TRIALS);
    uint8_t *dirt_grid = malloc((size_t)3 * (size_t)WET_DRY_TRIALS);
    TEST_ASSERT_NOT_NULL_MESSAGE(gp_grid,
        "gunpowder dry-out comparison grid must fit in what the "
        "framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(dirt_grid,
        "dirt dry-out comparison grid must fit in what the framebuffer "
        "leaves");

    sand_t gp, dirt;
    sand_init(&gp, gp_grid, 3, WET_DRY_TRIALS, 21u);
    sand_init(&dirt, dirt_grid, 3, WET_DRY_TRIALS, 21u);

    for (int y = 0; y < WET_DRY_TRIALS; y++) {
        sand_set(&gp, 0, y, STONE);
        sand_set(&gp, 2, y, STONE);
        sand_set(&gp, 1, y, with_moisture(GUNPOWDER_CELL(0), 1, gp_r));

        sand_set(&dirt, 0, y, STONE);
        sand_set(&dirt, 2, y, STONE);
        sand_set(&dirt, 1, y, CELL_SOIL(MAT_DIRT, 0, 1));
    }

    const int budget = 150;
    for (int i = 0; i < budget; i++) {
        sand_step(&gp, 0, 1000, 0);
        sand_step(&dirt, 0, 1000, 0);
    }

    int gp_wet = 0, dirt_wet = 0;
    for (int y = 0; y < WET_DRY_TRIALS; y++) {
        if (moisture_of(sand_at(&gp, 1, y), gp_r) != 0) {
            gp_wet++;
        }
        if (CELL_MOISTURE(sand_at(&dirt, 1, y)) != 0) {
            dirt_wet++;
        }
    }
    free(gp_grid);
    free(dirt_grid);

    char why[224];
    snprintf(why, sizeof why,
             "gunpowder dries at half dirt's rate so after the SAME "
             "%d-step budget more of it must still be wet - gunpowder "
             "%d/%d wet, dirt %d/%d wet (roughly 56%% against 31%% "
             "expected, a wide margin against both binomial spreads at "
             "n=%d)",
             budget, gp_wet, WET_DRY_TRIALS, dirt_wet, WET_DRY_TRIALS,
             WET_DRY_TRIALS);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(dirt_wet, gp_wet, why);
}

/* Total moisture across every gunpowder cell can only fall or move, never
 * rise - an upper bound, not full conservation (a lit cell reads
 * moisture_of() == 0, reaction_t.lit_from's own comment, which pulls the
 * total down without moving it anywhere) - adapted from the same claim
 * test_moisture_is_conserved_as_it_spreads makes for dirt, to gunpowder's
 * own same-species diffusion (soaks_to == 0, so there is no sand-like
 * neighbour for it to convert - spreading can only ever be gunpowder
 * handing a share to more gunpowder). */
static void test_gunpowder_moisture_never_multiplies_as_it_spreads(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GUNPOWDER_CELL(0));
    }
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    sand_set(&s, 0, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));
    const int placed = (int)r->moist_max;

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        int total = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (cell_is_gunpowder(c)) {
                    total += moisture_of(c, r);
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(total <= placed,
            "spreading moisture must move it, not multiply it - a front "
            "that gains on every hop is a watering can that fills "
            "itself");
    }
}

/* soaked_to/soaked_chance (material.c's GUNPOWDER_REACTION) place MAT_OIL
 * directly through place_reacted() once held reaches moist_max - a
 * different mechanism entirely from the moisture codec's own dry/wet
 * codes, so it is untouched by the drying-side bug documented above. A
 * bone-dry cell's held is always 0, so it can never roll this chance at
 * all. */
static void test_soaked_gunpowder_can_turn_into_oil_and_dry_never_does(void)
{
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));
    }

    bool saw_oil = false;
    for (int i = 0; i < 6000 && !saw_oil; i++) {
        sand_step(&s, 0, 1000, 0);
        saw_oil = count_cells_of(MAT_OIL) > 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_oil,
        "gunpowder saturated at moist_max must eventually turn into a "
        "full cell of oil - the one exit section 2's soaked_to design "
        "gives sitting-wet gunpowder besides drying back out");

    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GUNPOWDER_CELL(0));
    }
    for (int i = 0; i < 6000; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_OIL),
        "bone-dry gunpowder must never roll the saturated-to-oil chance "
        "- it only ever fires once held reaches moist_max, and a dry "
        "cell's held is always 0");
}

static void test_acid_dissolves_gunpowder(void)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int y = 1; y < H; y++) {
        sand_set(&s, 1, y, GLASS);
        sand_set(&s, W - 2, y, GLASS);
    }
    for (int y = H - 3; y < H - 1; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    for (int y = 1; y <= 2; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, count_cells_gunpowder(),
        "setup: there must be gunpowder to eat");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_gunpowder(),
        "acid must eat the gunpowder it settles onto, the same rate "
        "(dissolvable 200) it already eats sand and dirt at");
}

/* --- D1/E1/E2 regression: gunpowder is not soil, a lit fuse is not wet or
 * re-placed --------------------------------------------------------------
 *
 * D1 (the coordinator's own decision, GUNPOWDER_FIXES.md section 7): a new
 * reaction_t field `soil` (nonzero: plants may root in, sprout from,
 * drink from and conduct water into this material) replaces the old
 * `dries != 0` test at every plant/root site that meant "this is soil" -
 * dirt sets `.soil = 1`; nobody else. Moisture DIFFUSION between
 * same-species cells and percolation keep using `dries`, unchanged - only
 * the "is this ground a plant can use" question moves to `soil`. */

/* E2: a lit fuse must not be doused to an arbitrary level by a wet
 * same-species neighbour - soak diffusion's same_species() branch
 * (sand_reactions.c) is gated on !cell_is_burning(n), the same guard that
 * keeps percolation, find_water() and drinking off a burning cell. Decay
 * forced to 0 so this is purely about the diffusion gate, not a race
 * against burn-out. */
static void test_a_wet_neighbour_does_not_put_out_a_lit_fuse(void)
{
    fixture();
    sand_clear(&s);
    sand_set_decay(&s, 0);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 3, H - 2);
        char why[96];
        snprintf(why, sizeof why, "step %d", i);
        TEST_ASSERT_TRUE_MESSAGE(
            cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT, why);
    }
}

/* D1: a rooted tree beside wet gunpowder must never convert it into more
 * root - mirrors test_a_root_never_eats_dry_dirt_sand_or_empty_space's
 * own pattern, with a moist candidate this time rather than a dry one,
 * since gunpowder is excluded by MATERIAL (reaction_t.soil == 0), not by
 * moisture level - step_one_rooting_cell()'s own candidate scan rejects
 * it before the eligibility roll is ever drawn, let alone spend_soil_
 * moisture() called. Moisture 2, not moist_max: at moist_max reaction_t.
 * soaked_to's own saturated-to-oil roll (step_one_soaking_cell()) can
 * fire all on its own over a budget this long, which is a real,
 * independent exit this test has nothing to do with - see test_soaked_
 * gunpowder_can_turn_into_oil_and_dry_never_does.
 *
 * DELIBERATELY NOT ASSERTED: that the gunpowder's moisture stays exactly
 * where it started. reaction_t.dries's own ambient-drying roll
 * (step_one_soaking_cell(), the ~1/256-a-step branch with nothing to
 * hand off to) is real, independent of any root, and fires on its own
 * over a budget this long regardless - dirt ambient-dries with nobody
 * watching it too. What D1 actually guarantees, and the only thing
 * checked below, is that gunpowder is never CONVERTED - the root's own
 * candidate scan never reaching it at all, so spend_soil_moisture() is
 * never even called on it, whatever ambient drying does to it in the
 * meantime. */
static void test_a_root_does_not_drink_from_or_eat_gunpowder(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    for (int x = cx - 2; x <= cx + 2; x++) {
        sand_set(&s, x, cy + 1, STONE);
    }
    sand_set(&s, cx, cy - 1, CELL_MAKE(MAT_WOOD, 0));   /* shelter, up */
    sand_set(&s, cx, cy, MATX(MATX_ROOT));
    const reaction_t *gp_r = reaction_of(GUNPOWDER_BASE);
    const uint8_t gp_moisture = 2;
    const cell_t soaked = with_moisture(GUNPOWDER_CELL(0), gp_moisture, gp_r);
    sand_set(&s, cx + 1, cy, soaked);   /* right: moist gunpowder, the one
                                         * candidate this test is about */

    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const cell_t c = sand_at(&s, cx + 1, cy);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "a root must never convert gunpowder into a root cell, whatever "
        "moisture it holds - gunpowder is not soil (reaction_t.soil == 0)");
}

/* D1: a trunk beside wet gunpowder, with an empty cell free to seed a
 * leaf into, must never sprout - step_one_sprouting_cell()'s own
 * neighbour scan (reaction_of(n)->soil != 0) must reject gunpowder the
 * same way it already rejects dry dirt, sand and empty space. Moisture
 * 2, not moist_max, for the same reason the root test above picks it -
 * moist_max risks the independent soaked_to->oil roll over a budget this
 * long, which has nothing to do with sprouting. */
static void test_plants_do_not_sprout_in_gunpowder(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* A floor two cells wider than the candidates themselves, not flush
     * with them - the same margin test_a_root_never_eats_dry_dirt_sand_
     * or_empty_space uses and for the same reason: a powder blocked
     * straight down still has an open diagonal-down to scatter into and
     * escape the very cell this test means to watch, which a floor flush
     * with the candidates does not close off. */
    for (int x = cx - 2; x <= cx + 1; x++) {
        sand_set(&s, x, cy + 1, STONE);
    }
    sand_set(&s, cx, cy, CELL_MAKE(MAT_WOOD, 0));
    const reaction_t *gp_r = reaction_of(GUNPOWDER_BASE);
    const uint8_t gp_moisture = 2;
    const cell_t soaked = with_moisture(GUNPOWDER_CELL(0), gp_moisture, gp_r);
    sand_set(&s, cx - 1, cy, soaked);
    /* (cx + 1, cy) stays SAND_EMPTY from sand_clear() above - the
     * candidate cell a real sprout would seed a leaf into. */

    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    bool any_leaf = false;
    for (int y = 0; y < H && !any_leaf; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) == MATX(MATX_LEAF)) {
                any_leaf = true;
                break;
            }
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(any_leaf,
        "a trunk beside gunpowder, however wet, must never sprout a leaf "
        "- gunpowder is not soil (reaction_t.soil == 0), the same guard "
        "test_a_root_never_eats_dry_dirt_sand_or_empty_space pins for a "
        "root's own neighbour scan");

    /* Moisture is NOT asserted to stay exactly at gp_moisture here, the
     * same reason test_a_root_does_not_drink_from_or_eat_gunpowder's own
     * comment gives: reaction_t.dries's ambient-drying roll is real,
     * independent of any sprout, and can fire on its own over a budget
     * this long. Only the CONVERSION claim is checked. */
    const cell_t c = sand_at(&s, cx - 1, cy);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "and the gunpowder itself must still be gunpowder - never eaten "
        "or converted");
}

/* E1: a lit fuse must not be RE-PLACED by heat every single step it sits
 * beside a heat source - try_heat_transform_given()'s heat_chance roll,
 * run against a neighbour that is already burning, used to write the
 * IDENTICAL GUNPOWDER_LIT_CELL byte back onto itself whenever the roll
 * passed: same value, but still a write, so its row was marked dirty and
 * its block woken every step regardless (4 RNG draws + wakes per lit
 * cell, per the fix list this test pins). sand_track_dirty_rows() is the
 * observable chosen here - it is exactly the mechanism a real redraw
 * keys off, and a write that changes nothing must never trip it.
 *
 * Everything else sharing the fuse's row has to be provably inert, or a
 * legitimate, unrelated write there would look exactly like the bug this
 * pins. STONE was tried first and rejected: its own heat_ramp (32,
 * material.c) climbs a variant under conducted heat, which marks the row
 * dirty all by itself a few steps in - confirmed by direct instrumentation
 * while writing this test (dirty[row] flipped at step 6, STONE's own
 * variant climbing 0x33 -> 0x36 over the run, nothing to do with the
 * fuse). WOOD (KIND_STATIC, heat_ramp 0, no `heats_to`/`heat_chance` of
 * its own) walls lava in instead - immune to try_heat_transform_given()
 * outright (its first two checks both fail before either the ramp or the
 * heat_chance roll). Flammability forced to 0 board-wide so wood's own
 * small flammability (6) can never turn it to fire on contact with lava
 * either - this test is about the HEAT path only. */
static void test_a_lit_fuse_is_not_re_placed_by_heat(void)
{
    dirty_fixture();
    sand_set_decay(&s, 0);          /* immortal - stays lit for the whole
                                      * budget, itself a heat source too */
    sand_set_flammability(&s, 0);   /* isolate the heat path - see this
                                      * test's own top comment on wood */
    /* sand_set_mobility(0) only holds GAS still (see test_fire_beside_
     * dry_gunpowder_lights_it's own comment) - it says nothing about a
     * POWDER's own gravity or a LIQUID's own flow, so both still need a
     * real floor/wall to stay exactly in place for 50 steps. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);   /* floor - a different row entirely */
    }
    sand_set(&s, 2, H - 2, CELL_MAKE(MAT_WOOD, 0));   /* inert wall, not stone */
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set(&s, 4, H - 2, GUNPOWDER_LIT_CELL);
    const cell_t before = sand_at(&s, 4, H - 2);
    memset(dirty, 0, sizeof dirty);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(before, sand_at(&s, 4, H - 2),
        "a lit fuse beside a heat source must stay the identical byte - "
        "try_heat_transform_given() must reject an already-burning "
        "neighbour before its heat_chance roll, not merely happen to "
        "place the same value back");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[H - 2],
        "and it must never even be WRITTEN, identical value or not - a "
        "write that changes nothing still marks its row dirty and wakes "
        "its block every step, which is exactly the cost E1 exists to "
        "remove");
}

/* --- encoding: no reaction may mint an ambiguous byte ------------------ */

static void assert_reaction_field_never_bare_extended(uint8_t v,
    const char *field, const char *owner)
{
    char why[320];
    snprintf(why, sizeof why,
        "%s.%s == MAT_EXTENDED (15) - a bare material id of 15 names "
        "neither an ordinary material nor a resolved extended byte "
        "(>=0xF0), so place_reacted() would silently mint plain ice "
        "(CELL_MAKE(MAT_EXTENDED, 0)) instead of whatever this reaction "
        "actually meant",
        owner, field);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_EXTENDED, v, why);
}

/* Every "_to"-shaped field (a destination material, not a chance or a
 * count) on one reaction row - shared by both halves of the loop below so
 * the same sixteen fields are never checked by two verbatim copies that
 * could drift apart. */
static void assert_reaction_row_never_mints_bare_extended(const reaction_t *r,
    const char *owner)
{
    assert_reaction_field_never_bare_extended(r->ignites_to, "ignites_to", owner);
    assert_reaction_field_never_bare_extended(r->boils_to, "boils_to", owner);
    assert_reaction_field_never_bare_extended(r->quench_to, "quench_to", owner);
    assert_reaction_field_never_bare_extended(r->condenses_to, "condenses_to", owner);
    assert_reaction_field_never_bare_extended(r->heats_to, "heats_to", owner);
    assert_reaction_field_never_bare_extended(r->flaw_to, "flaw_to", owner);
    assert_reaction_field_never_bare_extended(r->spoils_to, "spoils_to", owner);
    assert_reaction_field_never_bare_extended(r->soaks_to, "soaks_to", owner);
    assert_reaction_field_never_bare_extended(r->soaked_to, "soaked_to", owner);
    assert_reaction_field_never_bare_extended(r->hardens_to, "hardens_to", owner);
    assert_reaction_field_never_bare_extended(r->clings_to, "clings_to", owner);
    assert_reaction_field_never_bare_extended(r->roots_to, "roots_to", owner);
    assert_reaction_field_never_bare_extended(r->canopy_to, "canopy_to", owner);
    assert_reaction_field_never_bare_extended(r->sprouts_to, "sprouts_to", owner);
    assert_reaction_field_never_bare_extended(r->buds_to, "buds_to", owner);
    assert_reaction_field_never_bare_extended(r->shatters_to, "shatters_to", owner);
}

/* Every "_to"-shaped field, across every reaction row, ordinary and
 * extended alike, must never hold the bare value 15 - gunpowder's own row
 * legitimately points ignites_to/heats_to at GUNPOWDER_LIT_CELL (0xFF, a
 * full resolved spec >= 0xF0, never confusable with a bare id), and
 * nothing else on the board has any business naming MAT_EXTENDED as a
 * destination at all - that would silently mint plain ice from a reaction
 * that never meant to touch the extended range, or (in the other
 * direction) a gunpowder-shaped byte from an ordinary one that never
 * meant gunpowder either. See place_reacted()'s own `spec >= 0xF0`
 * convention (sand_reactions.c) for the two valid shapes this pins the
 * boundary between. */
static void test_a_reaction_never_mints_a_static_from_gunpowder_or_the_reverse(void)
{
    for (int m = 1; m < MAT_COUNT; m++) {
        const reaction_t *r = &reactions[m];
        char owner[64];
        snprintf(owner, sizeof owner, "reactions[%s]",
            material_by_id((material_id_t)m)->name);
        assert_reaction_row_never_mints_bare_extended(r, owner);
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        const reaction_t *r = &extended_reactions[k];
        char owner[64];
        snprintf(owner, sizeof owner, "extended_reactions[%d]", k);
        assert_reaction_row_never_mints_bare_extended(r, owner);
    }
}

/* "lit" generalised from wood's whole variant range (nonzero) to
 * gunpowder's single code 7 - cell_is_burning() must still agree with
 * the OLD "any nonzero variant" test for every one of wood's sixteen
 * bytes, or wood's own burning behaviour has silently changed underneath
 * this generalisation. */
static void test_wood_burning_state_is_byte_identical_under_lit_from(void)
{
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        const cell_t c = CELL_MAKE(MAT_WOOD, v);
        char why[64];
        snprintf(why, sizeof why, "wood variant %d", v);
        TEST_ASSERT_EQUAL_MESSAGE(v != 0, cell_is_burning(c), why);
    }
}


/* Ice does what it exists for: it cracks hot glass, and it stays put.
 *
 * Snow already chills, but snow is a powder - it drifts as it falls,
 * floats on water, and melts in any liquid, so aiming it at one face of a
 * hot vessel is most of the difficulty of using it. Ice is the same cold
 * in a form that can be BUILT with. */
static void test_ice_cracks_hot_glass_and_stays_where_it_is_put(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
        sand_set(&s, x, H - 3, MATX(MATX_ICE));
    }
    const int ice_x = 1, ice_y = H - 3;

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "ice against glass at the top of its ramp must crack it, the same "
        "as snow does - that is what makes it worth having as a solid");

    /* It sat on glass that has now become sand, so it may have settled a
     * row; what matters is that it did not drift sideways the way a
     * powder does. */
    bool still_there = false;
    for (int y = ice_y; y < H; y++) {
        if (CELL_MATERIAL(sand_at(&s, ice_x, y)) == MAT_EXTENDED) {
            still_there = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(still_there,
        "and it must still be in the column it was placed in - a solid "
        "block is the whole difference from snow");
}

/* Snow floats, because it is lighter than what it lands on.
 *
 * Not decoration: floating is what puts snow ON TOP of a pool rather than
 * under it, which is where it has to be to reach anything. A snowfall that
 * sank would be a snowfall the player could not aim. */
static void test_snow_floats_on_water(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int x = 2; x < W - 2; x++) {
        sand_set(&s, x, 0, SNOW);
    }

    /* Twenty steps: long enough for the drift to fall and settle, short
     * enough that it has not melted yet. Snow in water is on a clock now
     * (see test_snow_melts_in_any_liquid), so a longer run would measure
     * an empty board and pass for the wrong reason - which is exactly
     * what it did when thawing was added under it. */
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int lowest_snow = -1, highest_water = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_SNOW && y > lowest_snow) {
                lowest_snow = y;
            }
            if (m == MAT_WATER && y < highest_water) {
                highest_water = y;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(lowest_snow >= 0,
        "fixture check: some snow has to survive the fall to say anything "
        "about where it ended up");
    TEST_ASSERT_TRUE_MESSAGE(lowest_snow <= highest_water,
        "snow must come to rest on top of the water, not under it - it is "
        "lighter than water and can_enter() is what makes that true");
}

/* Glass conducts heat, the same as stone.
 *
 * It did not, and the omission was invisible: glass had no reactions[] row
 * at all, so `conducts` defaulted to 0 and heat stopped dead at it. A
 * stone vessel over a flame boiled its contents and a glass one did
 * not - backwards, given glass is the vessel you have to MAKE and the only
 * one acid cannot eat.
 *
 * An absent row reads as "this material has no reactions", which is
 * correct for most materials and was wrong for this one. Nothing warns
 * about it, which is what this test is for.
 *
 * Asserted against stone rather than as an absolute figure: the two are
 * meant to be interchangeable thermally, so that choosing between them is
 * a decision about acid and nothing else. */
static void test_glass_conducts_heat_like_stone(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(reactions[MAT_STONE].conducts,
                                  reactions[MAT_GLASS].conducts,
        "glass must conduct heat as well as stone - a glass vessel over a "
        "flame has to boil what is in it, and glass differing from stone "
        "on any axis but acid makes the choice between them a guess");

    /* And in the simulation, not only in the table: a sealed vessel with
     * water in it and fire underneath. */
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, GLASS);          /* the vessel's base */
    }
    for (int x = 2; x < W - 2; x++) {
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    bool boiled = false;
    for (int i = 0; i < 400 && !boiled; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        boiled = count_cells_of(MAT_STEAM) > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(boiled,
        "fire under a glass base must boil the water above it, exactly as "
        "it does under a stone one");
}

/* Sand plus sustained heat makes glass: reaction_t.heats_to, which is a
 * phase change rather than combustion and so is kept apart from
 * flammability/ignites_to. Sand does not catch fire, and calling the field
 * that would send the next reader looking for a flame. */
static void test_sand_turns_to_glass_under_sustained_heat(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }

    /* A flame held against it, re-laid each step: fire is KIND_GAS and
     * rises away during the same step it is placed, so a single spark
     * never gets a turn to react - the same reason the wood-ignition
     * fixtures hold their spark down. */
    bool made = false;
    for (int i = 0; i < 600 && !made; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        made = count_cells_of(MAT_GLASS) > W;   /* more than the floor */
    }

    TEST_ASSERT_TRUE_MESSAGE(made,
        "sand held against a flame must eventually become glass - slowly, "
        "slowly, so it is something you set up and wait for "
        "rather than something a stray spark does to a dune");
}

/* Dissolving is a TRANSFER, like quenching a fire or soaking a grain: the
 * acid is consumed by the work it does. No longer an exact one-unit-per-
 * cell ratio - SAND_ACID_EAT_DEATH_CHANCE (sand.h) gives every bite a
 * chance to cost the acid its WHOLE remaining mass instead of just one
 * unit, so mass spent can only ever be >= cells eaten now, not always
 * equal to it. Checked both ways: the floor still holds (a bite can
 * never cost less than one unit - dissolving still is not free), and
 * over 400 steps against 8 sand cells and 8 separate acid cells, the
 * death roll landing at least once is close enough to certain that
 * spending MORE than one unit on at least one of those bites is the
 * real assertion here, not just the floor. */
static void test_acid_spends_at_least_a_unit_of_itself_per_cell_dissolved(void)
{
    const long acid_before = acid_tank(2, 2);
    const int sand_before = count_cells_of(MAT_SAND);

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const int eaten = sand_before - count_cells_of(MAT_SAND);
    const long spent = acid_before - mass_held_by(MAT_ACID);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, eaten, "setup: something eaten");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(eaten, spent,
        "every cell dissolved must cost the acid AT LEAST one unit of its "
        "own mass - without that a single drop eats an unbounded amount "
        "and remains a single drop");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(eaten, spent,
        "SAND_ACID_EAT_DEATH_CHANCE is supposed to let at least one of "
        "these bites cost more than the ordinary one-unit chip - mass "
        "spent came out exactly equal to cells eaten, as if the death "
        "roll never landed at all across 8 acid cells and 400 steps");
}

/* The consequence of that, and the reason it is worth paying for: a
 * finite amount of acid can only eat a finite amount. A drop lands on a
 * deep pile and stops partway rather than boring through the floor. */
/* Acid fizzes: a dissolve sometimes leaves smoke where the cell was.
 *
 * Without it acid worked in complete silence - cells simply stopped
 * existing, with nothing on screen to say what had happened or that the
 * acid was doing anything at all. reaction_t.fizz puts a wisp in the cell
 * that was just eaten, which is about to be empty anyway.
 *
 * MAT_SMOKE and not MAT_STEAM: steam here means water that got hot, and
 * acid fumes are not that - the same distinction that made smoke its own
 * material in the first place. */
/* A dedicated, larger fixture for the two fizz tests below, separate from
 * the shared 8x8 `s`/`cells` acid_tank() itself uses - see its own comment
 * for why. reaction_t.fizz dropped sharply (2026-09-01, see its own
 * comment in material.c) from "about one bite in six" to 6-in-256, and
 * the 8x8 tank's own acid_rows/sand_rows only ever offer a HANDFUL of
 * cells to dissolve in total (acid_tank(2,2)'s 4-wide, 2-row sand supply
 * is 8 cells, ever - once eaten, no more dissolve events can happen no
 * matter how many further steps run). 8 total tries at a 6-in-256 chance
 * has better than an 80% chance of landing ZERO fizzes for any ONE fixed
 * seed - not a step-budget problem, a TRIALS problem, which is why
 * raising the step count alone (tried first) did not fix it. A wide,
 * deep sand floor with acid poured over the whole top gives hundreds of
 * independent dissolve events instead of a handful, which is what
 * actually needs to change. */
#define FIZZ_W 40
#define FIZZ_H 12

/* cells is HEAP, not static file scope - each of the two callers below
 * mallocs its own FIZZ_W * FIZZ_H (480 byte) grid and frees it before
 * its own assertion can fail; see drop_impulse_buf's own comment above
 * for why this file's static test fixtures cannot share the framebuffer's
 * memory budget. */
static void acid_fizz_fixture(uint8_t *cells)
{
    sand_init(&fx.fizz_sim, cells, FIZZ_W, FIZZ_H, 5u);
    sand_set_evaporates(&fx.fizz_sim, 0);   /* isolate fizz - see the tests'
                                          * own comments for why */
    for (int y = 4; y < FIZZ_H; y++) {
        for (int x = 0; x < FIZZ_W; x++) {
            sand_set(&fx.fizz_sim, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < FIZZ_W; x++) {
            sand_set(&fx.fizz_sim, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
}

static void test_acid_fizzes_while_it_eats(void)
{
    uint8_t *fizz_cells = malloc((size_t)FIZZ_W * FIZZ_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(fizz_cells,
        "acid-fizz grid must fit in what the framebuffer leaves");
    acid_fizz_fixture(fizz_cells);

    /* Smoke OR gas: the fizz coin-flips between the two (see
     * step_one_dissolver_cell()), so either one is proof the fizz fired -
     * a single deterministic run can land entirely on one side of that
     * flip. evaporates is forced off above, so any gas seen here can
     * only have come from the fizz. */
    bool fizzed = false;
    for (int i = 0; i < 300 && !fizzed; i++) {
        sand_step(&fx.fizz_sim, 0, 1000, 0);
        for (int y = 0; y < FIZZ_H && !fizzed; y++) {
            for (int x = 0; x < FIZZ_W && !fizzed; x++) {
                const uint8_t m = CELL_MATERIAL(sand_at(&fx.fizz_sim, x, y));
                fizzed = (m == MAT_SMOKE || m == MAT_GAS);
            }
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(fizz_cells);

    TEST_ASSERT_TRUE_MESSAGE(fizzed,
        "acid eating a pile of sand must leave some smoke or gas behind - "
        "it is the only sign on screen that the acid is working");
}

/* And the fizz has to be able to get OUT of the acid, which it does for
 * free: smoke is lighter than every liquid, so try_bubble() (sand_gas.c)
 * swaps it up through the pool. Worth asserting, because a byproduct that
 * cannot leave the liquid that made it would just accumulate at the
 * bottom, invisible under the acid. */
static void test_the_fizz_rises_out_of_the_acid(void)
{
    const int surface = 0;      /* acid_fizz_fixture() fills from row 0 */
    uint8_t *fizz_cells = malloc((size_t)FIZZ_W * FIZZ_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(fizz_cells,
        "acid-fizz-rise grid must fit in what the framebuffer leaves");
    acid_fizz_fixture(fizz_cells);

    /* Smoke OR gas: the fizz coin-flips between the two (see
     * step_one_dissolver_cell()), and both rise through try_bubble() the
     * same way - the coin flip only picks which material, not whether it
     * floats. */
    int highest = FIZZ_H;
    for (int i = 0; i < 300; i++) {
        sand_step(&fx.fizz_sim, 0, 1000, 0);
        for (int y = 0; y < FIZZ_H; y++) {
            for (int x = 0; x < FIZZ_W; x++) {
                const uint8_t m = CELL_MATERIAL(sand_at(&fx.fizz_sim, x, y));
                if ((m == MAT_SMOKE || m == MAT_GAS) && y < highest) {
                    highest = y;
                }
            }
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(fizz_cells);

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(surface, highest,
        "smoke or gas made at the bottom of an acid pool must reach the "
        "top of it - a gas is lighter than any liquid, and try_bubble() "
        "is what lets it climb out instead of being trapped underneath");
}

/* A dedicated, wide fixture for the two dilution tests below - one row of
 * water directly above one row of acid, water on top because that is
 * ALREADY the stable density ordering (acid's density is 38, water's is
 * 30 - acid sinks through water on its own, see MAT_ACID's own comment in
 * material.c), so nothing moves due to gravity/density before reactions
 * runs and every column's water/acid pair stays put for a clean single-
 * step measurement. Wide rather than deep: each column is an INDEPENDENT
 * trial of the same roll (reaction_dirs tries "up" first, see
 * sand_reactions.c, so an acid cell's water neighbour is always the first
 * candidate checked, never skipped over), so width is what buys sample
 * size here, not steps.
 *
 * 4000, not the original 400 - SAND_ACID_DILUTE_TO_WATER_CHANCE (sand.h)
 * was tightened from a 3-in-4 split down to 55/45, and the fixed seed
 * below is deterministic, not flaky, but a narrow bias needs a
 * proportionally wider sample for the water/acid gap to clear the
 * count's own statistical noise reliably - 400 columns at 55/45 leaves
 * the two counts within roughly one standard deviation of each other,
 * which is not a safe margin for a fixed-seed assertion to depend on. */
#define DILUTE_W 4000
#define DILUTE_H 2

/* cells is HEAP, not static file scope - each caller mallocs its own
 * DILUTE_W * DILUTE_H (8000 byte) grid and frees it before its own
 * assertions can fail; see drop_impulse_buf's own comment above for why
 * this file's static test fixtures cannot share the framebuffer's
 * memory budget. */
static void acid_water_dilute_fixture(uint8_t *cells)
{
    sand_init(&fx.dilute_sim, cells, DILUTE_W, DILUTE_H, 7u);
    sand_set_evaporates(&fx.dilute_sim, 0);   /* isolate dilution from the
                                             * unrelated evaporates roll -
                                             * same reasoning as the fizz
                                             * fixture above */
    for (int x = 0; x < DILUTE_W; x++) {
        sand_set(&fx.dilute_sim, x, 0, CELL_MAKE(MAT_WATER, MASS_MAX));
        sand_set(&fx.dilute_sim, x, 1, CELL_MAKE(MAT_ACID, MASS_MAX));
    }
}

static void test_acid_and_water_dilute_each_other(void)
{
    uint8_t *dilute_cells = malloc((size_t)DILUTE_W * DILUTE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(dilute_cells,
        "acid/water dilution grid must fit in what the framebuffer leaves");
    acid_water_dilute_fixture(dilute_cells);

    /* Either direction counts: a diluted column either turned its acid
     * cell to water, or turned its water cell to acid - see
     * SAND_ACID_DILUTE_TO_WATER_CHANCE's own comment (sand.h) for why
     * both are a valid outcome of the same roll. 4000 independent columns
     * at roughly 20% chance each per step makes waiting past one step
     * essentially unnecessary, but a small loop keeps this from being
     * sensitive to exactly which seed sand_init() above happens to use. */
    bool diluted = false;
    for (int i = 0; i < 10 && !diluted; i++) {
        sand_step(&fx.dilute_sim, 0, 1000, 0);
        for (int x = 0; x < DILUTE_W && !diluted; x++) {
            const uint8_t top = CELL_MATERIAL(sand_at(&fx.dilute_sim, x, 0));
            const uint8_t bot = CELL_MATERIAL(sand_at(&fx.dilute_sim, x, 1));
            diluted = (top != MAT_WATER) || (bot != MAT_ACID);
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(dilute_cells);

    TEST_ASSERT_TRUE_MESSAGE(diluted,
        "acid touching water must eventually dilute - either the acid "
        "cell becoming water or the water cell becoming acid - or the "
        "reaction is not firing at all");
}

/* No longer a bias to measure - SAND_ACID_DILUTE_TO_WATER_CHANCE (sand.h)
 * is 118, genuinely half of the 236-wide range left over once
 * SAND_ACID_DILUTE_EVAPORATE_CHANCE's 20-in-256 has already been taken
 * off the top of the ladder (see that constant's own comment for why
 * 128 - half of the FULL 256 - was a bug, not just a rounder number),
 * so SAND_ACID_DILUTE_MASS_BIAS's own local backing is the only thing
 * that is supposed to tip a bite one way or the other (see the ladder's
 * own comment, sand_reactions.c). This fixture's packed, symmetric
 * layout gives every interior column a net backing of zero (see
 * acid_water_dilute_fixture's own comment), so the base rate is exactly
 * what this measures. Checked in a single step so the two outcome
 * counts are independent per-column samples, same reasoning this test
 * always used, just no longer expecting one side to win the count -
 * only that neither side is left out entirely, and that the two stay in
 * the same neighbourhood rather than one swamping the other the way a
 * real bias would produce. The tolerance below is ordinary sampling
 * noise now, not a structural asymmetry to absorb - the split itself is
 * genuinely even. */
static void test_the_dilution_split_favours_neither_side(void)
{
    uint8_t *dilute_cells = malloc((size_t)DILUTE_W * DILUTE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(dilute_cells,
        "acid/water dilution grid must fit in what the framebuffer leaves");
    acid_water_dilute_fixture(dilute_cells);
    sand_step(&fx.dilute_sim, 0, 1000, 0);

    int water_wins = 0;
    int acid_wins  = 0;
    for (int x = 0; x < DILUTE_W; x++) {
        const uint8_t top = CELL_MATERIAL(sand_at(&fx.dilute_sim, x, 0));
        const uint8_t bot = CELL_MATERIAL(sand_at(&fx.dilute_sim, x, 1));
        if (bot == MAT_WATER) {
            water_wins++;   /* the acid cell (row 1) became water */
        }
        if (top == MAT_ACID) {
            acid_wins++;    /* the water cell (row 0) became acid */
        }
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(dilute_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, water_wins,
        "expected at least some acid-becomes-water dilutions in 4000 "
        "independent columns");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, acid_wins,
        "expected at least some water-becomes-acid dilutions in 4000 "
        "independent columns - a 50/50 split still means both sides win "
        "regularly, not that one of them stops happening");

    /* Two-thirds, not near-equality - loose enough to absorb ordinary
     * sampling noise at this fixture's width, tight enough that the
     * old, clearly-biased split (roughly 500-ish vs 250-ish at this
     * width, back when this constant read 141) would still fail it. */
    const int smaller = (water_wins < acid_wins) ? water_wins : acid_wins;
    const int larger  = (water_wins < acid_wins) ? acid_wins : water_wins;
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(larger * 2 / 3, smaller,
        "water_wins and acid_wins should stay in the same neighbourhood "
        "now that SAND_ACID_DILUTE_TO_WATER_CHANCE is an even coin flip - "
        "one badly outnumbering the other means something is still "
        "favouring a side");
}

/* A dedicated fixture for the two pairing tests below - alternating
 * acid/water columns separated by a column of GLASS, not an empty gap.
 * Two different designs were tried first and both broke for their own
 * reason:
 *
 * - The packed dilute_sim fixture above lets an acid cell's LEFT or
 *   RIGHT neighbour be another acid cell. Once that neighbour has
 *   already won its own water-wins bite earlier in the same left-to-
 *   right row scan, it has already turned into water by the time THIS
 *   cell's own dissolve search reaches it - a failed give-roll on the
 *   water directly above falls through to left/right next, and can land
 *   on that freshly-converted neighbour instead. A water-wins column's
 *   steam ended up written to the column beside it, not above it -
 *   correct behaviour, just not what a test assuming "the water cell is
 *   always directly above" can tell apart from a bug.
 * - An EMPTY gap between pairs (the old "fizzle" fixture's own idiom)
 *   does not have that problem, but introduces a different one: an
 *   empty cell is something a liquid can spread INTO, and the main
 *   sweep runs before reactions do - by the time this pass even starts,
 *   water or acid may already have drifted sideways into the gap,
 *   scrambling the one-pair-per-column layout before a single bite ever
 *   lands.
 * - STONE was tried next, on the assumption that a wall would simply
 *   never be a dissolve target - wrong, and caught only by measuring
 *   it: stone IS dissolvable (material.c, dissolvable=60, "stone gives
 *   way to acid now, just slowly"), so an acid cell whose own water
 *   neighbour's give-roll happened to miss would fall through to the
 *   stone beside it and could eat the separator instead - rare enough
 *   in ONE step that the single-step tests below still passed on a
 *   slightly reduced sample, but a real, silent hole in what the
 *   fixture claimed to guarantee.
 *
 * GLASS closes both holes for real: acid's own material comment names
 * it "the sole exception" left once stone stopped being immune
 * (acid_tank()'s own comment, this file, makes the same point), and
 * like stone it is not something anything can flow into (nothing moves
 * before reactions runs, same as the fully packed fixture's own
 * stability). */
#define SEPARATED_W 4000
#define SEPARATED_H 2

/* cells is HEAP, not static file scope - see acid_water_dilute_fixture's
 * own comment above for why. */
static void acid_water_separated_fixture(uint8_t *cells)
{
    sand_init(&fx.separated_dilute_sim, cells, SEPARATED_W, SEPARATED_H, 7u);
    sand_set_evaporates(&fx.separated_dilute_sim, 0);
    for (int x = 0; x < SEPARATED_W; x++) {
        if (x % 2 == 0) {
            sand_set(&fx.separated_dilute_sim, x, 0, CELL_MAKE(MAT_WATER, MASS_MAX));
            sand_set(&fx.separated_dilute_sim, x, 1, CELL_MAKE(MAT_ACID, MASS_MAX));
        } else {
            sand_set(&fx.separated_dilute_sim, x, 0, GLASS);
            sand_set(&fx.separated_dilute_sim, x, 1, GLASS);
        }
    }
}

/* Both outcomes of the win/lose split now change BOTH cells, not just
 * one - see step_one_dissolver_cell()'s own comment (sand_reactions.c).
 * The winning side is not left untouched any more, it boils into its own
 * vapour (MAT_STEAM for water, MAT_GAS for acid) at the same moment the
 * losing side converts into the winner's material - a deterministic
 * PAIR, not two independent coin flips, so wherever one half of a pair
 * is seen the other must be too, every single time. */
static void test_water_winning_the_dilution_boils_the_water_cell_to_steam(void)
{
    uint8_t *cells = malloc((size_t)SEPARATED_W * SEPARATED_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "acid/water separated grid must fit in what the framebuffer leaves");
    acid_water_separated_fixture(cells);
    sand_step(&fx.separated_dilute_sim, 0, 1000, 0);

    int water_wins = 0, water_wins_with_steam = 0;
    for (int x = 0; x < SEPARATED_W; x += 2) {
        if (CELL_MATERIAL(sand_at(&fx.separated_dilute_sim, x, 1)) != MAT_WATER) {
            continue; /* not a water-wins column - see the sibling test */
        }
        water_wins++;
        if (CELL_MATERIAL(sand_at(&fx.separated_dilute_sim, x, 0)) == MAT_STEAM) {
            water_wins_with_steam++;
        }
    }

    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, water_wins,
        "expected at least some acid-becomes-water dilutions in 2000 "
        "independent pairs to check steam against");
    TEST_ASSERT_EQUAL_INT_MESSAGE(water_wins, water_wins_with_steam,
        "every column where the acid cell became water must ALSO show "
        "the water cell that won boiled into steam - the two are one "
        "outcome of the same roll, not independent");
}

static void test_acid_winning_the_dilution_boils_the_acid_cell_to_gas(void)
{
    uint8_t *cells = malloc((size_t)SEPARATED_W * SEPARATED_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "acid/water separated grid must fit in what the framebuffer leaves");
    acid_water_separated_fixture(cells);
    sand_step(&fx.separated_dilute_sim, 0, 1000, 0);

    int acid_wins = 0, acid_wins_with_gas = 0;
    for (int x = 0; x < SEPARATED_W; x += 2) {
        if (CELL_MATERIAL(sand_at(&fx.separated_dilute_sim, x, 0)) != MAT_ACID) {
            continue; /* not an acid-wins column - see the sibling test */
        }
        acid_wins++;
        if (CELL_MATERIAL(sand_at(&fx.separated_dilute_sim, x, 1)) == MAT_GAS) {
            acid_wins_with_gas++;
        }
    }

    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, acid_wins,
        "expected at least some water-becomes-acid dilutions in 2000 "
        "independent pairs to check gas against");
    TEST_ASSERT_EQUAL_INT_MESSAGE(acid_wins, acid_wins_with_gas,
        "every column where the water cell became acid must ALSO show "
        "the acid cell that won boiled into gas - the two are one "
        "outcome of the same roll, not independent");
}

/* A dedicated fixture for oil's own dilution - oil directly above acid,
 * oil on top because that is ALREADY the stable density ordering (oil's
 * density is 22, acid's is 38 - oil floats on acid on its own, see
 * MAT_OIL's own comment in material.c), so nothing moves due to
 * gravity/density before reactions runs. Oil's dissolvable (16 - retuned
 * more than once, see its own comment in material.c for the earlier 40
 * and 1) is much lower than water's (220) - "slowly dilutes", not
 * readily - so a single step is not a safe bet the way the water fixture
 * is; the tests below step this fixture repeatedly instead (300 steps,
 * same budget as before).
 *
 * They do NOT read the final state after all 300 steps, though - a
 * column's oil cell does react with the acid below it at most once ever
 * (once it stops being MAT_OIL there is nothing left to dissolve in a
 * two-material fixture), but MAT_GAS is buoyant and this fixture is only
 * two rows tall with nowhere "up" to rise to, so a gas cell born in one
 * column can drift sideways into a neighbour's over the remaining steps.
 * Each test below classifies a column the instant it stops holding
 * MAT_OIL instead, before any later step's drift has a chance to touch
 * it - see either test's own comment for why that specific instant is
 * safe. */
#define OIL_DILUTE_W 400
#define OIL_DILUTE_H 2

/* cells is HEAP, not static file scope - see acid_water_dilute_fixture's
 * own comment above for why. */
static void acid_oil_dilute_fixture(uint8_t *cells)
{
    sand_init(&fx.oil_dilute_sim, cells, OIL_DILUTE_W, OIL_DILUTE_H, 11u);
    sand_set_evaporates(&fx.oil_dilute_sim, 0);
    for (int x = 0; x < OIL_DILUTE_W; x++) {
        sand_set(&fx.oil_dilute_sim, x, 0, CELL_MAKE(MAT_OIL, MASS_MAX));
        sand_set(&fx.oil_dilute_sim, x, 1, CELL_MAKE(MAT_ACID, MASS_MAX));
    }
}

/* SAND_ACID_OIL_TO_GAS_CHANCE (sand.h): a bitten oil cell mostly boils
 * into gas now, acid is the minority outcome that survives from the old
 * "always becomes acid" rule.
 *
 * Classifies each column the instant its own oil cell disappears, not
 * from a final-state read taken after the fixture has run to completion.
 * MAT_GAS is buoyant (see try_bubble(), sand_gas.c) and this fixture is
 * only two rows tall with nowhere "up" left to rise to, so over 300 steps
 * a gas cell born in one column has plenty of opportunity to drift
 * sideways into a neighbour's - at which point "MAT_GAS appears
 * somewhere in this column" stops meaning "this column's own oil boiled
 * off" and a final read cannot tell the two apart. Sampling the column
 * the moment it stops holding MAT_OIL sidesteps that: at that instant the
 * only step that has touched it is the one that just ran the reaction
 * itself, so what is sitting in the two cells is still that step's own
 * output. Each column is then marked done and never resampled, so a
 * later step's drift cannot re-classify it. */
static void test_oil_mostly_boils_off_into_gas_not_acid(void)
{
    uint8_t *oil_dilute_cells = malloc((size_t)OIL_DILUTE_W * OIL_DILUTE_H);
    bool    *done = calloc((size_t)OIL_DILUTE_W, sizeof(bool));
    TEST_ASSERT_NOT_NULL_MESSAGE(oil_dilute_cells,
        "acid/oil dilution grid must fit in what the framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(done, "per-column done tracking must fit");
    acid_oil_dilute_fixture(oil_dilute_cells);

    int gas = 0, acid_spread = 0;
    for (int i = 0; i < 300; i++) {
        sand_step(&fx.oil_dilute_sim, 0, 1000, 0);
        for (int x = 0; x < OIL_DILUTE_W; x++) {
            if (done[x]) {
                continue;
            }
            const uint8_t m0 = CELL_MATERIAL(sand_at(&fx.oil_dilute_sim, x, 0));
            const uint8_t m1 = CELL_MATERIAL(sand_at(&fx.oil_dilute_sim, x, 1));
            if (m0 == MAT_OIL || m1 == MAT_OIL) {
                continue; /* not bitten yet - still a live sample */
            }
            done[x] = true;
            if (m0 == MAT_GAS || m1 == MAT_GAS) {
                gas++;
            } else {
                acid_spread++;
            }
        }
    }

    free(done);
    free(oil_dilute_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, gas,
        "expected at least one oil-to-gas conversion within 300 steps "
        "across 400 independent columns");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, acid_spread,
        "SAND_ACID_OIL_TO_GAS_CHANCE biases the outcome, it does not "
        "eliminate the other side entirely - acid spreading into oil "
        "should still happen sometimes");
    TEST_ASSERT_GREATER_THAN_MESSAGE(acid_spread, gas,
        "SAND_ACID_OIL_TO_GAS_CHANCE is supposed to favour gas heavily - "
        "oil boiling off should be clearly more common than oil turning "
        "into more acid");
}

/* SAND_ACID_OIL_DEATH_CHANCE (sand.h): the acid that ate an oil cell
 * separately rolls a much higher chance to die outright - its whole
 * remaining mass gone in this one bite - instead of pay_quench_cost()'s
 * ordinary one-unit chip. Both are reachable, neither is a certainty.
 *
 * Same instant-of-transition sampling as the test above, and for the
 * same reason: MAT_GAS drifting between columns over 300 steps would
 * make a final-state read unreliable, and here it is worse than merely
 * unreliable - a gas cell that later drifts back OUT of a column it
 * passed through leaves an empty cell behind, which is exactly what a
 * genuine died-outright column also looks like, so a final read could
 * mistake ordinary gas traffic for a death that never happened. Sampled
 * the instant a column stops holding MAT_OIL, before any step but its
 * own reaction has touched it, that ambiguity cannot arise: an empty
 * cell can only be the original acid cell gone in one bite (the
 * oil-derived cell is always gas or acid, never empty), and an acid
 * cell at exactly MASS_MAX - 1 can only be the ordinary one-unit chip
 * (MASS_MAX itself is a fresh acid cell born from the oil-becomes-acid
 * branch, not the one that did the eating). */
static void test_the_acid_that_ate_oil_can_die_in_a_single_bite(void)
{
    uint8_t *oil_dilute_cells = malloc((size_t)OIL_DILUTE_W * OIL_DILUTE_H);
    bool    *done = calloc((size_t)OIL_DILUTE_W, sizeof(bool));
    TEST_ASSERT_NOT_NULL_MESSAGE(oil_dilute_cells,
        "acid/oil dilution grid must fit in what the framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(done, "per-column done tracking must fit");
    acid_oil_dilute_fixture(oil_dilute_cells);

    int died_outright = 0, chipped_by_one = 0;
    for (int i = 0; i < 300; i++) {
        sand_step(&fx.oil_dilute_sim, 0, 1000, 0);
        for (int x = 0; x < OIL_DILUTE_W; x++) {
            if (done[x]) {
                continue;
            }
            const cell_t c0 = sand_at(&fx.oil_dilute_sim, x, 0);
            const cell_t c1 = sand_at(&fx.oil_dilute_sim, x, 1);
            if (CELL_MATERIAL(c0) == MAT_OIL || CELL_MATERIAL(c1) == MAT_OIL) {
                continue; /* not bitten yet - still a live sample */
            }
            done[x] = true;
            if (CELL_IS_EMPTY(c0) || CELL_IS_EMPTY(c1)) {
                died_outright++;
            }
            if ((CELL_MATERIAL(c0) == MAT_ACID && CELL_VARIANT(c0) == MASS_MAX - 1)
                || (CELL_MATERIAL(c1) == MAT_ACID && CELL_VARIANT(c1) == MASS_MAX - 1)) {
                chipped_by_one++;
            }
        }
    }

    free(done);
    free(oil_dilute_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, died_outright,
        "SAND_ACID_OIL_DEATH_CHANCE (sand.h) is supposed to let the acid "
        "that ate an oil cell die outright, from full mass to nothing in "
        "the same bite - none did across 400 independent columns");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, chipped_by_one,
        "SAND_ACID_OIL_DEATH_CHANCE is a chance, not a certainty - "
        "pay_quench_cost()'s ordinary one-unit chip must still be "
        "reachable too, not replaced outright");
}

/* evaporates forced to 255 so this is a one-step, deterministic
 * assertion instead of waiting on the material's own low figure - the
 * same technique test_stone_conducts_heat_into_water_beyond_it uses for
 * sand_set_conduction(). */
static void test_acid_evaporates_into_gas_when_forced(void)
{
    fixture();
    /* Boxed in on every side it could move to - a liquid's fall and
     * spread are not gated by mobility the way a gas grain's rise is
     * (see move_liquid_grain(), sand_liquid.c), so nothing short of
     * actually walling the cell in keeps it in place for its one step. */
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 4, 3, STONE);
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 3, 4, STONE);
    sand_set(&s, 4, 4, STONE);
    sand_set_evaporates(&s, 255);
    sand_set(&s, 3, 3, CELL_MAKE(MAT_ACID, MASS_MAX));

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "a cell of acid with evaporates forced to 255 must turn to gas "
        "in a single step");
}

/* SAND_ACID_DILUTE_MASS_BIAS's actual point, end to end - proven by
 * comparing the SAME sustained pour with the mechanism forced off
 * (sand_set_acid_dilute_mass_bias(&sim, 0), a pure unbiased coin flip)
 * against the same pour with it left at its real default, rather than
 * by watching one biased run alone approach total saturation.
 *
 * That was the bug in the first version of this test: at 400 steps, a
 * 100-wide tap re-filled every single step pours 40,000 cells into a
 * 4,900-cell basin - roughly eight times its capacity - which empties
 * the pool through sheer poured VOLUME regardless of whether the
 * reaction favours the tap at all. Confirmed directly: forcing the
 * win/lose split to make the tap NEVER win a single bite still left
 * the test passing, because the tap material displaces the pool by
 * gravity and refill alone once enough of it has been poured. Given
 * unlimited attempts against a fixed-size pool, sheer resupply wins
 * eventually even at a neutral 50/50 split - see this file's own
 * pour_and_count() history for why "does the tap end up dominant" was
 * never actually testing SAND_ACID_DILUTE_MASS_BIAS.
 *
 * Cut down to a step budget short enough that NEITHER run saturates
 * (leaving room for a difference to show at all) and turned into an
 * A/B comparison instead: the local-backing mechanism's whole claim is
 * that it makes a sustained excess convert its opposite FASTER, so
 * that is what gets measured - the biased run must convert strictly
 * more of the pool than the identically-seeded unbiased run does in
 * the same window, not just "some, eventually". */
#define DILUTE_POUR_W          100
#define DILUTE_POUR_H          50
#define DILUTE_POUR_POOL_DEPTH 20
#define DILUTE_POUR_STEPS      150

/* cells is HEAP, not static file scope - see acid_water_dilute_fixture's
 * own comment above for why. Same seed every call, deliberately - an
 * A/B comparison wants the same random inputs on both legs, with only
 * the mass-bias override differing, not seed-to-seed noise on top. */
static void
acid_water_pour_fixture(uint8_t *cells, material_id_t pool)
{
    sand_init(&fx.dilute_pour_sim, cells, DILUTE_POUR_W, DILUTE_POUR_H, 17u);
    sand_set_evaporates(&fx.dilute_pour_sim, 0); /* isolate the mass bias from
                                                * the unrelated ambient
                                                * evaporates roll - same
                                                * reasoning as the other
                                                * dilution fixtures above. */
    for (int x = 0; x < DILUTE_POUR_W; x++) {
        sand_set(&fx.dilute_pour_sim, x, DILUTE_POUR_H - 1, STONE);
    }
    for (int y = DILUTE_POUR_H - 1 - DILUTE_POUR_POOL_DEPTH; y < DILUTE_POUR_H - 1; y++) {
        for (int x = 0; x < DILUTE_POUR_W; x++) {
            sand_set(&fx.dilute_pour_sim, x, y, CELL_MAKE(pool, MASS_MAX));
        }
    }
}

static void
pour_and_count(material_id_t tap, int *out_pool_mat, int *out_tap_mat)
{
    for (int i = 0; i < DILUTE_POUR_STEPS; i++) {
        for (int x = 0; x < DILUTE_POUR_W; x++) {
            sand_set(&fx.dilute_pour_sim, x, 0, CELL_MAKE(tap, MASS_MAX));
        }
        sand_step(&fx.dilute_pour_sim, 0, 1000, 0);
    }

    int pool_mat = 0, tap_mat = 0;
    const material_id_t pool = (tap == MAT_ACID) ? MAT_WATER : MAT_ACID;
    for (int y = 0; y < DILUTE_POUR_H - 1; y++) {
        for (int x = 0; x < DILUTE_POUR_W; x++) {
            const uint8_t m = CELL_MATERIAL(sand_at(&fx.dilute_pour_sim, x, y));
            if (m == pool) {
                pool_mat++;
            } else if (m == tap) {
                tap_mat++;
            }
        }
    }
    *out_pool_mat = pool_mat;
    *out_tap_mat  = tap_mat;
}

/* Runs the whole pour, with the mass-bias override forced to `bias`
 * (negative restores the real default), and returns how much of the
 * tap material the pool ended up holding - the higher this is, the
 * more the pool converted. */
static int
pour_and_measure_tap_gain(material_id_t pool, material_id_t tap, int bias)
{
    uint8_t *cells = malloc((size_t)DILUTE_POUR_W * DILUTE_POUR_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "acid/water pour grid must fit in what the framebuffer leaves");
    acid_water_pour_fixture(cells, pool);
    sand_set_acid_dilute_mass_bias(&fx.dilute_pour_sim, bias);

    int pool_left, tap_now;
    pour_and_count(tap, &pool_left, &tap_now);

    free(cells);
    return tap_now;
}

static void
test_a_relentless_pour_of_acid_overwhelms_a_pool_of_water(void)
{
    const int tap_now_unbiased = pour_and_measure_tap_gain(MAT_WATER, MAT_ACID, 0);
    const int tap_now_biased   = pour_and_measure_tap_gain(MAT_WATER, MAT_ACID, -1);

    TEST_ASSERT_GREATER_THAN_MESSAGE(tap_now_unbiased, tap_now_biased,
        "SAND_ACID_DILUTE_MASS_BIAS is supposed to let a sustained excess "
        "of acid convert the water pool FASTER than an unbiased coin "
        "flip does in the same window - identically seeded runs, forced "
        "to bias 0 vs the real default, ended up converting the same "
        "amount, so the mechanism is not doing anything measurable");
}

/* NOT an A/B bias comparison, unlike the sibling test above - measured
 * directly, and it goes the WRONG way here: forcing bias to 0 converted
 * MORE of the acid pool than the real default did (3623 vs 3451 at 150
 * steps, one fixed seed). SAND_ACID_DILUTE_MASS_BIAS is not neutral in
 * this direction, it is actively counterproductive, and the reason is
 * density, not a bug in the bias arithmetic itself: acid (density 38)
 * sinks and disperses through water when poured on top of it, so a
 * poured acid grain is usually an isolated cell surrounded by water -
 * exactly the case SAND_ACID_DILUTE_MASS_BIAS's own comment (sand.h)
 * already covers ("an isolated drop in a big lake gets pushed even
 * further toward diluting"). Water (density 30) does the opposite when
 * poured onto acid: being LESS dense, it sits on top rather than
 * penetrating, so the acid cells actually doing the biting - only acid
 * ever rolls this reaction - stay backed by the deep, undisturbed pool
 * beneath them the whole time, and the bias tips every one of those
 * bites further toward "acid wins" instead of helping water win faster.
 * A real, structural asymmetry between the two pour directions, not
 * something this test should paper over by asserting a relationship
 * that does not hold - flagged for a design decision, not silently
 * fixed here, since the working direction above was tuned deliberately
 * and changing the bias formula to help this direction too risks
 * breaking that one.
 *
 * What IS still true, and worth pinning: water overwhelms a sustained
 * acid pool by sheer volume alone even with bias doing nothing useful
 * for it - the tap ends up the clear majority of the basin regardless. */
static void
test_a_relentless_pour_of_water_overwhelms_a_pool_of_acid(void)
{
    const int tap_now = pour_and_measure_tap_gain(MAT_ACID, MAT_WATER, -1);

    TEST_ASSERT_GREATER_THAN_MESSAGE(DILUTE_POUR_W * (DILUTE_POUR_H - 1) / 2, tap_now,
        "a sustained pour of water onto an acid pool must still end up "
        "the clear majority of the basin - SAND_ACID_DILUTE_MASS_BIAS "
        "does not help this direction (see this test's own comment for "
        "why), but sheer poured volume against a fixed-size pool still "
        "has to win eventually");
}

static void test_a_little_acid_cannot_eat_an_unlimited_amount(void)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 2; y < H - 1; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    const int sand_before = count_cells_of(MAT_SAND);
    /* One cell of acid: MASS_MAX units, so at one unit a cell it can
     * account for at most MASS_MAX cells however long it is left. */
    sand_set(&s, W / 2, 1, CELL_MAKE(MAT_ACID, MASS_MAX));

    for (int i = 0; i < 2000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const int eaten = sand_before - count_cells_of(MAT_SAND);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(MASS_MAX, eaten,
        "a single cell of acid holds MASS_MAX units and spends one per "
        "cell, so it can never dissolve more than that many - if it can, "
        "the bite has stopped costing anything");
}

static void test_every_liquid_declares_a_mobility(void)
{
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (material_by_id((material_id_t)m)->kind != KIND_LIQUID) {
            continue;
        }
        char msg[160];
        snprintf(msg, sizeof msg,
                 "%s is a liquid and must set its own `mobility` - leaving "
                 "it unset does not read as an error, it reads as a very "
                 "viscous liquid, which is how it lasts",
                 material_by_id((material_id_t)m)->name);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, material_by_id((material_id_t)m)->mobility, msg);
    }
}

/* Interfacial drag: a liquid pushing into another gets less willing the
 * further in it already is.
 *
 * Two liquids exchange by swapping whole cells gravity-ward, and under tilt
 * that direction is DITHERED between two octants step by step. Ungated,
 * every water cell with oil below it swaps every step, so water drills into
 * the oil along alternating diagonals and the boundary becomes a mixed
 * band - which on screen reads as straight lines running through what
 * should be a smooth surface.
 *
 * What the drag actually buys is not a smaller number, it is a BOUNDED one.
 * Measured across grid widths 24 to 40, eight seeds each, counting water
 * cells left sitting inside the oil after a tilt:
 *
 *     width      24    26    28    30    32    34    36    40
 *     ungated  17.4  18.9  20.5  21.4  14.3  16.9  22.4  29.8
 *     drag      8.1  12.5  12.1   8.9  12.5  12.1  12.0  12.3
 *
 * Ungated it grows with the length of the interface; with drag it sits flat
 * near twelve however wide the board gets. That is the property worth
 * having and the one worth testing, so this uses a deliberately WIDE grid -
 * on the 32-wide shared fixture the two are 14.3 against 12.5, close enough
 * that the test passed either way and proved nothing. It did, for one
 * round, before the sweep above was run.
 *
 * Averaged over seeds for the same reason: a single run of a chaotic scene
 * is not evidence. */
#define DRAG_W 40
#define DRAG_H 20

static int water_inside_oil(sand_t *g)
{
    static const int d[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
    int inside = 0;
    for (int y = 1; y < DRAG_H - 1; y++) {
        for (int x = 1; x < DRAG_W - 1; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) != MAT_WATER) {
                continue;
            }
            int oil = 0;
            for (int k = 0; k < 4; k++) {
                if (CELL_MATERIAL(sand_at(g, x + d[k][0], y + d[k][1]))
                        == MAT_OIL) {
                    oil++;
                }
            }
            if (oil >= 3) {
                inside++;
            }
        }
    }
    return inside;
}

static void test_water_does_not_drill_into_oil_when_tilted(void)
{
    /* HEAP, not static file scope - one malloc reused across all `seeds`
     * iterations via memset + a fresh sand_init() each time, freed once
     * after the loop - see drop_impulse_buf's own comment above for why
     * this file's static test fixtures cannot share the framebuffer's
     * memory budget. */
    uint8_t *drag_cells = malloc((size_t)DRAG_W * DRAG_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(drag_cells,
        "interfacial-drag grid must fit in what the framebuffer leaves");
    sand_t g;
    const int seeds = 4;
    int total = 0;

    for (int k = 0; k < seeds; k++) {
        memset(drag_cells, 0, (size_t)DRAG_W * DRAG_H);
        sand_init(&g, drag_cells, DRAG_W, DRAG_H, (uint32_t)(11 + k));
        sand_set_mobility(&g, SAND_MOBILITY_PER_MATERIAL);

        for (int x = 0; x < DRAG_W; x++) {
            sand_set(&g, x, 0, STONE);
            sand_set(&g, x, DRAG_H - 1, STONE);
        }
        for (int y = 0; y < DRAG_H; y++) {
            sand_set(&g, 0, y, STONE);
            sand_set(&g, DRAG_W - 1, y, STONE);
        }
        /* A thin slick of oil on the floor with water resting on it - the
         * unstable order, since water is the denser of the two. */
        for (int x = 1; x < DRAG_W - 1; x++) {
            sand_set(&g, x, DRAG_H - 2, CELL_MAKE(MAT_OIL, MASS_MAX));
            sand_set(&g, x, DRAG_H - 3, CELL_MAKE(MAT_OIL, MASS_MAX));
        }
        for (int y = DRAG_H - 7; y <= DRAG_H - 4; y++) {
            for (int x = 1; x < DRAG_W - 1; x++) {
                sand_set(&g, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
            }
        }

        for (int i = 0; i < 60; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        for (int i = 0; i < 300; i++) {
            sand_step(&g, 700, 700, 0);
        }
        total += water_inside_oil(&g);
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(drag_cells);

    /* 20 sits between the two measured means at this width (29.8 ungated,
     * 12.3 with drag) with room either side for ordinary variation. */
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(20 * seeds, total,
        "water must not end up riddled through the oil body after a tilt - "
        "without interfacial drag the gravity-ward swap fires on every "
        "interface cell every step, drilling into the oil along the "
        "dithered diagonals, and the intrusion grows with the width of the "
        "board instead of staying bounded");
}

static void test_oil_flows_more_slowly_than_water(void)
{
    int steps[2];
    const material_id_t liquids[2] = { MAT_WATER, MAT_OIL };

    /* On `wide` (32 cells across), not the 8-wide default fixture. Over
     * six cells of travel both liquids arrive in the same four steps and
     * the difference is pure quantisation; the ratio only means anything
     * across a real distance. One malloc reused across both liquids via
     * a fresh sand_init() each iteration, freed once after the loop - see
     * drop_impulse_buf's own comment above for why this file's static
     * test fixtures cannot share the framebuffer's memory budget. */
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "oil-vs-water flow-rate grid must fit in what the framebuffer "
        "leaves");
    for (int k = 0; k < 2; k++) {
        sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 5u);
        sand_set_mobility(&wide, SAND_MOBILITY_PER_MATERIAL);
        for (int x = 0; x < WIDE_W; x++) {
            sand_set(&wide, x, WIDE_H - 1, STONE);
        }
        for (int y = 1; y <= WIDE_H - 2; y++) {
            for (int x = 1; x <= 4; x++) {
                sand_set(&wide, x, y, CELL_MAKE(liquids[k], MASS_MAX));
            }
        }

        steps[k] = -1;
        for (int i = 1; i <= 3000 && steps[k] < 0; i++) {
            sand_step(&wide, 0, 1000, 0);
            if (!CELL_IS_EMPTY(sand_at(&wide, WIDE_W - 2, WIDE_H - 2))) {
                steps[k] = i;
            }
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, steps[k],
            "setup: both liquids must eventually reach the far wall");
    }

    /* Freed BEFORE the final assertion: Unity longjmps out of a failure,
     * so a free() after one never runs - see drop_impulse_buf's own
     * comment above. All reads of wide_cells are done by this point. */
    free(wide_cells);

    /* Half again, not double: oil is 140 against water's 255 and lands
     * around twice the time, but this is a "they are distinguishable"
     * assertion rather than a pin on the current figures, and oil is
     * deliberately tunable towards water. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE((steps[0] * 3) / 2, steps[1],
        "oil must take clearly longer than water to spread the same "
        "distance - before `mobility` had a liquid reader the two were "
        "indistinguishable");
}

static void test_oil_trapped_under_water_floats_to_the_surface(void)
{
    fixture();
    sand_set_decay(&s, 0);
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, 7, STONE);
    }
    for (int y = 2; y <= 6; y++) {
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);
    }
    for (int y = 2; y <= 5; y++) {
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, 6, OIL);      /* underneath the whole column */
    }

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Asserted as an ORDERING rather than "oil is at row 2", because
     * these cells are half full (see the shorthand macros at the top of
     * this file) so the column does not reach the brim and the surface
     * is not where counting rows would suggest. What matters is that
     * every oil cell ends up above every water cell. */
    /* Asserted as "which liquid is on top", not as a strict row
     * ordering of every cell, and not as "oil is at row 2".
     *
     * Two things make the tempting stronger assertions wrong. These
     * cells are half full (see the shorthand macros at the top of this
     * file), so the column never reaches the brim and the surface is not
     * where counting rows would put it. And the two liquids cannot mix
     * within a cell, so the interface between them is ragged - one row
     * genuinely holds some oil and some water at the same time, which
     * makes "every oil cell is above every water cell" false even when
     * the separation is perfect. */
    int top = -1, bottom = -1;
    for (int y = 0; y < H && top < 0; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_LIQUID) {
                top = CELL_MATERIAL(c);
                break;
            }
        }
    }
    for (int y = H - 1; y >= 0 && bottom < 0; y--) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_LIQUID) {
                bottom = CELL_MATERIAL(c);
                break;
            }
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_OIL, top,
        "the topmost liquid must be OIL - it started underneath the "
        "whole column and has to have risen through it, which only "
        "happens because the denser water sinks into it, a "
        "gravity-ward move riding the main sweep's own no-double-move "
        "guarantee");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, bottom,
        "and the bottom-most liquid must be WATER, for the same reason "
        "from the other end");
}

/* The one exception to "denser sinks": sand is 60 against oil's 22, and by
 * can_enter()'s ordinary rule that sinks straight through, the same way it
 * sinks through water (test_sand_sinks_through_water above). can_enter()
 * carries a named exception for exactly this pairing instead - see its own
 * comment in sand_priv.h for why oil's density cannot simply be raised to
 * fix this the way every other material pairing is resolved. */
static void test_sand_floats_on_oil(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, OIL);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(sand_at(&s, 3, 4)),
        "sand must rest on top of an oil pool rather than sink into it, "
        "despite being denser");
}

/* The exception is named by material id, not by kind or density band, so
 * it must not leak onto another powder that happens to share the same
 * fate. Dirt (62) is denser than sand and gets no exception - it must
 * keep sinking through oil exactly as the density table says. */
static void test_dirt_still_sinks_through_oil(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, OIL);
        }
    }
    sand_set(&s, 3, 3, CELL_SOIL(MAT_DIRT, 1, 0));

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_DIRT,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "dirt is not sand, and the sand/oil exception must not have "
        "spread to it - dirt is denser than oil and must still sink "
        "all the way through the pool");
}

/* Lava is the first material that is a liquid AND a heat source, so it
 * is the first place the variant nibble's two meanings could collide.
 * decay != 0 would make tick_decay() read a lava cell's MASS as a
 * lifespan and eat it. */
static void test_lava_does_not_decay_away(void)
{
    fixture();
    /* Deliberately NO sand_set_decay() here, and that is the point of
     * the test rather than an omission.
     *
     * The obvious version of this forces sand_set_decay(&s, 255) to make
     * any decay show up immediately - but that override replaces the
     * per-material figure for EVERY material at once (see tick_decay()),
     * lava included, so it forces lava to decay no matter what its own
     * row says and destroys the cell every time. It tests the override,
     * not the table.
     *
     * Running on the per-material defaults instead is what actually pins
     * the thing worth pinning: lava's own decay must be 0, so that
     * tick_decay() never reads its variant nibble - which for a liquid
     * is FILL LEVEL, not life - and never eats the cell's mass. Set
     * lava's decay to anything nonzero and this fails.
     *
     * A one-cell-wide well. Lava is a liquid, so a lone cell on an open
     * floor does not stay put - equalise_liquids() spreads it sideways
     * and thins it to nothing worth measuring. Penning it in is what
     * makes "is it still here, and still full" a question about DECAY
     * rather than about flow. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 4, H - 2, STONE);
    sand_set(&s, 3, H - 2, LAVA);

    for (int i = 0; i < 4 * MATERIAL_VARIANTS; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA, CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "lava must be immortal - its variant nibble is a FILL LEVEL, "
        "not life remaining, so any decay at all would consume the "
        "cell's own mass");
    TEST_ASSERT_EQUAL_INT_MESSAGE(CELL_VARIANT(LAVA),
        CELL_VARIANT(sand_at(&s, 3, H - 2)),
        "and at exactly the mass it was placed with, not merely present "
        "- a decay tick reads the variant nibble as life and would show "
        "up here first");
}

/* The reaction that makes lava worth having, and a straight reuse of
 * quench_to: what water does to a burning cell is put it out, and what
 * putting lava out means is rock. */
static void test_water_freezes_lava_into_stone(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, LAVA);
    sand_set(&s, 4, 3, WATER);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "lava quenched by water must become stone, not vanish - "
        "reaction_t.quench_to, the same field that turns a quenched "
        "fire into steam");
}

/* Guards may_have_heat_holder's arm-only design (sand.h/sand_priv.h): the
 * flag is armed the moment a heat_ramp cell exists on the grid and is
 * deliberately never cleared, because a heat_ramp cell can be CREATED
 * mid-pass, behind step_one_reacting_row()'s own scan pointer, rather
 * than painted onto the board before the step runs. Lava quenching into
 * stone is exactly that: place_reacted() writes the new MAT_STONE cell
 * from inside the very row walk that is checking may_have_heat_holder's
 * sibling flags, so the row that already passed this cell never reports
 * it, and an end-of-pass clear - "tidy it up like the other five flags" -
 * would erase what latch_content_flags() had just armed one line
 * earlier. That is the regression this test exists to catch: anyone who
 * adds `if (!(found & FOUND_HEAT_HOLDER)) s->may_have_heat_holder =
 * false;` next to the other five in sand_step_reactions() will fail here,
 * because the very first quenched cell has no way left to ever re-arm it.
 *
 * Deliberately no stone or glass is painted anywhere in this scene - not
 * even as a floor or walls - specifically so may_have_heat_holder starts
 * false and the only heat_ramp cell that ever exists is the one born from
 * the quench itself. Lava and water sit on the bottom row instead of
 * needing a floor: dest_row() returns NULL past the last row, which is
 * enough to stop either of them falling out from under themselves without
 * painting a single cell that would pre-arm the flag.
 *
 * Also worth noting because it is the whole reason this flag has to be
 * independent of may_have_temperature: place_reacted() gives a freshly
 * created heat-ramp cell SAND_AMBIENT_HEAT, not a hot variant, so the new
 * stone does NOT arm may_have_temperature (see latch_content_flags()'s
 * ambient-exception). It must still arm may_have_heat_holder, because
 * that is exactly what a later convecting gas cell needs to find it. */
static void test_lava_quenched_into_stone_mid_pass_arms_the_heat_holder_flag(void)
{
    fixture();
    sand_clear(&s);

    TEST_ASSERT_FALSE_MESSAGE(s.may_have_heat_holder,
        "setup: a freshly cleared grid holds nothing with a heat_ramp, so "
        "the flag must start false");

    /* Lava IS the heat rather than holding one (no heat_ramp of its own);
     * water quenches it into stone, which does. Resting on the bottom row
     * so neither needs a floor cell to stay put. */
    sand_set(&s, 3, H - 1, LAVA);
    sand_set(&s, 4, H - 1, WATER);

    bool saw_heat_holder = false;
    for (int i = 0; i < 5; i++) {
        sand_step(&s, 0, 1000, 0);

        bool any_heat_holder = false;
        for (int y = 0; y < H && !any_heat_holder; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (!CELL_IS_EMPTY(c) && reaction_of(c)->heat_ramp != 0) {
                    any_heat_holder = true;
                    break;
                }
            }
        }
        if (any_heat_holder) {
            saw_heat_holder = true;
        }

        TEST_ASSERT_TRUE_MESSAGE(!any_heat_holder || s.may_have_heat_holder,
            "a cell with a non-zero heat_ramp exists on the grid, so "
            "may_have_heat_holder must be armed - even though this cell "
            "(lava quenched to stone) was created mid-pass, behind the "
            "scan pointer, rather than painted onto the board before the "
            "step ran");
    }

    /* If this never went true, the loop above proved nothing - it never
     * saw a heat holder and passed for the wrong reason. */
    TEST_ASSERT_TRUE_MESSAGE(saw_heat_holder,
        "setup: lava next to water must have quenched into stone within "
        "5 steps, or this test never exercised the mid-pass creation case "
        "it exists to guard");
}

/* A single-file shaft of lava, walled on both sides so the only cell a
 * lava neighbour can ever be is the one straight down - which is what
 * makes cool_off_chain()'s own walk (sand_reactions.c) deterministic here
 * rather than wandering sideways through a wide pool. Water sits directly
 * on top of the shaft's first cell; everything below it is stone, so the
 * whole column can only ever be read as: the crust cool_off_chain() has
 * reached so far, contiguous from the top. */
#define LAVA_SHAFT_W    3
#define LAVA_SHAFT_H    32
#define LAVA_SHAFT_X    1
#define LAVA_SHAFT_TOP  1
#define LAVA_SHAFT_BOT  (LAVA_SHAFT_H - 2)  /* one cell above the floor */

static void build_lava_shaft(sand_t *p, uint8_t *cells)
{
    /* 0u, not the 71u used elsewhere in this file as a generic fixed
     * seed: with cool-off pinned to its own max (255/256 per link), seed
     * 71 happens to roll a failure after only ~12 links regardless of how
     * deep the shaft is - verified by brute-forcing seeds against this
     * exact scene, where 0 (like most seeds) instead runs the walk all
     * the way to the shaft's floor when nothing caps it. That makes 0 the
     * one that actually exercises SAND_LAVA_COOLOFF_MAX_CHAIN in
     * test_the_cool_off_chain_is_bounded below - 71 would leave that
     * test passing for the wrong reason, bounded by luck rather than by
     * the cap, for any cap above ~12. */
    sand_init(p, cells, LAVA_SHAFT_W, LAVA_SHAFT_H, 0u);
    for (int y = 0; y < LAVA_SHAFT_H; y++) {
        sand_set(p, LAVA_SHAFT_X - 1, y, STONE);
        sand_set(p, LAVA_SHAFT_X + 1, y, STONE);
    }
    sand_set(p, LAVA_SHAFT_X, LAVA_SHAFT_H - 1, STONE); /* floor */
    /* MASS_MAX, not the LAVA macro's mass of 8 - every cell in the shaft
     * is full, so move_liquid_grain() finds no room in the cell below and
     * the column sits motionless from the first step. A column of
     * UNDER-full liquid cells is not at rest: each has room to take more,
     * so ordinary liquid movement pours mass downward through it every
     * step regardless of anything this test cares about, scrambling
     * which cell holds how much lava before the reactions pass ever gets
     * a turn - caught by this test itself, which saw its water vanish
     * and its crust depth read 0 before this was full mass. */
    for (int y = LAVA_SHAFT_TOP; y <= LAVA_SHAFT_BOT; y++) {
        sand_set(p, LAVA_SHAFT_X, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }
}

/* How many cells of MAT_STONE sit contiguous from the shaft's own top,
 * i.e. how far the crust has eaten into what was a solid column of lava. */
static int lava_shaft_crust_depth(sand_t *p)
{
    int depth = 0;
    for (int y = LAVA_SHAFT_TOP; y <= LAVA_SHAFT_BOT; y++) {
        if (CELL_MATERIAL(sand_at(p, LAVA_SHAFT_X, y)) != MAT_STONE) {
            break;
        }
        depth++;
    }
    return depth;
}

/* A sustained pour reaches past the one cell water can physically touch.
 * Water is re-sand_set() every step - the pour, sustained - directly onto
 * whatever now sits at the shaft's top; with the cool-off chain pinned
 * on, the first quench's own cool_off_chain() carries the freeze several
 * cells deeper in that same event. Pinned OFF, the exact same scene must
 * freeze only the one cell water ever actually touches - which is what
 * proves the extra depth in the first half came from cool_off_chain()
 * and not some other route lava has to cool. */
static void test_a_water_pour_freezes_a_lava_pool_below_its_crust(void)
{
    uint8_t *cells = malloc((size_t)LAVA_SHAFT_W * LAVA_SHAFT_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells, "lava shaft grid must fit in what "
        "the framebuffer leaves");

    sand_t p;
    build_lava_shaft(&p, cells);
    sand_set_lava_cooloff(&p, 255);
    for (int i = 0; i < 5; i++) {
        sand_set(&p, LAVA_SHAFT_X, LAVA_SHAFT_TOP - 1, WATER);
        sand_step(&p, 0, 1000, 0);
    }
    const int poured_depth = lava_shaft_crust_depth(&p);
    free(cells);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, poured_depth,
        "a sustained pour with the cool-off chain pinned on must freeze "
        "the pool below the single cell water actually touches - if this "
        "is 1, cool_off_chain() never chained past the cell neighbor_"
        "quenches() itself converts");

    uint8_t *cells_off = malloc((size_t)LAVA_SHAFT_W * LAVA_SHAFT_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells_off, "lava shaft grid must fit in "
        "what the framebuffer leaves");

    sand_t p_off;
    build_lava_shaft(&p_off, cells_off);
    sand_set_lava_cooloff(&p_off, 0);
    for (int i = 0; i < 5; i++) {
        sand_set(&p_off, LAVA_SHAFT_X, LAVA_SHAFT_TOP - 1, WATER);
        sand_step(&p_off, 0, 1000, 0);
    }
    const int off_depth = lava_shaft_crust_depth(&p_off);
    free(cells_off);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, off_depth,
        "with the cool-off chain pinned OFF, the same sustained pour must "
        "still freeze the one cell water actually touches, and no "
        "further - the baseline quench neither of these two variants "
        "ever disabled");
}

/* The ALWAYS-ON-DRAIN GUARD. A single lava cell walled in by stone on all
 * four sides, no water anywhere on the board, cool-off pinned to its
 * maximum - the same scene test_lava_buried_in_stone_is_not_deleted uses
 * to prove burial does not delete lava, with the one addition that
 * matters here.
 *
 * Stone's own heat_ramp climb (banking one more level, same material, no
 * melt - stone's heats_to is 0, see test_stone_never_melts_however_hot)
 * returns TRUE from try_heat_transform_given() on nearly every step it
 * still has room to climb. Gating cool_off_chain() on that return value
 * ALONE, rather than on CELL_MATERIAL actually changing, would rack up a
 * roll on almost every one of those climbs - an always-on drain that
 * needs no pour and no fuel anywhere in the scene, which would empty this
 * lava cell within the first handful of steps even though nothing here
 * ever touches water. */
static void test_a_lava_pool_in_a_dry_stone_bowl_does_not_freeze_itself(void)
{
    fixture();
    sand_clear(&s);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    sand_set(&s, W / 2, H / 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set_lava_cooloff(&s, 255);
    /* Same reasoning as test_lava_buried_in_stone_is_not_deleted's own
     * addition: this scene is covered enough to be burst-eligible (bd
     * esp32c6-mqt), and this test's job is trigger A/cool_off_chain's own
     * ALWAYS-ON-DRAIN guard, not the unrelated burst roll. */
    sand_set_lava_burst(&s, 0);
    const int before = liquid_mass_of(MAT_LAVA);

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, liquid_mass_of(MAT_LAVA),
        "lava buried in dry stone, with the cool-off chance pinned to its "
        "maximum and no water anywhere on the board, must not freeze "
        "itself at all - the only thing that may ever cost it that chance "
        "is doing the WORK of a genuine melt, and nothing here ever melts");
}

/* THE POSITIVE HALF of the guard above - which only ever proves trigger A
 * did NOT fire, and would pass just as well if it were dead code. Sand's
 * own melt to glass is the MEMORYLESS heats_to/heat_chance form
 * (material.c - sand carries no heat_ramp of its own, unlike stone or
 * glass), so a single successful roll flips CELL_MATERIAL straight from
 * sand to glass with no "banks heat forever, never actually melts"
 * escape hatch to hide behind - the exact, unambiguous kind of WORK
 * trigger A is meant to charge lava for.
 *
 * Same scene, twice, cool-off pinned to the two extremes: pinning it does
 * not gate whether the melt itself happens - conversion is sand's own
 * heat_chance roll, untouched by any of this - only whether that melt
 * then costs lava anything. Pinned high, the lava cell must become
 * stone; pinned off, the identical melt must leave lava as lava. */
static void test_lava_that_melts_sand_into_glass_sometimes_freezes_itself(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, LAVA);
    sand_set(&s, 4, 3, SAND);
    sand_set_lava_cooloff(&s, 255);

    bool melted = false;
    for (int i = 0; i < 500 && !melted; i++) {
        sand_step(&s, 0, 1000, 0);
        melted = CELL_MATERIAL(sand_at(&s, 4, 3)) == MAT_GLASS;
    }

    TEST_ASSERT_TRUE_MESSAGE(melted,
        "setup: sand beside lava must melt to glass within this budget, "
        "or this test never reached the event trigger A exists to charge "
        "for");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "with the cool-off chance pinned to its maximum, lava that just "
        "did the WORK of a genuine melt must pay for it - the lava cell "
        "itself must become stone in that same step, or trigger A is "
        "dead code the negative guard above would never catch on its "
        "own");

    fire_room(3, 4);
    sand_set(&s, 3, 3, LAVA);
    sand_set(&s, 4, 3, SAND);
    sand_set_lava_cooloff(&s, 0);

    melted = false;
    for (int i = 0; i < 500 && !melted; i++) {
        sand_step(&s, 0, 1000, 0);
        melted = CELL_MATERIAL(sand_at(&s, 4, 3)) == MAT_GLASS;
    }

    TEST_ASSERT_TRUE_MESSAGE(melted,
        "setup: sand beside lava must melt to glass within this budget "
        "with the cool-off chance pinned OFF too, or the two halves of "
        "this test are not actually comparable");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "with the cool-off chance pinned to zero, the exact same melt "
        "must cost lava nothing - it must still be lava, not stone");
}

/* THE CHAIN IS BOUNDED. Pinned fully on, against a shaft deep enough that
 * a chain running unbounded would eat the whole thing -
 * SAND_LAVA_COOLOFF_MAX_CHAIN (sand.h) is well under the shaft's 30 lava
 * cells, so retuning the cap leaves a wide, unambiguous margin either way. One quench event only: a single step
 * is enough for cool_off_chain() to run its entire walk, since the chain
 * itself is not spread across steps the way the sustained pour above is.
 *
 * SAND_LAVA_COOLOFF_MAX_CHAIN + 1, not the chain's own cap alone, because
 * the cap only bounds the CHAIN cool_off_chain() itself walks - the one
 * cell neighbor_quenches() converts before ever calling it is not part
 * of that walk, and this test has to count it too. Asserted against the
 * real constant, not a hand-copied literal, so raising the cap cannot
 * leave this silently stale the way a bare `9` would have. */
static void test_the_cool_off_chain_is_bounded(void)
{
    uint8_t *cells = malloc((size_t)LAVA_SHAFT_W * LAVA_SHAFT_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells, "lava shaft grid must fit in what "
        "the framebuffer leaves");

    sand_t p;
    build_lava_shaft(&p, cells);
    sand_set_lava_cooloff(&p, 255);
    sand_set(&p, LAVA_SHAFT_X, LAVA_SHAFT_TOP - 1, WATER);
    sand_step(&p, 0, 1000, 0);
    const int depth = lava_shaft_crust_depth(&p);
    free(cells);

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(SAND_LAVA_COOLOFF_MAX_CHAIN + 1, depth,
        "one quench event, chained fully on, must not convert more than "
        "the quenched cell plus SAND_LAVA_COOLOFF_MAX_CHAIN further links "
        "- an unbounded walk would run to the floor of a shaft this deep");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(LAVA_SHAFT_BOT - LAVA_SHAFT_TOP + 1, depth,
        "the shaft must still hold real, unconverted lava after one "
        "event - if this fails the chain ate the WHOLE pool, which is "
        "the failure mode the cap exists to rule out");
}

/* Lava burns, so it must not quench - the same rule oil needs, arrived
 * at from the other side (oil is fuel, lava is a heat source, and
 * neighbor_quenches() excludes both). */
static void test_lava_does_not_put_fire_out(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, LAVA);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_EMPTY, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "a fire touching LAVA must not be extinguished by it - lava is "
        "a heat source, and a liquid only quenches if it is neither "
        "fuel nor burning itself");
}

/* reaction_t.flare (material.h) exists to look like a heat source licking
 * a flame upward while staying PUT itself - see try_flare()'s own comment
 * (sand_reactions.c) for the mechanic's original, ember-shaped case and
 * why it does the wrong thing, over and over, for a material that
 * actually moves: a poured stream of lava lands as many single-cell
 * grains each free-falling for several steps before settling, and every
 * one of those falling steps used to roll flare exactly as if the grain
 * were a settled pool. Two halves in one test, same grain: it must never
 * flare while genuinely airborne, and must still flare normally once it
 * lands - the falling check is a temporary skip, not a permanent one. */
static void test_falling_lava_does_not_flare(void)
{
    fixture();
    sand_set(&s, 3, 0, LAVA);

    bool found_fire = false;
    for (int i = 0; i < H - 1 && !found_fire; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !found_fire; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_FIRE) {
                    found_fire = true;
                }
            }
        }
    }

    TEST_ASSERT_FALSE_MESSAGE(found_fire,
        "a lava grain in free fall (nothing beneath it, gravity-relative) "
        "must not flare - try_flare() skips the roll entirely while a "
        "non-KIND_STATIC material is still falling, which is what keeps "
        "a long pour from rolling (and, on a hit, spawning a fresh "
        "MAT_FIRE cell for) flare on every single step of its fall");

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_LAVA),
        "setup check: the grain must actually have reached the grid's "
        "own floor (off-grid reads as STONE, sand_at()'s own convention) "
        "by now, or the loop above proved nothing about landing, only "
        "about a fixed number of steps");

    for (int i = 0; i < 200 && !found_fire; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !found_fire; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_FIRE) {
                    found_fire = true;
                }
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_fire,
        "once the same grain has settled on the floor, it must eventually "
        "flare like any other supported lava cell - the falling check "
        "must only ever suppress flare while genuinely airborne, not "
        "disable it for the rest of that cell's life");
}

static void test_steam_bubbles_up_through_standing_water(void)
{
    water_column();
    sand_set(&s, 3, 6, CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1));

    TEST_ASSERT_EQUAL_INT_MESSAGE(6, first_row_holding(MAT_STEAM),
        "setup: the steam must start at the BOTTOM of the water column, "
        "with the full depth of it to climb through");

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const int reached = first_row_holding(MAT_STEAM);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, reached,
        "the steam must still exist - decay is off for this test, so "
        "losing it means it was destroyed rather than moved");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(2, reached,
        "steam under standing water must rise all the way OUT of the "
        "column, not sit where it was made - a gas lighter than the "
        "liquid above it displaces that liquid downward one cell at a "
        "time");
}

/* A bubble swaps two whole cells, so the liquid it shoves aside has to
 * arrive intact - same material, same amount. Nothing here splits mass,
 * which is what makes that guarantee exact rather than approximate, and
 * this pins it: a bubble that quietly rounded a partial cell away would
 * drain a boiler every time one rose. */
static void test_bubbling_conserves_the_water_it_displaces(void)
{
    water_column();
    /* One deliberately PARTIAL cell in the bubble's path, so this would
     * catch a swap that rebuilt the liquid at full mass instead of
     * carrying its own variant nibble across. */
    sand_set(&s, 3, 4, CELL_MAKE(MAT_WATER, 7));
    sand_set(&s, 3, 6, CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1));

    const long before = mass_held_by(MAT_WATER);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, mass_held_by(MAT_WATER),
            "a bubble must never create or destroy water mass on any "
            "step - it is a swap of two whole cells, and the liquid "
            "keeps its own variant nibble as it moves");
    }
}

/* Bubbling is keyed on KIND_GAS and a density comparison, not on steam
 * specifically, so plain gas gets it too - asserted directly so nobody
 * "fixes" try_bubble() into a steam special case later. */
static void test_plain_gas_bubbles_up_through_water_too(void)
{
    water_column();
    sand_set(&s, 3, 6, GAS);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(2, first_row_holding(MAT_GAS),
        "gas is lighter than water too, so it must bubble out of a "
        "column of it exactly the way steam does - try_bubble() tests "
        "kind and density, not material identity");
}

/* The other half of the rule: only LIQUIDS get shoved aside. A gas capped
 * by something solid stays put, or "bubbling" would quietly become a
 * licence to walk through walls. */
static void test_a_bubble_does_not_push_through_a_solid(void)
{
    fixture();
    sand_set_decay(&s, 0);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 3, STONE);
        sand_set(&s, x, 5, STONE);
    }
    sand_set(&s, 3, 4, CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1));

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "the stone ceiling must still be stone - a bubble displaces "
        "liquid only, never a solid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 4)),
        "and the steam must still be under it, not through it");
}

static void test_quenching_makes_steam_but_burning_out_makes_smoke(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, WATER);
    sand_set(&s, 4, 3, FIRE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "a fire put out by water must leave STEAM - water that got hot, "
        "which is exactly what happened");

    /* Same fire, no water anywhere, forced to burn out and forced to
     * smoke: the residue must be the OTHER material. sand_set_decay()
     * at 255 burns it out on the first step it gets; smoke's own 40 in
     * 256 chance is not forced, so this loops until it fires rather
     * than asserting on a single roll. */
    fixture();
    sand_set_decay(&s, 255);
    sand_set_mobility(&s, 0);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, FIRE);
    }

    bool found_smoke = false, found_steam = false;
    for (int i = 0; i < 2 * (MATERIAL_VARIANTS - 1); i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const uint8_t m = CELL_MATERIAL(sand_at(&s, x, y));
                if (m == MAT_SMOKE) found_smoke = true;
                if (m == MAT_STEAM) found_steam = true;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_smoke,
        "fire burning out with no water in the scene must leave SMOKE");
    TEST_ASSERT_FALSE_MESSAGE(found_steam,
        "and must never leave STEAM - there is no water anywhere on this "
        "grid, so a steam cell here would mean the two byproducts have "
        "been collapsed back into one material");
}

static void test_stone_conducts_heat_into_water_beyond_it(void)
{
    fixture();
    sand_set_mobility(&s, 0);   /* keep fire from rising away before it
                                 * gets a turn to conduct - same
                                 * technique used throughout this
                                 * section */
    sand_set_conduction(&s, 255);
    sand_set_boils(&s, 255);   /* deterministic single-step boil once
                                 * conducted heat arrives - see
                                 * reaction_t.boils's own comment for the
                                 * real, much slower figure this
                                 * overrides */
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, STONE);
    /* Floor plus both down-diagonals under the water, not the floor
     * alone - see test_creating_steam_arms_the_gas_pass's own comment
     * for why a partial-mass WATER cell needs all three blocked. */
    sand_set(&s, 4, 4, STONE);
    sand_set(&s, 5, 4, STONE);
    sand_set(&s, 6, 4, STONE);
    sand_set(&s, 5, 3, WATER);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 5, 3)),
        "a fire beside a single cell of stone must boil water sitting "
        "on the OTHER side of that stone - the whole boiler mechanism - "
        "without fire ever crossing the stone itself");
}

static void test_stone_does_not_conduct_fire_into_empty_space(void)
{
    fixture();
    sand_set_mobility(&s, 0);
    sand_set_conduction(&s, 255);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, STONE);
    /* (5, 3) deliberately left empty. */

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 5, 3)),
        "conduction must never create fire in empty space on the far "
        "side of a conductor, even with the roll forced to succeed "
        "every time it is tried - a sealed stone container must stay "
        "sealed");
}

/* Wide enough for an eleven-cell-thick stone wall (matching
 * app_sand.c's own pour brush, POUR_RADIUS 5 with no size control - see
 * conduct_heat()'s own top-of-file comment for why that thickness
 * specifically) plus fire, water and margin, all in one row of `wide`
 * (WIDE_W/WIDE_H, declared above). Builds fire at column 1, a stone
 * wall `wall_len` cells thick starting at column 2, and one water cell
 * just past it - boxes the water (floor plus both down-diagonals, see
 * test_creating_steam_arms_the_gas_pass) so it cannot drain away before
 * conduction gets a look at it, and pins fire with mobility rather than
 * physically boxing it in, since a physical box dense enough to also
 * stop the diagonal slides would make every side of fire denser than
 * fire itself and smother it outright (confirmed: this is what the
 * first version of this helper, walled on every side, actually did).
 * Returns the column the water cell sits at. */
static int build_boiler_room(int wall_len)
{
    /* Mallocs wide_cells fresh on every call and does NOT free it -
     * ownership passes to whichever caller reads `wide` afterward: see
     * each call site below (test_a_thick_wall_still_conducts frees it
     * directly once done; steps_to_boil() frees it itself, since it is
     * the one that wraps a whole call here in a self-contained round
     * trip). */
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "boiler-room grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_mobility(&wide, 0);

    const int y = 2;
    const int fire_x  = 1;
    const int wall_x0 = fire_x + 1;
    const int water_x = wall_x0 + wall_len;

    sand_set(&wide, water_x - 1, y + 1, STONE);
    sand_set(&wide, water_x,     y + 1, STONE);
    sand_set(&wide, water_x + 1, y + 1, STONE);

    sand_set(&wide, fire_x, y, FIRE);
    for (int i = 0; i < wall_len; i++) {
        sand_set(&wide, wall_x0 + i, y, STONE);
    }
    sand_set(&wide, water_x, y, WATER);

    return water_x;
}

static void test_a_thick_wall_still_conducts(void)
{
    /* AFTER build_boiler_room(), not before - it calls sand_init()
     * internally, which would otherwise wipe this override right back
     * to its default. */
    const int water_x = build_boiler_room(11);
    sand_set_conduction(&wide, 255);

    bool boiled = false;
    for (int i = 0; i < 10 && !boiled; i++) {
        sand_step(&wide, 0, 1000, 0);
        boiled = CELL_MATERIAL(sand_at(&wide, water_x, 2)) == MAT_STEAM;
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of wide_cells (via `wide`) are done by this
     * point. */
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(boiled,
        "an eleven-cell-thick stone wall - what app_sand.c's own pour "
        "brush actually draws, not a one-cell idealisation - must still "
        "conduct and eventually boil the water beyond it. A reach of "
        "exactly one cell (an earlier version of this feature) would "
        "make this fail forever: that is the whole reason the walk "
        "attenuates with depth instead of stopping cold at one cell");
}

static void test_conduction_stops_at_the_reach_cap(void)
{
    /* Its own grid, not the shared `wide` one: this test needs a
     * conductor run longer than CONDUCT_REACH, and the cap is now 32,
     * which does not fit across WIDE_W (32). Widening the shared grid
     * instead would have changed the cell count every other test using
     * it draws random numbers over, so this one test gets its own.
     *
     * The wall length here tracks CONDUCT_REACH and has to stay ahead
     * of it: this test asserts the cap EXISTS, not that it sits at any
     * particular depth, so raising the cap means raising this too.
     *
     * Forcing conduction to 255 (AFTER sand_init(), which would
     * otherwise wipe the override) makes every ROLL along the walk
     * succeed, so the only thing left that can stop it is the reach cap
     * itself, which is exactly what this pins down. */
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t *cap_cells = malloc((size_t)CAP_W * CAP_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cap_cells,
        "conduction-reach-cap grid must fit in what the framebuffer "
        "leaves");
    sand_t cap;
    sand_init(&cap, cap_cells, CAP_W, CAP_H, 3u);
    sand_set_mobility(&cap, 0);
    sand_set_conduction(&cap, 255);

    const int y = 2;
    const int wall_x0 = 2;
    const int wall_len = CONDUCT_REACH_TEST + 8;
    const int water_x = wall_x0 + wall_len;

    sand_set(&cap, water_x - 1, y + 1, STONE);
    sand_set(&cap, water_x,     y + 1, STONE);
    sand_set(&cap, water_x + 1, y + 1, STONE);
    sand_set(&cap, 1, y, FIRE);
    for (int i = 0; i < wall_len; i++) {
        sand_set(&cap, wall_x0 + i, y, STONE);
    }
    sand_set(&cap, water_x, y, WATER);

    for (int i = 0; i < 50; i++) {
        sand_step(&cap, 0, 1000, 0);
    }
    const uint8_t result_material = CELL_MATERIAL(sand_at(&cap, water_x, 2));

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(cap_cells);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, result_material,
        "a conductor run longer than CONDUCT_REACH must never conduct "
        "at all, even with every per-cell roll forced to succeed - the "
        "walk has to actually stop at the cap, not merely be unlikely "
        "to reach that far");
}

/* Steps until MAT_STEAM appears past a stone wall `wall_len` cells
 * thick, using the REAL per-material conduction figure (material.c's
 * stone row) rather than an override - or `budget` if it never appears
 * within that many steps. */
static int steps_to_boil(int wall_len, int budget)
{
    const int water_x = build_boiler_room(wall_len);
    int result = budget;
    for (int i = 0; i < budget; i++) {
        sand_step(&wide, 0, 1000, 0);
        if (CELL_MATERIAL(sand_at(&wide, water_x, 2)) == MAT_STEAM) {
            result = i + 1;
            break;
        }
    }
    /* Freed here, not by build_boiler_room() - this wrapper is a whole
     * self-contained round trip, called twice by the test below, each
     * call getting its own fresh grid; see build_boiler_room()'s own
     * comment. */
    free(wide_cells);
    return result;
}

static void test_a_thick_wall_conducts_more_slowly_than_a_thin_one(void)
{
    /* At the real figure (176 in 256, ~0.69/step), a thin wall's
     * cumulative miss probability is negligible within a handful of
     * steps (0.31^10 =~ 9e-6); a thick eleven-cell one needs roughly 41
     * steps on average (0.69^11 =~ 0.024/step) and has real spread
     * around that - so this compares actual step counts rather than
     * asserting a fixed pass/fail line either wall would sometimes
     * cross the wrong way on an unlucky seed. */
    const int budget = 200;
    const int thin  = steps_to_boil(1, budget);
    const int thick = steps_to_boil(11, budget);

    TEST_ASSERT_LESS_THAN_MESSAGE(thick, thin,
        "at the real per-material conduction figure, a thin (1-cell) "
        "stone wall must boil the water beyond it sooner than an "
        "eleven-cell one - thermal resistance falling out of the "
        "attenuating walk itself, not a second constant");
}

/* Boiling happens where the HEAT is, not where the steam wants to end up.
 *
 * This test asserted the exact opposite until bubbling existed, and the
 * reversal is worth keeping visible rather than quietly rewriting.
 * conduct_heat() reaches the bottom cell of the column - the one touching
 * the hot stone - and used to hand it to a boil_surface() walk that
 * climbed against gravity to convert the TOP cell instead. That walk was
 * not decoration: steam made at the bottom of a pool was permanently
 * stuck there (can_enter() only lets a denser mover displace a lighter
 * target, and room_in() will not let water fall into a steam cell
 * either), so boiling anywhere but the surface produced nothing anyone
 * could see.
 *
 * try_bubble() (sand_gas.c) removed that constraint, and with it the
 * only reason to boil anywhere other than the heat source. Boiling the
 * bottom cell now reads the way a real pot does - a column of bubbles
 * climbing from the hot base - instead of steam appearing at the surface
 * from nowhere.
 *
 * sand_set_mobility(&wide, 0) keeps the newly made steam still for the
 * duration, so this measures WHERE the boil happened rather than where
 * the bubble had got to by the time it was inspected. */
static void test_boiling_converts_the_cell_nearest_the_heat(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "boil-nearest-the-heat grid must fit in what the framebuffer "
        "leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_mobility(&wide, 0);

    const int x = 5;
    const int fire_y = 6, stone_y = 5, water_top = 1, water_bottom = 4;

    sand_set(&wide, x, fire_y, FIRE);
    sand_set(&wide, x, stone_y, STONE);
    for (int y = water_top; y <= water_bottom; y++) {
        sand_set(&wide, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    /* Side walls the height of the column, so cross-flow has nowhere to
     * send any mass and the column stays exactly this shape while
     * conduction does its work. */
    for (int y = water_top; y <= fire_y; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    bool boiled = false;
    for (int i = 0; i < 10 && !boiled; i++) {
        sand_step(&wide, 0, 1000, 0);
        boiled = CELL_MATERIAL(sand_at(&wide, x, water_bottom)) == MAT_STEAM;
    }
    const uint8_t surface_material = CELL_MATERIAL(sand_at(&wide, x, water_top));

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of wide_cells (via `wide`) are done by this
     * point. */
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(boiled,
        "the cell nearest the stone - the one conduct_heat() actually "
        "reaches - must be the one that boils");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, surface_material,
        "and the surface must still be plain water at that moment - "
        "boiling happens at the heat source now, and the steam makes "
        "its own way up by bubbling rather than being conjured at the "
        "top of the column. A surface cell boiling first would mean the "
        "old against-gravity walk had come back");
}

/* reaction_t.boils gates conduct_heat()'s conversion above behind a
 * second roll, so a liquid can resist conducted-heat boiling instead of
 * flashing to steam the instant heat reaches it - see this field's own
 * comment in material.h. sand_set_boils(0) must be able to disable that
 * conversion completely, the same override discipline every other
 * chance field in this file already gives (sand_set_conduction(0) seals
 * a wall shut, sand_set_lava_burst(0) keeps a covered pool as lava
 * forever, ...) - proof this is a real second condition and not
 * decoration. */
static void test_sand_set_boils_zero_disables_conducted_heat_boiling(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "boils-disabled grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_boils(&wide, 0);
    sand_set_mobility(&wide, 0);

    const int x = 5;
    const int fire_y = 6, stone_y = 5, water_y = 4;

    sand_set(&wide, x, fire_y, FIRE);
    sand_set(&wide, x, stone_y, STONE);
    sand_set(&wide, x, water_y, CELL_MAKE(MAT_WATER, MASS_MAX));
    for (int y = water_y; y <= fire_y; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    for (int i = 0; i < 50; i++) {
        sand_step(&wide, 0, 1000, 0);
    }
    const uint8_t result_material = CELL_MATERIAL(sand_at(&wide, x, water_y));

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(wide_cells);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, result_material,
        "sand_set_boils(0) must stop conducted heat from ever boiling a "
        "liquid, even with conduction itself forced to 255 and fifty "
        "steps to try");
}

/* And boiling a liquid that DOES pass its `boils` roll must hand the new
 * steam a FULL cell's own worth of life, the same place_reacted() default
 * any other fresh, non-ramping material gets - see conduct_heat()'s own
 * comment (sand_reactions.c) for the account. This used to assert the
 * opposite (a deliberately shortened life, so a boiler visibly made less
 * steam to look at); asked to make steam last LONGER instead, the cut was
 * removed rather than tuned, so this test now checks the plain
 * place_reacted() guarantee holds for boiling too. */
static void test_boiled_steam_starts_at_full_life(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "boiled-steam-life grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_boils(&wide, 255);
    sand_set_mobility(&wide, 0);

    const int x = 5;
    const int fire_y = 6, stone_y = 5, water_y = 4;

    sand_set(&wide, x, fire_y, FIRE);
    sand_set(&wide, x, stone_y, STONE);
    sand_set(&wide, x, water_y, CELL_MAKE(MAT_WATER, MASS_MAX));
    for (int y = water_y; y <= fire_y; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    sand_step(&wide, 0, 1000, 0);

    const cell_t c = sand_at(&wide, x, water_y);

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. `c` was read out above, so this is safe. */
    free(wide_cells);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(c),
        "sand_set_boils(255) must still boil deterministically in one "
        "step, exactly as boiling always has");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1, CELL_VARIANT(c),
        "boiling must hand the new steam cell a full cell's worth of "
        "life, same as place_reacted() gives any other fresh material - "
        "asked to make steam last longer, not shorter");
}

/* conduct_heat()'s boiling branch used to hand every liquid MAT_STEAM
 * regardless of which one was actually boiling - acid conducted through
 * a wall came out as the same white kettle-steam water does, instead of
 * the MAT_GAS acid produces everywhere else it evaporates (`evaporates`,
 * `fizz` - see reaction_t.boils_to's own comment in material.h). Same
 * scene as test_boiled_steam_starts_at_full_life just above, water
 * swapped for acid, MAT_STEAM swapped for MAT_GAS. */
static void test_boiling_acid_produces_gas_not_steam(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "boiling-acid-produces-gas grid must fit in what the framebuffer "
        "leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_boils(&wide, 255);
    sand_set_mobility(&wide, 0);

    const int x = 5;
    const int fire_y = 6, stone_y = 5, acid_y = 4;

    sand_set(&wide, x, fire_y, FIRE);
    sand_set(&wide, x, stone_y, STONE);
    sand_set(&wide, x, acid_y, CELL_MAKE(MAT_ACID, MASS_MAX));
    for (int y = acid_y; y <= fire_y; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    sand_step(&wide, 0, 1000, 0);
    const uint8_t result_material = CELL_MATERIAL(sand_at(&wide, x, acid_y));

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(wide_cells);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, result_material,
        "conducted heat boiling acid must leave MAT_GAS behind, the same "
        "byproduct acid leaves everywhere else it evaporates - not "
        "MAT_STEAM, which is water's own byproduct");
}

static void test_the_boiler_end_to_end(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "boiler end-to-end grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);

    const int x = 10;
    const int wood_y    = 19;
    const int slab_top  = 8, slab_bottom = 18;   /* 11 rows thick,
                                                  * matching the pour
                                                  * brush's real
                                                  * thickness */
    const int water_top = 5, water_bottom = 7;   /* 3 cells of water */

    sand_set(&wide, x, wood_y, WOOD);
    for (int y = slab_top; y <= slab_bottom; y++) {
        sand_set(&wide, x, y, STONE);
    }
    for (int y = water_top; y <= water_bottom; y++) {
        sand_set(&wide, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    /* Side walls the height of the water and the slab (not wood's own
     * row - the ignition spark below needs an open cell beside the
     * wood, and wood is KIND_STATIC so it needs no walls to stay put
     * regardless), so nothing drains or drifts sideways out of the
     * column while the boiler does its work. */
    for (int y = water_top; y <= slab_bottom; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    const long water_before = mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER);

    /* Light it: a forced-certain spark, pinned in place just long
     * enough to catch - mirrors wood_ignition_room()'s reasoning above,
     * except the fuel here is KIND_STATIC and never drifts itself, only
     * the spark that lights it could.
     *
     * sand_set_mobility(&s, 0) alone is NOT enough here, and it is worth
     * knowing why: it only blocks sub-pass 1 of sand_step_gas() (the
     * rise/diagonal-slide attempt), not sub-pass 2 (equalise_gas()'s
     * sideways spread), which is gated on has_room_above() - "is
     * there room to rise" - not on mobility at all. The spark's own row
     * sits one below the stone slab's bottom, so the cell directly
     * above it is real stone, not open sky: has_room_above() correctly
     * reports false, so equalise_gas() does NOT defer to sub-pass 1 the
     * way it would with open sky above (see
     * test_creating_steam_arms_the_gas_pass, where that deferral is
     * exactly what pins fire) - it goes ahead and looks sideways
     * instead, and an open cell beside the spark is exactly what it
     * would use to drift away before ever touching the wood (confirmed:
     * this is what the first version of this test, without the block
     * below, actually did). Blocking that one remaining open side is
     * what actually pins it: every neighbour is then either denser
     * stone, wood, or off the grid (which never counts, win or lose -
     * see neighbor_smothers()), so neither sub-pass has anywhere left
     * to send it. */
    sand_set(&wide, x - 2, wood_y, STONE);
    sand_set_flammability(&wide, 255);
    sand_set_mobility(&wide, 0);
    sand_set(&wide, x - 1, wood_y, FIRE);
    sand_step(&wide, 0, 1000, 0);

    TEST_ASSERT_TRUE_MESSAGE(cell_is_burning(sand_at(&wide, x, wood_y)),
        "setup: the wood must have caught and charred into an ember");

    /* Back to realistic behaviour for the boiler itself - the whole
     * point of this test is the REAL per-material conduction figure
     * (material.c's stone row) working through a slab as thick as the
     * app's own pour brush actually draws, not a forced one. */
    sand_set_flammability(&wide, SAND_FLAMMABILITY_PER_MATERIAL);
    sand_set_mobility(&wide, SAND_MOBILITY_PER_MATERIAL);

    for (int i = 0; i < 300; i++) {
        sand_step(&wide, 0, 1000, 0);
    }

    bool steam_above_basin = false;
    for (int y = 0; y < water_top && !steam_above_basin; y++) {
        for (int x2 = 0; x2 < WIDE_W; x2++) {
            if (CELL_MATERIAL(sand_at(&wide, x2, y)) == MAT_STEAM) {
                steam_above_basin = true;
                break;
            }
        }
    }

    const long water_after = mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER);

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of wide_cells (via `wide`) are done by this
     * point. */
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(steam_above_basin,
        "the boiler must eventually produce steam that rises above "
        "where the water started - wood catching, charring into an "
        "ember, conducting heat through an eleven-cell stone slab, and "
        "boiling the water on the other side, all through the real "
        "per-material figures with nothing forced. This is the "
        "feature; if it does not pass, nothing else in this section "
        "matters");
    TEST_ASSERT_LESS_THAN_MESSAGE(water_before, water_after,
        "the water level must have dropped - the boiler consumed at "
        "least one cell's worth of it");
}

/* ===================================================================
 * Metal: dirt smelted by sustained heat - see
 * docs/Sand/Metal-Smelting-Plan.md, which every test below follows.
 * =================================================================== */

/* Mirrors sand_reactions.c's own HEAT_FLAW_CLUMP, which is private to
 * that file - same risk CONDUCT_REACH_TEST above already carries: if the
 * two drift apart, the clumping assertions below stop proving anything,
 * so keep them together. */
#define HEAT_FLAW_CLUMP_TEST 5

/* A single lava cell boxed on three sides by stone, open only towards a
 * single dirt cell beside it - the smallest scene that puts lava and
 * dirt in direct contact without the lava draining away to level itself
 * (can_enter() needs a strictly denser neighbour to move into, and both
 * stone and dirt are denser than lava - see MAT_LAVA's own density
 * comment in material.c). A floor across the whole width means neither
 * powder cell has anywhere to fall. */
static void lava_beside_dirt(uint8_t moisture)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);          /* boxes the lava on its left */
    sand_set(&s, 3, H - 3, STONE);          /* and above */
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set(&s, 4, H - 2, CELL_SOIL(MAT_DIRT, 1, moisture));
}

/* Whether the dirt cell in lava_beside_dirt()'s scene has smelted into
 * metal specifically. */
static bool dirt_cell_is_metal(void)
{
    const cell_t c = sand_at(&s, 4, H - 2);
    return cell_is_extended(c) && CELL_VARIANT(c) == MATX_METAL;
}

/* Whether it has smelted at all - metal OR stone, reaction_t.flaw_to's
 * two possible dry-path outcomes (material.h). A bone-dry cell has no
 * moisture to spoil, so these are the only two a dry smelt can reach. */
static bool dirt_cell_is_smelted(void)
{
    const cell_t c = sand_at(&s, 4, H - 2);
    return dirt_cell_is_metal() || CELL_MATERIAL(c) == MAT_STONE;
}

/* Whether it is no longer dirt at all - smelted (metal or stone) OR
 * spoiled (sand, reaction_t.spoils_to). The generic "this cell has
 * resolved, whichever of the three ways" check the wet-earth tests below
 * want: they exist to pin down the MOISTURE sequencing, not which of the
 * now three possible outcomes one particular seed happens to land on. */
static bool dirt_cell_resolved(void)
{
    return CELL_MATERIAL(sand_at(&s, 4, H - 2)) != MAT_DIRT;
}

/* How many cells anywhere on the board have smelted, metal OR stone - for
 * scenes below that scatter dirt across a row rather than pinning it to
 * lava_beside_dirt()'s one fixed cell. See dirt_cell_is_smelted()'s own
 * comment for why a dry smelt can land on either. */
static int count_smelted_cells(void)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if ((cell_is_extended(c) && CELL_VARIANT(c) == MATX_METAL) ||
                CELL_MATERIAL(c) == MAT_STONE) {
                n++;
            }
        }
    }
    return n;
}

/* Steps until lava_beside_dirt()'s dirt cell smelts (metal or stone), or
 * `budget` if it never does within that many steps. */
static int steps_to_smelt(uint8_t moisture, int budget)
{
    lava_beside_dirt(moisture);
    for (int i = 0; i < budget; i++) {
        sand_step(&s, 0, 1000, 0);
        if (dirt_cell_is_smelted()) {
            return i + 1;
        }
    }
    return budget;
}

static void test_dry_dirt_beside_lava_smelts_into_metal_or_stone(void)
{
    const int budget = 3000;
    const int steps = steps_to_smelt(0, budget);

    TEST_ASSERT_LESS_THAN_MESSAGE(budget, steps,
        "dirt with no moisture in it, held against lava, must smelt into "
        "metal or stone - the one reaction lava and dirt have, and the "
        "whole reason MATX_METAL exists. Which of the two is "
        "reaction_t.flaw_to's call (material.h); either counts here");
}

/* Saturated dirt needs SOIL_MOISTURE_MAX successful moisture-lowering
 * events to reach bone dry, one level at a time, before a further
 * success can ever convert the cell - see the wet-earth branch of
 * try_heat_transform() (sand_reactions.c). That holds regardless of
 * WHICH mechanism drives any one level off - the wet stage of the new
 * branch (visible as steam) or dirt's own ordinary ambient drying
 * (reaction_t.dries, ambient and silent, ticking independently of any
 * heat source) - because both only ever remove one level at a time and
 * the cell cannot smelt while any is left. So this counts the number of
 * DISTINCT moisture values the cell passes through, deterministically,
 * rather than comparing wall-clock step counts against bone-dry dirt -
 * which is a race against two independent RNG-driven rates and was
 * measured to occasionally land either side of even a generous margin.
 *
 * reaction_t.spoils_to (material.h) means the cell can now also leave
 * this process early by spoiling to sand rather than drying all the way
 * to metal or stone - see the branch below on dirt_cell_is_smelted() for
 * how the two paths get different bounds. */
static void test_saturated_dirt_smelts_roughly_eight_times_slower(void)
{
    lava_beside_dirt(SOIL_MOISTURE_MAX);

    int distinct_moisture_levels_seen = 1;   /* SOIL_MOISTURE_MAX itself */
    int last_moisture = SOIL_MOISTURE_MAX;
    bool resolved = false;
    for (int i = 0; i < 8000 && !resolved; i++) {
        sand_step(&s, 0, 1000, 0);
        /* dirt_cell_resolved(), not dirt_cell_is_metal() - reaction_t.
         * spoils_to (material.h) means a wet cell can now crack to sand
         * partway through drying instead of ever reaching metal or stone.
         * This test is about the MOISTURE sequence, not the destination,
         * so any of the three ways off MAT_DIRT counts as done. */
        resolved = dirt_cell_resolved();
        if (!resolved) {
            const int m = CELL_MOISTURE(sand_at(&s, 4, H - 2));
            if (m != last_moisture) {
                distinct_moisture_levels_seen++;
                last_moisture = m;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(resolved,
        "fixture check: saturated dirt must eventually resolve - smelt "
        "into metal or stone, or spoil into sand - within the budget");

    if (dirt_cell_is_smelted()) {
        /* SOIL_MOISTURE_MAX, not +1: sampled once per wall-clock step, so
         * the rare step where BOTH mechanisms roll a success at once
         * (heat and ambient drying, independently gated) merges two
         * adjacent moisture values into a single observation - measured
         * to happen on this seed. The bound stays tight enough to
         * distinguish "passed through nearly every level" from
         * "converted in a handful of events", which is the property this
         * test exists to pin down.
         *
         * Only checked on the SMELTED path. A cell that spoils instead
         * (reaction_t.spoils_to, material.h) can leave off partway
         * through drying by design - see spoils_chance's own comment for
         * why that is a real risk, not a bug - so the "very nearly every
         * level" claim is specifically a claim about reaching metal or
         * stone, not about resolving in general. */
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SOIL_MOISTURE_MAX,
            distinct_moisture_levels_seen,
            "saturated dirt that smelts (rather than spoiling) must pass "
            "through very nearly every one of its SOIL_MOISTURE_MAX + 1 "
            "moisture values - SOIL_MOISTURE_MAX itself down to bone dry "
            "- one level at a time, before it can smelt at all. That is "
            "what makes it roughly SOIL_MOISTURE_MAX + 1 times as much "
            "work as bone-dry dirt's single conversion");
    } else {
        /* Spoiled instead - and, at spoils_chance 235/256, the LIKELY path
         * for this whole test now (rebalanced 2026-08-31 specifically so
         * wet dirt reaching metal or stone at all is the rare outcome).
         * No lower bound to assert here any more: spoils_chance is
         * unconditional (see its own comment in material.h for why an
         * earlier "spare the first roll" gate could not actually be made
         * to work), so a cell can spoil on the very first successful
         * heat_chance roll it ever gets, with distinct_moisture_levels_
         * seen staying at 1 - that is expected, not a bug to bound
         * against. This branch exists so the test does not silently stop
         * meaning anything once spoiling became the common case; there is
         * nothing left to assert on this path beyond "it resolved at
         * all", already checked above. */
    }
}

/* A FULL tank of moisture rather than a single level - originally so
 * ambient drying (reaction_t.dries, ticking independently of any heat
 * source) winning every one of SOIL_MOISTURE_MAX levels before heat ever
 * won one would be vanishingly unlikely, guaranteeing steam eventually
 * appeared. That guarantee is GONE since spoils_chance's 2026-08-31
 * rebalances: at 235/256, a saturated cell now has roughly a 92% chance of
 * spoiling straight to sand on the very FIRST successful heat_chance roll
 * it ever gets - unconditional, no "spare the first roll" gate any more
 * (see spoils_chance's own comment in material.h for why that gate could
 * never really be made to work). So for ONE cell, steam appearing at all
 * is now the MINORITY outcome, not a near-certainty - test_dry_dirt_
 * flaws_into_stone_at_least_sometimes's sibling test proves the STEAM path
 * still exists at all, from a sample large enough that chance is not a
 * factor; this test keeps only the ordering claim that is STILL true
 * whenever steam does happen: it cannot ever land on the same step this
 * cell resolves (spoiling and steaming-while-draining are mutually
 * exclusive outcomes of the same roll - see try_heat_transform()), and it
 * cannot happen on any step after resolution either, since a resolved
 * cell has left MAT_DIRT and this branch never runs on it again. */
static void test_watered_dirt_steaming_precedes_resolving_when_it_happens(void)
{
    lava_beside_dirt(SOIL_MOISTURE_MAX);

    int steamed_at = -1, resolved_at = -1;
    for (int i = 0; i < 4000 && resolved_at < 0; i++) {
        sand_step(&s, 0, 1000, 0);
        if (steamed_at < 0 && count_cells_of(MAT_STEAM) > 0) {
            steamed_at = i;
        }
        /* dirt_cell_resolved(), not dirt_cell_is_metal() - see
         * test_saturated_dirt_smelts_roughly_eight_times_slower's own
         * comment on why: reaction_t.spoils_to means resolving no longer
         * always means reaching metal or stone. */
        if (dirt_cell_resolved()) {
            resolved_at = i;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(resolved_at >= 0,
        "fixture check: it must eventually resolve");
    if (steamed_at < 0) {
        return;   /* did not steam this run - now the expected majority
                   * outcome at spoils_chance 235/256, and there is nothing
                   * left to assert an ORDER over */
    }
    /* Structurally guaranteed whenever steam DOES appear (see this
     * function's own top comment) - resolving strictly later, whichever
     * of the three ways this cell eventually resolves. */
    TEST_ASSERT_LESS_THAN_MESSAGE(resolved_at, steamed_at,
        "when watered dirt against lava does steam, that must happen "
        "strictly BEFORE the cell resolves - spoiling and steaming are "
        "mutually exclusive outcomes of the same roll, and a resolved "
        "cell is no longer dirt so it can never steam again afterward");
}

/* The steam path itself still exists at all - not dead code the previous
 * test can no longer exercise reliably. At spoils_chance 235/256
 * unconditional, a single saturated cell steams before it resolves only
 * on the roughly 8% of first rolls that do NOT immediately spoil (see
 * the sequencing test just above for the full reasoning), so a single-
 * cell scene is now the wrong tool to prove the path is live at all -
 * exactly the same shape of problem test_dry_dirt_smelting_reaches_both_
 * metal_and_stone solved for the now-rare metal case. STEAM_TEST_PODS
 * independent saturated pockets, each in a lava_beside_dirt()-shaped box
 * on one dedicated wide grid (SPOILS_TEST_PODS's shared `wide` above is
 * far too narrow to hold this many): per pod, P(never steams before
 * resolving) is the chance its very first roll spoils outright,
 * spoils_chance/256 ~= 0.918, so P(NONE of STEAM_TEST_PODS pods ever
 * steam) is 0.918^STEAM_TEST_PODS - with 200 pods that is on the order of
 * 1 in 27 million, vanishingly small regardless of seed. */
#define STEAM_TEST_PODS 200
#define STEAM_TEST_SPACING 4
#define STEAM_TEST_W (2 + STEAM_TEST_SPACING * STEAM_TEST_PODS + 2)
#define STEAM_TEST_H 6
static void test_wet_dirt_can_still_steam_before_spoiling_at_least_sometimes(void)
{
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t *steam_cells = malloc((size_t)STEAM_TEST_W * STEAM_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(steam_cells,
        "wet-dirt steam-pods grid must fit in what the framebuffer leaves");
    sand_t st;
    sand_init(&st, steam_cells, STEAM_TEST_W, STEAM_TEST_H, 3u);
    sand_set_mobility(&st, 0);

    const int y = 2;
    for (int x = 0; x < STEAM_TEST_W; x++) {
        sand_set(&st, x, y + 1, STONE);        /* one shared floor */
    }
    for (int k = 0; k < STEAM_TEST_PODS; k++) {
        const int lava_x = 2 + STEAM_TEST_SPACING * k;
        sand_set(&st, lava_x - 1, y, STONE);
        sand_set(&st, lava_x, y - 1, STONE);
        sand_set(&st, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        sand_set(&st, lava_x + 1, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }

    /* Existence only - not per-pod sequencing. Steam is KIND_GAS and
     * rises/drifts once emitted (sand_step_gas()), so pinning WHICH pod a
     * given steam cell came from would mean fighting the same dispersal
     * the simulation is supposed to do; a single board-wide count is
     * immune to that because it does not care which pod produced it,
     * only that at least one did.
     *
     * NOT count_cells_of() - that helper is hardcoded to the shared
     * fixture's `s`/`W`/`H` globals (see its own definition), not
     * whichever sand_t is passed to sand_step() - it would silently
     * count cells on a completely unrelated grid here. Scanned inline
     * against `st`/STEAM_TEST_W/STEAM_TEST_H instead. */
    bool steamed_any = false;
    for (int i = 0; i < 8000 && !steamed_any; i++) {
        sand_step(&st, 0, 1000, 0);
        for (int y = 0; y < STEAM_TEST_H && !steamed_any; y++) {
            for (int x = 0; x < STEAM_TEST_W && !steamed_any; x++) {
                steamed_any = CELL_MATERIAL(sand_at(&st, x, y)) == MAT_STEAM;
            }
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(steam_cells);

    TEST_ASSERT_TRUE_MESSAGE(steamed_any,
        "at least one of many saturated dirt cells against lava must "
        "still steam before spoiling - the drain-then-steam path in "
        "try_heat_transform() must still be reachable even though "
        "spoils_chance 235/256 makes it the minority outcome; if this "
        "never fires across STEAM_TEST_PODS independent attempts, either "
        "spoils_chance regressed to 255 (unconditional, path dead) or the "
        "steam emit itself broke");
}

/* reaction_t.spoils_to (material.h) actually fires, rather than just being
 * a field nobody reaches: many INDEPENDENT saturated-dirt-beside-lava
 * pockets side by side, each the same shape as lava_beside_dirt()'s one
 * cell, run until every one of them has resolved. Written when
 * spoils_chance was still 24/256 (~9%), where a single cell spoiling was
 * unlikely enough to need padding against; two rebalances later it sits at
 * 235/256 (~92%, unconditional - no gate any more, see that field's own
 * comment in material.h) and a single pod would already be enough on its
 * own, but PODS pockets costs nothing extra and keeps this test's
 * confidence independent of which exact cell it happens to be. */
#define SPOILS_TEST_PODS 6
static void test_wet_dirt_can_spoil_into_sand_instead_of_smelting(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "wet-dirt-spoils grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_mobility(&wide, 0);

    const int y = 2;
    for (int x = 0; x < WIDE_W; x++) {
        sand_set(&wide, x, y + 1, STONE);     /* one shared floor */
    }
    for (int k = 0; k < SPOILS_TEST_PODS; k++) {
        const int lava_x = 2 + 4 * k;
        sand_set(&wide, lava_x - 1, y, STONE);
        sand_set(&wide, lava_x, y - 1, STONE);
        sand_set(&wide, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        sand_set(&wide, lava_x + 1, y,
                 CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }

    bool spoiled = false;
    for (int i = 0; i < 8000 && !spoiled; i++) {
        sand_step(&wide, 0, 1000, 0);
        for (int k = 0; k < SPOILS_TEST_PODS && !spoiled; k++) {
            const int lava_x = 2 + 4 * k;
            spoiled = CELL_MATERIAL(sand_at(&wide, lava_x + 1, y)) == MAT_SAND;
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(spoiled,
        "at least one of several saturated dirt cells against lava must "
        "spoil into sand instead of smelting - reaction_t.spoils_to "
        "(material.h) exists precisely so watering ore before it fires is "
        "a real risk; if this never fires, spoils_to/spoils_chance "
        "regressed to zero or the gate is wrong");
}

/* reaction_t.flaw_to (material.h) fires AT ALL, and so does its complement
 * - metal itself - from a sample large enough that chance is not a factor
 * either way. See test_the_rod_terminates_at_conduct_reach_not_the_far_
 * wall's own comment for why a single ~30-cell rod is NOT big enough to
 * make either claim reliably (the clump mechanism only rerolls once every
 * HEAT_FLAW_CLUMP_TEST triggers, so a rod that long gets only ~6
 * independent rerolls).
 *
 * Both directions need their own proof now, not just flaw_to's: the second
 * 2026-08-31 rebalance moved flaw_chance to 220/256 (~86%) specifically to
 * make METAL the rare outcome, which means "does metal still ever happen
 * at all" is now exactly as real a question as "does stone" was when
 * metal was still the default.
 *
 * FLAW_TEST_PODS independent bone-dry dirt cells, each in its own
 * lava_beside_dirt()-shaped box, laid out in one dedicated wide grid
 * (SPOILS_TEST_PODS's shared `wide` is far too narrow to hold this many).
 * At HEAT_FLAW_CLUMP_TEST 5, this many pods gives FLAW_TEST_PODS /
 * HEAT_FLAW_CLUMP_TEST independent reroll opportunities; with 400 pods
 * that is 80 of them. The chance NONE of the 80 ever flaws is
 * (1 - 220/256)^80, and the chance ALL 80 flaw (never leaving room for a
 * metal cell) is (220/256)^80 - both vanishingly small regardless of seed,
 * so both a total absence of stone and a total absence of metal would be
 * a real regression, not bad luck. Runs the full budget rather than
 * exiting on the first stone sighting (unlike the old flaw-only version of
 * this test) precisely because stone is now the FAST, common outcome and
 * metal the slow, rare one - stopping early would answer the easy question
 * and never even look for the hard one. */
#define FLAW_TEST_PODS 400
#define FLAW_TEST_SPACING 4
#define FLAW_TEST_W (2 + FLAW_TEST_SPACING * FLAW_TEST_PODS + 2)
#define FLAW_TEST_H 6
static void test_dry_dirt_smelting_reaches_both_metal_and_stone(void)
{
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t *flaw_cells = malloc((size_t)FLAW_TEST_W * FLAW_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(flaw_cells,
        "dry-dirt flaw-pods grid must fit in what the framebuffer leaves");
    sand_t flaw;
    sand_init(&flaw, flaw_cells, FLAW_TEST_W, FLAW_TEST_H, 3u);
    sand_set_mobility(&flaw, 0);

    const int y = 2;
    for (int x = 0; x < FLAW_TEST_W; x++) {
        sand_set(&flaw, x, y + 1, STONE);     /* one shared floor */
    }
    for (int k = 0; k < FLAW_TEST_PODS; k++) {
        const int lava_x = 2 + FLAW_TEST_SPACING * k;
        sand_set(&flaw, lava_x - 1, y, STONE);
        sand_set(&flaw, lava_x, y - 1, STONE);
        sand_set(&flaw, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        sand_set(&flaw, lava_x + 1, y, CELL_SOIL(MAT_DIRT, 1, 0)); /* dry */
    }

    for (int i = 0; i < 6000; i++) {
        sand_step(&flaw, 0, 1000, 0);
    }

    int stone_count = 0, metal_count = 0;
    for (int k = 0; k < FLAW_TEST_PODS; k++) {
        const int lava_x = 2 + FLAW_TEST_SPACING * k;
        const cell_t c = sand_at(&flaw, lava_x + 1, y);
        if (CELL_MATERIAL(c) == MAT_STONE) {
            stone_count++;
        } else if (cell_is_extended(c) && CELL_VARIANT(c) == MATX_METAL) {
            metal_count++;
        }
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of flaw_cells are done by this point. */
    free(flaw_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, stone_count,
        "at least one of many bone-dry dirt cells against lava must come "
        "out as stone instead of metal - reaction_t.flaw_to/flaw_chance "
        "(material.h) exists precisely so a smelt is not a guaranteed "
        "clean bar; if this never fires across FLAW_TEST_PODS independent "
        "attempts, flaw_to/flaw_chance regressed to zero");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, metal_count,
        "at least one of many bone-dry dirt cells against lava must still "
        "come out as metal - flaw_chance 220/256 makes it the RARE "
        "outcome, not an impossible one; if this never fires across "
        "FLAW_TEST_PODS independent attempts, flaw_chance is effectively "
        "255 (metal can no longer exist) rather than merely high");
}

/* Every smelting test above this one holds a POOL OF LAVA against the
 * dirt. That proves lava and dirt have the reaction; it says nothing
 * about whether the reaction is keyed on being LAVA or on being hot and
 * alight, because try_heat_transform() (sand_reactions.c) is reached from
 * any burning neighbour - cell_is_burning() gates it, checking
 * reaction_t.burns, not CELL_MATERIAL(n) == MAT_LAVA. An ordinary flame
 * is the cheapest thing that also satisfies cell_is_burning(), and the
 * whole point of this test is that swapping it in for lava changes
 * nothing about the outcome.
 *
 * If a future change narrowed dirt's smelting path to check for lava
 * specifically - or for anything else lava has that a plain flame does
 * not - this is the test that would catch it while every lava-based test
 * above kept passing right through the regression.
 *
 * Fire rises and burns itself out in around forty steps
 * (material_by_id((material_id_t)MAT_FIRE)->decay), so exactly like
 * test_a_fire_held_long_enough_melts_glass_to_lava it has to be
 * re-placed every step rather than dropped once and left unattended. */
static void test_a_held_flame_smelts_dirt_as_lava_does(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 4, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));   /* bone dry */

    const int budget = 3000;
    int smelted = 0;
    for (int i = 0; i < budget && !smelted; i++) {
        if (CELL_IS_EMPTY(sand_at(&s, 4, H - 3))) {
            sand_set(&s, 4, H - 3, FIRE);
        }
        sand_step(&s, 0, 1000, 0);
        smelted = count_smelted_cells() > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(smelted,
        "an ordinary flame held against dirt must smelt it into metal or "
        "stone exactly as lava does - the reaction is keyed on "
        "cell_is_burning(), not on CELL_MATERIAL(n) == MAT_LAVA, and "
        "every other smelting test in this file only ever reaches for "
        "lava as its heat source");
}

/* And the conducted path is just as general as the contact path: dirt on
 * the far side of a plain stone wall smelts from heat that crossed the
 * wall via conduct_heat(), not by touching the fire that is driving it.
 *
 * test_the_rod_terminates_at_conduct_reach_not_the_far_wall (below)
 * already proves the far-side hit works for a METAL conductor, but that
 * scene only exists because a metal rod grows itself one smelted cell at
 * a time - it never demonstrates the far-side hit landing through an
 * ordinary conductor that was not itself produced by smelting. This is
 * the plain-stone-wall version test_heat_through_a_pan_lights_oil_rather
 * _than_boiling_it already is for oil, one section up: fire heats a
 * stone slab, and dirt sitting on the far side of that slab - never
 * touching the fire - smelts from the heat that walked through the
 * stone.
 *
 * Real per-material `conducts` applies (sand_set_conduction(&s,
 * SAND_CONDUCTION_PER_MATERIAL) - see that test's own comment on why this
 * one wants the same call rather than a forced 255), so this is stone's
 * actual 220-in-256 figure attenuating across a one-cell-thick wall, not
 * a tuned-up test double. */
static void test_heat_through_a_stone_wall_smelts_the_dirt_beyond_it(void)
{
    fixture();
    sand_clear(&s);
    sand_set_conduction(&s, SAND_CONDUCTION_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 3, STONE);             /* the wall */
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 4, CELL_SOIL(MAT_DIRT, 1, 0));   /* bone dry,
                                                               * far side */
    }

    const int budget = 6000;
    int smelted = 0;
    for (int i = 0; i < budget && !smelted; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        smelted = count_smelted_cells() > 0;

        /* The wall must stay exactly what it was every single step - not
         * just at the end - so a geometry slip that let the fire reach it
         * directly (which would consume or convert it) cannot pass on a
         * lucky final frame. This is also what proves no dirt cell was
         * ever in direct contact with the fire: the fire is only ever
         * placed at H - 2, and every cell of H - 3 (the only row between
         * the fire and the dirt) staying MAT_STONE means dirt's one
         * downward neighbour was stone on every step, never flame. */
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE,
                CELL_MATERIAL(sand_at(&s, x, H - 3)),
                "the wall must stay intact and unlit - if it changes, "
                "either the fire reached it directly or the far-side hit "
                "is landing on the conductor instead of past it, and "
                "either way this test can no longer tell a conducted "
                "smelt from a contact one");
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(smelted,
        "dirt behind a plain stone wall must smelt into metal or stone "
        "from conducted heat alone - conduct_heat() (sand_reactions.c) "
        "applies "
        "try_heat_transform() to whatever it finds past the far side of "
        "a conductor run exactly as contact does, and that path has "
        "otherwise only ever been proven for a metal conductor grown by "
        "smelting itself, never for an ordinary wall");
}

/* Regression guard for the wet-dirt branch just added to
 * try_heat_transform(): sand has no `dries` at all, so `r->dries != 0`
 * must gate the new branch out entirely and sand -> glass must be
 * completely unaffected by it - see material.h's own comment on `dries`
 * for why that field, and not a new one, is what the branch tests. */
static void test_sand_still_becomes_glass_beside_the_new_dirt_branch(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }

    int made = 0;
    for (int i = 0; i < 2000 && !made; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        made = count_cells_of(MAT_GLASS) > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(made,
        "sand -> glass must still work after the wet-dirt branch was "
        "added to try_heat_transform() - sand has no `dries`, so the new "
        "branch must never catch it");
}

/* Steps until MAT_STEAM appears past a `wall_len`-cell wall of
 * `wall_cell`, heated by an immortal LAVA source rather than fire. Fire
 * decays away in around forty steps (material_by_id((material_id_t)MAT_FIRE)->decay), which
 * would cap how many attempts a slow conductor ever gets and confuse
 * "does it conduct at all" with "did the fire survive long enough to
 * find out". Lava never decays (`decay` MUST stay 0 - see its own row
 * in material.c), so this isolates the one thing under test: the real
 * per-material `conducts` figure. Boxes the lava on three sides for the
 * same reason lava_beside_dirt() does above - a liquid otherwise drains
 * to level itself instead of staying put against the wall. Mirrors
 * build_boiler_room()/steps_to_boil() above, generalised over the wall
 * material and the heat source. */
static int steps_to_boil_through(int wall_len, cell_t wall_cell, int budget)
{
    /* Self-contained, like steps_to_boil() above: malloc, use, free, all
     * within one call - the test below calls this twice and gets a
     * fresh grid each time. */
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "steps-to-boil-through grid must fit in what the framebuffer "
        "leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_mobility(&wide, 0);
    /* This measures CONDUCTION speed, not water's own new resistance to
     * boiling once heat arrives - forced to 255 so a slow real `boils`
     * figure cannot be mistaken for slow conduction. */
    sand_set_boils(&wide, 255);

    const int y = 2;
    const int lava_x  = 1;
    const int wall_x0 = lava_x + 1;
    const int water_x = wall_x0 + wall_len;

    sand_set(&wide, water_x - 1, y + 1, STONE);
    sand_set(&wide, water_x,     y + 1, STONE);
    sand_set(&wide, water_x + 1, y + 1, STONE);

    /* Boxes the lava on every side but the one facing the wall - INCLUDING
     * both down-diagonals, not just the cardinals. A liquid blocked
     * straight down still tries a diagonal fall, and leaving either one
     * open drains the source clean off the grid within a single step
     * (found by instrumenting exactly that - a cardinal-only box was not
     * enough). */
    sand_set(&wide, lava_x, y - 1, STONE);
    sand_set(&wide, lava_x, y + 1, STONE);
    sand_set(&wide, lava_x - 1, y, STONE);
    sand_set(&wide, lava_x - 1, y + 1, STONE);
    sand_set(&wide, lava_x + 1, y + 1, STONE);
    sand_set(&wide, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
    for (int i = 0; i < wall_len; i++) {
        sand_set(&wide, wall_x0 + i, y, wall_cell);
    }
    sand_set(&wide, water_x, y, WATER);

    int result = budget;
    for (int i = 0; i < budget; i++) {
        sand_step(&wide, 0, 1000, 0);
        if (CELL_MATERIAL(sand_at(&wide, water_x, y)) == MAT_STEAM) {
            result = i + 1;
            break;
        }
    }

    /* Freed BEFORE returning: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(wide_cells);
    return result;
}

/* The performance-relevant claim the plan itself flags as the one thing
 * no benchmark scene would catch: metal's `conducts` (248) makes the
 * conduction walk reach roughly CONDUCT_REACH cells on average, against
 * stone and glass's 220 - see Metal-Smelting-Plan.md's own attenuation
 * table. This is the minimum host guard the plan asks for before merge:
 * heat must cross a 20-cell metal run comfortably inside a short shared
 * budget where a 20-cell stone run - real per-material figures, nothing
 * forced - must not have gotten through yet. Extended materials appear
 * in no benchmark scene today, so this is what stands between metal's
 * real conduction cost and shipping completely unmeasured. */
static void test_a_metal_run_conducts_further_than_a_stone_one(void)
{
    const int wall_len = 20;
    const int budget = 10;

    const int metal = steps_to_boil_through(wall_len, MATX(MATX_METAL),
                                            budget);
    const int stone = steps_to_boil_through(wall_len, STONE, budget);

    TEST_ASSERT_LESS_THAN_MESSAGE(budget, metal,
        "a 20-cell metal wall must conduct well within a ten-step "
        "budget - conducts 248 puts the mean walk at roughly "
        "CONDUCT_REACH (32), well past this depth");
    TEST_ASSERT_EQUAL_INT_MESSAGE(budget, stone,
        "a 20-cell stone wall must NOT conduct within the same ten-step "
        "budget - at conducts 220 the walk needs on the order of twenty "
        "steps on average to get through this depth, an order of "
        "magnitude slower than metal");
}

/* "A lava source grows its own 32-cell metal rod out of a dirt bed and
 * then stops" - Metal-Smelting-Plan.md's own description of the
 * self-growing rod, and "the thing most likely to surprise someone".
 * Dirt at the far side of a metal conductor run smelts via
 * conduct_heat()'s walk exactly as dirt directly against lava smelts via
 * direct contact, which lengthens the run by one cell each time it
 * happens - until the walk can no longer reach past CONDUCT_REACH
 * conductor cells to find the next un-smelted one.
 *
 * The bed is twice CONDUCT_REACH long specifically so a rod that failed
 * to cap would be caught running all the way to the far wall instead of
 * merely running a little further than expected. Conduction is forced
 * to 255 so every roll along an existing metal run succeeds - the ONLY
 * thing left to gate growth is dirt's own heat_chance at the growing
 * tip, and the only thing left to stop it is the reach cap itself.
 *
 * Measured at 33 cells, not 32: the plan's own prose ("stops at
 * CONDUCT_REACH") is off by the one cell that is placed by DIRECT
 * contact rather than by the walk - conduct_heat()'s own loop can still
 * succeed with an existing run of exactly CONDUCT_REACH conductor cells
 * (its depth counter reaches CONDUCT_REACH - 1, which satisfies
 * `depth < CONDUCT_REACH`), so the walk itself can add one cell beyond
 * a run already at the cap before the NEXT attempt finally fails to fit.
 * Not something this change gets to silently correct by tightening the
 * bounds below to hide it - flagged here and in the report instead. The
 * bounds are loose enough to pass at either 32 or 33, which is the
 * point: this test pins "stops near the cap, not at the far wall", not
 * the exact off-by-one. */
static void test_the_rod_terminates_at_conduct_reach_not_the_far_wall(void)
{
    enum { ROD_W = CONDUCT_REACH_TEST * 2, ROD_H = 6 };
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t *rod_cells = malloc((size_t)ROD_W * ROD_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(rod_cells,
        "metal-rod grid must fit in what the framebuffer leaves");
    sand_t rod;
    sand_init(&rod, rod_cells, ROD_W, ROD_H, 3u);
    sand_set_mobility(&rod, 0);
    sand_set_conduction(&rod, 255);
    /* The lava source sits boxed in on 3-4 sides for the whole run - more
     * than enough cover to be burst-eligible (bd esp32c6-mqt) over 6000
     * steps. Pinned off: this test's job is the rod's own growth/cap
     * mechanism, not the unrelated chance of the source itself bursting
     * away mid-growth. */
    sand_set_lava_burst(&rod, 0);

    const int y = 2;
    const int lava_x = 1;
    const int bed_x0 = lava_x + 1;
    const int bed_len = ROD_W - bed_x0 - 2;

    /* A floor under the whole bed - dirt is KIND_POWDER and falls into
     * any empty cell beneath it, mobility override or not (powders never
     * read that field at all - material.h's own comment on `mobility`).
     * Without this every cell of the bed drops out of row y on the very
     * first step, and the rest of this scene tests an empty row. */
    for (int x = lava_x; x < ROD_W; x++) {
        sand_set(&rod, x, y + 1, STONE);
    }
    /* And the lava boxed on every remaining side - INCLUDING both
     * down-diagonals, which the floor above only half covers (it already
     * takes the straight-down and down-right cells; down-left still
     * needs its own block). A liquid blocked straight down still tries a
     * diagonal fall, and leaving one open drains the source clean off
     * the grid within a single step. */
    sand_set(&rod, lava_x, y - 1, STONE);
    sand_set(&rod, lava_x - 1, y, STONE);
    sand_set(&rod, lava_x - 1, y + 1, STONE);
    sand_set(&rod, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
    for (int i = 0; i < bed_len; i++) {
        sand_set(&rod, bed_x0 + i, y, CELL_SOIL(MAT_DIRT, 1, 0));
    }

    for (int i = 0; i < 6000; i++) {
        sand_step(&rod, 0, 1000, 0);
    }

    /* The run is contiguous from the lava outward, so the first cell that
     * is NEITHER metal NOR stone ends it - reaction_t.flaw_to
     * (material.h) means the rod is no longer guaranteed all-metal, and
     * a stone cell mid-run still conducts (its own `conducts`, 220, is
     * nonzero) so growth carries on past it exactly as it would past
     * another metal cell. `flawed[]` records which of the two each cell
     * came out as, for the clumping check below. */
    bool flawed[ROD_W];
    int smelted_len = 0;
    for (int i = 0; i < bed_len; i++) {
        const cell_t c = sand_at(&rod, bed_x0 + i, y);
        const bool is_metal = cell_is_extended(c) && CELL_VARIANT(c) == MATX_METAL;
        const bool is_stone = CELL_MATERIAL(c) == MAT_STONE;
        if (!is_metal && !is_stone) {
            break;
        }
        flawed[smelted_len] = is_stone;
        smelted_len++;
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of rod_cells (via `rod`) are done by this point -
     * everything below reads only smelted_len and the local flawed[]. */
    free(rod_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(CONDUCT_REACH_TEST - 4, smelted_len,
        "the rod must actually reach close to CONDUCT_REACH - if this "
        "fails the growth mechanism itself is broken, not merely capped "
        "in the wrong place");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(CONDUCT_REACH_TEST + 2, smelted_len,
        "the rod must stop at (approximately) CONDUCT_REACH - a lava "
        "source growing its own metal-or-stone bar out of a dirt bed is "
        "meant to be self-limiting, not to run until it hits whatever "
        "wall the player happened to draw");
    TEST_ASSERT_LESS_THAN_MESSAGE(bed_len, smelted_len,
        "and it must stop well short of the far end of the bed - this "
        "bed is twice CONDUCT_REACH long specifically so a rod that "
        "failed to cap would be caught reaching the far wall instead");

    /* NOT asserting stone_count > 0 here, on purpose, even though this is
     * exactly the scene that motivated flaw_to in the first place. The
     * clump mechanism only RE-ROLLS once every HEAT_FLAW_CLUMP_TEST
     * triggers (see the comment below), so a rod ~30 cells long gets only
     * ~6 independent rerolls, not ~30. At flaw_chance 220/256 (rebalanced
     * twice on 2026-08-31, 40 -> 90 -> 220, to make METAL the rare
     * outcome), the odds have flipped from the original worry: the chance
     * of landing on all-metal by pure chance is now (1 - 220/256)^6, on
     * the order of 0.0008% - effectively never - but the chance of landing
     * on all-STONE, zero metal anywhere in the rod, is (220/256)^6, ~40%,
     * a real and unremarkable outcome for a sample this small. Either
     * extreme is still not this test's job to rule out.
     * test_dry_dirt_smelting_reaches_both_metal_and_stone below proves
     * both flaw_to AND metal itself still fire, from a sample large
     * enough that chance is not a factor either way; this test's job is
     * the SHAPE of a flaw when one happens, not proving one happens (or
     * doesn't) here. */

    /* THE CLUMPING ITSELF. Successes along this rod happen one at a time,
     * in strict spatial order - each cell has to smelt before conduction
     * can even reach the next one (see this test's own top comment) - so
     * heat_flaw_seq's trigger order here is exactly this array's index
     * order, with no interleaving from elsewhere on the grid. That makes
     * the clump bound EXACT rather than statistical: heat_flaw_is_flawed
     * only ever changes at a trigger index that is a multiple of
     * HEAT_FLAW_CLUMP_TEST, so a run of this length can contain at most
     * ceil(smelted_len / HEAT_FLAW_CLUMP_TEST) maximal same-outcome
     * stretches - see try_heat_transform()'s own SMELT FLAW comment
     * (sand_reactions.c) for the mechanism this is checking. */
    int runs = 1;
    for (int i = 1; i < smelted_len; i++) {
        if (flawed[i] != flawed[i - 1]) {
            runs++;
        }
    }
    const int max_runs =
        (smelted_len + HEAT_FLAW_CLUMP_TEST - 1) / HEAT_FLAW_CLUMP_TEST;
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(max_runs, runs,
        "stone must appear in CLUMPED runs along the rod, not scattered "
        "one cell at a time - the number of maximal metal/stone stretches "
        "cannot exceed ceil(smelted_len / HEAT_FLAW_CLUMP), which is what "
        "the rolling-modulo mechanism in try_heat_transform() guarantees "
        "by construction");
}

/* A GLASS-walled vat with `floor_rows` of `floor_cell` sitting on a
 * GLASS floor, topped with `acid_rows` of acid - the same box
 * acid_tank() builds above, generalised over what is being eaten so it
 * can compare materials rather than always eating sand. */
static void acid_over(cell_t floor_cell, int floor_rows, int acid_rows)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int y = 1; y < H; y++) {
        sand_set(&s, 1, y, GLASS);
        sand_set(&s, W - 2, y, GLASS);
    }
    for (int y = H - 1 - floor_rows; y < H - 1; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, floor_cell);
        }
    }
    for (int y = 1; y <= acid_rows; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
}

/* Steps until every cell counted by `counted_id` is gone from
 * acid_over()'s scene, or `budget` if some survive that long. */
static int steps_for_acid_to_clear(uint8_t counted_id, cell_t floor_cell,
                                   int budget)
{
    acid_over(floor_cell, 1, 4);
    for (int i = 0; i < budget; i++) {
        sand_step(&s, 0, 1000, 0);
        if (count_cells_of(counted_id) == 0) {
            return i + 1;
        }
    }
    return budget;
}

/* Balance revision, 2026-08-30: metal now RESISTS acid (dissolvable 1,
 * not immune at 0 - see that field's own comment in material.c) instead
 * of being acid's intended counter (previously 110, deliberately above
 * stone's 60) - see Metal-Smelting-Plan.md's own numbers table for the
 * full account. This test's name is now backwards from what it checks;
 * left as-is pending a rename in a future balance pass rather than
 * touched here alongside the value itself. */
static void test_acid_eats_metal_between_stone_and_sand(void)
{
    const int budget = 5000;
    const int stone = steps_for_acid_to_clear(MAT_STONE, STONE, budget);
    const int metal = steps_for_acid_to_clear(MAT_EXTENDED,
                                              MATX(MATX_METAL), budget);
    const int sand  = steps_for_acid_to_clear(MAT_SAND,
                                              CELL_MAKE(MAT_SAND, 8), budget);

    TEST_ASSERT_LESS_THAN_MESSAGE(budget, sand,
        "fixture check: acid must fully clear a floor of sand within the "
        "budget");
    TEST_ASSERT_LESS_THAN_MESSAGE(budget, metal,
        "fixture check: acid must fully clear a floor of metal within "
        "the same budget");
    TEST_ASSERT_LESS_THAN_MESSAGE(budget, stone,
        "fixture check: acid must fully clear a floor of stone within "
        "the same budget too");

    TEST_ASSERT_LESS_THAN_MESSAGE(metal, stone,
        "acid must eat metal SLOWER than stone - dissolvable 1 against "
        "stone's 60. Metal now resists acid rather than being its "
        "counter (balance revision 2026-08-30)");
    TEST_ASSERT_LESS_THAN_MESSAGE(stone, sand,
        "and stone slower than sand - dissolvable 60 against 200, the "
        "obviously softest target on the board");
}

static void test_wood_and_steam_grain_count_is_conserved(void)
{
    fixture();
    /* Condensation forced off: this is purely a movement (or, for wood,
     * non-movement) conservation check, and reaction_t.condenses is NOT
     * one-for-one the way an ordinary material swap is - a 2x2 patch of
     * steam collapsing into a single water cell is a real, deliberate
     * loss of three grains, which is exactly what this test exists to
     * catch when it is a BUG rather than a feature working as designed.
     * With it off, wood never burns on its own (it has no burning
     * neighbour in this scene) and steam has nothing left to react with
     * (its only other field, `warms`, needs a heat-holder neighbour this
     * scene has none of), so may_have_burning and may_have_condenser
     * both stay clear and sand_step_reactions() early-returns every
     * step. Ember is deliberately NOT covered here: its flare
     * (reaction_t.flare) can spawn a brand new MAT_FIRE cell out of an
     * empty neighbour, which is the feature working as designed
     * (test_an_ember_flares_fire_into_an_empty_neighbour), not a
     * conservation violation - but it does mean a strict grain-count
     * invariant, the kind this test checks, does not apply to ember the
     * way it does to every other material here. */
    sand_set_condenses(&s, 0);

    for (int y = 1; y <= 2; y++) {
        for (int x = 1; x <= 3; x++) {
            sand_set(&s, x, y, WOOD);
        }
    }
    for (int y = 4; y <= 5; y++) {
        for (int x = 1; x <= 3; x++) {
            sand_set(&s, x, y, STEAM);
        }
    }
    const int expected = sand_count(&s);
    TEST_ASSERT_EQUAL_INT(12, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a step must conserve wood and steam grains in every "
                "gravity direction, the same as every other material");
        }
    }
}

/* Fake condensation - reaction_t.condenses/condenses_to. A really small,
 * deliberately rare per-step chance in real play, so a test that wants
 * to actually see it happen forces the override to 255 instead of
 * waiting on the real figure - the same discipline sand_set_boils() and
 * every other chance override in this file already gives.
 *
 * The 2x2 pocket is sealed in stone on every side a stationary gas cell
 * could otherwise be moved out of by sand_step_gas() (which runs BEFORE
 * sand_step_reactions() within one sand_step() call - see sand.c): stone
 * above the top row blocks a rise attempt, stone left of the left column
 * and right of the right column blocks the cross-flow spread pass. Without
 * that sealing, a single step's own gas movement could scatter the block
 * before the condensation check ever saw it intact. */
static void test_a_2x2_block_of_steam_condenses_into_one_water_cell(void)
{
    fixture();
    sand_set_condenses(&s, 255);
    sand_set_mobility(&s, 0);

    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 5, 3, STONE);
    sand_set(&s, 5, 4, STONE);

    sand_set(&s, 3, 3, STEAM);
    sand_set(&s, 4, 3, STEAM);
    sand_set(&s, 3, 4, STEAM);
    sand_set(&s, 4, 4, STEAM);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "a forced roll must condense the square into water at its own "
        "top-left corner");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 3)),
        "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 4)),
        "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 4)),
        "and clear the other three corners of the square");
}

/* Three matching corners and a fourth cell that is NOT steam (stone,
 * here, not left empty - an empty fourth cell risks a neighbouring gas
 * cell sliding into it during the very same step's gas pass, before the
 * reactions pass ever gets to look, which would turn this into an
 * accidental positive instead of the negative it is meant to be) must
 * never condense, no matter how the roll would have gone. */
static void test_condensation_needs_a_genuine_2x2_square(void)
{
    fixture();
    sand_set_condenses(&s, 255);
    sand_set_mobility(&s, 0);

    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 5, 3, STONE);
    sand_set(&s, 5, 4, STONE);

    sand_set(&s, 3, 3, STEAM);
    sand_set(&s, 4, 3, STEAM);
    sand_set(&s, 3, 4, STEAM);
    sand_set(&s, 4, 4, STONE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "three steam cells beside one that is not steam must never "
        "condense, even with the roll forced to succeed every time");
}

/* Acid rain - SAND_ACID_RAIN_CHANCE's own comment (sand.h) for the
 * feature, step_one_acid_rain_cell()'s own (sand_reactions.c) for the
 * mechanism. sand_set_acid_rain(&s, 255) makes a single-step conversion
 * overwhelmingly likely, not strictly certain - it is still a roll
 * against a byte-wide field, 255 in 256, not the 256-and-up special case
 * util/rng.h's own rng_chance() helper reserves for "always" - the same
 * discipline test_a_2x2_block_of_steam_condenses_into_one_water_cell just
 * above already leans on for the sibling mechanic this one extends, and
 * this fixture now matches that one's own footprint exactly: acid rain is
 * a 2x2 pocket, sized like plain rain, not the bigger 4x4 an earlier
 * version checked.
 *
 * Sealed on top and both sides exactly like that test's own fixture -
 * see its comment for why (sand_step_gas() runs before
 * sand_step_reactions() within one sand_step() call, so an unsealed
 * pocket can scatter before the reactions pass ever sees it intact).
 * Column-striped (steam, gas) rather than diagonal or any other
 * arrangement of the required two-of-each: with only four cells total,
 * no arrangement can accidentally also satisfy ordinary condensation
 * (that needs all four cells identical, and two-of-each never is), so
 * unlike the old 4x4 fixture there is no ambiguity left to design
 * around - condenses is still explicitly disabled below, belt and
 * braces.
 *
 * Does NOT assert which of Acid or Water the corner becomes - that is
 * a genuine 50/50 coin flip (see step_one_acid_rain_cell()'s own
 * comment), and this test's job is the COLLAPSE SHAPE, not the outcome
 * of that flip; test_acid_rain_resolves_to_both_acid_and_water below
 * checks the flip itself, across many independent pockets. */
static void test_a_qualifying_gas_steam_pocket_collapses_into_one_cell(void)
{
    fixture();
    sand_set_acid_rain(&s, 255);
    sand_set_condenses(&s, 0);
    sand_set_mobility(&s, 0);

    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 5, 3, STONE);
    sand_set(&s, 5, 4, STONE);

    sand_set(&s, 3, 3, STEAM);
    sand_set(&s, 3, 4, STEAM);
    sand_set(&s, 4, 3, GAS);
    sand_set(&s, 4, 4, GAS);

    sand_step(&s, 0, 1000, 0);

    const uint8_t corner_mat = CELL_MATERIAL(sand_at(&s, 3, 3));
    TEST_ASSERT_TRUE_MESSAGE(corner_mat == MAT_ACID || corner_mat == MAT_WATER,
        "a forced roll must collapse the pocket at its own top-left "
        "corner into either Acid or Water - the 50/50 coin flip - not "
        "leave it as gas/steam or anything else");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 3)),
        "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 4)),
        "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 4)),
        "and clear the other three corners of the square");
}

/* Guards step_one_reacting_row()'s own found |= FOUND_DISSOLVER report at
 * its acid-rain call site (sand_reactions.c) the same way
 * test_lava_quenched_into_stone_mid_pass_arms_the_heat_holder_flag guards
 * may_have_heat_holder: the survivor a matching pocket collapses into is
 * written at the row walk's own current scan position and never gets a
 * turn of its own this same pass, so nothing but that call site's own
 * report keeps may_have_dissolver armed once the pass ends. Skip that
 * report and the new acid is created inert - it sits there and never
 * dissolves anything again, since sand_step_reactions() early-returns on
 * every later step and ordinary liquid movement does not call
 * latch_content_flags() to re-arm it (measured directly while diagnosing
 * this: a rained acid cell sealed in the room below ate none of its 12
 * surrounding stone cells across 200 steps with the report missing,
 * against 2 of 12 eaten in the same room once it was restored).
 *
 * A full sealed 4x4 room, not just the acid's own immediate neighbours -
 * sand_set_mobility(0) reads as NO VISCOSITY for a liquid (see
 * liquid_may_move()'s own comment, sand_liquid.c - a documented trap,
 * not this test's own invention: 0 means "moves every step", the
 * opposite of "pinned in place"), so the acid drifts around inside the
 * room rather than sitting still at its own birth cell. The room gives
 * it four walls to eventually reach regardless of which way it wanders,
 * the same shape the control comparison above used to first measure
 * this bug.
 *
 * Seeded to land the coin flip on Acid specifically (see the loop) so
 * there is one concrete follow-up behaviour - does the acid actually go
 * on to dissolve some wall of the room it is boxed in - to assert on,
 * not two; may_have_moisture's equivalent for a rained Water cell has no
 * comparably cheap single observable, and the same found |= report line
 * covers both bits together regardless of which one fires. */
static void test_a_rained_acid_cell_keeps_dissolving_after_the_collapse(void)
{
    bool found_acid_seed = false;
    for (unsigned seed = 1u; seed < 64u && !found_acid_seed; seed++) {
        sand_init(&s, cells, W, H, seed);
        sand_set_acid_rain(&s, 255);
        sand_set_condenses(&s, 0);
        sand_set_mobility(&s, 0);

        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, 2, STONE);
            sand_set(&s, x, 5, STONE);
        }
        sand_set(&s, 2, 3, STONE);
        sand_set(&s, 5, 3, STONE);
        sand_set(&s, 2, 4, STONE);
        sand_set(&s, 5, 4, STONE);

        sand_set(&s, 3, 3, STEAM);
        sand_set(&s, 3, 4, STEAM);
        sand_set(&s, 4, 3, GAS);
        sand_set(&s, 4, 4, GAS);

        sand_step(&s, 0, 1000, 0);

        if (CELL_MATERIAL(sand_at(&s, 3, 3)) != MAT_ACID) {
            continue; /* the coin flip landed on Water this seed - try another */
        }
        found_acid_seed = true;

        TEST_ASSERT_TRUE_MESSAGE(s.may_have_dissolver,
            "the acid a collapse just produced must leave may_have_dissolver "
            "armed - it was created behind the row walk's own scan pointer, "
            "so nothing else this same pass reports it");

        int stone_left = 12;
        for (int i = 0; i < 120 && stone_left == 12; i++) {
            sand_step(&s, 0, 1000, 0);
            stone_left = 0;
            for (int yy = 2; yy <= 5; yy++) {
                for (int xx = 2; xx <= 5; xx++) {
                    if ((xx == 2 || xx == 5 || yy == 2 || yy == 5)
                        && CELL_MATERIAL(sand_at(&s, xx, yy)) == MAT_STONE) {
                        stone_left++;
                    }
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(stone_left < 12,
            "a rained acid cell sealed in a stone room must go on to "
            "dissolve some of that room's walls within 120 further steps - "
            "if may_have_dissolver silently cleared at the end of the "
            "collapse's own pass, the acid is created inert and never eats "
            "anything again");
    }
    TEST_ASSERT_TRUE_MESSAGE(found_acid_seed,
        "setup: none of the first 64 seeds landed the coin flip on Acid - "
        "widen the seed range, this test never exercised its own scenario");
}

/* One cell short of the two-of-each requirement (one steam, three gas -
 * still a pure gas/steam pocket, still boxed in identically) must never
 * collapse, no matter how the roll would have gone - the same "needs a
 * genuine match, not almost one" property
 * test_condensation_needs_a_genuine_2x2_square just above already
 * checks for its own sibling mechanic. */
static void test_acid_rain_needs_at_least_two_of_each_species(void)
{
    fixture();
    sand_set_acid_rain(&s, 255);
    sand_set_condenses(&s, 0);
    sand_set_mobility(&s, 0);

    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 5, 3, STONE);
    sand_set(&s, 5, 4, STONE);

    sand_set(&s, 3, 3, STEAM);
    sand_set(&s, 4, 3, GAS);
    sand_set(&s, 3, 4, GAS);
    sand_set(&s, 4, 4, GAS);

    sand_step(&s, 0, 1000, 0);

    /* Not just "the corner didn't become acid or water" - a miss must
     * leave the whole pocket untouched, the same strength
     * test_condensation_needs_a_genuine_2x2_square just above asserts
     * for its own sibling mechanic (MAT_STEAM there, not merely
     * "not MAT_WATER"). A future bug that cleared the block without
     * placing a residue - or picked the wrong corner - would pass the
     * weaker check and fail this one. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "one steam cell short of the two-steam/two-gas requirement must "
        "never collapse, even with the roll forced to succeed every time");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "and must leave the rest of the pocket exactly as it was");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 3, 4)),
        "and must leave the rest of the pocket exactly as it was");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 4, 4)),
        "and must leave the rest of the pocket exactly as it was");
}

/* SAND_ACID_RAIN_CHANCE's own comment (sand.h): the surviving cell is a
 * genuine 50/50 coin flip between Acid and Water, not always Acid.
 * ACID_RAIN_TRIALS independent collapses, forced acid_rain=255 so every
 * one fires - the roll under test here is the SECOND one, the residue
 * coin flip, which has no override of its own and so has to be observed
 * through real RNG variation across many independent trials instead.
 *
 * One shared sealed pocket on the ordinary WxH fixture, repainted and
 * re-stepped ACID_RAIN_TRIALS times in a row, rather than ACID_RAIN_TRIALS
 * side-by-side pockets on a bespoke wide grid - the trials are independent
 * either way (a fresh RNG draw per collapse), and this needs neither a
 * malloc nor a second sand_t: the shared 8x8 fixture already has room for
 * one sealed 2x2 with stone to spare, and the survivor is fully
 * overwritten by the next trial's steam/gas repaint before it could ever
 * be re-observed. An unbiased coin: P(all ACID_RAIN_TRIALS trials agree)
 * is 2 * 0.5^ACID_RAIN_TRIALS, astronomically small at 40 - the same bar
 * test_wet_dirt_can_still_steam_before_spoiling_at_least_sometimes (this
 * file) computes for its own many-independent-attempts test. */
#define ACID_RAIN_TRIALS 40
static void test_acid_rain_resolves_to_both_acid_and_water(void)
{
    fixture();
    sand_set_acid_rain(&s, 255);
    sand_set_condenses(&s, 0);
    sand_set_mobility(&s, 0);

    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, 2, STONE);
        sand_set(&s, x, 5, STONE);
    }
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 5, 3, STONE);
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 5, 4, STONE);

    int acid_seen = 0, water_seen = 0;
    for (int trial = 0; trial < ACID_RAIN_TRIALS; trial++) {
        sand_set(&s, 3, 3, STEAM);
        sand_set(&s, 3, 4, STEAM);
        sand_set(&s, 4, 3, GAS);
        sand_set(&s, 4, 4, GAS);

        sand_step(&s, 0, 1000, 0);

        const uint8_t mat = CELL_MATERIAL(sand_at(&s, 3, 3));
        if (mat == MAT_ACID) {
            acid_seen++;
        } else if (mat == MAT_WATER) {
            water_seen++;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, acid_seen,
        "expected at least one acid-rain collapse to resolve to Acid "
        "across ACID_RAIN_TRIALS independent trials");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, water_seen,
        "expected at least one acid-rain collapse to resolve to Water "
        "across ACID_RAIN_TRIALS independent trials - always Acid would "
        "mean the coin flip is not actually being rolled");
}

/* --- dirty rows: nothing changes without saying so ---------------------- */

/* The invariant the renderer depends on, asserted directly for every
 * material rather than inferred from the passes that maintain it.
 *
 * app_sand.c only repaints rows whose dirty_rows byte is set, so a cell
 * that changes on a row nobody marked is a pixel left stale on the panel
 * until something else happens to redraw that band. That failure is
 * invisible in every other test here - the grid is right, only the screen
 * is wrong - and it is exactly the kind of thing that gets noticed on
 * device and not before.
 *
 * TRANSIENT materials are the reason this is worth a test of its own.
 * A grain of sand changes when it moves, and a move is hard to forget
 * about. Fire, gas, steam and smoke also change when they merely AGE:
 * tick_decay() rewrites the variant nibble in place, the palette turns
 * that into a different colour, and nothing has moved at all. A pass that
 * remembered to mark its moves and forgot to mark its decay would look
 * perfectly correct right up until a flame stopped fading on screen. */
static void assert_every_change_is_marked(material_id_t m, int steps,
                                          const char *what)
{
    static uint8_t seen[W * H];

    fixture();
    sand_track_dirty_rows(&s, dirty);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 0, STONE);
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 0; y < H; y++) {
        sand_set(&s, 0, y, STONE);
        sand_set(&s, W - 1, y, STONE);
    }
    for (int y = 2; y <= 4; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(m, MATERIAL_VARIANTS - 1));
        }
    }

    for (int i = 0; i < steps; i++) {
        memcpy(seen, s.cells, sizeof seen);
        memset(dirty, 0, sizeof dirty);   /* the renderer clears as it draws */
        sand_step(&s, 0, 1000, 0);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (seen[y * W + x] == s.cells[y * W + x]) {
                    continue;
                }
                /* The whole byte, not just the material nibble: a cell
                 * that only faded still has to be repainted. */
                TEST_ASSERT_TRUE_MESSAGE(dirty[y] != 0, what);
            }
        }
    }
}

static void test_every_cell_change_marks_its_row_dirty(void)
{
    assert_every_change_is_marked(MAT_SAND, 120,
        "a sand grain must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_WATER, 120,
        "a water cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_GAS, 240,
        "a gas cell must never change on a row left unmarked - it AGES as "
        "well as moving, and a fade is a repaint too");
    assert_every_change_is_marked(MAT_FIRE, 240,
        "a fire cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_SMOKE, 240,
        "a smoke cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_STEAM, 240,
        "a steam cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_OIL, 120,
        "an oil cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_LAVA, 240,
        "a lava cell must never change on a row left unmarked");
}

/* --- conservation ------------------------------------------------------- */

static void test_grains_are_never_created_or_destroyed(void)
{
    fixture();

    /* A slab dropped into the middle, then shaken through every gravity
     * direction. Whatever the rules do, the count must not drift. */
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }
    const int expected = sand_count(&s);
    TEST_ASSERT_EQUAL_INT(15, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a step must conserve grains in every gravity direction");
        }
    }
}

static void test_a_grain_keeps_its_shade_as_it_falls(void)
{
    fixture();
    const uint8_t shade = SAND_LAST_SHADE;
    sand_set(&s, 3, 0, shade);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(shade, sand_at(&s, 3, 3),
        "shade travels with the grain, or a falling pile shimmers");
}

/* --- gravity in other directions ---------------------------------------- */

static void test_grains_fall_upward_when_the_board_is_inverted(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);

    sand_step(&s, 0, -1, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, H - 2),
        "gravity is whatever direction it is given, including up");
}

static void test_grains_fall_sideways_when_the_board_is_on_its_edge(void)
{
    fixture();
    sand_set(&s, 0, 3, SAND_FIRST_SHADE);

    sand_step(&s, 1, 0, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 1, 3),
        "with gravity to the right, the far wall is the floor");
}

static void test_a_heap_settles_against_whichever_wall_is_down(void)
{
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 4; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    /* Long enough for everything to reach the right-hand wall and stop. */
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 1, 0, 0);
    }

    /* Note what is NOT asserted: that every grain ends up in the last column
     * or two. It does not, and should not - the heap forms a wedge with a 45
     * degree angle of repose, and grains on the slope are held up by their
     * neighbours rather than by the wall. Demanding they pack flat would be
     * asserting the absence of the very behaviour that makes it look like
     * sand. What matters is the side, the contact and the stability. */
    int touching_wall = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) == SAND_EMPTY) {
                continue;
            }
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(W / 2, x,
                "every grain must migrate to the side gravity points at");
            if (x == W - 1) {
                touching_wall++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, touching_wall,
        "the heap must actually reach the wall, not stall short of it");

    uint8_t settled[W * H];
    memcpy(settled, cells, sizeof(settled));
    sand_step(&s, 1, 0, 0);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(settled, cells, sizeof(settled),
        "a settled heap must be completely stable, not creep for ever");
}

/* --- spawning ----------------------------------------------------------- */

static void test_spawn_fills_a_disc(void)
{
    fixture();

    const int filled = sand_spawn(&s, 4, 4, 2, MAT_SAND);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, filled, "a spawn must place grains");
    TEST_ASSERT_EQUAL_INT_MESSAGE(filled, sand_count(&s),
        "the reported count must match what is actually on the grid");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 4, 4),
        "the centre is inside the disc");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 0, 0),
        "a corner far outside the radius must stay empty");
}

static void test_spawn_is_clipped_to_the_grid(void)
{
    fixture();

    /* Centred off the top-left corner: most of the disc is out of bounds. */
    const int filled = sand_spawn(&s, 0, 0, 3, MAT_SAND);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, filled,
        "the part of the disc that is on the grid must still be placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(filled, sand_count(&s),
        "clipped cells must not be counted as filled");
}

static void test_spawning_onto_existing_grains_does_not_double_count(void)
{
    fixture();

    sand_spawn(&s, 4, 4, 2, MAT_SAND);
    const int after_first = sand_count(&s);

    const int filled = sand_spawn(&s, 4, 4, 2, MAT_SAND);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, filled,
        "spawning onto a full disc fills nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(after_first, sand_count(&s),
        "and must not change the grid");
}

static void test_erase_removes_a_disc(void)
{
    fixture();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    const int removed = sand_erase(&s, 4, 4, 2);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, removed, "an erase must remove grains");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 4, 4),
        "the centre is inside the disc");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 0, 0),
        "a corner far outside the radius must be untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W * H - removed, sand_count(&s),
        "the reported count must match what actually left the grid");
}

static void test_erasing_empty_space_removes_nothing(void)
{
    fixture();

    const int removed = sand_erase(&s, 4, 4, 3);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, removed,
        "erasing an empty area must report nothing removed, or the count "
        "drifts the same way a double-counting spawn would");
}

static void test_erase_is_clipped_to_the_grid(void)
{
    fixture();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    const int removed = sand_erase(&s, 0, 0, 3);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, removed, "the on-grid part must go");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W * H - removed, sand_count(&s),
        "cells off the grid must not be counted as removed");
}

static void test_erase_marks_the_rows_it_emptied(void)
{
    dirty_fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 4, SAND_FIRST_SHADE);
    }
    memset(dirty, 0, sizeof(dirty));

    sand_erase(&s, 4, 4, 1);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4],
        "a row a grain was removed from has changed and must be redrawn - "
        "otherwise the erased sand stays visible on the panel");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[0], "a distant row has not");
}

static void test_spawned_grains_use_the_full_range_of_shades(void)
{
    fixture();

    sand_spawn(&s, 4, 4, 3, MAT_SAND);

    bool seen[SAND_LAST_SHADE + 1] = { false };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t c = sand_at(&s, x, y);
            if (c != SAND_EMPTY) {
                TEST_ASSERT_TRUE_MESSAGE(c >= SAND_FIRST_SHADE && c <= SAND_LAST_SHADE,
                    "every grain must carry a valid shade");
                seen[c] = true;
            }
        }
    }

    int distinct = 0;
    for (int i = SAND_FIRST_SHADE; i <= SAND_LAST_SHADE; i++) {
        distinct += seen[i] ? 1 : 0;
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct,
        "a flat-coloured pile looks like a solid block, not sand");
}

/* --- emitters ------------------------------------------------------------- */

/* A persistent point source - see sand_add_emitter() in sand.h. Unlike
 * sand_spawn()/sand_erase() above, an emitter is stepped by sand_step()
 * itself rather than acting the moment it is called, so most of these
 * tests run at least one step before looking at the grid. */

static void test_an_emitter_fills_its_own_cell_when_empty(void)
{
    fixture();
    /* The bottom row, so gravity cannot immediately carry the fresh grain
     * away - this test is about placement, not about liquid movement, and
     * placing it anywhere else would make it about both. */
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, H - 1, WATER),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "an emitter must fill its own point once that point is empty");
}

static void test_an_emitter_does_not_overwrite_an_occupied_cell(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, H - 1, WATER),
        "setup: an emitter may be placed over an already-occupied cell");

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "an emitter must never overwrite whatever is already sitting on "
        "its own point - that is the entire rate control, and an emitter "
        "that ignored it would be a firehose");
}

/* The failure mode this test exists to catch: a write that places material
 * but forgets to wake the block it landed in. That failure would still
 * pass a test that only checked the cell was written - sand_set() writes
 * the byte regardless of block-sleeping state - so this one goes on to
 * demand that the material actually MOVES afterwards, which only happens
 * if the sweep is still visiting that block. */
static void test_an_emitter_wakes_a_sleeping_block(void)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);

    /* An empty grid settles on its very first step: nothing in the block
     * moved, and neither did any neighbour (there is only the one block on
     * this WxH fixture - see BLOCK_COLS/BLOCK_ROWS above). */
    sand_step(&s, 0, 1000, 0);
    TEST_ASSERT_TRUE_MESSAGE(sand_block_settled(&s, 0, 0),
        "setup: the block must actually be asleep, or this test proves "
        "nothing about waking one");

    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 0, WATER),
        "setup: the emitter's own point is empty and in bounds");

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    bool moved_off_row_zero = false;
    for (int x = 0; x < W && !moved_off_row_zero; x++) {
        for (int y = 1; y < H; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                moved_off_row_zero = true;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(moved_off_row_zero,
        "emitted water must have moved somewhere below row 0 over 60 steps "
        "- if it is still confined to the row it was emitted on, the block "
        "that went to sleep before the emitter arrived was never woken, "
        "and the sweep has been skipping it ever since (it would still be "
        "written there every step, since sand_set() does not care whether "
        "the block sleeps - only whether it later MOVES proves the wake "
        "happened)");
}

static void test_emitted_water_produces_a_continuing_stream(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 0, WATER),
        "setup: the emitter's own point is empty and in bounds");

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int water_cells = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                water_cells++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(3, water_cells,
        "a water emitter left running over many steps must read as a "
        "continuing stream, not a single cell - each step the source cell "
        "clears by flowing away and the emitter refills it, over and over "
        "for as long as the tap runs");
}

/* The bug this whole block exists to catch: emit_from_emitters() used to
 * write s->emitters[i].cell RAW, via sand_set(). But that cell is not the
 * exact byte to write - it is whatever the app's brush table handed
 * sand_add_emitter() (see brushes[] in app_sand.c), and every entry there
 * is CELL_MAKE(material, 0), a PLACEHOLDER. What variant 0 means depends on
 * the material's kind (see material.h's top comment and random_cell() in
 * sand.c): for a KIND_LIQUID it is a fill level of zero - no water, no
 * lava, nothing to render or flow, even though the high nibble still says
 * MAT_WATER or MAT_LAVA. A water or lava emitter reported as producing
 * nothing visible is exactly that cell.
 *
 * Every test below places the emitter with CELL_MAKE(material, 0), the
 * literal brush placeholder, rather than one of this file's own WATER/
 * LAVA/GAS/... macros - those already carry a non-placeholder variant (8),
 * which would not reproduce what the app actually hands the emitter.
 *
 * And every test steps with gravity (0, 0, 0) rather than a real vector.
 * sand_step() runs emit_from_emitters() first and then returns immediately
 * when the dithered direction is (0, 0) - "free fall: no down, so nothing
 * settles" - which skips the gravity sweep, the liquid pass, the gas pass
 * and the reactions pass entirely (see sand_step()'s own comment). That
 * isolates the one thing under test - what the emitter itself wrote - from
 * anything that could move or react the cell a moment later and make a
 * mismatch about something else. */

static void test_an_emitted_liquid_cell_is_full_not_the_placeholders_zero_mass(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, H - 1, CELL_MAKE(MAT_WATER, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, H - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX, CELL_VARIANT(c),
        "an emitted liquid must be a FULL cell, exactly as a pour is - "
        "random_cell() always hands a fresh liquid cell MASS_MAX, never a "
        "random amount and never the placeholder's zero - a water tap "
        "that instead wrote variant 0 raw would place a cell with "
        "MAT_WATER in it and no water");
}

static void test_an_emitted_lava_cell_is_full_not_the_placeholders_zero_mass(void)
{
    /* Lava specifically, because it is what was reported on hardware: a
     * lava source produced steam (the reactions pass still saw MAT_LAVA
     * and quenched it) but no lava - because the cell it quenched never
     * carried any mass to look like lava in the first place. */
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, H - 1, CELL_MAKE(MAT_LAVA, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, H - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX, CELL_VARIANT(c),
        "an emitted lava cell must be FULL, not the placeholder's zero "
        "mass - a zero-mass lava cell is exactly what let a lava tap on "
        "hardware produce steam (the reactions pass still saw MAT_LAVA "
        "and quenched it) but no lava anyone could see or that could "
        "flow");
}

static void test_an_emitted_transient_cell_has_full_life_not_the_placeholders_zero(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, 0, CELL_MAKE(MAT_GAS, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1, CELL_VARIANT(c),
        "an emitted transient material must start at FULL LIFE, exactly "
        "as a pour does - random_cell() hands a fresh decaying cell "
        "MATERIAL_VARIANTS - 1, never the placeholder's variant 0, which "
        "decay reads as life already spent: a gas tap that wrote it raw "
        "would emit cells already dead");
}

static void test_an_emitted_powder_still_lands_in_a_valid_shade(void)
{
    /* Unlike the liquid and transient cases above, this one cannot
     * distinguish the fix from the bug by itself - variant 0 happens to
     * be a valid shade for a powder (see material.h's top comment), which
     * is exactly why sand and snow LOOKED fine on hardware while water
     * and lava did not. It is here anyway, as a regression guard: nothing
     * about routing the emitter through sand_spawn_cell() may push a
     * powder's variant outside the range a pour would ever produce. */
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, H - 1, CELL_MAKE(MAT_SAND, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, H - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) < SAND_DUNE_SHADES,
        "an emitted grain of sand must land within the dune shade band, "
        "same as a pour does - random_cell() never hands a freshly "
        "painted grain one of the shades reserved for cullet");
}

/* The same claim test_the_brush_and_the_setter_agree_about_every_material
 * makes about sand_set() versus the brush, but for the emitter versus
 * sand_spawn_cell() - and exhaustive over every material an emitter may
 * ever hold, rather than sampled to the handful above. The reason to walk
 * all of them rather than trust water/lava/gas/sand as representatives is
 * exactly what made this bug ship: variant 0 means something DIFFERENT for
 * each material kind - a liquid's fill level, a transient's life, glass's
 * temperature, soil's tone and moisture, a powder's shade - and picking
 * representatives only catches the kinds someone thought to check. A loop
 * over every emit-eligible material catches the next one added with a
 * variant meaning nobody anticipated, the same way this one got through. */
static void test_an_emitter_and_sand_spawn_cell_agree_about_every_material(void)
{
    const int x = 3, y = 3;

    for (int m = 1; m < MAT_COUNT; m++) {
        const cell_t placeholder = CELL_MAKE((material_id_t)m, 0);
        if (!material_can_emit(placeholder)) {
            continue;   /* material_can_emit() is the same gate
                         * sand_add_emitter()'s caller applies (see
                         * test_material_can_emit_matches_every_brush_by_kind)
                         * - a material that can never legally be an
                         * emitter has nothing to agree about here */
        }

        /* The emitter, given the exact placeholder the brush table would
         * hand it. Zero gravity, so this step does nothing but emit - see
         * this block's own top comment. */
        fixture();
        TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, x, y, placeholder),
            "setup");
        sand_step(&s, 0, 0, 0);
        const cell_t emitted = sand_at(&s, x, y);

        /* sand_spawn_cell(), given the very same placeholder, on an
         * identically fresh board - same seed, same RNG draw count so
         * far (zero), so any random pick inside random_cell() lands on
         * the same value in both. */
        fixture();
        sand_spawn_cell(&s, x, y, 0, placeholder);
        const cell_t spawned = sand_at(&s, x, y);

        char why[256];
        snprintf(why, sizeof why,
                 "an emitter of %s disagrees with sand_spawn_cell() about "
                 "what a fresh cell of it looks like - if the emitter's "
                 "byte is the unresolved placeholder (variant 0) rather "
                 "than spawned's, the emitter is writing the brush's raw "
                 "byte instead of resolving it",
                 material_by_id((material_id_t)m)->name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(spawned, emitted, why);
    }

    /* GUNPOWDER, again separately - material_can_emit(placeholder) is
     * already exercised for every ordinary id by the loop above; this is
     * the one emit-eligible material the loop's CELL_MAKE(id, 0)
     * placeholder can never construct at all, since gunpowder has no
     * material_id_t of its own. */
    {
        const cell_t placeholder = GUNPOWDER_CELL(0);

        fixture();
        TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, x, y, placeholder),
            "setup");
        sand_step(&s, 0, 0, 0);
        const cell_t emitted = sand_at(&s, x, y);

        fixture();
        sand_spawn_cell(&s, x, y, 0, placeholder);
        const cell_t spawned = sand_at(&s, x, y);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(spawned, emitted,
            "an emitter of gunpowder disagrees with sand_spawn_cell() "
            "about what a fresh cell of it looks like");
    }
}

/* The observable claim behind all the byte-level tests above: a liquid
 * emitter left running has to make a POOL, not merely keep writing cells
 * that carry the right material and no water. Summing CELL_VARIANT (the
 * fill level) rather than counting cells is the point - the old bug's
 * cells were entirely present, entirely MAT_WATER, and entirely empty of
 * mass, so a count-based check (see
 * test_emitted_water_produces_a_continuing_stream above) passed against it
 * without noticing anything was wrong. */
static void test_a_running_water_emitter_accumulates_mass_on_the_floor(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, 0, CELL_MAKE(MAT_WATER, 0)),
        "setup: the emitter's own point is empty and in bounds");

    /* Grid walls are solid past the last row (see sand_at()'s own
     * comment), so the bottom row is already a floor with no need to
     * paint one - the same shape test_an_emitter_fills_its_own_cell_when_
     * empty relies on. */
    for (int i = 0; i < 150; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    long total_mass = 0;
    bool water_on_the_floor = false;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_IS_EMPTY(c) || CELL_MATERIAL(c) != MAT_WATER) {
                continue;
            }
            total_mass += CELL_VARIANT(c);
            if (y == H - 1) {
                water_on_the_floor = true;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(3 * MASS_MAX, total_mass,
        "a water emitter left running above a floor must ACCUMULATE far "
        "more mass than one full cell's worth - this is the reported "
        "symptom itself: a source producing cells that carry MAT_WATER "
        "but no mass never pools no matter how long the tap runs, because "
        "there is never anything in any of the cells it writes");
    TEST_ASSERT_TRUE_MESSAGE(water_on_the_floor,
        "the accumulated water must be sitting on the floor, not stranded "
        "only at the emitter's own point - a source with no mass has "
        "nothing that could ever fall");
}

static void test_adding_an_emitter_over_an_occupied_cell_still_registers(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);

    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, H - 1, WATER),
        "an emitter may be placed over an already-occupied cell");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s),
        "and it must actually be registered, not silently dropped for "
        "landing on something");

    sand_step(&s, 0, 1000, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "and it must not emit while the cell stays occupied");

    /* Cleared directly, not via sand_erase() - erase would also remove the
     * emitter itself (see test_erase_stops_an_emitter_from_emitting below)
     * and defeat the point of this test, which is only about the delay
     * between registering and first emitting. */
    sand_set(&s, 3, H - 1, SAND_EMPTY);
    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "once the cell clears, the already-registered emitter must start "
        "emitting into it");
}

static void test_adding_at_an_existing_emitter_replaces_its_cell(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 4, WATER), "setup");
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 4, GAS),
        "re-adding at the same point must succeed, not fail because the "
        "point is already an emitter");

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s),
        "the second add must replace the first emitter's cell, not add a "
        "second emitter at the same point");

    int x, y;
    cell_t cell;
    TEST_ASSERT_TRUE_MESSAGE(sand_emitter_at(&s, 0, &x, &y, &cell), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(cell),
        "the surviving emitter must emit the SECOND cell it was given, not "
        "the first");
}

static void test_the_emitter_cap_is_respected(void)
{
    fixture();
    /* W * H (64) is well over SAND_MAX_EMITTERS (16), so every one of these
     * has a distinct, in-bounds point and none of them collide with each
     * other. */
    for (int i = 0; i < SAND_MAX_EMITTERS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, i % W, i / W, WATER),
            "every emitter up to the cap must be accepted");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_MAX_EMITTERS, sand_emitter_count(&s),
        "setup");

    /* (0, 2) was not used by the loop above (it only reaches y = 1). */
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, 0, 2, GAS),
        "one more than the cap must be refused");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_MAX_EMITTERS, sand_emitter_count(&s),
        "and the refusal must not have corrupted the list - the count must "
        "stay exactly at the cap");

    for (int i = 0; i < SAND_MAX_EMITTERS; i++) {
        int x, y;
        cell_t cell;
        TEST_ASSERT_TRUE_MESSAGE(sand_emitter_at(&s, i, &x, &y, &cell),
            "every emitter that was already there must still be readable");
        TEST_ASSERT_EQUAL_INT_MESSAGE(i % W, x,
            "and unchanged by the refused add");
        TEST_ASSERT_EQUAL_INT_MESSAGE(i / W, y,
            "and unchanged by the refused add");
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(cell),
            "and unchanged by the refused add");
    }
}

static void test_out_of_bounds_emitters_are_rejected(void)
{
    fixture();
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, -1, 0, WATER),
        "negative x is off the grid");
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, 0, -1, WATER),
        "negative y is off the grid");
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, W, 0, WATER),
        "x == W is one past the right edge");
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, 0, H, WATER),
        "y == H is one past the bottom edge");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "none of the rejected adds may have registered anything");
}

static void test_remove_emitters_only_removes_those_in_radius(void)
{
    fixture();
    sand_add_emitter(&s, 4, 4, WATER);   /* the centre: distance 0 */
    sand_add_emitter(&s, 5, 4, WATER);   /* distance 1: inside a radius of 1 */
    sand_add_emitter(&s, 4, 6, GAS);     /* distance 2: outside it */
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, sand_emitter_count(&s), "setup");

    const int removed = sand_remove_emitters(&s, 4, 4, 1);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, removed,
        "only the two emitters within the radius must be removed - the "
        "same disc test sand_erase() uses");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s),
        "and exactly one emitter must remain");

    int x, y;
    cell_t cell;
    TEST_ASSERT_TRUE_MESSAGE(sand_emitter_at(&s, 0, &x, &y, &cell), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, x,
        "the survivor must be the one outside the radius");
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, y,
        "the survivor must be the one outside the radius");
}

static void test_erase_stops_an_emitter_from_emitting(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 0, WATER), "setup");

    sand_erase(&s, 3, 0, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "erasing over an emitter must remove it - without this a running "
        "tap could never be turned off, which would make the whole "
        "feature a trap");

    sand_step(&s, 0, 1000, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0),
        "and it must actually have stopped emitting, not merely been "
        "forgotten by sand_emitter_count() while still running");
}

static void test_erase_count_excludes_emitters(void)
{
    fixture();

    /* An emitter with nothing under it: erasing here changes no cell, so
     * the count sand_erase() returns must be zero even though an emitter
     * also went away. */
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 3, WATER), "setup");
    const int removed_empty = sand_erase(&s, 3, 3, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, removed_empty,
        "removing an emitter over an already-empty cell must not count as "
        "a cell removed - sand_remove_emitters() is what counts emitters, "
        "not this return value");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "setup check: the emitter must actually be gone");

    /* An emitter WITH a grain under it: erasing here changes exactly one
     * cell, and the emitter leaving too must not make it look like two -
     * see sand_erase()'s own comment in sand.h on why that count must stay
     * exactly "cells changed", not "cells changed plus emitters removed". */
    sand_set(&s, 4, 4, SAND_FIRST_SHADE);
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 4, 4, WATER), "setup");
    const int removed_occupied = sand_erase(&s, 4, 4, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, removed_occupied,
        "the count must still mean exactly one cell changed, whether or "
        "not an emitter also happened to sit on it");
}

static void test_sand_init_clears_emitters_from_a_previous_use(void)
{
    fixture();
    sand_add_emitter(&s, 3, 3, WATER);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s), "setup");

    sand_init(&s, cells, W, H, 12345u);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "sand_init() must clear emitters left over from a previous use of "
        "the struct, the same as it clears every other piece of per-run "
        "state - see the THIRD-COPY bug in sand_init()'s own comment for "
        "what carrying stale state across a reused sand_t already cost "
        "once");
}

/* Every entry the app's palette offers - see brushes[] in app_sand.c - by
 * KIND rather than by importing that table: this file cannot see
 * app_sand.c and should not start to. The four flowing kinds (powder,
 * liquid, gas) come out true and the two static ones false, which is
 * exactly the KIND_POWDER/LIQUID/GAS-may, KIND_STATIC-may-not rule
 * material_can_emit() implements - see its own comment in material.h, and
 * test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on
 * above for why the two extended entries both land on `false` through one
 * shared row rather than two independent answers. */
static void test_material_can_emit_matches_every_brush_by_kind(void)
{
    static const struct { cell_t cell; bool can_emit; const char *why; } cases[] = {
        { SAND,                    true,  "sand is a powder" },
        { WATER,                   true,  "water is a liquid" },
        { STONE,                   false, "stone is static" },
        { GAS,                     true,  "gas is a gas" },
        { FIRE,                    true,  "fire is a gas" },
        { WOOD,                    false, "wood is static" },
        { OIL,                     true,  "oil is a liquid" },
        { LAVA,                    true,  "lava is a liquid" },
        { CELL_MAKE(MAT_ACID, MASS_MAX), true,  "acid is a liquid" },
        { GLASS,                   false, "glass is static" },
        { SNOW,                    true,  "snow is a powder" },
        { CELL_MAKE(MAT_DIRT, 0),  true,  "dirt is a powder" },
        { MATX(MATX_ICE),          false, "ice shares the extended row's KIND_STATIC" },
        { MATX(MATX_PLANT),        false, "plant shares the extended row's KIND_STATIC" },
        { GUNPOWDER_CELL(0),       true,  "gunpowder is the one extended-range material that reads KIND_POWDER, not the statics' shared KIND_STATIC" },
    };

    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[k].can_emit,
            material_can_emit(cases[k].cell), cases[k].why);
    }
}

/* --- explosions -----------------------------------------------------------
 *
 * sand_explode() throws grains outward one cell per step, in a bounded
 * transient list rather than a per-cell velocity field - see
 * docs/Sand/Explosion-Plan.md, which this whole section implements.
 */

/* One entry per cell of the 8x8 fixture grid - big enough that no test below
 * needs to think about the cap, except the one written specifically to
 * exercise it (which uses its own, deliberately tiny buffer instead). */
static impulse_t impulse_buf[W * H];

/* Written FIRST, because it is what the plan calls out as forcing the actual
 * design decision: "stop when blocked" (what this implements) versus a
 * radial line-of-sight raycast from the centre (the obvious first instinct
 * the plan rejects) would both pass every other test in this file, but only
 * the first one keeps a blast that starts inside a sealed container from
 * reaching outside it.
 *
 * HALF OF A TWO-PART GUARANTEE, not the whole of it, since a wall gained a
 * density-scaled chance to be dislodged (see queue_flying_grain()'s own
 * comment in sand.c). This half is the one that still has to hold
 * absolutely: a WEAK OR DISTANT blast - radius 1, here, against a wall
 * three cells away - never reaches the wall's own candidate cells at all,
 * so the density roll never gets a turn and containment stays exact, the
 * same as it always did. See test_a_strong_close_blast_can_breach_a_wall,
 * right after this one, for the other half - proof the wall CAN give way
 * when a blast is pointed directly at it with enough force, so that
 * capability has real coverage instead of being an unverified side effect
 * of the density roll's existence. */
static void test_a_blast_inside_a_sealed_vessel_stays_inside_it(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* A stone box drawn on the grid itself, not merely relying on the grid's
     * own edge (sand_at()'s off-grid-is-STONE convention is exercised by the
     * separate bounds test below) - x=0/W-1 and y=0/H-1. The payload sits at
     * its centre with two or three empty cells of clearance on every side,
     * so a thrown grain has real room to fly before it ever meets the wall. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (x == 0 || x == W - 1 || y == 0 || y == H - 1) {
                sand_set(&s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }
    sand_set(&s, 3, 3, SAND_FIRST_SHADE);
    sand_set(&s, 4, 3, SAND_FIRST_SHADE);
    sand_set(&s, 3, 4, SAND_FIRST_SHADE);
    sand_set(&s, 4, 4, SAND_FIRST_SHADE);

    /* Centred on one corner of the 2x2 payload, radius 1: its two occupied
     * cardinal neighbours (RIGHT and DOWN) each get thrown towards a wall
     * that is three cells away - not an immediate bounce, an actual flight. */
    sand_explode(&s, 3, 3, 1);

    /* sand_explode() fills a small core with fire before it queues
     * anything - see SAND_EXPLODE_CORE_DIVISOR - so the centre must be fire
     * right away, with no step required to see it. Checked before
     * anything else runs, since fire is KIND_GAS and may well have risen
     * away by the time later assertions run (that is expected - see the
     * wall check below, which is what actually matters once it has), which
     * would hide a core that was never filled at all behind a coincidence. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "the blast's own centre must flash into fire, not four grains still "
        "occupying their original footprint");
    const int after_explode = sand_count(&s);

    for (int i = 0; i < 30; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (x == 0 || x == W - 1 || y == 0 || y == H - 1) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
                    CELL_MATERIAL(sand_at(&s, x, y)),
                    "the wall must still be exactly the wall - a flying grain "
                    "that reached it must have stopped, not passed through "
                    "or displaced it");
            }
        }
    }
    /* Measured AFTER the explode, not before it, and bounded rather than
     * exact - see test_a_blast_conserves_grains's own comment for why: the
     * core's fire can genuinely be smothered and vanish if it never finds
     * an escape route, which this sealed vessel is a plausible place for.
     * Nothing here can ever push the count the OTHER way, though - see
     * that same comment for why an increase is a hard bug regardless of
     * geometry. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(after_explode, sand_count(&s),
        "outside the core, a blast only ever loses cells to fire being "
        "smothered, never creates or duplicates one, even when it is "
        "fully contained");
}

/* THE OTHER HALF - see test_a_blast_inside_a_sealed_vessel_stays_inside_it's
 * own comment just above for the first. A small box (walls at x=1/x=6 and
 * y=1/y=6, a 4x4 open interior) with genuine empty MARGIN outside its own
 * walls (x=0, x=7, y=0, y=7 - not just the grid's implicit edge, which
 * would give a dislodged cell nowhere to actually go and prove nothing),
 * detonated close to one corner at a radius that reaches every wall cell
 * at least once: centre (3,3), radius 4, so the nearest wall (x=1, two
 * cells away) sits well inside the annulus and the farthest (x=6, three
 * cells away) still does. Every wall cell this radius reaches gets its own
 * independent density roll (see queue_flying_grain()'s own comment in
 * sand.c) - stone's chance is 55-in-256 (~21%) per cell, and enough of the
 * box wall falls inside this annulus that at least one succeeding is the
 * expected outcome, not a coin flip on a single cell.
 *
 * CHECKED AGAINST THE WALL'S OWN ORIGINAL CELLS, not "did anything land
 * outside the box" - a dislodged KIND_STATIC entry now ALSO falls under
 * gravity every step it is airborne (step_impulses()'s own comment, sand.
 * c, "AIRBORNE SOLIDS FALL TOO"), same as a thrown grain of sand always
 * has. That is a real, wanted change: a chunk knocked off a wall by a
 * blast that also opens a hole right behind it can tumble back into that
 * hole instead of sailing cleanly away, exactly as a rock actually would.
 * fixture()'s own fixed seed (12345) still deterministically dislodges
 * the corner at (6,1) - confirmed by running the real, shipped code, not
 * derived by hand - but WHERE that corner ends up once gravity has a say
 * is no longer a single pinned coordinate worth asserting on its own; the
 * capability this test exists to prove is that the density roll actually
 * fires and moves real stone off the wall, which "the wall's own (6,1)
 * cell is no longer stone" demonstrates directly regardless of where the
 * dislodged material lands afterward. */
static void test_a_strong_close_blast_can_breach_a_wall(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 1; y <= 6; y++) {
        for (int x = 1; x <= 6; x++) {
            const bool on_wall = (x == 1 || x == 6 || y == 1 || y == 6);
            if (on_wall) {
                sand_set(&s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }

    sand_explode(&s, 3, 3, 4);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, 6, 1)),
        "a strong enough blast pointed directly at a close wall must be "
        "able to dislodge the corner cell at (6,1) - the density roll's "
        "own capability, confirmed against this exact deterministic seed "
        "- and it needs to be seen actually happening, not just assumed "
        "from the roll's own arithmetic; where it lands afterward is no "
        "longer pinned, see this test's own top comment for why");
}

/* A ROLL FAILING IS NOT THE SAME AS LANDING - step_impulses()'s own
 * comment (sand.c, the block right before the outward-push roll) spells
 * out the bug this guards against: dropping a KIND_STATIC entry from
 * impulse tracking the instant its per-turn outward-push roll fails,
 * regardless of whether it has actually reached anything to rest on.
 * Since that roll is a per-turn coin flip on `speed` rather than a
 * threshold, it can fail on literally the FIRST turn - speed 0, forced
 * here, guarantees it - while the dislodged cell is still hanging over
 * open space with nothing beneath it. The only thing that ever makes a
 * KIND_STATIC cell fall at all is the unconditional gravity-drift that
 * runs "while an entry is tracked here at all" (that block's own
 * comment) - so an entry dropped while still airborne would previously
 * freeze exactly where gravity-drift happened to leave it after ONE
 * step, floating there for the rest of the run with nothing left to
 * ever revisit it.
 *
 * dir DOES NOT MATTER HERE - speed 0 means the outward-push roll can
 * never succeed, so the only thing moving this cell at all is the
 * unconditional gravity-drift, which ignores `dir` entirely. */
static void test_a_dislodged_wall_keeps_falling_even_if_its_first_push_roll_fails(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 3, 0, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_impulse_dislodge(&s, 3, 0, 0, 0, SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_STONE),
        "a dislodged KIND_STATIC cell whose very first outward-push roll "
        "fails (speed 0 here, guaranteeing it on every turn) must still "
        "keep falling under the unconditional gravity-drift every step "
        "until it is genuinely supported - stopping partway down means "
        "the failed roll dropped it from impulse tracking while it was "
        "still airborne, exactly the bug this test exists to catch");
}

/* THE OTHER HALF OF sand_explode()'s OWN SPLIT (see sand_displace()'s own
 * comment in sand.h for the two reasons a caller might want the push
 * without the fire - correctness, for a future pure-pressure event like
 * confined steam, and cost, since fire latches `may_have_burning` and
 * keeps the whole reactions pass alive until it burns out). Wood placed
 * EXACTLY at the centre is the sharpest possible check: sand_explode()'s
 * own core fill (SAND_EXPLODE_CORE_DIVISOR) would flash that exact cell
 * into fire unconditionally, occupied or not, material or not.
 * sand_displace() has no core concept to do that with at all - the centre
 * offset is skipped for the ordinary "no direction to throw it in" reason
 * every other test in this file already relies on (see queue_outward_
 * impulse()'s own comment in sand.c), not because anything here decided
 * to spare it. If that wood is still wood, nothing tried to burn it. */
static void test_sand_displace_alone_never_creates_fire_or_smoke(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 1; y <= 5; y++) {
        for (int x = 1; x <= 5; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }
    sand_set(&s, 3, 3, CELL_MAKE(MAT_WOOD, 0));

    sand_displace(&s, 3, 3, 3);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_WOOD, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "sand_displace() has nothing that fills a core with fire - the "
        "centre cell must be exactly what it was before the call");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, s.impulse_count,
        "the surrounding sand must actually have been queued to fly, or "
        "this scene never exercised the displacement half of this "
        "function at all - a test proving 'no fire' means nothing if "
        "nothing else happened either");

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, x, y)),
                "sand_displace() must never place fire anywhere on the "
                "board - that is sand_explode()'s own addition on top of "
                "this function, not something this function does itself");
        }
    }

    for (int i = 0; i < 40; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_FIRE, CELL_MATERIAL(c),
                "still no fire anywhere after settling - nothing sand_"
                "displace() did should have given the reactions pass "
                "anything to ignite");
            /* Smoke, in this simulation, is physically the same material
             * a kettle's own steam is (see MAT_FIRE's own `.residue`
             * comment in material.c) - a burnt-out flame or a finished
             * log leaves MAT_STEAM behind, not a separate "smoke"
             * material. Nothing in this scene ever boils water either,
             * so any MAT_STEAM found here could only have come from
             * something burning out - which nothing did. */
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_STEAM, CELL_MATERIAL(c),
                "and no smoke either - smoke/steam residue is what a "
                "burnt-out fire or finished log leaves behind, and "
                "nothing here was ever set alight to finish burning");
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(cell_is_burning(sand_at(&s, 3, 3)),
        "the wood at the centre must not have caught fire from anything "
        "sand_displace() did, however long the simulation runs "
        "afterward");
}

static void test_a_blast_conserves_grains(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 2; y < 5; y++) {
        for (int x = 2; x < 6; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 3, 3, 2);

    /* Measured AFTER the explode, not before it. sand_explode() clears a
     * small core outright before it queues anything - see
     * SAND_EXPLODE_CORE_DIVISOR - so the grain count genuinely, deliberately
     * drops once, right here: that is a real removal, exactly like any
     * other sand_erase() call, not something the flight pass did. The
     * invariant from here on is that nothing ELSE may touch the count -
     * outside the core, a blast only ever relocates a cell. */
    const int expected = sand_count(&s);

    /* Checked every step, not just at the end - the same idiom as
     * test_dithering_still_conserves_grains - so a bug that briefly
     * duplicates or drops a cell mid-flight cannot cancel itself out before
     * a final comparison would ever see it.
     *
     * BOUNDED, NOT EXACT - and this is the honest invariant, not a
     * loosened one. The flight pass itself only ever relocates a cell, so
     * by itself it could never move the count at all, in either
     * direction - but the core it just filled with fire is a real burning
     * cell now, sitting in a bed of ordinary sand that is denser than
     * fire (see can_enter()'s displacement rule): sand directly above a
     * fire cell sinks straight through it via the ordinary sweep, which
     * is what usually lets fire rise clear before anything can trap it -
     * but if the geometry ever leaves it with nowhere to rise TO, it gets
     * fully surrounded by strictly denser material and smothered()
     * (sand_reactions.c) puts it out, which is a real, deliberate loss of
     * one cell, not a bug. Measured, not assumed: a materially identical
     * scene detonated at a packed grid CORNER (see the bounds test below)
     * hit exactly this on 136 of 20,000 independent seeds. What can never
     * legitimately happen, from any of this, is the count going UP - and
     * that half of the invariant is checked as strictly as ever. */
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(expected, sand_count(&s),
            "the flight pass itself must never create or duplicate a cell - "
            "a count ABOVE the post-explode value is always a bug");
    }
}

static void test_a_blast_at_the_edge_stays_in_bounds(void)
{
    /* Every corner, and the middle of one edge - each centred exactly on the
     * grid boundary, so most of the disc is off-grid and every direction is
     * exercised against the wall sand_at() makes of it, not a painted one. */
    static const struct { int cx, cy; } spots[] = {
        { 0, 0 }, { W - 1, 0 }, { 0, H - 1 }, { W - 1, H - 1 }, { W / 2, 0 },
    };

    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        fixture();
        sand_enable_impulses(&s, impulse_buf, W * H);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, SAND_FIRST_SHADE);
            }
        }

        sand_explode(&s, spots[i].cx, spots[i].cy, 3);

        /* Measured AFTER the explode - see test_a_blast_conserves_grains's
         * own comment on why the core's removal is real and everything
         * past this point is the invariant under test. */
        const int expected = sand_count(&s);

        for (int step = 0; step < 20; step++) {
            sand_step(&s, 0, 1000, 0);
            /* Bounded, not exact - see test_a_blast_conserves_grains's own
             * comment for why. This is in fact the scene that FIRST
             * surfaced it: a corner blast in a grid packed solid on every
             * side can leave the core's fire with nowhere to rise into at
             * all, and smothered() (sand_reactions.c) then puts it out for
             * real - measured at 136 of 20,000 seeds across these five
             * spots. An INCREASE past `expected`, from off-grid cells or
             * anywhere else, remains a hard bug regardless. */
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a blast centred on the grid edge must never manufacture a "
                "cell from the off-grid space it can never queue an entry "
                "for - CRACK_MAX exists for the same worst-case reason");
        }
    }
}

static void test_a_dropped_entry_never_moves_someone_elses_cell(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 4, 4, SAND_FIRST_SHADE);
    sand_explode(&s, 3, 4, 1);   /* (4,4) is the RIGHT neighbour of centre */
    /* The centre itself, (3,4), is now fire - sand_explode() fills its
     * core before it queues anything (see SAND_EXPLODE_CORE_DIVISOR). That
     * makes (3,4) an honest burning neighbour of the stone placed below,
     * which is why this checks MATERIAL rather than the exact byte -
     * see the comment on the assertion itself. */

    /* Something else claims the exact cell the entry still names, before it
     * ever gets another turn - a reaction or a second paint stroke would do
     * this on the real board just as easily as this test does it directly. */
    sand_set(&s, 4, 4, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    sand_step(&s, 0, 1000, 0);

    /* Material only, not the exact byte: (4,4) is now directly beside the
     * fire the core-fill just lit at (3,4), and stone banks heat from a
     * burning neighbour (reaction_t.heat_ramp) - so its own heat variant
     * legitimately drifts off SAND_AMBIENT_HEAT within this one step on
     * some seeds. That drift is real physics happening to the stone that
     * proves the entry was dropped, not a sign it was not: an entry that
     * had wrongly RE-ACQUIRED and relocated the stone would still trigger
     * it identically. What actually distinguishes "dropped" from "wrongly
     * moved" is exactly this test's other assertion below. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 4, 4)),
        "the entry must have been dropped - the cell it named no longer "
        "holds the grain it threw");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 5, 4),
        "relocating the stone would have moved it exactly here - a dropped "
        "entry must not move whatever now sits in its old cell instead");
}

static impulse_t tiny_impulse_buf[2];

/* Sized to exactly one full ring (see sand_explode()'s own "QUEUED BY
 * RING" comment in sand.h) around a centre with radius >= 2, so a radius-3
 * blast's ring 1 - all 8 of a centre's immediate Chebyshev neighbours,
 * corners included - fits with nothing left over. Used by
 * test_a_blast_queues_impulses_on_every_side_of_the_centre below, which
 * needs the cap to actually bind for scan order to matter at all - a
 * buffer as generous as the standard impulse_buf[] never truncates a
 * radius-3 disc in an 8x8 grid, so it could not have told ring order from
 * the old row-order bug this pins. */
static impulse_t axis_impulse_buf[8];

static void test_the_cap_degrades_gracefully(void)
{
    fixture();
    sand_enable_impulses(&s, tiny_impulse_buf, 2);

    /* Three FULL-WIDTH rows, the same shape the sleeping tests settle - not
     * a free-floating block. Full width matters here specifically: every
     * cell's sliding diagonals are either another occupied cell in the same
     * rows or off-grid (which sand_at() reads as solid too), so nothing in
     * it can move under ordinary gravity AT ALL, on any edge. A free block
     * narrower than its own support looked simpler but was not - its
     * corner cells had an open diagonal past their own footprint and slid
     * away under plain gravity regardless of the blast, which is exactly
     * the false failure this shape rules out. */
    for (int y = 5; y <= 7; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    /* Centre (3,6), radius 1: the four cardinal neighbours all qualify -
     * radius 1's true disc is exactly 5 cells (the centre plus the four
     * cardinals; the four diagonals fail the r2 <= 1 test) - see
     * exact_disc_count()'s own comment in sand.c. The buffer holds 2, so
     * queue_outward_impulse()'s accumulator THINS 5 candidates down to 2,
     * evenly rather than truncating to "however many the scan reaches
     * first" - see that function's own comment for the accumulator
     * itself. Worked by hand for this exact case (keep=2, disc_count=5,
     * scan order centre/UP/DOWN/LEFT/RIGHT - see sand_explode()'s own
     * "QUEUED BY RING" comment in sand.h for why that is the order):
     * accum starts at 0 and gains 2 per candidate that passes the r2
     * test, firing whenever it reaches 5 -
     *   centre: accum 0->2, no fire (and no direction to throw it in
     *           regardless)
     *   UP:     accum 2->4, no fire
     *   DOWN:   accum 4->6, FIRES (accum -> 1) - 1st entry queued
     *   LEFT:   accum 1->3, no fire
     *   RIGHT:  accum 3->5, FIRES (accum -> 0) - 2nd entry queued
     * DOWN and RIGHT are what a buffer of 2 affords here, not UP and DOWN
     * the way a first-come truncation would have picked - the whole point
     * of thinning by density instead of by scan position. Radius 1 also
     * means the filled core (radius 1 / SAND_EXPLODE_CORE_DIVISOR = 0) is
     * only the centre cell itself, (3,6) - none of the four cardinal
     * neighbours is it. */
    sand_explode(&s, 3, 6, 1);

    /* Checked directly against the queue itself, before a single step has
     * run, rather than inferred from where anything ends up on the board
     * afterward - DOWN and RIGHT are both structurally unable to move in
     * this scene regardless of whether they were queued (DOWN by the
     * grid's own bottom edge, RIGHT by the packed bed beside it), which
     * would make "did it move" the wrong question for THEM. "Was it
     * queued at all" is what the accumulator's own arithmetic above
     * already answers exactly. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.impulse_count,
        "the buffer holds 2, so exactly 2 of the 5 true disc members "
        "must have been queued - not fewer, and the rest must not have "
        "silently bumped one of the first two out");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(7 * W + 3),
        s.impulse_buf[0].index,
        "the accumulator's own arithmetic (see this test's top comment) "
        "fires on DOWN (3,7) first, not UP - even thinning, not a "
        "first-come truncation");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(6 * W + 4),
        s.impulse_buf[1].index,
        "and on RIGHT (4,6) second - the last of the buffer's 2 slots");

    /* Measured AFTER the explode - see test_a_blast_conserves_grains's own
     * comment on why the core's removal is real and everything past this
     * point is the invariant under test: UP specifically, since it is the
     * one candidate here with an actually open landing cell (row 4 above
     * the packed bed is empty - see this file's own comment on
     * test_a_blast_wakes_the_blocks_it_touches for the same geometry) and
     * was NOT queued, must survive completely untouched - a bug that
     * queued it anyway would show up here as a real, visible move, not
     * just a wrong index. LEFT gets the same check for good measure, even
     * though the packed bed beside it already makes "did it move" a weak
     * question on its own. */
    const int expected = sand_count(&s);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 5),
        "UP did not fit in a buffer of 2 and must not fly - and unlike "
        "the other three candidates, UP had a genuinely open path to fly "
        "through if it had been wrongly queued");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 2, 6),
        "LEFT did not fit either, for the same reason");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
        "over the cap is a smaller-looking blast, not a bug - nothing may "
        "be lost");
}

/* THE BUG THIS PINS: sand_explode()'s density math used to size `keep`
 * against `s->impulse_max` - the buffer's TOTAL capacity - not against
 * how much of it a still-in-flight EARLIER explosion had already spent.
 * sand_impulse() itself never overflows regardless (its own
 * `impulse_count >= impulse_max` guard is unconditional), but the
 * DENSITY the accumulator aims for was computed as if the whole buffer
 * were free, so a second explosion fired before a first one's grains
 * finish would seed entries the buffer no longer had room for, and
 * sand_impulse() would silently refuse them one by one - reintroducing
 * the exact lopsided, one-sided truncation the accumulator exists to
 * prevent, just from CONTENTION between two blasts instead of bias
 * within one. See sand_explode()'s own comment on `room` in sand.c for
 * the fix and the full reasoning; this proves it holds.
 *
 * THE SCENE: axis_impulse_buf[8] (8 entries total, no more), grid fully
 * packed with sand so every disc candidate either explosion visits is
 * occupied and queueable - no gravity-direction or empty-cell exits to
 * complicate which candidates are "true" disc members. Two blasts, far
 * enough apart (x 0-2 versus x 3-7) that neither's core or ring ever
 * touches the other's cells, fired back to back with NO sand_step() in
 * between - the first blast's four grains are still exactly where they
 * were queued, "in flight" in every sense this bug cares about.
 *
 * FIRST BLAST: centre (1,1), radius 1 - the same shape test_the_cap_
 * degrades_gracefully already works out by hand: disc_count 5, buffer
 * fully free (room 8), so `keep` == 5 and all 4 real neighbours (the
 * centre itself has no direction to throw it in) queue cleanly.
 * impulse_count is 4 afterward - checked below as this test's own
 * precondition, not really the property under test.
 *
 * SECOND BLAST: centre (5,2), radius 2 - true disc_count 13 (see
 * exact_disc_count()'s own comment in sand.c), fired with only
 * `s->impulse_max - s->impulse_count` == 8 - 4 == 4 entries of room
 * actually left. Worked by hand against THAT room, in sand_explode()'s
 * own ring-then-edge scan order (centre, then ring 1's four diagonals-
 * then-cardinals interleaved per column, then ring 2's own edges - see
 * sand_explode()'s "QUEUED BY RING" comment in sand.h): accum starts at
 * 0, gains keep=4 per true disc member (r2 <= 4) visited, fires whenever
 * it reaches disc_count=13 -
 *   (0,0) centre:  accum  0->4,  no fire (no direction either way)
 *   (-1,-1):       accum  4->8,  no fire
 *   (-1,1):        accum  8->12, no fire
 *   (0,-1) UP:     accum 12->16, FIRES (->3) - 1st: (5,1), index 13
 *   (0,1):         accum  3->7,  no fire
 *   (1,-1):        accum  7->11, no fire
 *   (1,1):         accum 11->15, FIRES (->2) - 2nd: (6,3), index 30
 *   (-1,0):        accum  2->6,  no fire
 *   (1,0):         accum  6->10, no fire
 *   ring 2, (0,-2) - the only ring-2 top/bottom cell inside r2<=4:
 *                  accum 10->14, FIRES (->1) - 3rd: (5,0), index 5
 *   (0,2):         accum  1->5,  no fire
 *   ring 2, (-2,0) - the only ring-2 left cell inside r2<=4:
 *                  accum  5->9,  no fire
 *   ring 2, (2,0) - the only ring-2 right cell inside r2<=4:
 *                  accum  9->13, FIRES (->0) - 4th: (7,2), index 23
 * Exactly 4 fires for a `keep` of 4, ending with accum back at 0 - the
 * whole disc visited, the whole `room` spent, nothing wasted and nothing
 * overrun. Against the OLD, buggy `keep` (computed from `s->impulse_max`
 * == 8, not `room` == 4), the SAME accumulator instead fires 8 times,
 * and the first 4 of those 8 - (-1,-1) index 12, (0,-1) index 13,
 * (0,1) index 29, (1,1) index 30 - are what actually queue before
 * sand_impulse()'s own hard cap silently swallows the remaining 4: a
 * completely different, ring-1-only set that never reaches ring 2 at
 * all, which is exactly the lopsided shape this test exists to catch. */
static void test_two_overlapping_blasts_share_the_buffer_evenly(void)
{
    fixture();
    sand_enable_impulses(&s, axis_impulse_buf, 8);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 1, 1, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, s.impulse_count,
        "precondition: the first blast's own 4 real neighbours must all "
        "have queued cleanly against a fully free buffer, or this test "
        "is not actually exercising a second blast with only 4 slots "
        "left");

    /* NO sand_step() HERE - the first blast's grains stay exactly where
     * they were queued, still "in flight" by every measure sand_explode()
     * itself can see (s->impulse_count unchanged), which is the whole
     * scenario this test exists to create. */
    sand_explode(&s, 5, 2, 2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, s.impulse_count,
        "the second blast had exactly 4 slots of real room left and must "
        "have filled every one of them, no more and no less");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(1 * W + 5),
        s.impulse_buf[4].index,
        "worked by hand against room=4 (see this test's own top comment): "
        "UP, (5,1), fires first");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(3 * W + 6),
        s.impulse_buf[5].index,
        "down-right, (6,3), fires second");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(0 * W + 5),
        s.impulse_buf[6].index,
        "ring 2's own UP cell, (5,0), fires third - reaching ring 2 at "
        "all is exactly what the old, buggy keep (sized from the WHOLE "
        "buffer instead of what was actually left) never did, because it "
        "ran out of real room while still inside ring 1");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(2 * W + 7),
        s.impulse_buf[7].index,
        "and RIGHT, (7,2), fires fourth and last - the buffer's own "
        "final slot, spent evenly across the whole disc rather than "
        "concentrated in the first ring the scan happened to reach");
}

/* The failure this guards against is invisible to every test above: a
 * grain thrown into open air above a settled, sleeping pile freezes there
 * forever if the block it landed in is never told it is worth examining
 * again - see Adding-a-Material.md's own lesson on exactly this shape of
 * bug. Reuses settle_with_sleeping()/assert_nothing_left_to_do() from the
 * "sleeping" section above, which already embody the right check: run the
 * same final grid again with sleeping OFF, and require that nothing at all
 * moves. */
static void test_a_blast_wakes_the_blocks_it_touches(void)
{
    static const char *bed[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "oooooooo",
        "oooooooo",
        "oooooooo",
    };
    settle_with_sleeping(bed, 8, 100, 0, 1000);
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* Centre inside the now-asleep bed, radius 1: the UP neighbour is
     * thrown off the bed's own surface into the open air above it - exactly
     * the displaced-grain-freezes-mid-air scenario this test exists for, if
     * the block it lands in is never woken back up. */
    sand_explode(&s, 3, 6, 1);

    /* PAST THE DETERMINISTIC FLIGHT-TIME BOUND, not a bare 60 any more - the
     * same derivation test_the_sand_dune_scene_throws_grains_beyond_its_own_
     * footprint (this file) already uses for a KIND_STATIC entry's own full
     * decay to zero. A thrown KIND_POWDER grain can now stay TRACKED (kept,
     * not merely still falling under the ordinary sweep) for as long as its
     * speed is at or above SAND_IMPULSE_BOUNCE_MIN_SPEED - see step_impulses
     * ()'s own "!rolled_move" KIND_POWDER branch, sand.c - so a fixed low
     * step budget risks taking this test's snapshot while a grain is still
     * legitimately being nudged by its own fading impulse (a real, bounded
     * bounce near the top of the bed) rather than by anything sleeping got
     * wrong. That is exactly what the old fixed 60 hit: measured directly,
     * this scene's own s.impulse_count did not reach zero until step 111 of
     * 200 sampled, so a snapshot at 60 could still show a grain hovering
     * mid-bounce - which the fresh, impulse-free "awake" replay inside
     * assert_nothing_left_to_do() (no impulses enabled on it at all) then
     * naturally continues falling under plain gravity, misreading a real,
     * still-in-flight grain as one sleeping had wrongly frozen. */
    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    assert_nothing_left_to_do(0, 1000);
}

/* A SECOND REGRESSION GUARD - the identity check's own blind spot, not
 * sand_explode()'s. A real device confirmed the crater above finally
 * worked, then reported the very next thing: "now there's a crater but
 * the grains don't arc."
 *
 * The cause was the identity check itself, not sand_explode(). Per step
 * the order is sweep, then liquids, then gas, then reactions, then
 * step_impulses() (see sand_step()) - so by the time this pass gets a turn,
 * ordinary gravity has already had ITS turn, on every cell, including
 * ones this list still has an eye on. A grain sitting in open air is not
 * special to the sweep: gravity moves it down one cell before step_impulses()
 * ever looks at it, the stored index it is still watching is empty, the
 * old check read that as "gone", and the entry was dropped - meaning any
 * airborne grain lost its impulse after exactly one flight move and spent
 * the rest of its fall as an ordinary grain with no further push. Lateral
 * scatter out of a crater, not an arc.
 *
 * Several independent single-grain trials, not one: SAND_EXPLODE_INITIAL_SPEED
 * currently gives roughly a 1 in 5 chance that a single grain's very
 * first speed roll fails outright, unrelated to the identity mechanism
 * entirely, which would make a one-shot version of this test flaky across
 * the seed space even though the mechanism it is actually checking is
 * completely deterministic once that roll succeeds. */
static void test_a_flying_grain_keeps_its_outward_push_while_falling(void)
{
    /* fixture() ONCE, outside the trial loop - not once per trial. Calling
     * it every trial would re-seed s->rng to 12345 each time, making every
     * "independent" trial replay the exact same random sequence from the
     * exact same starting state: not several trials at all, just one
     * trial performed several times identically. sand_clear() between
     * trials instead wipes the grid but leaves s->rng exactly where the
     * previous trial's rolls left it, so each trial's speed rolls come
     * from a genuinely different point in the one long sequence a fixed
     * seed still deterministically produces.
     *
     * Confirmed by measurement, not assumed. The fixture()-per-trial
     * version of this test, at 8 trials, measured a 2347/20000 (~11.7%)
     * failure rate across a seed sweep - every one of the 8 "independent"
     * trials was in fact identical, so a seed whose very first roll failed
     * failed all 8 at once. Fixed to sand_clear() between trials, the same
     * 8-trial version measured 1/20000. Widened to 16 trials here for
     * margin rather than trusting that single result alone. */
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* Computed from the constants themselves, not a bare number - see the
     * comment on the assertion below for what this bounds and why it must
     * track SAND_EXPLODE_INITIAL_SPEED/SAND_IMPULSE_SPEED_RAMP rather than
     * assume whatever value they happened to hold when this was written.
     * +5 is slack for the loop itself: the ramp guarantees a roll with a
     * zero numerator by this many steps, but that roll's failure is what
     * actually drops the entry, so the step AT max_lifetime can still
     * succeed on a small nonzero `speed` one decrement shy of zero - see
     * step_impulses()'s own comment on the roll happening before the
     * decay is applied. */
    const int max_lifetime =
        (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) /
        SAND_IMPULSE_SPEED_RAMP;
    const int steps = max_lifetime + 5;

    int max_x = 1;

    for (int trial = 0; trial < 16; trial++) {
        sand_clear(&s);

        /* A single grain already in open air - nothing above, below or
         * beside it - so gravity's sweep claims it on literally every
         * step from the first, which is the worst case for the identity
         * check: if step_impulses() cannot re-acquire a grain gravity just
         * moved, this entry dies on turn one and the grain falls dead
         * straight down from then on. */
        sand_set(&s, 1, 1, SAND_FIRST_SHADE);
        /* Centre one cell to the left, radius 1: (1,1) is the RIGHT
         * neighbour, so the only way it ever gains x is the flight pass -
         * gravity here only ever pulls straight down, column 1. */
        sand_explode(&s, 0, 1, 1);

        for (int i = 0; i < steps; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        /* THE CURVATURE ITSELF, not just sustained motion: `steps` is
         * past ceil(SAND_EXPLODE_INITIAL_SPEED / SAND_IMPULSE_SPEED_RAMP),
         * the fixed step count at which `speed` is guaranteed to have
         * ramped all the way to zero - see SAND_EXPLODE_INITIAL_SPEED's own
         * comment in sand.h. Once that happens, rng_chance() with a zero
         * numerator can never succeed again, so the entry MUST have been
         * dropped by now, on every single one of these 16 independent
         * rolls of the dice - not "probably", not "on average", but always,
         * regardless of what any of them individually rolled. This is
         * exactly the guarantee the old SAND_BLAST_DECAY could not make:
         * a fixed chance every turn only ever shrinks the ODDS of still
         * being airborne, it never actually bounds how long that can
         * last. Checking impulse_count directly, rather than inferring
         * "stopped flying" from where the grain ended up on the board, is
         * what makes this a check of the RAMP'S OWN TERMINATION rather
         * than a check of gravity having settled it - a grain wedged
         * against something would keep its x unchanged too, for a
         * completely different reason. */
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
            "flight must have ended within a fixed, deterministic step "
            "count once speed ramps to zero - not merely become "
            "improbable, as the old fixed-chance decay left it");

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_SAND && x > max_x) {
                    max_x = x;
                }
            }
        }
    }

    /* Gravity alone could never move any of these eight grains sideways
     * at all - straight down is the only direction it ever pulls on its
     * own. Any of the eight ending up past x=1 proves the outward push
     * survived past the very first step, which is exactly where the old
     * identity check would have dropped it the moment gravity claimed the
     * grain out from under it - this is the exact regression that made a
     * crater with no arc. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, max_x,
        "a flying grain must keep drifting outward across more than one "
        "step, not lose its push the instant gravity also touches it");
}

/* --- KIND_STATIC support agreeing with the drift, not merely CELL_IS_EMPTY
 * on the one cell straight below (bd - "a thrown static chunk settles too
 * eagerly") -------------------------------------------------------------- */

/* THE CORE BUG, IN ITS SIMPLEST FORM: a thrown KIND_STATIC chunk directly
 * over a powder column. can_impulse_enter() (sand.c) only ever refuses
 * KIND_STATIC - a powder is not static, so the gravity-drift ("AIRBORNE
 * SOLIDS FALL TOO", step_impulses(), sand.c) has always been willing to
 * swap the falling chunk straight into the sand beneath it, one row per
 * step. The OLD settled check disagreed with its own drift: it asked only
 * CELL_IS_EMPTY() on that same straight-down cell, saw sand (not empty),
 * and declared the chunk settled - dropping it from impulse tracking after
 * a single row of fall, even though the very next step's drift would have
 * happily swapped it another row down had it still been tracked.
 *
 * TWO TESTS NOW, NOT ONE - device feedback for rung 4 ("a thrown chunk
 * entering a powder bank does not stop") added SAND_IMPULSE_SINK_MIN_SPEED
 * (sand.h), which narrows can_impulse_enter_gravity_ward() (sand.c, the one
 * predicate the drift and this settled check both call) so a SPENT
 * KIND_STATIC entry may only continue into a genuinely CELL_IS_EMPTY()
 * cell. The single test that used to live here drove a speed-0 entry -
 * spent from its very first step, by design, to isolate drift-vs-settle
 * disagreement from any lateral push - and asserted it reached the bottom
 * row. That assertion is now exactly backwards for a spent entry: rung 4
 * says a spent chunk over a packed bank settles right there instead of
 * swapping through the whole thing. Splitting keeps BOTH halves of the
 * history alive rather than picking one: an ENERGETIC chunk must still
 * sink all the way through - the original bug this section guards against,
 * a chunk settling after only one row - while a SPENT one must now rest
 * instead of sinking forever, this rung's own new guarantee. Neither test
 * touches the OTHER concern (drift/settle agreement on the CANDIDATE list
 * itself), which test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_
 * of_settling_and_both_eventually_land, below, still covers on its own.
 *
 * BOTH SCENES FILL THE ENTIRE ROW THE CHUNK SITS OVER, not just the one
 * column beneath it, and both call sand_clear() first rather than trusting
 * `cells` to already be blank - two things the original single test got
 * away without. Neither matters for an ENERGETIC entry, which can swap
 * through any non-static occupant regardless of what is either side of it,
 * but a SPENT entry can now be turned aside by an EMPTY diagonal exactly
 * as readily as it is stopped by a full one: a single narrow column (the
 * original scene's own shape) leaves both diagonal neighbours of a spent
 * chunk empty, and CELL_IS_EMPTY() there is true, so the chunk slides
 * sideways into open air and free-falls the rest of the way down - not
 * "no opening anywhere," which is what the spent test actually needs to
 * exercise. Measured directly: the narrow-column shape under the spent
 * test below settles at row 5, not row SETTLE_TOP_ROW, purely from that
 * sideways escape route - whatever residue `cells` happened to still hold
 * from an earlier test decided which column it escaped into. Filling every
 * column removes both problems at once.
 *
 * "STILL SINKS ALL THE WAY THROUGH" IS NO LONGER THE CLAIM, as of a later
 * rung (step_impulses(), sand.c: impulse_charge_displacement(), shared by
 * this gravity-drift and the ordinary push move) - device evidence was a
 * sand pyramid with a stone column beside it: a chunk thrown from CLOSE
 * moved sand visibly, one thrown from FAR tunnelled straight through and
 * ejected nothing, penetrating exactly as deep regardless of how far it
 * had already flown. The cause was this test's own "energetic chunk sinks
 * to the bottom" behaviour, taken to its logical extreme: the drift used
 * to charge NO drag at all, so an energetic KIND_STATIC mover swapped
 * through an entire packed bank for free, one row a step, with nothing to
 * show for it on the way. Charging the same drag (and firing the same
 * TRANSFER) at the drift site that the push site already charged is what
 * fixes the device bug, and it means an energetic chunk over a packed
 * powder bank now stops within the first few layers instead - see
 * SAND_IMPULSE_DRAG_POWDER_SHIFT's own comment in sand.h for the measured
 * push-site figure ("dirt stopping inside one cell is the point, not an
 * overshoot") this now matches from the drift side too. The test below is
 * renamed and its assertion rewritten to say so; the SPENT test right
 * after it needed no change at all, since a spent (speed 0) entry never
 * reaches this call in the first place - can_impulse_enter_gravity_ward()
 * already refuses it a non-empty candidate before any drag would be
 * charged. */
enum { SETTLE_COL = 3, SETTLE_TOP_ROW = 0 };

static void test_an_energetic_static_chunk_over_a_powder_bank_now_stops_within_the_first_few_layers(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 255,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* MEASURED: row 2 (of 8), deterministically, for this exact scene - two
     * rows of fall before SAND's own drag (density 60, doubled twice by
     * SAND_IMPULSE_DRAG_POWDER_SHIFT) saturates the chunk's speed to 0. Not
     * pinned to that exact row here: what this test actually guards against
     * is regressing toward either extreme this rung sits between - staying
     * at the rim (drag charged so hard, or so early, that the chunk never
     * moves at all) or sinking to the bottom again (drag silently not
     * charged at the drift site, reopening the exact device bug this rung
     * fixes) - so a range comfortably inside those two failure shapes is
     * the more durable check. */
    const int landed_row = first_row_holding(MAT_STONE);
    TEST_ASSERT_GREATER_THAN_MESSAGE(SETTLE_TOP_ROW, landed_row,
        "an ENERGETIC thrown KIND_STATIC chunk must still fall AT LEAST one "
        "row into the bank - a chunk stuck exactly at the rim would mean "
        "drag is now charged so eagerly the drift cannot move it at all, "
        "which is not this rung's own fix either");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(H / 2, landed_row,
        "and it must stop well short of the bottom (measured row 2 of 8) - "
        "reaching anywhere near H-1 again would mean the drift's own "
        "displacement is silently paying no drag, exactly the device bug "
        "(a far-thrown chunk tunnels through a sand pyramid and ejects "
        "nothing, however deep, regardless of how far it travelled to get "
        "there) impulse_charge_displacement() exists to close");
}

static void test_a_spent_static_chunk_rests_on_a_powder_bank_instead_of_sinking_forever(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 0,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SETTLE_TOP_ROW, first_row_holding(MAT_STONE),
        "a SPENT thrown KIND_STATIC chunk - speed 0, below SAND_IMPULSE_"
        "SINK_MIN_SPEED from its very first step, forcing every push-roll "
        "to fail so the scene is driven entirely by gravity-drift plus the "
        "settled check - must rest right where it landed once every "
        "gravity-ward candidate is genuinely occupied, rather than "
        "swapping through the entire bank the way an unconditional "
        "gravity-drift once did (device feedback: a thrown chunk entering "
        "a powder bank did not stop)");
}

/* --- PENETRATION MUST STOP BEING DISTANCE-DEPENDENT ---------------------
 *
 * A SECOND, SEPARATE BUG from the ones above - device evidence was a sand
 * pyramid with a stone column beside it: blasted so its chunks flew into
 * the pyramid, a chunk thrown from CLOSE moved sand visibly, one thrown
 * from FAR tunnelled straight through and ejected nothing, penetrating
 * exactly as deep regardless of how far it had already flown.
 *
 * THE CAUSE: the gravity-drift move just above (the "AIRBORNE SOLIDS FALL
 * TOO" block, step_impulses(), sand.c) performs a swap that DISPLACES a
 * cell, and until impulse_charge_displacement() existed it charged no drag
 * and fired no transfer - only the push move at the bottom of that same
 * loop did either, and only when rolled_move (whose own chance IS
 * entry.speed) succeeds. A chunk near a blast arrives fast, so its push
 * rolls mostly succeed and it pays for what it hits; a chunk that has
 * flown a long way arrives slow and mostly FALLING, so nearly every
 * displacement it makes is the drift's own then-free swap - it tunnels in
 * silently, no drag to stop it and no transfer to show for it, however far
 * it had already travelled to get there.
 *
 * ISOLATED FROM THE PUSH SITE ON PURPOSE - `DIR_UP` queues the chunk's own
 * push in a direction that can only ever nudge it into open air above
 * where it started, so every displacement this scene measures is
 * attributable to the gravity-drift's own swap and nothing else; a push
 * direction that could ALSO usefully displace into the bed (down, or
 * sideways through a full-width bed) let the push site's own,
 * already-correct accounting quietly cover for a broken drift in an
 * earlier draft of this exact scene, masking precisely the bug this test
 * exists to catch.
 *
 * A FLOOR DIRECTLY UNDER THE BED, NOT SOME FIXED ROW FAR BELOW IT - a bed
 * with open air beneath it is not a bed, it is a second falling body: an
 * earlier draft rested the bed on a floor 175 rows below it and measured
 * the bed free-falling in lockstep with the chunk chasing it, at the same
 * one-row-a-step rate, never actually touched at all (a "penetration" of
 * 200+ rows into air the bed had long since vacated - a scene bug, not a
 * sand.c one).
 *
 * MEASURED, 40 seeds, FAR_SINK_DISTANCE (50 rows of open-air fall before
 * the chunk ever reaches the bed): mean penetration 6.775 rows into a
 * 20-row bed with the drift's own charge hard-mutated back out (the
 * live-mutation check this file uses elsewhere), 0.000 with this rung's
 * fix - the bed is FAR_SINK_BED_DEPTH deep specifically so a regression
 * back to the old, unconditional drift has real room to bury the chunk in
 * rather than hitting a floor that would mask it. */
#define FAR_SINK_W 20
#define FAR_SINK_BED_DEPTH 20
#define FAR_SINK_DISTANCE 50
#define FAR_SINK_H (FAR_SINK_DISTANCE + FAR_SINK_BED_DEPTH + 10)
#define FAR_SINK_SEEDS 40
static long far_sink_penetration_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)FAR_SINK_SEEDS; seed++) {
        const int bed_y0    = FAR_SINK_DISTANCE;
        const int bed_y1    = bed_y0 + FAR_SINK_BED_DEPTH - 1;
        const int floor_row = bed_y1 + 1;
        sand_t g;
        memset(cells, 0, (size_t)FAR_SINK_W * FAR_SINK_H);
        sand_init(&g, cells, FAR_SINK_W, FAR_SINK_H, seed);
        sand_enable_impulses(&g, buf, FAR_SINK_W * FAR_SINK_H);

        for (int x = 0; x < FAR_SINK_W; x++) {
            sand_set(&g, x, floor_row, STONE);
        }
        for (int y = bed_y0; y <= bed_y1; y++) {
            for (int x = 0; x < FAR_SINK_W; x++) {
                sand_set(&g, x, y, SAND);
            }
        }

        const int mover_x = FAR_SINK_W / 2;
        sand_set(&g, mover_x, 0, STONE);
        enum { DIR_UP = 4 };
        sand_impulse_dislodge(&g, mover_x, 0, DIR_UP, 255,
                              SAND_IMPULSE_SPEED_RAMP);

        /* PAST THE DETERMINISTIC FLIGHT-TIME BOUND - see this file's own
         * max_lifetime derivation elsewhere for the same reasoning: a
         * regression back to the unconditional drift can keep this entry
         * tracked (and sinking) for up to a full ramp's worth of steps
         * after it enters the bed, on top of the open-air fall it already
         * took to get there. A shorter budget here (measured directly)
         * still shows the chunk mid-sink in some seeds under the bug,
         * understating exactly the depth this test exists to catch. */
        for (int i = 0; i < FAR_SINK_DISTANCE + 150; i++) {
            sand_step(&g, 0, 1000, 0);
        }

        int landed_row = -1;
        for (int y = 0; y < floor_row && landed_row < 0; y++) {
            for (int x = 0; x < FAR_SINK_W; x++) {
                if (CELL_MATERIAL(sand_at(&g, x, y)) == MAT_STONE) {
                    landed_row = y;
                    break;
                }
            }
        }
        /* landed_row < bed_y0 means the chunk stopped short of the bed
         * entirely (its own ramp exhausted before it ever arrived) - zero
         * penetration, nothing to add, rather than a negative figure. */
        if (landed_row >= bed_y0) {
            total += (landed_row - bed_y0);
        }
    }
    return total;
}

static void test_a_static_chunk_thrown_far_still_stops_shallow_in_the_bed_it_hits(void)
{
    uint8_t *cells = malloc((size_t)FAR_SINK_W * FAR_SINK_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "far-sink grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(FAR_SINK_W * FAR_SINK_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "far-sink impulse queue must fit in what the framebuffer leaves");

    const long total = far_sink_penetration_total(cells, buf);

    free(buf);
    free(cells);

    TEST_ASSERT_LESS_THAN_MESSAGE(FAR_SINK_SEEDS * 2, total,
        "a KIND_STATIC chunk thrown 50 rows of open air before it ever "
        "reaches a packed bed must still stop within a couple of rows of "
        "the surface, same as one thrown from right beside it - measured "
        "a mean of 6.775 rows deep (271 summed across 40 seeds) with the "
        "gravity-drift's own drag/transfer charge hard-mutated out, "
        "against 0 with this rung's fix: the whole point of "
        "impulse_charge_displacement() is that a chunk cannot silently "
        "bury itself just because it arrived slow and mostly falling "
        "instead of fast and mostly pushing");
}

/* THE SAME SPLIT, OVER A LIQUID - and the user's own explicit call (see
 * step_impulses()'s own comment on the gravity-drift block, sand.c): an
 * ENERGETIC chunk sinks into water (and lava, its own dedicated test
 * below) rather than resting on the surface the way an ordinary,
 * never-thrown KIND_STATIC cell would (see
 * test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder,
 * this file, for that other half of the distinction) - while a SPENT one
 * now rests on the surface instead, the KNOWN, CHOSEN TRADE named in
 * SAND_IMPULSE_SINK_MIN_SPEED's own comment (sand.h): this floor is not
 * kind-aware, so a spent chunk stalls on a liquid exactly as it does on a
 * powder, not only several cells down as an energetic one would. */
static void test_an_energetic_static_chunk_still_sinks_into_water_instead_of_resting_on_its_surface(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* CELL_MAKE(MAT_WATER, MASS_MAX), NOT THE WATER MACRO - the WATER
     * macro is only half mass (variant 8 of MASS_MAX's own 15), which
     * leaves ordinary liquid equalisation room to reshuffle it into a
     * genuinely empty cell somewhere in the column within a step or two,
     * exactly the sideways-escape problem this section's own top comment
     * describes for a spent entry. A fully packed column has no such gap
     * to find. */
    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 255,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_STONE),
        "an ENERGETIC thrown KIND_STATIC chunk over a water column must "
        "still sink all the way to the bottom rather than stopping at the "
        "surface - the old drift's own liquid exclusion stays deleted, "
        "and rung 4's new SPENT narrowing must not apply to anything that "
        "still has push left");
}

static void test_a_spent_static_chunk_still_sinks_through_water_to_the_bottom(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* CELL_MAKE(MAT_WATER, MASS_MAX), NOT THE WATER MACRO - see the
     * energetic test just above for why the half-mass WATER macro leaves
     * ordinary equalisation room to open a genuine gap on its own. */
    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 0,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_STONE),
        "a SPENT thrown KIND_STATIC chunk over a water column must still "
        "sink all the way down - the settle rule IS kind-aware now, and "
        "only packed grain holds an exhausted chunk up. A fluid parts "
        "around a solid whether or not the solid has energy left, and on "
        "device a chunk stalled mid-pool read as wrong where sinking to "
        "the floor had always looked right");
}

/* THE INVARIANT GUARD. Deleting the drift's liquid exclusion is only safe
 * because the move it gates is a SWAP, not an overwrite - four lines below
 * the can_impulse_enter() check in step_impulses(), `gdisplaced` is read
 * out of the target cell and written back into the entry's OLD cell, the
 * same move_to() trick every other kind here already relies on. A lava
 * cell a thrown chunk swaps into does not stop existing, it changes which
 * cell it occupies. This test is what turns that claim from an argument
 * into a measurement.
 *
 * MASS, NOT CELL COUNT, IS THE EXACT INVARIANT - see mass_of()'s own
 * comment for why: a liquid's cell count is free to change even under
 * ordinary, unrelated movement (one full cell splitting into several
 * shallower ones as it spreads is not a leak), while the sum of every
 * lava cell's own CELL_VARIANT is not. MEASURED, not assumed: an early
 * version of this test asserted the cell count unchanged too, and it
 * failed - 48 before, 55 after - which looked at first like exactly the
 * duplication this test exists to catch. It was not. Instrumenting the
 * scene (a full material-by-material dump, kept out of the final test)
 * showed mass staying exactly 720 throughout, and the extra 7 cells were
 * two genuinely unrelated, pre-existing mechanics, neither one anything
 * to do with step_impulses(): the swap itself displaces lava upward one
 * cell at a time as the chunk sinks, ordinary sand_step_liquids()
 * equalisation then spreads that displaced lava sideways across the row
 * (see mass_of()'s own comment on this being expected for ANY liquid,
 * lava included), and once spread that lava's own top surface sits
 * exposed to open air for the first time - which is exactly what lets
 * reaction_t.flare (sand_reactions.c, "the flame licking up off it") roll
 * and place a handful of fresh MAT_FIRE cells above it, cells that were
 * never lava mass in the first place. Cell count is therefore checked
 * only as "did not go DOWN" below - a genuine deletion would still be
 * caught, a legitimate spread or an unrelated flare above the exposed
 * surface would not be mistaken for one. The row assertion first confirms
 * the scene actually exercises a sink (not "stayed on the surface because
 * the exclusion is still there," which would make the conservation
 * assertions vacuous).
 *
 * SPEED 255, NOT 0, SINCE RUNG 4 - this scene originally drove a speed-0
 * entry, harmless before SAND_IMPULSE_SINK_MIN_SPEED existed because the
 * gravity-drift was unconditional regardless of speed. It is not harmless
 * now: a spent entry (speed 0, below that floor) only continues into a
 * genuinely CELL_IS_EMPTY() cell (can_impulse_enter_gravity_ward(),
 * sand.c), and this scene's own pool has no empty cell anywhere for it to
 * find, so a speed-0 chunk here would settle at row 0 and never exercise
 * the sink this test measures at all. 255 keeps the chunk ENERGETIC for
 * the whole 8-row fall (the plain ramp only costs 2 speed a step), which
 * is what the swap-conservation claim below was always actually about -
 * see SAND_IMPULSE_SINK_MIN_SPEED's own comment (sand.h) for why an
 * energetic chunk still sinks into a liquid exactly as before. */
static void test_a_thrown_static_chunk_conserves_lava_mass_on_sink(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { POOL_TOP = 2 };
    sand_set(&s, 3, 0, STONE);
    for (int y = POOL_TOP; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    const long lava_mass_before  = mass_of(&s, W, H, MAT_LAVA);
    const int  lava_count_before = count_of(MAT_LAVA);

    sand_impulse_dislodge(&s, 3, 0, 0, 255, SAND_IMPULSE_SPEED_RAMP);

    /* H * 3, not H: how many steps a chunk needs to clear the pool now
     * depends on SAND_IMPULSE_CELLS_PER_STEP_DIVISOR, and this scene only
     * needs ENOUGH steps, never exactly H. A budget tied to the grid was
     * really a budget tied to that constant without saying so. */
    for (int i = 0; i < H * 3; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(POOL_TOP, first_row_holding(MAT_STONE),
        "the chunk must actually have sunk past the pool's own surface for "
        "the conservation assertions below to mean anything - stopping at "
        "or above POOL_TOP would mean the exclusion never actually got "
        "exercised by this scene");

    TEST_ASSERT_EQUAL_INT_MESSAGE(lava_mass_before,
        mass_of(&s, W, H, MAT_LAVA),
        "total lava mass must be exactly conserved - a thrown chunk "
        "swapping into a lava cell relocates that cell, it does not "
        "destroy it, and this is the measurement that makes dropping the "
        "drift's old liquid exclusion safe rather than merely assumed");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(lava_count_before, count_of(MAT_LAVA),
        "and no lava cell may have been deleted outright either - checked "
        "as 'did not go down' rather than 'stayed identical', because a "
        "displaced lava cell legitimately splitting across more, shallower "
        "cells as it spreads (ordinary sand_step_liquids() equalisation,"
        " same as any liquid) is not a deletion - see this test's own top "
        "comment for the measurement that ruled out a genuine duplication");
}

/* A SUPPORT THAT IS ITSELF IN FLIGHT MEANS WAIT, NOT SETTLE. Two KIND_
 * STATIC chunks, A directly above B, both dislodged into impulse tracking
 * this same step - A queued first, so step_impulses()'s own loop gives A
 * its turn before B gets a chance to move out from under it. A's three
 * gravity-ward candidates are ALL blocked: straight down is B (still
 * sitting exactly where it started, its own turn not yet taken), and both
 * diagonals are walled off with WOOD (KIND_STATIC, but never dislodged -
 * an ordinary immovable wall, not a competing impulse entry) so A cannot
 * simply slide around the problem. Naively, "no opening this step" reads
 * as settled - but B is not a wall, it is a chunk that is about to move
 * out of the way in this very same step (open floor below it, columns 4-7
 * clear). Settling A right there would freeze it stacked in mid-air over
 * B forever, since nothing else in this engine ever revisits a settled
 * KIND_STATIC cell.
 *
 * WOOD, not more STONE, for the side walls - so a stray STONE cell can
 * never be mistaken for one of the two chunks this test is actually
 * tracking when scanning the board afterward. */
static void test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_of_settling_and_both_eventually_land(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { COL = 3, TOP_ROW = 2, BOTTOM_ROW = 3 };

    sand_set(&s, COL, TOP_ROW, STONE);
    sand_set(&s, COL, BOTTOM_ROW, STONE);
    sand_set(&s, COL - 1, BOTTOM_ROW, WOOD);
    sand_set(&s, COL + 1, BOTTOM_ROW, WOOD);

    /* A first, THEN B - see this test's own top comment for why the order
     * matters: it is what puts A's turn before B's in step_impulses()'s
     * own loop. */
    sand_impulse_dislodge(&s, COL, TOP_ROW, 0, 0, SAND_IMPULSE_SPEED_RAMP);
    sand_impulse_dislodge(&s, COL, BOTTOM_ROW, 0, 0, SAND_IMPULSE_SPEED_RAMP);

    /* One step is enough to show the bug: A's own support check runs
     * before B has moved, so a settle-on-first-failure design drops A
     * from tracking on literally this first call. */
    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, COL, TOP_ROW)),
        "the upper chunk must still be exactly where it started after "
        "just one step - it was blocked on every side this step, so it "
        "must have WAITED, not moved; the regression this guards against "
        "is not motion, it is losing impulse TRACKING while waiting");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, s.impulse_count,
        "and it must still be TRACKED - dropped from s->impulse_buf here "
        "means settled, which is exactly what must not happen while its "
        "own support (the lower chunk) is itself still in flight and "
        "about to move out from under it");

    /* Now run it out. Both chunks have a clear path to the bottom - B
     * immediately, A one step behind once B has moved - so both must
     * eventually come to rest with nothing left flying. */
    for (int i = 0; i < 4 * H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
        "both chunks must eventually land - if the upper one were still "
        "frozen waiting on a support that can never again change, "
        "tracking would never clear");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, COL, TOP_ROW)),
        "and the upper chunk must not have settled back at its OWN "
        "starting cell either - both chunks had a clear run to the "
        "bottom, so ending up frozen at the top is itself a sign "
        "something settled prematurely");

    /* BOTH REACHED THE FLOOR - checked by ROW alone, not by column too.
     * An earlier version of this test asserted the upper chunk lands
     * DIRECTLY ON TOP of the lower one (same column, one row up) and it
     * failed: measurement (a full-board dump, not left in this test)
     * showed both chunks side by side in the bottom row instead, columns
     * 2 and 3. That is not a bug - once B has landed at (COL, H-1), A's
     * OWN straight-down candidate at that same cell is blocked (B is
     * KIND_STATIC and, once settled, no longer a tracked entry for
     * impulse_index_still_tracked() to find), so the SAME shared
     * candidate list this whole feature is built on tries the next
     * candidate in line - the diagonal slide - exactly as it would for
     * any other obstacle, and that cell is open. Sliding around a landed
     * neighbour instead of freezing above it is the correct behaviour of
     * the shared predicate, not a special case; this test only ever
     * existed to prove neither chunk freezes mid-air, not to pin an exact
     * final column, so it checks exactly that. */
    int found_rows[2];
    int found = 0;
    for (int y = 0; y < H && found < 2; y++) {
        for (int x = 0; x < W && found < 2; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_STONE) {
                found_rows[found++] = y;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, found,
        "both KIND_STATIC chunks must still exist somewhere on the board "
        "- this test is about WHEN they settle, not whether either one "
        "survives");
    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, found_rows[0],
        "both chunks had a clear run all the way to the floor once B "
        "moved out from under A, so both must have reached it - one "
        "still short of H - 1 would mean it froze somewhere on the way "
        "down instead of following once its support cleared");
    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, found_rows[1],
        "same for the second chunk found - see the previous assertion");
}

/* --- the other half of the distinction: IMPULSE earns the sinking, being
 * a solid does not ------------------------------------------------------- */

/* AN ORDINARY, NEVER-THROWN KIND_STATIC CELL gets none of the above - no
 * sand_impulse()/sand_impulse_dislodge() call here at all, so this cell is
 * never added to s->impulse_buf and step_impulses() never looks at it. The
 * main sweep (sand_step(), sand.c) skips KIND_STATIC outright by design -
 * that is what makes stone or glass hold its shape - so a static cell
 * placed directly on top of a liquid or a powder, with no impulse behind
 * it, has no mechanism in this engine that could ever move it at all, let
 * alone sink it. This is the test that fails if a future change moves the
 * new can_impulse_enter()-based sinking rule out of step_impulses() and
 * into ordinary movement instead - see the drift block's own comment
 * (step_impulses(), sand.c) for why that generalisation is explicitly not
 * what this feature is. */
static void test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 2, 0, STONE);
    for (int y = 1; y < H; y++) {
        sand_set(&s, 2, y, WATER);
    }

    sand_set(&s, 5, 0, STONE);
    for (int y = 1; y < H; y++) {
        sand_set(&s, 5, y, SAND);
    }

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, 2, 0)),
        "an ordinary KIND_STATIC cell resting on a liquid, with no "
        "impulse behind it, must not move at all - it is IMPULSE that "
        "earns the sinking this feature adds, not merely being a solid");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, 5, 0)),
        "same for a powder - an ordinary KIND_STATIC cell must still just "
        "sit exactly where it was placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
        "nothing here was ever thrown, so nothing should ever have been "
        "queued into impulse tracking in the first place");
}

/* --- Rung 1: medium drag on a thrown KIND_STATIC chunk --------------------
 *
 * step_impulses() (sand.c) now charges extra `speed` loss at the move site,
 * per non-empty cell a KIND_STATIC mover displaces, proportional to that
 * cell's own `density` (impulse_drag_of(), sand_priv.h) - a thrown wall
 * chunk should cross a bank of dirt slower than it crosses open air, not
 * teleport through it at the same rate. KIND_STATIC only; see SAND_IMPULSE_
 * DRAG_POWDER_SHIFT's own comment (sand.h) for why liquid and powder
 * movers are out of scope for this rung.
 *
 * TWO SHAPES OF TEST, NOT ONE. The two below actually measure horizontal
 * distance travelled - the thing the feature exists to change, and the
 * thing a name like "travels less far" has to mean, following the
 * interfacial-drag idiom above (test_water_does_not_drill_into_oil_when_
 * tilted): averaged over PLOW_SEEDS seeds, because "a single run of a
 * chaotic scene is not evidence" is exactly as true here as it is there.
 * The two after that are single-step, single-displaced-cell pins on the
 * exact arithmetic (speed == 255 - SAND_IMPULSE_SPEED_RAMP - density) -
 * asserting on s.impulse_buf[0].speed directly
 * is honest ONLY there, because for that one test the formula itself IS
 * the claim, not a stand-in for it.
 *
 * A FIRST VERSION OF THE DISTANCE TESTS FLOORED THE FLIGHT ROW DIRECTLY
 * BENEATH ITSELF, which was wrong twice over. First: with no vertical
 * opening ever available, a single missed push roll - a roughly 1-in-256
 * event every step, `rolled_move`, step_impulses() - ended the flight
 * immediately instead of costing one step's worth of distance the way it
 * would in the open field sand_explode() actually throws into, turning
 * "distance travelled" into the stopping point of a race against a
 * shrinking success chance rather than a measurement of drag. Second, and
 * worse: a wide packed medium is not inert while it waits to be hit -
 * try_slide_impl() (sand_priv.h) draws one s->rng number for EVERY resting
 * powder cell EVERY step regardless of whether it moves, so a wide bank of
 * dirt measurably shifts which random numbers the impulse's own roll
 * consumes relative to open air. Confirmed by running that version against
 * the UNMODIFIED file: dirt already "won" against water there, averaged
 * over seeds and all, with no drag mechanism to explain it.
 *
 * THE FIX IS OPEN SPACE, NOT A FLOOR UNDER THE FLIGHT ROW. `medium` fills
 * ONLY row 0, the row the chunk is thrown along; every row below it is
 * empty, down to a single STONE floor at the very bottom of a grid tall
 * enough to give real room to fall. A missed push roll now finds a real
 * opening on the very next gravity-drift check (`has_opening`,
 * step_impulses()) and simply keeps falling instead of settling - exactly
 * the "still airborne" path the code already has for this. The medium
 * itself is not inert either: with nothing under IT, ordinary gravity
 * (the ordinary sweep, which runs before step_impulses() every step) pulls
 * unsupported dirt or water down into the open space below just as the
 * falling chunk arrives there, so the chunk keeps encountering displaceable
 * medium for several steps of its fall rather than only the one row it
 * started in - confirmed by tracing a single seed step by step: the chunk
 * left row 0 after its very first move, and the cell it swapped into on
 * EVERY later step was already the falling medium material, not open air.
 * That is a real, mechanism-driven difference between dirt and water
 * (materials that fall) and open air (nothing to fall), not an artefact of
 * scene geometry.
 *
 * PLOW_STEPS IS SHORT ON PURPOSE - long enough to cover the fall to the
 * floor (PLOW_H - 1 rows) plus a few, not long enough to let post-landing
 * wandering (still tracked, still rolling sideways through open ground with
 * nothing left to displace) dilute the signal. Measured both ways, back
 * when this rung first shipped at SAND_IMPULSE_DRAG_SHIFT 2: at 80 steps the
 * three averages over PLOW_SEEDS seeds sat at roughly 14.2 (air) / 8.9
 * (water) / 7.3 (dirt) cells and the unmodified file's own equivalent
 * numbers already clustered within a few percent of each other by chance,
 * which was the right shape pre-fix but left too little room for genuine
 * noise on either side of the assertions. At 15 steps the unmodified file's
 * three averages sat at 12.5 / 12.0 / 12.6 - indistinguishable, exactly as
 * they must be with no drag mechanism to tell dirt from water from air -
 * while the fixed file's separated out to 12.5 / 8.8 / 7.3, the same
 * mechanism-driven ordering, just measured where it was least diluted.
 *
 * RESHARPENED, NOT JUST RECONFIRMED, AT DRAG_SHIFT 0 - device feedback
 * (rung 4) said even that separation was not enough: a chunk still crossed
 * a real bank of dirt too far to read as an impact. At the new figure,
 * this same 15-step measurement now sits at 12.5 (air) / 3.6 (dirt) / 5.2
 * (water) - roughly the "first few layers" and "sink partway into a pool"
 * the device report asked for, not the old 7.3 / 8.8 that merely ordered
 * dirt behind water correctly. PLOW_STEPS itself did not need to change -
 * the separation only got sharper. */
#define PLOW_W 40
#define PLOW_H 10
#define PLOW_SEEDS 32
#define PLOW_STEPS 15

/* `medium` fills row 0 (unless empty) for `mover`, thrown right from
 * (1, 0), to fly and fall through; a single STONE floor spans the very
 * bottom row. STATIC movers go through sand_impulse_dislodge()
 * (unconditional, matching this suite's other thrown-chunk tests);
 * anything else through plain sand_impulse(), which a KIND_STATIC cell
 * would just refuse. `cells` is memset here rather than by each caller,
 * matching test_water_does_not_drill_into_oil_when_tilted's own
 * drag_cells above - a fresh sand_init() alone never clears the array.
 *
 * A DIRT medium must be built with a DRY variant (0 .. SOIL_DRY_TONES-1,
 * material.h) - the callers below use 0, the same tone every other dirt
 * scene in this file uses. Anything from SOIL_DRY_TONES up is WET, and wet
 * soil carries moisture that percolates and dries while the measurement is
 * running: passing variant 8 here measures a medium that is quietly
 * changing state under the chunk, and moves dirt's own averaged distance
 * from 7.25 cells to 6.66. Drag itself reads per-material density and does
 * not care, so nothing fails - the scene just stops being the one the test
 * name claims. */
static void plow_build(sand_t *g, uint8_t *cells, impulse_t *buf, int buf_max,
                       uint32_t seed, material_id_t mover, cell_t medium)
{
    memset(cells, 0, (size_t)PLOW_W * PLOW_H);
    sand_init(g, cells, PLOW_W, PLOW_H, seed);
    sand_enable_impulses(g, buf, buf_max);

    for (int x = 0; x < PLOW_W; x++) {
        sand_set(g, x, PLOW_H - 1, STONE);
    }
    sand_set(g, 1, 0, CELL_MAKE(mover, 8));
    if (!CELL_IS_EMPTY(medium)) {
        for (int x = 2; x < PLOW_W; x++) {
            sand_set(g, x, 0, medium);
        }
    }

    enum { DIR_RIGHT = 2 };
    if (material_by_id((material_id_t)mover)->kind == KIND_STATIC) {
        sand_impulse_dislodge(g, 1, 0, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);
    } else {
        sand_impulse(g, 1, 0, DIR_RIGHT, 255);
    }
}

/* The mover's current column, wherever it ended up - flying or settled,
 * anywhere above the floor row. Scans every row EXCEPT the floor (which is
 * STONE across the whole width and would otherwise be mistaken for the
 * mover on any seed where it has not yet reached the bottom). */
static int plow_mover_x(sand_t *g)
{
    for (int y = 0; y < PLOW_H - 1; y++) {
        for (int x = 0; x < PLOW_W; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == MAT_STONE) {
                return x;
            }
        }
    }
    return -1;
}

/* Summed, not averaged, across PLOW_SEEDS seeds 1..PLOW_SEEDS - matching
 * test_water_does_not_drill_into_oil_when_tilted's own `total` shape
 * above. Every caller uses the SAME seed set (1..PLOW_SEEDS), so a
 * comparison between two calls is apples to apples. */
static long plow_total_distance(uint8_t *cells, impulse_t *buf, cell_t medium)
{
    long total = 0;
    for (uint32_t k = 1; k <= (uint32_t)PLOW_SEEDS; k++) {
        sand_t g;
        plow_build(&g, cells, buf, 4, k, MAT_STONE, medium);
        for (int i = 0; i < PLOW_STEPS; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += plow_mover_x(&g) - 1;
    }
    return total;
}

static void test_a_thrown_chunk_travels_less_far_through_dirt_than_through_air(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long air_total  = plow_total_distance(cells, buf, 0);
    const long dirt_total = plow_total_distance(cells, buf, CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(air_total, dirt_total,
        "a thrown KIND_STATIC chunk must travel less far, summed across "
        "the same seeds, through a bank of dirt than through open air - "
        "today it crosses both at exactly the same rate and simply "
        "teleports the dirt behind it, which is the reported bug this "
        "rung fixes");
}

/* THE ENERGY EXIT'S OWN PIN (finding 1, bd esp32c6-w2h - an adversarial
 * architecture review of step_impulses()). push_count (the DISTANCE
 * budget) is computed once, from the speed the mover carried BEFORE this
 * step's own drag ever charges - so a chunk that pays most of its speed on
 * hop 0 still had every remaining hop of that budget to spend, drag or no
 * drag, because nothing inside the loop ever asked whether there was any
 * energy left to justify another one. Measured before this rung existed
 * (32 seeds, plow_total_distance(), the same harness the test above uses):
 * dirt penetration averaged 3.0 cells, where SAND_IMPULSE_DRAG_POWDER_
 * SHIFT's own comment states the intent as "stop at the rim" - roughly
 * one. `PLOW_SEEDS * 2` as the threshold asserts an average comfortably
 * under 2 cells, a wide margin below the pre-fix ~3.0 and above the
 * roughly-1.0 target, so this does not double as a tuning pin for
 * whatever exact figure the drag sweep below eventually lands on. */
static void test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long dirt_total = plow_total_distance(cells, buf, CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(PLOW_SEEDS * 2, dirt_total,
        "a chunk plowing into packed dirt must stop within about one cell "
        "of the rim, ON AVERAGE - not several. Before the energy exit, "
        "push_count's own DISTANCE budget was the hop loop's only exit "
        "besides a wall, so a mover that spent nearly all its speed on the "
        "very first hop still took every remaining hop that budget "
        "allowed, at whatever near-zero speed drag left it. Drag charging "
        "a hop must be able to end the push before push_count is "
        "exhausted, not merely slow the mover down while it keeps moving");
}

static void test_a_thrown_chunk_travels_less_far_through_dirt_than_through_water(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long water_total = plow_total_distance(cells, buf, CELL_MAKE(MAT_WATER, MASS_MAX));
    const long dirt_total  = plow_total_distance(cells, buf, CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(water_total, dirt_total,
        "drag must be ordered by density, not a flat penalty - dirt (62) "
        "is denser than water (30), so summed across the same seeds it "
        "must charge more speed per cell displaced and leave the chunk "
        "travelling less far");
}

/* THE FORMULA ITSELF, single step, single displaced cell - the one place in
 * this rung where asserting on s.impulse_buf[0].speed directly is honest
 * rather than a stand-in for the thing the feature actually does, because
 * here the arithmetic IS the claim. A floor under the row (matching
 * test_a_flying_water_grain_does_not_swap_into_dirt_in_its_path above)
 * keeps ordinary gravity from pulling the dirt cell (or the mover) out of
 * the row before the impulse gets its one step.
 *
 * impulse_count IS 2, NOT 1, NOW - rung 3's TRANSFER (step_impulses()'s own
 * comment at the transfer site) queues a second entry for the struck dirt
 * cell itself once this scene's own numbers clear SAND_IMPULSE_TRANSFER_
 * MIN_SPEED: the mover's post-drag speed here is 255 - SAND_IMPULSE_SPEED_
 * RAMP - dirt's own density, comfortably above 64.
 * Entry 0 is still the mover - the deferred transfer is appended only AFTER
 * this loop's own compaction finishes (step_impulses()'s own top comment),
 * so it lands at entry 1, after everything this test actually pins. */
static void test_a_thrown_chunk_loses_speed_proportional_to_the_density_it_displaces(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_DIRT, 0));
    /* A wall one cell past the target, so the entry moves EXACTLY one
     * cell this step. This pin is about what ONE displacement costs;
     * since an entry can now cover several cells a step, an open lane
     * would have it paying several ramps and the number here would
     * stop meaning what the test says it means. */
    sand_set(&s, SX + 2, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255, so the mover (entry 0) losing tracking would "
        "mean the scene itself is broken - and rung 3's transfer adds "
        "exactly one more entry (the struck dirt cell, entry 1) whenever "
        "the mover's post-drag speed clears SAND_IMPULSE_TRANSFER_MIN_"
        "SPEED, which this scene's own numbers do");
    const uint8_t expected = (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP -
                             impulse_drag_of(CELL_MAKE(MAT_DIRT, 0)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "speed lost displacing a single dirt cell must equal exactly the "
        "plain ramp plus impulse_drag_of()'s own density-derived cost - "
        "this is the formula step_impulses() actually implements, not an "
        "inequality standing in for it");
}

/* THE SAME PIN, KIND_POWDER THIS TIME: a thrown grain pays the identical
 * density-scaled drag a thrown chunk does - see SAND_IMPULSE_DRAG_POWDER_
 * SHIFT's own comment in sand.h for when powders joined drag's scope (they
 * were briefly out; this comment used to say so and was stale by the time
 * this rung's own name says otherwise). impulse_count is 2 for the same
 * transfer reason as the test just above - see its own comment. */
static void test_a_thrown_powder_grain_pays_drag_displacing_dirt(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, CELL_MAKE(MAT_SAND, 8));
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_DIRT, 0));
    /* A wall one cell past the target, so the entry moves EXACTLY one
     * cell this step. This pin is about what ONE displacement costs;
     * since an entry can now cover several cells a step, an open lane
     * would have it paying several ramps and the number here would
     * stop meaning what the test says it means. */
    sand_set(&s, SX + 2, ROW, STONE);
    sand_impulse(&s, SX, ROW, DIR_RIGHT, 255);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255, so the mover (entry 0) losing tracking would "
        "mean the scene itself is broken - and rung 3's transfer adds "
        "exactly one more entry (the struck dirt cell, entry 1) whenever "
        "the mover's post-drag speed clears SAND_IMPULSE_TRANSFER_MIN_"
        "SPEED, which this scene's own numbers do");
    const uint8_t expected = (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP -
                             impulse_drag_of(CELL_MAKE(MAT_DIRT, 0)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "a KIND_POWDER mover pays the same density-scaled drag a "
        "KIND_STATIC one does - a thrown grain is most of what a blast "
        "actually puts in the air, so leaving powders out made the whole "
        "mechanism imperceptible on the device even though it worked");
}

/* THE SCOPE PIN, now that powders are in: the boundary is LIQUIDS, and it
 * has to stay where it is. Water and acid decay geometrically
 * (SAND_SPLASH_SPEED_DECAY_SHIFT) instead of on the plain ramp, and the
 * splash and cascade features are tuned around that shape - charging them
 * drag on top would move a tuned feature nobody asked to move. A liquid
 * mover only ever displaces another liquid (can_impulse_enter(), sand.c),
 * so water into water is the whole of the case. */
static void test_a_thrown_liquid_grain_pays_no_drag_displacing_water(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_impulse(&s, SX, ROW, DIR_RIGHT, 255);

    sand_step(&s, 0, 1000, 0);

    /* Not == 1, unlike the two pins above: this is the only one of the
     * three whose mover is a liquid, and the liquid passes run BEFORE the
     * flight pass every step - splash_displace() (sand_liquid.c) queues
     * impulses of its own, so the buffer legitimately holds more than the
     * one this test put there. Ours is still entry 0: it was queued first,
     * and step_impulses()'s compaction keeps surviving entries in order. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    /* Its own geometric decay, applied once PER CELL of travel - an entry
     * at full speed covers more than one, and the decay is charged for each
     * the same way the ramp is for everything else. Derived from the
     * constants rather than restated, so retuning how far a step reaches
     * cannot quietly turn this pin into a different claim. */
    const int liquid_cells = 1 + (int)255 / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;
    unsigned expect_speed = 255u;
    for (int c = 0; c < liquid_cells; c++) {
        expect_speed -= expect_speed >> SAND_SPLASH_SPEED_DECAY_SHIFT;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        (int)expect_speed, s.impulse_buf[0].speed,
        "a KIND_LIQUID mover must lose exactly its own geometric decay and "
        "no drag term at all - drag stops at liquids on purpose, and this "
        "is what says so");
}

/* A THROWN GRAIN BOUNCES OFF A WALL TOO, not just a dislodged chunk - the
 * same reason powders had to be let into drag. A blast against a stone
 * wall puts far more sand in the air than it ever dislodges stone, and
 * every one of those grains used to stall against the wall and sit there
 * until its flight aged out. Thrown right into a wall with the floor
 * carrying on beneath it: the arc is wall + floor-corner, which dominance-
 * quantises to a normal of `left` and reflects head-on back to `left`,
 * paying the head-on half. */
static void test_a_thrown_powder_grain_bounces_off_a_wall_instead_of_waiting(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2, DIR_LEFT = 6 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, CELL_MAKE(MAT_SAND, 8));
    sand_set(&s, TX, ROW, STONE);
    sand_impulse(&s, SX, ROW, DIR_RIGHT, 255);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "a blocked entry waits rather than being dropped, so the grain "
        "must still be tracked whether or not it bounced");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_LEFT, s.impulse_buf[0].dir,
        "a thrown KIND_POWDER grain must reflect off the wall it hits, "
        "not keep its original heading and stall against it for the rest "
        "of its flight");
    TEST_ASSERT_EQUAL_INT_MESSAGE((255 - SAND_IMPULSE_SPEED_RAMP) >> 1,
        s.impulse_buf[0].speed,
        "and it must pay the head-on half of its remaining speed for the "
        "bounce, exactly as a thrown chunk does");
}

/* OPEN AIR MUST COST NOTHING BUT THE RAMP - the pin that says
 * sand_explode()'s own swept tuning (SAND_IMPULSE_SPEED_RAMP,
 * SAND_EXPLODE_CORE_DIVISOR, sand.h) did not move when drag arrived, and
 * the reason the two distance tests above can compare a medium against air
 * at all. Structural rather than a measured figure: a blast throws most of
 * its grains through empty space, so if a future change ever makes drag a
 * flat per-move charge instead of a density-scaled one, every explosion in
 * the app changes shape at once and this is what says so. The same one-step
 * shape as the two pins above, with an EMPTY cell ahead instead of dirt. */
static void test_a_thrown_chunk_displacing_nothing_loses_only_the_plain_ramp(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    /* The ramp is charged PER CELL of travel, and an entry at full speed
     * covers several - so the pin is ramp times the cells it was entitled
     * to, derived from the constants rather than restated. The claim is
     * unchanged: open air costs the ramp and nothing else, no drag, because
     * there was nothing there to displace. */
    const int air_cells = 1 + (255 - SAND_IMPULSE_SPEED_RAMP) /
                              SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(255 - SAND_IMPULSE_SPEED_RAMP * air_cells,
        s.impulse_buf[0].speed,
        "a chunk moving into an EMPTY cell must lose exactly the plain "
        "ramp and not one speck more - drag is charged per cell displaced, "
        "so flight through open air has to stay byte-for-byte what it was "
        "before this rung existed");
}

/* --- Rung: multi-cell push, SAND_IMPULSE_CELLS_PER_STEP_DIVISOR -----------
 *
 * Before this rung, every displacing move in step_impulses() - the push
 * roll's own move included - advanced an entry exactly one cell per
 * successful roll, identical to how far the ordinary gravity sweep moves a
 * falling grain in that same step. An impulse could therefore never outrun
 * gravity: a horizontal throw sank at close to 45 degrees, and ejecta
 * thrown off a powder volume could only ever reposition material, never
 * visibly leave it. See SAND_IMPULSE_CELLS_PER_STEP_DIVISOR's own comment
 * (sand.h) for the formula and the measured before/after numbers.
 *
 * TWO CLAIMS, TWO TESTS - a change like this is only really pinned by
 * BOTH ends of it: the negative case (nothing changes below the divisor)
 * right below, and the positive case (several cells at once, above it)
 * just after. */

#define SUBDIV_W 40
#define SUBDIV_H 80
#define SUBDIV_STEPS 60
/* One under SAND_IMPULSE_CELLS_PER_STEP_DIVISOR - the highest speed that
 * still computes 1 + speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR == 1. */
#define SUBDIV_SPEED (SAND_IMPULSE_CELLS_PER_STEP_DIVISOR - 1)

/* THE NEGATIVE CASE, swept over many steps rather than pinned as a single
 * arithmetic value: SUBDIV_SPEED is comfortably below SAND_IMPULSE_BOUNCE_
 * MIN_SPEED and SAND_IMPULSE_TRANSFER_MIN_SPEED, so a one-step scene like
 * the drag pins above would only ever exercise the plain, undramatic
 * "roll then maybe move" path - which is exactly what needs sweeping,
 * since a bug that let push_cells creep above 1 at low speed would not
 * show up in a single sample. Run for SUBDIV_STEPS steps and check EVERY
 * one of them: the mover must never move more than one cell in a single
 * step, matching exactly what every entry already did before SAND_IMPULSE_
 * CELLS_PER_STEP_DIVISOR existed.
 *
 * A FULL FLOOR keeps the KIND_STATIC gravity-drift (the "AIRBORNE SOLIDS
 * FALL TOO" block, step_impulses(), sand.c) from ever touching X: gravity
 * is straight down here, so the drift's own first candidate is always
 * straight down too, and a floor spanning the grid's full width keeps that
 * candidate open for every one of SUBDIV_STEPS steps (the grid is tall
 * enough, SUBDIV_H, that the mover never gets anywhere near it) - so any
 * horizontal movement observed below can only be this rung's own push,
 * never the drift confusing the measurement the way it would near a floor
 * or wall (see the diagonal-drift case test_an_energetic_static_chunk_
 * over_a_powder_bank... exercises elsewhere in this file). ramp is passed
 * as 0 so `speed`, and so push_cells, stays fixed at SUBDIV_SPEED for
 * every step of the sweep - what is being pinned is the formula at one
 * fixed speed, not its decay. */
static void test_a_sub_divisor_speed_impulse_never_moves_more_than_one_cell_a_step(void)
{
    uint8_t *cells = malloc((size_t)SUBDIV_W * SUBDIV_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "sub-divisor sweep grid must fit in what the framebuffer leaves");
    /* Only ever one entry tracked (the lone mover, never near enough to a
     * wall or the floor to spawn a TRANSFER or CASCADE follow-up) - a small
     * stack array, not a grid-sized malloc(), same as this file's other
     * single-mover scenes. */
    impulse_t buf[8];

    sand_t g;
    memset(cells, 0, (size_t)SUBDIV_W * SUBDIV_H);
    sand_init(&g, cells, SUBDIV_W, SUBDIV_H, 4242u);
    sand_enable_impulses(&g, buf, 8);

    enum { SX = 1, SY = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < SUBDIV_W; x++) {
        sand_set(&g, x, SUBDIV_H - 1, STONE);
    }
    sand_set(&g, SX, SY, STONE);
    sand_impulse_dislodge(&g, SX, SY, DIR_RIGHT, SUBDIV_SPEED, 0);

    int prev_x = SX;
    long total_moved = 0;
    for (int i = 0; i < SUBDIV_STEPS; i++) {
        sand_step(&g, 0, 1000, 0);

        TEST_ASSERT_EQUAL_INT_MESSAGE(1, g.impulse_count,
            "the mover must still be tracked every step of this sweep - it "
            "starts far from the floor and every wall, so nothing here "
            "should ever drop or settle it before SUBDIV_STEPS is up");

        const int x = (int)((unsigned)g.impulse_buf[0].index %
                            (unsigned)SUBDIV_W);
        const int dx = x - prev_x;
        char msg[192];
        snprintf(msg, sizeof msg,
                 "step %d: a speed-%d entry (one under SAND_IMPULSE_CELLS_"
                 "PER_STEP_DIVISOR) moved %d cells in this one step - it "
                 "must move at most one, exactly as every entry did before "
                 "that constant existed", i, SUBDIV_SPEED, dx);
        TEST_ASSERT_TRUE_MESSAGE(dx == 0 || dx == 1, msg);
        total_moved += dx;
        prev_x = x;
    }

    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, total_moved,
        "the push roll must have succeeded at least once across "
        "SUBDIV_STEPS steps at speed SUBDIV_SPEED, or this sweep never "
        "actually exercised the one-cell-per-step path it claims to pin");
}

/* THE POSITIVE CASE - a full-speed push clears SEVERAL cells in a single
 * step, the entire point of SAND_IMPULSE_CELLS_PER_STEP_DIVISOR. Same
 * one-step, single-scene shape as the drag pins above (a full floor
 * beneath keeps the KIND_STATIC gravity-drift from ever touching X, so any
 * horizontal movement observed here is this rung's own push and nothing
 * else) - open air ahead, so nothing is displaced and no TRANSFER entry
 * gets queued alongside the mover (contrast the dirt-displacing pins
 * above, whose impulse_count is 2 for exactly that reason). */
static void test_a_full_speed_static_chunk_moves_several_cells_in_one_push(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky, and "
        "open air ahead means no TRANSFER entry should appear either");

    const int expected_cells = 1 + (255 - SAND_IMPULSE_SPEED_RAMP) /
                                   SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;
    const int expected_index = ROW * W + (SX + expected_cells);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected_index, s.impulse_buf[0].index,
        "a full-speed KIND_STATIC push through open air must cover several "
        "cells in one step, not one - this is the whole point of "
        "SAND_IMPULSE_CELLS_PER_STEP_DIVISOR (sand.h)");
}

/* --- Rung 2: reflection off solids, with restitution ----------------------
 *
 * step_impulses()'s blocked branch (sand.c) now turns a KIND_STATIC mover's
 * "wait against the wall" into "bounce off it, along the reflection of its
 * own travel direction about the blocking surface's approximate normal" -
 * blocker_normal() reads the wall, reflect_off_normal() turns that into a
 * new ring direction, and restitution (half speed on a head-on reflection,
 * a quarter on a glancing one) plus the SAND_IMPULSE_BOUNCE_MIN_SPEED floor
 * (sand.h) are what keep this from reopening the bounce-in-place pathology
 * step_impulses()'s own roll comment records the history of. Water/acid
 * keep their existing flat 180 flip untouched - this only widens
 * KIND_STATIC.
 *
 * QUANTISED BY DOMINANCE, NOT BY SIGN - blocker_normal()'s own comment
 * (sand.c) has the full account, but the shape of it matters here too: a
 * first version summed the arc's negated unit vectors and took the SIGN of
 * each axis independently, which degenerates for every diagonal `dir` (the
 * arc's centre cell is always covered - that is what "blocked" means - and
 * on a diagonal throw it alone already pins both signs to the mover's own
 * reverse, so a diagonal bounce could only ever reverse, never glance,
 * whatever the wall looked like). Comparing MAGNITUDES instead - the axis
 * with the larger push wins outright, both count only when they are close
 * - fixes that: a chunk thrown down-right into a flat floor (down AND
 * down-right covered, right open) now reads a normal of pure "up", the
 * vertical push winning over the single-cell horizontal one, and glances
 * up-right rather than reversing. Axis-aligned throws still always
 * reverse off a flat wall - a flat wall's own normal has no other axis to
 * weigh against - which is the correct physics, not a remaining gap. See
 * test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table
 * below for the full 8-direction x 4-configuration ground truth this was
 * checked against. */

/* THE EXHAUSTIVE ARC TABLE - blocker_normal()/reflect_off_normal()'s own
 * counterpart to test_cover_primitive_matches_the_exhaustive_shape_table
 * above: a primitive whose whole point is a geometric rule checked at only
 * one or two hand-picked directions is not really tested, since exactly
 * that gap is what let a degenerate quantisation (sign instead of
 * dominance - see blocker_normal()'s own comment in sand_priv.h) through
 * for every diagonal direction while every axis-aligned one kept working.
 * Every one of the 8 ring directions, crossed with all 4 ways the 3-cell
 * arc (L = flank at dir-1, C = centre at dir, R = flank at dir+1) can be
 * covered - C is always covered, since that is the blocked branch's own
 * precondition (see blocker_normal()'s comment) - is 32 cases, and this
 * checks the primitive DIRECTLY, the same way the cover-mask table above
 * calls cover_mask()/covered_at() directly rather than only ever
 * exercising them through a stochastic scene.
 *
 * THE GROUND TRUTH BELOW IS NOT RE-DERIVED FROM THE FORMULA THIS PINS -
 * it would be circular, and it is exactly how the original sign-quantised
 * bug shipped clean: every hand test agreed with itself. These 32 values
 * were computed independently (by hand, cross-checked component by
 * component against blocker_normal()'s own dominance rule and
 * reflect_off_normal()'s r = d*|n|^2 - 2(d.n)n) before being written down
 * here - if a future change to either primitive disagrees with a row
 * below, the change is what is wrong, not the table. */
static const int blocker_arc_table[8][4][2] = {
    /* dir 0: down. Axis-aligned - every config reverses (normal up,
     * reflect up), the flat-wall case: there is no other axis to weigh
     * a corner against. */
    { { 4, 4 }, { 4, 4 }, { 4, 4 }, { 4, 4 } },
    /* dir 1: down-right. */
    { { 5, 5 },   /* -C-: normal up-left,  reflect up-left  (head-on)   */
      { 4, 3 },   /* LC-: normal up,       reflect up-right (glances)  */
      { 6, 7 },   /* -CR: normal left,     reflect down-left           */
      { 5, 5 } }, /* LCR: normal up-left,  reflect up-left  (head-on)  */
    /* dir 2: right. Axis-aligned - always reverses. */
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    /* dir 3: up-right. */
    { { 7, 7 },   /* -C-: down-left,  down-left              */
      { 6, 5 },   /* LC-: left,       up-left                */
      { 0, 1 },   /* -CR: down,       down-right             */
      { 7, 7 } }, /* LCR: down-left,  down-left               */
    /* dir 4: up. Axis-aligned - always reverses. */
    { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
    /* dir 5: up-left. */
    { { 1, 1 },   /* -C-: down-right, down-right              */
      { 0, 7 },   /* LC-: down,       down-left               */
      { 2, 3 },   /* -CR: right,      up-right                */
      { 1, 1 } }, /* LCR: down-right, down-right               */
    /* dir 6: left. Axis-aligned - always reverses. */
    { { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 } },
    /* dir 7: down-left. */
    { { 3, 3 },   /* -C-: up-right,   up-right                */
      { 2, 1 },   /* LC-: right,      down-right              */
      { 4, 5 },   /* -CR: up,         up-left                 */
      { 3, 3 } }, /* LCR: up-right,   up-right                 */
};

static void test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table(void)
{
    fixture();
    sand_clear(&s);
    const int cx = W / 2, cy = H / 2;

    for (int dir = 0; dir < 8; dir++) {
        const int *l = ring_dir(dir - 1);
        const int *c = ring_dir(dir);
        const int *r = ring_dir(dir + 1);

        for (int cfg = 0; cfg < 4; cfg++) {
            const bool has_l = (cfg & 1) != 0;
            const bool has_r = (cfg & 2) != 0;

            for (int i = 0; i < 8; i++) {
                const int *d = ring_dir(i);
                sand_set(&s, cx + d[0], cy + d[1], CELL_EMPTY);
            }
            sand_set(&s, cx + c[0], cy + c[1], STONE);   /* C: always covered */
            if (has_l) {
                sand_set(&s, cx + l[0], cy + l[1], STONE);
            }
            if (has_r) {
                sand_set(&s, cx + r[0], cy + r[1], STONE);
            }

            const int expected_normal  = blocker_arc_table[dir][cfg][0];
            const int expected_reflect = blocker_arc_table[dir][cfg][1];

            const int normal = blocker_normal(&s, cx, cy, dir);
            char msg[160];
            snprintf(msg, sizeof msg,
                     "dir=%d cfg=%d (L=%d R=%d): blocker_normal() must "
                     "match the ground-truth table exactly", dir, cfg,
                     has_l, has_r);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected_normal, normal, msg);

            const int reflect = reflect_off_normal(dir, normal);
            snprintf(msg, sizeof msg,
                     "dir=%d cfg=%d (L=%d R=%d): reflect_off_normal() must "
                     "match the ground-truth table exactly", dir, cfg,
                     has_l, has_r);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected_reflect, reflect, msg);
        }
    }
}

/* HEAD-ON REVERSAL - single step, exact arithmetic (the formula itself is
 * the claim, following rung 1's own drag-pin tests): a KIND_STATIC chunk
 * thrown square into a flat, wide floor must end up travelling the exact
 * OPPOSITE direction, at exactly half its already-ramped speed. The floor
 * spans the full grid width so all three of blocker_normal()'s arc cells
 * are covered and the quantised sum reduces to pure "up" - the head-on
 * case (see this section's own worked-table cross-check in the code
 * review, table row 1). */
static void test_a_thrown_chunk_reverses_direction_bouncing_off_a_flat_floor(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 3, SX = 4, DIR_DOWN = 0, DIR_UP = 4 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_DOWN, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_UP, s.impulse_buf[0].dir,
        "a chunk thrown straight down into a flat, wide floor must bounce "
        "back the exact opposite way it came, not merely lose speed while "
        "still travelling down - today it just waits there forever, same "
        "direction, until its flight ages out");
    const uint8_t expected = (uint8_t)((255 - SAND_IMPULSE_SPEED_RAMP) >> 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "a head-on bounce must cost exactly half the already-ramped speed "
        "- restitution's head-on branch - not the plain ramp alone and not "
        "the glancing branch's quarter");
}

/* GLANCING DEFLECTION - same single-step, exact-arithmetic shape as the
 * head-on test above, and now the SPEC'S OWN scene: a chunk thrown
 * DOWN-RIGHT into a FLAT floor (not a corner - blocker_normal()'s
 * dominance rule is what makes a flat floor produce a glance here rather
 * than a reverse; see this section's own intro comment and
 * blocker_normal()'s in sand.c). The mover sits on stone directly below it
 * AND down-right of it (both part of the floor), with open space to its
 * right - the vertical push from two covered cells dominates the single
 * horizontal one, so the normal reads as pure "up" rather than "up-left",
 * and reflecting a down-right travel off a pure-up normal sends the chunk
 * UP-RIGHT: neither reversed (that would be up-left) nor still travelling
 * down-right. This is the test that proves the normal is real rather than
 * a dressed-up 180 flip. */
static void test_a_thrown_chunk_deflects_off_a_flat_floor_instead_of_reversing(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 3, SX = 4, DIR_DOWN_RIGHT = 1, DIR_UP_RIGHT = 3, DIR_UP_LEFT = 5 };
    sand_set(&s, SX - 1, ROW + 1, STONE);   /* floor: down-left of the mover -
                                              * not in blocker_normal()'s arc
                                              * for this dir, only here so
                                              * gravity-drift finds no
                                              * opening and cannot be what
                                              * moves the mover */
    sand_set(&s, SX,     ROW + 1, STONE);   /* floor: down - arc flank L */
    sand_set(&s, SX + 1, ROW + 1, STONE);   /* floor: down-right - arc
                                              * centre C, and the move
                                              * target that blocks this
                                              * throw */
    /* (SX + 1, ROW), right - arc flank R - is left open on purpose: a
     * FLAT floor, not a corner, is the whole point of this scene. */
    sand_set(&s, SX, ROW, STONE);           /* the mover itself */
    sand_impulse_dislodge(&s, SX, ROW, DIR_DOWN_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(DIR_UP_LEFT, s.impulse_buf[0].dir,
        "must not be a flat 180 - DIR_UP_LEFT is straight back the way it "
        "came, which is what a dressed-up flip (or the water/acid rule "
        "this rung deliberately leaves alone) would produce here instead");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(DIR_DOWN_RIGHT, s.impulse_buf[0].dir,
        "must not keep travelling in its original direction either - it "
        "was genuinely blocked, this is not a missed collision");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_UP_RIGHT, s.impulse_buf[0].dir,
        "reflecting a down-right travel about a flat floor's normal must "
        "send the chunk UP-RIGHT - the vertical push from the floor's two "
        "covered arc cells dominates the single horizontal one");
    const uint8_t ramped = (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP);
    const uint8_t expected = (uint8_t)(ramped - (ramped >> 2));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "a glancing (non-head-on) bounce must cost exactly a quarter of "
        "the already-ramped speed, not the head-on branch's half");
}

/* THE FLOOR GUARD - the pin against the bounce-in-place pathology
 * step_impulses()'s own roll comment records the history of: below
 * SAND_IMPULSE_BOUNCE_MIN_SPEED, a blocked KIND_STATIC entry must just
 * wait, exactly as it always has, not bounce at all. A deliberately
 * oversized `ramp` (not a realistic caller) is the cleanest way to land
 * the roll - which reads entry.speed BEFORE this step's own ramp - at its
 * reliable, near-certain value of 255, while still landing entry.speed
 * AFTER the ramp comfortably under the floor for this one step. */
static void test_a_low_speed_entry_below_the_bounce_floor_still_just_waits(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 3, SX = 4, DIR_DOWN = 0, BIG_RAMP = 230 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_DOWN, 255, BIG_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    const uint8_t expected_speed = (uint8_t)(255 - BIG_RAMP);
    TEST_ASSERT_TRUE_MESSAGE(expected_speed < SAND_IMPULSE_BOUNCE_MIN_SPEED,
        "fixture check: this scene must actually land under the floor, or "
        "the test proves nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_DOWN, s.impulse_buf[0].dir,
        "below SAND_IMPULSE_BOUNCE_MIN_SPEED a blocked entry must keep "
        "waiting in its ORIGINAL direction, exactly as it did before this "
        "rung existed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected_speed, s.impulse_buf[0].speed,
        "and must not pay any restitution either - the plain ramp only, "
        "same as the old plain wait, with no extra charge for a bounce "
        "that never happened");
}

/* GRID EDGE REFLECTS - sand_at()'s off-grid-is-STONE convention
 * (step_impulses()'s own comment above) has to fold into this bounce for
 * free, the same way it already folds into can_impulse_enter()'s own
 * blocking rule: a chunk thrown at the edge of the board must bounce off
 * it exactly as it would off a real wall there, not wait at the edge
 * forever. BEHAVIOURAL, not a whitebox pin - this is not one of the three
 * tests where asserting on impulse_buf directly is honest (see this file's
 * own drag-rung precedent), so this reads the BOARD instead.
 *
 * DELIBERATELY AIRBORNE, NOT RESTING ON A FLOOR AT THE EDGE ITSELF - a
 * first version of this test put the mover on a full-width floor directly
 * under it, the same shape test_a_thrown_chunk_reverses_direction_
 * bouncing_off_a_flat_floor uses. That measurably broke: with genuine
 * gravity-relative support right there, step_impulses()'s own SETTLED
 * check (its "SUPPORTED, NOT MERELY ROLLED" block) drops the entry from
 * tracking the very first time a single push-roll fails - usually the
 * NEXT step, since a bounce more than halves `speed` and the roll is
 * literally that number out of 256 - freezing it back at EDGE_X before it
 * ever gets a second chance to execute its new (bounced) direction. That
 * is a real, separate interaction (a chunk resting on solid ground gets
 * roughly ONE shot to skid before "settled" wins), not a rung 2 bug, and
 * it is why THIS scene instead drops the mover from open air well above a
 * distant floor: while genuinely still falling, a failed push-roll never
 * reads as settled (the gravity-relative candidates below it are open, so
 * step_impulses() keeps it tracked and falling every step, per the "A
 * SUPPORT THAT IS ITSELF IN FLIGHT" / has_opening logic - here it is
 * simpler still, the opening is just open air), so it keeps getting fresh
 * chances at its current direction on the way down - plenty of opportunity
 * for the post-bounce leftward push to actually fire before it lands.
 * Confirmed with a throwaway host probe (same production code, gcc -O1):
 * across 20 seeds this exact scene always ended up strictly left of
 * EDGE_X, settling in 13 to 15 steps. */
#define EDGE_W 8
#define EDGE_H 16
#define EDGE_SEEDS 8
#define EDGE_MAX_STEPS 60

static void test_a_chunk_bounces_off_the_grid_edge_instead_of_waiting_there_forever(void)
{
    uint8_t *edge_cells = malloc((size_t)EDGE_W * EDGE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(edge_cells,
        "grid-edge-bounce grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EDGE_W * EDGE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "grid-edge-bounce impulse queue must fit in what the framebuffer "
        "leaves");

    for (uint32_t k = 1; k <= (uint32_t)EDGE_SEEDS; k++) {
        memset(edge_cells, 0, (size_t)EDGE_W * EDGE_H);
        sand_t g;
        sand_init(&g, edge_cells, EDGE_W, EDGE_H, k);
        sand_enable_impulses(&g, buf, EDGE_W * EDGE_H);

        for (int x = 0; x < EDGE_W; x++) {
            sand_set(&g, x, EDGE_H - 1, STONE);
        }
        enum { EDGE_X = EDGE_W - 1, SY = 1, DIR_RIGHT = 2 };
        sand_set(&g, EDGE_X, SY, STONE);
        sand_impulse_dislodge(&g, EDGE_X, SY, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < EDGE_MAX_STEPS && g.impulse_count > 0) {
            sand_step(&g, 0, 1000, 0);
            steps++;
        }

        int fx = -1;
        for (int x = 0; x < EDGE_W; x++) {
            for (int y = 0; y < EDGE_H - 1; y++) {
                if (CELL_MATERIAL(sand_at(&g, x, y)) == MAT_STONE) {
                    fx = x;
                }
            }
        }
        char msg[320];
        snprintf(msg, sizeof msg,
                 "seed %u: fixture check - the thrown chunk must still be "
                 "on the board somewhere above the floor row, not vanished",
                 (unsigned)k);
        TEST_ASSERT_TRUE_MESSAGE(fx >= 0, msg);
        snprintf(msg, sizeof msg,
                 "seed %u: a chunk thrown at the grid edge must end up "
                 "STRICTLY LEFT of EDGE_X (%d) - the old behaviour left it "
                 "waiting exactly there, unmoved, for the rest of its "
                 "(linear-ramped, ~128-step) flight, because "
                 "can_impulse_enter() reads the off-grid target as STONE "
                 "and the plain wait never changes direction", (unsigned)k, EDGE_X);
        TEST_ASSERT_TRUE_MESSAGE(fx < EDGE_X, msg);
    }

    free(buf);
    free(edge_cells);
}

/* ANTI-PINBALL, BEHAVIOURAL - a chunk thrown into a fully closed stone box
 * must come to rest (s.impulse_count reaches 0) within a BOUNDED number of
 * steps, not rattle around the box indefinitely - the exact failure mode
 * step_impulses()'s own roll comment's reverted-attempt history warns a
 * bounce mechanism can reopen if left undamped and unfloored.
 *
 * BOX_MAX_STEPS (50) is measured against the ACTUAL 16 seeds this test
 * runs (k = 1..BOX_SEEDS, throw direction k % 8), not a separate sample -
 * a throwaway host probe (same production sand.c/sand.h/sand_priv.h this
 * test links, gcc -O1, no test-only shortcuts) ran exactly this scene at
 * exactly these seeds and measured settle-step counts of 3 to 27, worst
 * case 27 (seed 12, throw direction 4 - straight up). 50 is under 2x that
 * measured worst case - real headroom, not an order of magnitude padding
 * it can never trip.
 *
 * A WIDER SWEEP WAS ALSO RUN TO CHECK THE TAIL BEHAVIOUR THIS FIXED SET
 * MIGHT BE HIDING: 300,000 seeds, same box-and-throw shape, found a slow-
 * growing worst case (44 at seed 76, 47 at 2,532, 48 at 11,316, 49 at
 * 34,412, 53 at 49,844, 54 at 57,300, nothing higher in the remaining
 * ~242,700 seeds) - a heavy but visibly converging tail, always the same
 * "thrown straight up" direction. This test does not chase that tail: it
 * runs the same fixed 16 seeds every time (deterministic, no run-to-run
 * variance to guard against), and 50 is chosen with real margin over
 * THOSE seeds' own worst case, not the wider distribution's.
 *
 * THIS BOUND DOES NOT RELIABLY CATCH A RESTITUTION REGRESSION, AND SAYING
 * SO IS MORE HONEST THAN PRETENDING OTHERWISE. In a fully enclosed box,
 * unlike the two open-scene tests below, the entry bounces many times
 * before it can possibly settle, so the PRE-EXISTING linear ramp
 * (SAND_IMPULSE_SPEED_RAMP, sand.h - not this rung's own code) ends up
 * the dominant thing bounding settle time here, not restitution. Deleting
 * restitution's speed charge entirely (a throwaway mutation, reverted
 * immediately) moved this test's OWN 16-seed worst case from 27 to only
 * 30 - both comfortably under 50, so no bound in the "real headroom, not
 * an order of magnitude" range this rung's other two tests use can split
 * the two. What THIS bound DOES catch, confirmed the same way: deleting
 * the RAMP decrement entirely - simulating the literal reverted-attempt
 * pathology step_impulses()'s own comment describes, "never settle" -
 * broke this exact test (seed 4, throw direction 4, still rattling past
 * 50 steps, and past the old 150 too) while every other test in this rung
 * stayed silent about it. So this bound is a genuine backstop against
 * that specific historical failure mode, not a restitution pin - the two
 * open-scene tests below cover restitution instead, where it is actually
 * the dominant mechanism.
 * REUSES THE 8x8 GLOBAL FIXTURE ARRAYS (cells[], impulse_buf[]) rather
 * than a heap allocation - the box is exactly W x H, so nothing bigger is
 * needed, unlike the wider drag-rung and wall-notch scenes elsewhere in
 * this file. Averaged in the sense of "checked against every seed", not
 * summed - "a single run of a chaotic scene is not evidence", same idiom
 * as test_water_does_not_drill_into_oil_when_tilted above, but this
 * property (bounded, not a distance) is a per-seed pass/fail rather than
 * something to sum. */
#define BOX_SEEDS 16
#define BOX_MAX_STEPS 50

static void test_a_chunk_thrown_into_a_closed_box_comes_to_rest(void)
{
    for (uint32_t k = 1; k <= (uint32_t)BOX_SEEDS; k++) {
        memset(cells, 0, sizeof cells);
        sand_init(&s, cells, W, H, k);
        sand_enable_impulses(&s, impulse_buf, W * H);

        for (int x = 0; x < W; x++) {
            sand_set(&s, x, 0, STONE);
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = 0; y < H; y++) {
            sand_set(&s, 0, y, STONE);
            sand_set(&s, W - 1, y, STONE);
        }
        const int cx = W / 2, cy = H / 2;
        sand_set(&s, cx, cy, STONE);
        const int dir = (int)(k % 8);
        sand_impulse_dislodge(&s, cx, cy, dir, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < BOX_MAX_STEPS && s.impulse_count > 0) {
            sand_step(&s, 0, 1000, 0);
            steps++;
        }

        char msg[192];
        snprintf(msg, sizeof msg,
                 "seed %u, throw direction %d: still rattling after "
                 "BOX_MAX_STEPS (%d) steps in a fully closed box - this is "
                 "the bounce-in-place pathology this rung must not reopen",
                 (unsigned)k, dir, BOX_MAX_STEPS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count, msg);
    }
}

/* THE REGRESSION THAT MATTERS MOST - reflection must not turn ordinary
 * landing into bouncing. A chunk thrown straight down through open air
 * onto a plain floor must still come to rest close to where gravity alone
 * would put it (directly on the floor), within a bounded number of steps,
 * not just eventually.
 *
 * OPEN_MAX_STEPS and the expected landing row are both measured, same
 * probe methodology as BOX_MAX_STEPS above: across 64 seeds this exact
 * scene (a lone floor at the bottom of a tall, otherwise open grid, mover
 * dropped from near the top at full speed) landed on OPEN_H - 2 - directly
 * on the floor - every single time, whatever the settle time.
 *
 * RE-MEASURED, RAISED FROM 20 TO 52, alongside SAND_IMPULSE_CELLS_PER_
 * STEP_DIVISOR (sand.h) - a chunk now reaches the floor in far fewer
 * steps (this exact scene's fastest seed dropped from 7 to 4), but that is
 * not the whole story: the ordinary per-step ramp still only sheds
 * SAND_IMPULSE_SPEED_RAMP of speed per step regardless of how many cells
 * that step covered, so arriving in fewer steps also means arriving with
 * far more speed still on the clock - the reflection at the floor now has
 * more to dissipate, and does so over more steps of its own, not fewer.
 * The worst case in this same 64-seed sweep rose from 13 to 34 steps. 52
 * is over 1.5x that new worst case, the same margin convention the old
 * bound used over its own worst case. */
#define OPEN_W 8
#define OPEN_H 16
#define OPEN_SEEDS 8
#define OPEN_MAX_STEPS 52

static void test_a_chunk_dropped_on_flat_ground_still_settles_on_it(void)
{
    uint8_t *open_cells = malloc((size_t)OPEN_W * OPEN_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(open_cells,
        "open-floor settle grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(OPEN_W * OPEN_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "open-floor settle impulse queue must fit in what the framebuffer "
        "leaves");

    for (uint32_t k = 1; k <= (uint32_t)OPEN_SEEDS; k++) {
        memset(open_cells, 0, (size_t)OPEN_W * OPEN_H);
        sand_t g;
        sand_init(&g, open_cells, OPEN_W, OPEN_H, k);
        sand_enable_impulses(&g, buf, OPEN_W * OPEN_H);

        for (int x = 0; x < OPEN_W; x++) {
            sand_set(&g, x, OPEN_H - 1, STONE);
        }
        enum { SX = OPEN_W / 2, SY = 1, DIR_DOWN = 0 };
        sand_set(&g, SX, SY, STONE);
        sand_impulse_dislodge(&g, SX, SY, DIR_DOWN, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < OPEN_MAX_STEPS && g.impulse_count > 0) {
            sand_step(&g, 0, 1000, 0);
            steps++;
        }

        char msg[192];
        snprintf(msg, sizeof msg,
                 "seed %u: still airborne or bouncing after OPEN_MAX_STEPS "
                 "(%d) steps of an ordinary drop onto open, flat ground",
                 (unsigned)k, OPEN_MAX_STEPS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.impulse_count, msg);

        int landed_y = -1;
        for (int x = 0; x < OPEN_W; x++) {
            if (CELL_MATERIAL(sand_at(&g, x, OPEN_H - 2)) == MAT_STONE) {
                landed_y = OPEN_H - 2;
            }
        }
        snprintf(msg, sizeof msg,
                 "seed %u: the dropped chunk must rest directly on the "
                 "floor (row %d), not hover, sink through, or wander off "
                 "sideways to a different row", (unsigned)k, OPEN_H - 2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(OPEN_H - 2, landed_y, msg);
    }

    free(buf);
    free(open_cells);
}

/* A BRUSH-DRAWN WALL, NOT A CLEAN ONE-CELL ONE - clean walls have hidden
 * real shape-dependent bugs in this simulation before (see
 * test_lava_in_a_wall_notch_never_bursts above, and cover_mask()'s own
 * comment in sand_priv.h). Four overlapping stone discs, radius 2 to 4,
 * centres about 3 cells apart - the way a person actually draws a wall
 * with a round brush, notches and bulges included - rather than a flat
 * one-cell-thick line. A chunk thrown into it must still come to rest,
 * conserving itself exactly (nothing created or destroyed by a bounce),
 * within a bounded number of steps. Direction is deliberately NOT
 * asserted - the wall's own irregular shape makes the exact bounce path
 * uninteresting; conservation and boundedness are the properties that
 * matter here.
 *
 * WALL_MAX_STEPS (50) is measured the same way as BOX_MAX_STEPS above:
 * this exact scene, run across 64 seeds against the production code, took
 * 17 to 38 steps to settle, every seed conserving the thrown chunk
 * exactly. 50 is about 1.3x that measured worst case - and, same as
 * OPEN_MAX_STEPS above, deliberately tight rather than padded: the same
 * probe with restitution's speed charge temporarily deleted rose to a
 * 60-step worst case over the same 64 seeds, so a restitution regression
 * here trips this test too. */
#define WALL_W 24
#define WALL_H 16
#define WALL_SEEDS 16
#define WALL_MAX_STEPS 50

static void test_a_chunk_thrown_into_a_brush_drawn_wall_conserves_itself_and_settles(void)
{
    uint8_t *wall_cells = malloc((size_t)WALL_W * WALL_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wall_cells,
        "brush-wall settle grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(WALL_W * WALL_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "brush-wall settle impulse queue must fit in what the framebuffer "
        "leaves");

    for (uint32_t k = 1; k <= (uint32_t)WALL_SEEDS; k++) {
        memset(wall_cells, 0, (size_t)WALL_W * WALL_H);
        sand_t g;
        sand_init(&g, wall_cells, WALL_W, WALL_H, k);
        sand_enable_impulses(&g, buf, WALL_W * WALL_H);

        for (int x = 0; x < WALL_W; x++) {
            sand_set(&g, x, WALL_H - 1, STONE);
        }
        /* Four brush strokes, radius 2-4, centres ~3 apart - a hand-drawn
         * wall with real notches, not a flat line. */
        sand_spawn(&g, 8,  8, 3, MAT_STONE);
        sand_spawn(&g, 11, 7, 2, MAT_STONE);
        sand_spawn(&g, 14, 9, 4, MAT_STONE);
        sand_spawn(&g, 17, 6, 3, MAT_STONE);

        const int before = sand_count(&g);

        enum { SX = 2, SY = 2, DIR_RIGHT = 2 };
        sand_set(&g, SX, SY, STONE);
        sand_impulse_dislodge(&g, SX, SY, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < WALL_MAX_STEPS && g.impulse_count > 0) {
            sand_step(&g, 0, 1000, 0);
            steps++;
        }

        char msg[160];
        snprintf(msg, sizeof msg,
                 "seed %u: still rattling around the brush-drawn wall "
                 "after WALL_MAX_STEPS (%d) steps", (unsigned)k, WALL_MAX_STEPS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.impulse_count, msg);

        snprintf(msg, sizeof msg,
                 "seed %u: exactly one cell (the thrown chunk) must have "
                 "been added relative to the wall alone - a bounce must "
                 "never create or destroy a cell", (unsigned)k);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, sand_count(&g), msg);
    }

    free(buf);
    free(wall_cells);
}

/* THE REGRESSION GUARD. Every test above this line already existed the day
 * a real device reported: "in water nothing happens, in sand also no
 * holes, i can see some faint movement when near pixels they do move but
 * that's it." None of them caught it, because none of them detonated
 * somewhere with no adjacent empty cell anywhere inside the radius - a
 * packed bed, or a body of water - which is the one scene the plan's v1
 * "stop on any obstruction" rule could never move a single grain in: every
 * queued entry's very first target was already occupied, so every entry
 * died on turn one, and the mechanic was silently a no-op everywhere it
 * was actually supposed to matter.
 *
 * Packed on every side, deliberately, with only ONE piece of open space
 * anywhere on the board (row 0) and it nowhere near the blast: this is
 * exactly the scene that read as nothing happening. */
static void test_a_blast_in_a_packed_bed_opens_a_cavity_and_reaches_beyond_the_radius(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 4, 5, 2);

    /* A cavity exists - immediately, and independent of the flight pass,
     * the speed ramp, or a single step having run: sand_explode() fills
     * its core (here, the plus-shaped disc (4,5)/(3,5)/(5,5)/(4,4)/(4,6))
     * with fire before it ever queues an entry - see
     * SAND_EXPLODE_CORE_DIVISOR. An explosion flashes and leaves a plume; it
     * does not silently delete whatever was standing there. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 4, 5)),
        "the blast's own core must flash into fire, not repositioned "
        "grains still filling the same footprint");

    for (int i = 0; i < 30; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Grains reached beyond the original radius. Row 1 is 4 cells from the
     * centre, well outside radius 2, and started this test fully packed
     * (all 8 columns).
     *
     * The core is fire now, not a hole - fire is far LIGHTER than sand
     * (density 15 against sand's 60), and can_enter()'s ordinary "a denser
     * mover displaces a lighter fluid" rule, the same one that lets sand
     * sink through water or gas, applies here with no special-casing at
     * all: a sand grain directly above a fire cell simply swaps through
     * it via the main sweep, exactly as it would sink through smoke. That
     * turns out to be enough on its own - measured on a 20,000-seed sweep
     * with decay left OFF (this fixture's default), the row-1 disturbance
     * checked below appears on literally the first step, every time, with
     * no dependence on fire ever rising or decaying away first. So there
     * is no path for row 1 to stay fully packed except something above
     * the core swapping down through the fire that filled it, one cell at
     * a time, propagating exactly the way
     * test_undermining_a_sleeping_pile_collapses_it already proves a hole
     * propagates - and row 0 has nothing above it to refill whichever
     * column runs out of material first, so that column's row-1 cell is
     * left empty once things settle.
     *
     * Which column that turns out to be is NOT fixed to directly above
     * the centre: the core's own diagonal-adjacent cells (3,4) and (5,4)
     * are themselves queued flight entries (see SAND_EXPLODE_CORE_DIVISOR),
     * and retrying-until-clear (see step_impulses()'s "blocked means wait")
     * can walk the disturbance sideways by the time it reaches this far
     * up - column 4 collapsing was only ever the simplest of several
     * columns that could plausibly hollow out first. So this checks the
     * row generally rather than one hand-picked cell: some column in the
     * blast's own horizontal span must have given up material this far
     * out, not necessarily the one directly above where it started. A
     * "stop on any obstruction" rule with no filled core could never have
     * produced this from a fully packed bed at all, regardless of which
     * column ends up being the one that shows it. */
    int empty_in_row1 = 0;
    for (int x = 2; x <= 6; x++) {
        if (sand_at(&s, x, 1) == SAND_EMPTY) {
            empty_in_row1++;
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, empty_in_row1,
        "the disturbance must reach further than the blast radius itself - "
        "this is the exact scene that read as \"no holes\" on the panel");
}

/* THE SPECIFIC REGRESSION THAT JUST BIT US, pinned directly. A device
 * pass on the first real-radius detonation reported it as "barely
 * noticeable" with "solids barely move" - traced to sand_explode()'s
 * OLD scan order (dy-outer, dx-inner, top row to bottom row) handing an
 * undersized cap entirely to the disc's top nine or ten rows before the
 * scan ever reached the core or the lower half. Every test above this
 * line happens to detonate with a buffer at least as big as the disc
 * (impulse_buf[] is W*H, and an 8x8 grid's largest disc never comes
 * close), so none of them ever truncate at all - they would pass exactly
 * as they do now even with the old row-order scan restored, because
 * nothing was ever cut off for the order to be unfair ABOUT. This is the
 * one test in the file where the cap must actually bind for the fix to
 * be tested at all.
 *
 * axis_impulse_buf[8] is sized to exactly one full ring - see its own
 * comment above - so a radius-3 blast's immediate ring of 8 neighbours
 * fits with nothing to spare, and ring 2 and beyond never get a slot.
 * Under ring order that ring is EVERY direction at once - all four axes,
 * all four diagonals - so all four axis checks below must pass. Under
 * the old row order, the same cap of 8 is exhausted while still working
 * through the rows above the centre (dy = -3 contributes 1 candidate, dy
 * = -2 contributes 5, already 6 of the 8 slots, and dy = -1 supplies the
 * rest before the scan reaches dy = 0 at all) - so DOWN, LEFT and RIGHT
 * would all still be waiting for a slot that never comes, while UP alone
 * succeeds. A regression to row order would fail exactly three of these
 * four assertions, never all four and never zero - which is what makes
 * this a test of ORDER specifically, not merely of whether the cap
 * exists.
 *
 * Checked directly against the queue itself (s.impulse_count entries in
 * s.impulse_buf), not inferred from where anything ends up on the board -
 * inferring it from the board is what let the old bug ship in the first
 * place, since a test that only watches for "something moved" cannot
 * tell a fair ring from a lopsided crescent. ring_dir()'s own numbering
 * (sand_priv.h, not included here - this suite only sees the public dir
 * byte) is 0 down, 2 right, 4 up, 6 left; 1/3/5/7 are the diagonals
 * between them and are not checked here, since the four axes are the
 * ones whose presence or absence actually distinguishes ring order from
 * row order at this cap. */
static void test_a_blast_queues_impulses_on_every_side_of_the_centre(void)
{
    fixture();
    sand_enable_impulses(&s, axis_impulse_buf, 8);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 4, 4, 3);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, s.impulse_count,
        "the buffer holds exactly one ring's worth (8) and every one of "
        "a fully packed grid's neighbours qualifies, so all 8 slots must "
        "have been used");

    bool saw_up = false, saw_down = false, saw_left = false, saw_right = false;
    for (int i = 0; i < s.impulse_count; i++) {
        switch (s.impulse_buf[i].dir) {
        case 4: saw_up    = true; break;
        case 0: saw_down  = true; break;
        case 6: saw_left  = true; break;
        case 2: saw_right = true; break;
        default: break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_up,
        "an impulse above the centre must be queued - this direction "
        "worked even under the old bug, so its absence here would mean "
        "something else broke");
    TEST_ASSERT_TRUE_MESSAGE(saw_down,
        "an impulse below the centre must be queued - this is exactly "
        "the direction the old top-to-bottom scan order starved first, "
        "along with the entire core between it and the centre");
    TEST_ASSERT_TRUE_MESSAGE(saw_left,
        "an impulse left of the centre must be queued");
    TEST_ASSERT_TRUE_MESSAGE(saw_right,
        "an impulse right of the centre must be queued");
}

/* NOT a no-op any more, and deliberately so: an explosion in a vacuum
 * still flashes. sand_explode() fills its core with fire unconditionally
 * (see SAND_EXPLODE_CORE_DIVISOR) - occupied or already empty alike - so
 * detonating over nothing still lights the core; what stays a no-op is
 * everything BEYOND the core, since there is nothing there to queue a
 * flight entry for. This replaces the old
 * test_detonating_empty_space_is_a_no_op, which asserted exactly the
 * behaviour this round deliberately changed. */
static void test_detonating_empty_space_still_flashes_the_core(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_explode(&s, 4, 4, 3);

    /* Mirrors sand_explode()'s own `core_radius` in sand.c EXACTLY,
     * clamp included - not just the bare division. Plain `3 /
     * SAND_EXPLODE_CORE_DIVISOR` used to agree with the real, clamped
     * value at every divisor this constant had ever held (2, then 3),
     * purely by coincidence: raising it to 5 made 3 / 5 round down to 0,
     * while sand_explode() itself still clamps a radius-3 blast's core to
     * 1 (see SAND_EXPLODE_CORE_DIVISOR's own comment in sand.h) - so the
     * unclamped copy here started asserting SAND_EMPTY over four cells
     * that are, correctly, fire. A local recomputation that quietly
     * assumes away a documented clamp is exactly the kind of thing that
     * only breaks the next time a constant moves, which is now. */
    const int core_radius_raw = 3 / SAND_EXPLODE_CORE_DIVISOR;
    const int core_radius = (core_radius_raw == 0 && 3 >= 2) ? 1 : core_radius_raw;
    const int core_r2 = core_radius * core_radius;
    int fire_cells = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const int dx = x - 4;
            const int dy = y - 4;
            if (dx * dx + dy * dy <= core_r2) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_FIRE,
                    CELL_MATERIAL(sand_at(&s, x, y)),
                    "the core must flash into fire even where there was "
                    "nothing at all to convert");
                fire_cells++;
            } else {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, x, y),
                    "nothing beyond the core may appear from empty space - "
                    "there was nothing there to queue a flight entry for");
            }
        }
    }

    const int expected = fire_cells;
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
        "the core's fire must be the only thing on the board");

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
        "and stepping afterwards must not conjure or destroy anything "
        "either");
}

static void test_without_a_buffer_explode_does_nothing(void)
{
    fixture();
    /* sand_enable_impulses() deliberately never called. */

    sand_set(&s, 4, 4, SAND_FIRST_SHADE);
    sand_explode(&s, 4, 4, 2);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 4, 4),
        "with no buffer enabled, sand_explode() must be a pure no-op");

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    /* Getting here at all is most of what this test is for - step_impulses()
     * must treat "no buffer" exactly like "nothing queued" rather than ever
     * touching a NULL impulse_buf. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_count(&s),
        "and ordinary gravity alone must still account for the one grain");
}

/* --- Rung 3, Part A: the ricochet scene (measurement, not a new feature) --
 *
 * Two full-height MAT_STONE walls facing each other across a 42-cell gap, a
 * blast at the left wall, and a per-step count of how many times each
 * dislodged chunk's OWN `dir` changes before it finally settles - a
 * ricochet reads as more than one change, a single stop as zero or one.
 * This does not exercise anything new: rungs 1 and 2 (drag, reflection) are
 * already shipped. It exists to tell rung 3's tuning apart from a guess -
 * see the numbers below, and step_impulses()'s own "FLOORED AT SAND_
 * IMPULSE_BOUNCE_MIN_SPEED" comment for the mechanism being measured.
 *
 * RADIUS IS THE APP'S OWN FIGURE, CONVERTED TO CELLS - DETONATE_RADIUS_PX
 * is 50 (app_sand.c) and the default quality is NORMAL, 4 px/cell
 * (QUALITY_DEFAULT, qualities[], app_sand.c), so the app's own detonate
 * call computes (DETONATE_RADIUS_PX + cell / 2) / cell = (50 + 2) / 4 =
 * 13 - the exact arithmetic app_sand.c's DETONATE handler uses.
 *
 * THE BLAST CENTRE IS OFF-GRID, BEHIND THE WALL, NOT A TYPO. A queued
 * grain's direction is "away from the blast centre, through the grain's own
 * cell" (queue_outward_impulse(), sand.c) - so a wall standing to the RIGHT
 * of the centre gets thrown further right, into the gap, while a wall to
 * the LEFT of the centre gets thrown further left, away from the gap and
 * off the grid. Centring the blast IN the gap, next to the wall, throws the
 * wall the wrong way - straight into itself. Centring it AT x = -11 puts
 * the left wall (x = 0) 11 cells to its right, which is what "an explosion
 * AT the wall" has to mean for the wall's own rubble to end up flying
 * toward the far one: a charge set against/behind the wall's face, blowing
 * it into the room. This placement has a second, welcome effect: the
 * unconditional fire-filled core (radius / SAND_EXPLODE_CORE_DIVISOR = 13 /
 * 5 = 2 around the centre) sits entirely off-grid too (x in [-13, -9]), so
 * nothing here ever creates a MAT_FIRE entry - every tracked entry is
 * genuine dislodged wall stone, not core debris.
 *
 * IDENTITY IS TRACKED BY POSITION, NOT BY CELL BYTE, and that is a real
 * finding of its own, not a style choice: MAT_STONE's variant is a
 * TEMPERATURE nibble now (heat_ramp = 32, cools = 5, material.c, "Stone
 * carries a temperature exactly as glass does"), and it drifts toward
 * ambient every step even with no heat source anywhere on the board - an
 * early version of this test gave every wall row a distinct variant to use
 * as a per-entry id and measured MORE distinct bytes appearing over a run
 * than were ever actually dislodged at explode time, purely from that
 * drift. Position survives this cleanly, but RICOCHET_MATCH_RADIUS had to
 * grow well past the raw per-step bound to keep doing so once entries
 * started covering more ground per step (SAND_IMPULSE_CELLS_PER_STEP_
 * DIVISOR, sand.h): the INSTANTANEOUS bound is SAND_IMPULSE_CELLS_PER_
 * STEP_MAX + 1 cells in one step (5 at today's cap of 4 - a multi-cell
 * push-roll move plus, for a KIND_STATIC mover, one more unconditional
 * gravity-drift move, both in step_impulses(), sand.c), but this GREEDY,
 * array-order tracker is not immune to a single early ambiguous link: once
 * two candidates are plausibly close to the same lineage (far more likely
 * now that several concurrently-flying entries cover 4-5x the ground per
 * step they used to, closing the "several rows apart" gap this scene
 * relies on faster than before), one wrong pick compounds every step after
 * it, and recovering needs slack well beyond the instantaneous bound - a
 * radius of 6 (5 + 1 slack, the same margin convention the old radius-3
 * used over the old 2-cell bound) left 236 of the 100 seeds' candidates
 * unmatched; 15 leaves none. MEASURED, NOT GUESSED: swept 6/8/10/12/15,
 * unmatched counts of 236/156/60/28/0 - 15 is the smallest of those that
 * actually clears every seed, not a round number picked to be safe.
 * Checked, not merely assumed: the measurement below counts entries that
 * ever failed to find any match at all (would mean a genuinely new impulse
 * got queued mid-run, which nothing in this liquid/gas/fire-free scene
 * should ever do, OR that this tracker's own radius is still too tight) -
 * zero, every seed, at RICOCHET_MATCH_RADIUS 15.
 *
 * RE-MEASURED AFTER SAND_IMPULSE_CELLS_PER_STEP_DIVISOR (sand.h) LANDED -
 * the arithmetic walkthrough this comment used to carry (a 42-cell gap
 * costing 42 * SAND_IMPULSE_SPEED_RAMP speed to cross) assumed exactly one
 * cell of travel per step, which is no longer true: a fast entry now
 * crosses the same gap in roughly a quarter as many steps, so it arrives
 * having paid roughly a quarter as much ramp - THE RAMP SPENT CROSSING THE
 * GAP IS NO LONGER THE LIMITER IT WAS. SEEDS 1..100, this exact scene: 284
 * tracked stone entries total (unchanged - the blast's own dislodge count
 * has nothing to do with how far anything travels afterward), 160 (56.3%)
 * changed direction at least once, 45 (15.8%) at least twice, 3 (1.1%)
 * three times - see docs/Sand/Sand-Simulation.md for the before/after
 * table this rung is judged against; a real shift in the >= 1 bucket (150
 * before, 160 now) from entries that used to run out of ramp mid-gap and
 * now cross with speed to spare.
 *
 * THE OLD "GENTLER RESTITUTION" ARITHMETIC (a speculative exploration of
 * lowering SAND_IMPULSE_BOUNCE_MIN_SPEED or equalising the head-on/glancing
 * split, never acted on) is dropped rather than reworked here - it was
 * built entirely on the now-stale one-cell-per-step gap-crossing cost
 * above, and rederiving it against multi-cell travel is a separate
 * exploration nobody has asked for. Nothing it discussed was ever changed
 * by this rung: SAND_IMPULSE_BOUNCE_MIN_SPEED and the head-on/glancing
 * split are exactly what they were. */
#define RICOCHET_W 44
#define RICOCHET_H 40
#define RICOCHET_RADIUS 13
#define RICOCHET_CX (-11)
#define RICOCHET_CY (RICOCHET_H / 2)
#define RICOCHET_SEEDS 100
#define RICOCHET_MAX_STEPS 300
#define RICOCHET_MATCH_RADIUS 15
#define RICOCHET_MAX_TRACK 32

typedef struct {
    bool alive;
    int  x, y, dir;
    int  bounces;
} ricochet_lineage_t;

/* One seed of the ricochet scene, folded into `hist` (hist[k] += 1 for
 * every entry that changed direction at least k times before it stopped
 * being trackable, k = 0..RICOCHET_MAX_BOUNCES) and *total_entries -
 * exactly the measurement this test's own top comment reports. Position-
 * matched across steps, not byte-matched - see that comment for why. */
#define RICOCHET_MAX_BOUNCES 20
static void ricochet_measure_seed(uint8_t *cells, impulse_t *buf,
                                  uint32_t seed, long hist[RICOCHET_MAX_BOUNCES + 1],
                                  long *total_entries, int *any_unmatched_new)
{
    sand_t g;
    memset(cells, 0, (size_t)RICOCHET_W * RICOCHET_H);
    sand_init(&g, cells, RICOCHET_W, RICOCHET_H, seed);
    sand_enable_impulses(&g, buf, RICOCHET_W * RICOCHET_H);

    for (int x = 0; x < RICOCHET_W; x++) {
        sand_set(&g, x, RICOCHET_H - 1, STONE);
    }
    for (int y = 0; y < RICOCHET_H; y++) {
        sand_set(&g, 0,               y, STONE);
        sand_set(&g, RICOCHET_W - 1,  y, STONE);
    }

    sand_explode(&g, RICOCHET_CX, RICOCHET_CY, RICOCHET_RADIUS);

    /* static, not a stack array - see check_stack_usage.py's own gate
     * (docs/Sand/Performance-Tuning-Attempts.md): this helper runs on the
     * device's 3584-byte main task stack too (the on-device selftest
     * links every suite), and RICOCHET_MAX_TRACK copies of every array
     * below pushed a stack-local version of this function well past the
     * 1024-byte ceiling. One call is in flight at a time (a plain
     * sequential loop over seeds in the test below, no re-entrancy), so
     * moving these to static costs nothing but the .bss they already
     * would have cost on the stack. */
    static ricochet_lineage_t lin[RICOCHET_MAX_TRACK];
    int n_lin = 0;
    for (int i = 0; i < g.impulse_count && n_lin < RICOCHET_MAX_TRACK; i++) {
        if (CELL_MATERIAL(g.impulse_buf[i].cell) != MAT_STONE) {
            continue;
        }
        const int idx = g.impulse_buf[i].index;
        lin[n_lin].alive   = true;
        lin[n_lin].x       = idx % RICOCHET_W;
        lin[n_lin].y       = idx / RICOCHET_W;
        lin[n_lin].dir     = g.impulse_buf[i].dir;
        lin[n_lin].bounces = 0;
        n_lin++;
    }

    int steps = 0;
    while (g.impulse_count > 0 && steps < RICOCHET_MAX_STEPS) {
        sand_step(&g, 0, 1000, 0);
        steps++;

        static int cand_x[RICOCHET_MAX_TRACK], cand_y[RICOCHET_MAX_TRACK],
                   cand_dir[RICOCHET_MAX_TRACK];
        int n_cand = 0;
        for (int i = 0; i < g.impulse_count && n_cand < RICOCHET_MAX_TRACK; i++) {
            if (CELL_MATERIAL(g.impulse_buf[i].cell) != MAT_STONE) {
                continue;
            }
            const int idx = g.impulse_buf[i].index;
            cand_x[n_cand]   = idx % RICOCHET_W;
            cand_y[n_cand]   = idx / RICOCHET_W;
            cand_dir[n_cand] = g.impulse_buf[i].dir;
            n_cand++;
        }
        static bool cand_used[RICOCHET_MAX_TRACK];
        memset(cand_used, 0, sizeof cand_used);

        for (int li = 0; li < n_lin; li++) {
            if (!lin[li].alive) {
                continue;
            }
            int best = -1, best_d = RICOCHET_MATCH_RADIUS * 4 + 1;
            for (int ci = 0; ci < n_cand; ci++) {
                if (cand_used[ci]) {
                    continue;
                }
                const int dx = im_abs(cand_x[ci] - lin[li].x);
                const int dy = im_abs(cand_y[ci] - lin[li].y);
                if (dx > RICOCHET_MATCH_RADIUS || dy > RICOCHET_MATCH_RADIUS) {
                    continue;
                }
                const int d = dx + dy;
                if (d < best_d) {
                    best_d = d;
                    best   = ci;
                }
            }
            if (best < 0) {
                lin[li].alive = false;   /* settled, or lost to re-acquisition */
                continue;
            }
            cand_used[best] = true;
            if (cand_dir[best] != lin[li].dir) {
                lin[li].bounces++;
            }
            lin[li].x   = cand_x[best];
            lin[li].y   = cand_y[best];
            lin[li].dir = cand_dir[best];
        }
        for (int ci = 0; ci < n_cand; ci++) {
            if (!cand_used[ci]) {
                *any_unmatched_new = 1;
            }
        }
    }

    for (int li = 0; li < n_lin; li++) {
        (*total_entries)++;
        const int bc = lin[li].bounces > RICOCHET_MAX_BOUNCES
                           ? RICOCHET_MAX_BOUNCES
                           : lin[li].bounces;
        for (int k = 0; k <= bc; k++) {
            hist[k]++;
        }
    }
}

static void test_the_two_wall_explosion_scene_bounces_more_than_once_before_settling(void)
{
    uint8_t *ricochet_cells = malloc((size_t)RICOCHET_W * RICOCHET_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(ricochet_cells,
        "ricochet grid must fit in what the framebuffer leaves");
    impulse_t *ricochet_buf =
        malloc((size_t)(RICOCHET_W * RICOCHET_H) * sizeof *ricochet_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(ricochet_buf,
        "ricochet impulse queue must fit in what the framebuffer leaves");

    long hist[RICOCHET_MAX_BOUNCES + 1];
    memset(hist, 0, sizeof hist);
    long total_entries = 0;
    int any_unmatched_new = 0;

    for (uint32_t seed = 1; seed <= (uint32_t)RICOCHET_SEEDS; seed++) {
        ricochet_measure_seed(ricochet_cells, ricochet_buf, seed, hist,
                              &total_entries, &any_unmatched_new);
    }

    free(ricochet_buf);
    free(ricochet_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, total_entries,
        "the scene must actually have dislodged wall material across 100 "
        "seeds, or the measurement above is vacuous");
    TEST_ASSERT_FALSE_MESSAGE(any_unmatched_new,
        "a tracked entry appeared with no plausible predecessor within "
        "RICOCHET_MATCH_RADIUS - either the position-matching tracker "
        "above needs a wider radius, or something in this liquid/gas/fire-"
        "free scene queued a fresh impulse mid-run when nothing should");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, hist[1],
        "at least one tracked entry must have changed direction at least "
        "once somewhere across the 100 seeds - a ricochet, not a single "
        "stop; this is the floor of what this scene must show, not the "
        "measurement itself (see this test's own top comment for the full "
        "distribution)");
}

/* --- Rung 3, Part B: momentum transfer into a volume ----------------------
 *
 * Today a flying cell swaps with whatever it displaces and the volume
 * closes behind it - nothing else in it ever learns it was hit. This is
 * what lets a struck cell pick up impulse of its own instead (see the
 * TRANSFER site's own comment at the move site in step_impulses(), sand.c,
 * right after the swap and right where the drag charge already sits) -
 * including flying clean out of the volume it was sitting in.
 *
 * EJECTA IS MEASURED, NOT ASSUMED, and DIRT and WATER measure completely
 * differently - a real finding, not a shape mismatch between the two
 * scenes below. DIRT (KIND_POWDER) is inert once packed: an undisturbed
 * bank does not spread on its own, so "cells found outside the bank's own
 * original footprint" is a clean signal - transfer on, or transfer off via
 * a temporary mutation confirmed live (see below), the difference is
 * exactly zero vs a real number. WATER (KIND_LIQUID) is not inert: even a
 * perfectly flat, resting layer spreads sideways on its own, at a rate
 * (measured: exactly 1.0 mass-equivalent cell per seed per step, dead
 * linear, confirmed by running the SAME scene with no mover in it at all)
 * that is comparable to whatever transfer adds on top. "Zero with transfer
 * removed" is consequently the wrong bar for water and was checked, not
 * assumed: at 200 seeds, this exact scene measures 387 cells outside its
 * own original footprint with transfer's own gate hard-mutated to `false`,
 * not zero - that is water's own ordinary equalisation, present with or
 * without this rung's own change, and no geometry tried (a taller block, a
 * walled and fully-settled basin, a basin with a genuine overflow gap, an
 * upward throw) separated it from transfer's own contribution any better;
 * every one of those either reproduced the same baseline or, walled tightly
 * enough to suppress it, blocked transfer too (a transferred entry can
 * never breach a KIND_STATIC wall regardless - can_impulse_enter()'s own
 * rule, unconditional for a KIND_STATIC target). What the SAME hard-mutated
 * run does show is a real, reproducible MARGIN over that baseline - 454
 * with transfer against 387 without, both at the same 200 seeds - so the
 * water test below asserts against that margin (a threshold clearly above
 * 387, clearly below 454) rather than against zero.
 *
 * RECONFIRMED, RUNG 4: both numbers moved from the rung 3 measurement (260
 * baseline, 422 with transfer) once SAND_IMPULSE_DRAG_SHIFT dropped to 0 and
 * the transfer direction became a backward cone rather than the mover's own
 * heading (see the TRANSFER site's own comment, step_impulses(), sand.c) -
 * this is a different mechanism now, so it earned a fresh measurement rather
 * than trusting the old one to still be in the right place. */

#define EJECTA_W 40
#define EJECTA_H 20
#define EJECTA_SEEDS 200

/* Count cells of `mat` on the board that sit OUTSIDE [x0,x1) x [y0,y1) -
 * shared by both ejecta measurements below, the DIRT and the WATER scene
 * alike, so "outside the volume's own original footprint" means the exact
 * same thing in both. */
static int ejecta_count_outside(sand_t *g, int gw, int gh, uint8_t mat,
                                int x0, int x1, int y0, int y1)
{
    int n = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) != mat) {
                continue;
            }
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                continue;
            }
            n++;
        }
    }
    return n;
}

/* Total cells of `mat` anywhere on the board - the conservation tests'
 * own "before" count, taken with no "inside" region at all rather than
 * reusing ejecta_count_outside() with a degenerate empty rectangle. */
static int ejecta_count_material(sand_t *g, int gw, int gh, uint8_t mat)
{
    int n = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == mat) {
                n++;
            }
        }
    }
    return n;
}

/* DIRT, packed against the floor, no gap underneath it - a floating bank
 * would slump under its own ordinary gravity before the mover ever arrived
 * (measured on an earlier version of this scene: a bank left floating a
 * few rows above the real floor had already relocated itself, whole,
 * before firing), which would read as ejecta this feature never caused.
 * VARIANT 0, NOT 8 - see plow_build()'s own comment a few hundred lines up
 * for why 8 is WET soil under main's re-encoding and quietly changes state
 * mid-measurement; this scene does not need dirt's moisture at all, so it
 * never risks it.
 *
 * THE MOVER STARTS INSIDE THE BANK'S OWN FOOTPRINT, not adjacent to it -
 * an earlier version placed it one cell to the left (outside), and the
 * very first ordinary swap (mover in, one bank cell out to the mover's old
 * - now outside - cell) counted as "ejecta" on EVERY seed whether transfer
 * fired or not, since that swap is rung 1's own mechanic, not this rung's.
 * Starting inside means the first several swaps relocate bank material to
 * cells still inside the original footprint, so anything found outside
 * afterward is actually attributable to a cell having FLOWN there under
 * its own power. */
#define EJECTA_DIRT_W 2
#define EJECTA_DIRT_H 4
static long ejecta_dirt_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)EJECTA_SEEDS; seed++) {
        const int dx0 = 10, dx1 = dx0 + EJECTA_DIRT_W;
        const int dy1 = EJECTA_H - 1, dy0 = dy1 - EJECTA_DIRT_H;
        sand_t g;
        memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
        sand_init(&g, cells, EJECTA_W, EJECTA_H, seed);
        sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
        for (int x = 0; x < EJECTA_W; x++) {
            sand_set(&g, x, EJECTA_H - 1, STONE);
        }
        for (int y = dy0; y < dy1; y++) {
            for (int x = dx0; x < dx1; x++) {
                sand_set(&g, x, y, CELL_MAKE(MAT_DIRT, 0));
            }
        }
        sand_set(&g, dx0, dy1 - 1, STONE);
        enum { DIR_RIGHT = 2 };
        sand_impulse_dislodge(&g, dx0, dy1 - 1, DIR_RIGHT, 255,
                              SAND_IMPULSE_SPEED_RAMP);
        for (int i = 0; i < 150; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += ejecta_count_outside(&g, EJECTA_W, EJECTA_H, MAT_DIRT,
                                      dx0, dx1, dy0, dy1);
    }
    return total;
}

/* MEASURED, 200 seeds: 50 dirt cells end up outside the bank's own 2x4
 * footprint (was 31 before rung 4 dropped SAND_IMPULSE_DRAG_SHIFT to 0 and
 * pointed the transfer at a backward cone instead of the mover's own
 * heading - see the TRANSFER site's own comment, step_impulses(), sand.c -
 * both raise how much of what a struck cell picks up actually clears the
 * bank's own footprint, so a bigger number here is the fix working, not
 * drift). RED CHECK, live: with the transfer site's own gate in
 * step_impulses() (sand.c) hard-mutated to `false` and reverted after,
 * this exact scene measures exactly 0 - every seed, no exceptions. Dirt
 * packed and resting does not spread on its own; every one of the 50 is a
 * cell transfer flung. */
static void test_a_thrown_powder_grain_flings_dirt_out_of_the_bank_it_hits(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "ejecta grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "ejecta impulse queue must fit in what the framebuffer leaves");

    const long total = ejecta_dirt_total(cells, buf);

    free(buf);
    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, total,
        "summed across 200 seeds, at least one dirt cell must end up "
        "outside the bank's own original footprint - a packed, resting "
        "dirt bank never spreads on its own (measured zero with transfer's "
        "own gate hard-mutated off), so any cell found outside it was "
        "flung there by the transfer this rung adds");
}

/* --- MATERIAL ACTUALLY FLIES, NOW - SAND_IMPULSE_CELLS_PER_STEP_DIVISOR ---
 *
 * A DIFFERENT SIGNAL FROM THE EJECTA TESTS ABOVE, on purpose: "outside the
 * bank's own footprint" (ejecta_count_outside(), above) counts a cell that
 * merely relocated, however it got there or however briefly it was ever
 * off the ground. AIRBORNE - every one of a cell's eight neighbours
 * genuinely CELL_IS_EMPTY() at the instant sampled - is the stricter claim
 * the maintainer actually asked for: material visibly FLYING, suspended in
 * open air, not just shuffled sideways within a settling pile. Before this
 * rung, every displacing move in step_impulses() advanced an entry exactly
 * one cell per successful roll - identical to how far the ordinary gravity
 * sweep moves a falling grain in the same step - so a thrown chunk could
 * never outrun gravity: ejecta could reposition but not visibly leave the
 * volume it came from.
 *
 * THE SCENE: an 8-cell-wide row of MAT_STONE, thrown together at a
 * diagonal (down-right, dir 1 - "at an angle", not straight down or
 * straight along the surface) into a settled MAT_SAND bed resting on a
 * solid floor. Airborne sand is counted every step for AIRBORNE_STEPS,
 * peak kept per seed, over AIRBORNE_SEEDS seeds.
 *
 * MEASURED, this exact scene, seeds 1..40: BEFORE this rung, 26 of 40
 * seeds ever showed any airborne sand at all, peaking at 3 simultaneously
 * airborne cells. AFTER, all 40 seeds show airborne sand, peaking at 8 -
 * both more often and more of it at once. Different numbers from the
 * maintainer's own hand-measured scene (12 of 40, peak 1, before) - a
 * different bed/chunk/angle, not a discrepancy - but the same shape: a
 * capped, modest peak before this rung existed no matter the scene, a
 * clearly higher one after. */

#define AIRBORNE_W 80
#define AIRBORNE_H 40
#define AIRBORNE_SEEDS 40
#define AIRBORNE_STEPS 60
#define AIRBORNE_SURFACE (AIRBORNE_H - 10)
#define AIRBORNE_CHUNK_W 8
#define AIRBORNE_CHUNK_X0 5
#define AIRBORNE_CHUNK_Y (AIRBORNE_SURFACE - 6)

/* Sand cells with all eight neighbours genuinely CELL_IS_EMPTY() - see
 * this section's own top comment for why that is a stricter, more
 * specific claim than "outside its original footprint". */
static int airborne_sand_count(sand_t *g)
{
    int n = 0;
    for (int y = 0; y < AIRBORNE_H; y++) {
        for (int x = 0; x < AIRBORNE_W; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) != MAT_SAND) {
                continue;
            }
            bool all_empty = true;
            for (int dy = -1; dy <= 1 && all_empty; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    if (!CELL_IS_EMPTY(sand_at(g, x + dx, y + dy))) {
                        all_empty = false;
                        break;
                    }
                }
            }
            if (all_empty) {
                n++;
            }
        }
    }
    return n;
}

/* One seed: builds the scene, runs AIRBORNE_STEPS, returns the peak
 * airborne-sand count seen at any single step. */
static int airborne_bed_peak(uint8_t *cells, impulse_t *buf, uint32_t seed)
{
    sand_t g;
    memset(cells, 0, (size_t)AIRBORNE_W * AIRBORNE_H);
    sand_init(&g, cells, AIRBORNE_W, AIRBORNE_H, seed);
    sand_enable_impulses(&g, buf, AIRBORNE_W * AIRBORNE_H);

    for (int x = 0; x < AIRBORNE_W; x++) {
        sand_set(&g, x, AIRBORNE_H - 1, STONE);
    }
    for (int y = AIRBORNE_SURFACE; y < AIRBORNE_H - 1; y++) {
        for (int x = 0; x < AIRBORNE_W; x++) {
            sand_set(&g, x, y, SAND_FIRST_SHADE);
        }
    }

    enum { DIR_DOWN_RIGHT = 1 };
    for (int k = 0; k < AIRBORNE_CHUNK_W; k++) {
        sand_set(&g, AIRBORNE_CHUNK_X0 + k, AIRBORNE_CHUNK_Y, STONE);
        sand_impulse_dislodge(&g, AIRBORNE_CHUNK_X0 + k, AIRBORNE_CHUNK_Y,
                              DIR_DOWN_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);
    }

    int peak = 0;
    for (int i = 0; i < AIRBORNE_STEPS; i++) {
        sand_step(&g, 0, 1000, 0);
        const int n = airborne_sand_count(&g);
        if (n > peak) {
            peak = n;
        }
    }
    return peak;
}

static void test_a_stone_chunk_thrown_into_a_sand_bed_launches_sand_airborne(void)
{
    uint8_t *cells = malloc((size_t)AIRBORNE_W * AIRBORNE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "airborne-bed grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(AIRBORNE_W * AIRBORNE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "airborne-bed impulse queue must fit in what the framebuffer leaves");

    int seeds_with_any = 0;
    int peak = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)AIRBORNE_SEEDS; seed++) {
        const int p = airborne_bed_peak(cells, buf, seed);
        if (p > 0) {
            seeds_with_any++;
        }
        if (p > peak) {
            peak = p;
        }
    }

    free(buf);
    free(cells);

    char msg[420];
    snprintf(msg, sizeof msg,
             "peak airborne sand across %d seeds was %d, seeds_with_any "
             "%d/%d - this exact scene measured a peak of 3 (26/40 seeds "
             "with any) before SAND_IMPULSE_CELLS_PER_STEP_DIVISOR existed "
             "(sand.h) and a peak of 8 (40/40) after; the assertion sits "
             "strictly between those two measured figures so this test "
             "actually distinguishes the two, not merely confirms some "
             "airborne sand exists at all",
             AIRBORNE_SEEDS, peak, seeds_with_any, AIRBORNE_SEEDS);
    /* 4, not 1 - see this section's own top comment for why the OLD code
     * already clears a bar as low as 1 on this scene (a thrown chunk's own
     * gravity-drift plus rung 1/2's ordinary bounce can separate two
     * grains onto different steps even without this rung's multi-cell
     * push), so a threshold that low would pass on unfixed code and prove
     * nothing. 4 sits strictly between the measured 3 (before) and 8
     * (after). */
    TEST_ASSERT_GREATER_THAN_MESSAGE(4, peak, msg);
}

/* --- A THROWN GRAIN THAT HAS FLOWN A LONG WAY STILL EJECTS ON IMPACT ------
 *
 * THE BUG: step_impulses()'s `!rolled_move` branch (the push-roll failed
 * this turn) only ever kept a KIND_STATIC entry tracked once it verified
 * the entry was still genuinely airborne - every OTHER kind, KIND_POWDER
 * included, fell straight through to the plain `continue` right after and
 * was DROPPED, whether or not it had actually landed. The roll's own
 * chance IS entry.speed (that loop's own comment), and the ramp erodes it
 * every step, so a thrown powder grain's odds of still being tracked
 * collapsed with `speed` long before its actual energy did: roughly 62% at
 * 10 steps, 16% at 20, 2% at 30, 0.1% at 40 - while `speed` at step 30 is
 * still 193, nearly three times SAND_IMPULSE_TRANSFER_MIN_SPEED. The grain
 * kept flying regardless (the ordinary sweep falls a powder grain every
 * step whether or not this loop still has it tracked), but it arrived with
 * no impulse entry attached, so the TRANSFER block never ran on impact and
 * nothing was ever ejected - harmless before TRANSFER existed, which is
 * exactly why only KIND_STATIC was ever checked here, and not any more.
 *
 * A TALL WALL, NOT A BANK RESTING ON A SHARED FLOOR - the mover has to
 * stay genuinely AIRBORNE for its whole flight, which a floor would end
 * early: the instant a KIND_POWDER mover is actually supported,
 * can_impulse_enter_gravity_ward() correctly finds no opening in any of
 * its three gravity-ward candidates and it settles, exactly as it should
 * (that is the intended floor, not a bug to route around). Resting the
 * wall's own footprint on the SAME floor as the open lane made the mover
 * land on that floor itself long before covering any real distance, which
 * measured as "no ejecta at every distance" even for the FIXED code - a
 * scene bug, not a sand.c one. A wall tall enough to span whatever row the
 * mover has fallen to by the time it crosses the wall's own column (it
 * falls under the ordinary sweep the whole flight, independent of this
 * mechanism) keeps it genuinely airborne until the moment of impact,
 * whatever that row turns out to be. */
#define EJECTA_FAR_W 45
#define EJECTA_FAR_H 70
#define EJECTA_FAR_WALL_X 35
#define EJECTA_FAR_WALL_W 6
#define EJECTA_FAR_SEEDS 60
#define EJECTA_FAR_DISTANCE 30
static long ejecta_far_thrown_powder_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)EJECTA_FAR_SEEDS; seed++) {
        const int mover_x = EJECTA_FAR_WALL_X - EJECTA_FAR_DISTANCE;
        sand_t g;
        memset(cells, 0, (size_t)EJECTA_FAR_W * EJECTA_FAR_H);
        sand_init(&g, cells, EJECTA_FAR_W, EJECTA_FAR_H, seed);
        sand_enable_impulses(&g, buf, EJECTA_FAR_W * EJECTA_FAR_H);
        for (int y = 0; y < EJECTA_FAR_H; y++) {
            for (int x = EJECTA_FAR_WALL_X; x < EJECTA_FAR_WALL_X + EJECTA_FAR_WALL_W; x++) {
                sand_set(&g, x, y, CELL_MAKE(MAT_DIRT, 0));
            }
        }
        sand_set(&g, mover_x, 0, SAND_FIRST_SHADE);
        enum { DIR_RIGHT = 2 };
        sand_impulse(&g, mover_x, 0, DIR_RIGHT, 255);
        for (int i = 0; i < 160; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        for (int y = 0; y < EJECTA_FAR_H; y++) {
            for (int x = 0; x < EJECTA_FAR_W; x++) {
                if (CELL_MATERIAL(sand_at(&g, x, y)) != MAT_DIRT) {
                    continue;
                }
                if (x >= EJECTA_FAR_WALL_X &&
                    x < EJECTA_FAR_WALL_X + EJECTA_FAR_WALL_W) {
                    continue;   /* still inside the wall's own footprint */
                }
                total++;
            }
        }
    }
    return total;
}

/* MEASURED, 60 seeds, at EJECTA_FAR_DISTANCE (30 cells of open-air flight
 * before impact - kept smaller than the full 50 cells the diagnosis itself
 * used, so the wall this scene needs to stay tall enough for fits inside
 * the host heap arena, DP_FREE_HEAP_BYTES, alongside this test's own grid):
 * 60 dirt cells ejected (one per seed - a single struck cell's own
 * TRANSFER, every time) with this rung's fix; 0 with the bug (KIND_POWDER
 * hard-mutated back out of the `!rolled_move` branch, the live-mutation
 * check this file uses elsewhere). Every distance in the diagnosis's own
 * range measured for the maintainer's own record (not asserted
 * individually here, to keep this one assertion cheap): 0.933/0.367/0.000/
 * 0.000 mean ejecta per seed before the fix at 5/15/30/50 cells, 0.900/
 * 1.000/1.000/1.000 after - 30 cells (this test's own distance) is already
 * comfortably past where tracking used to collapse entirely. */
static void test_a_powder_grain_thrown_far_still_ejects_from_the_bank_it_hits(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_FAR_W * EJECTA_FAR_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "far-throw ejecta grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_FAR_W * EJECTA_FAR_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "far-throw ejecta impulse queue must fit in what the framebuffer "
        "leaves");

    const long total = ejecta_far_thrown_powder_total(cells, buf);

    free(buf);
    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(EJECTA_FAR_SEEDS / 2, total,
        "a powder grain thrown 30 cells before it ever reaches a wall must "
        "still eject material on impact in most seeds - measured 0 across "
        "all 60 seeds with KIND_POWDER excluded from step_impulses()'s own "
        "`!rolled_move` tracking branch (the bug this test guards against: "
        "tracking died long before the grain's actual energy did), and 60 "
        "of 60 with it included");
}

/* WATER, a single row deep, resting directly on the floor - see this
 * section's own top comment for why a taller block was tried and
 * rejected: multiple rows of standing liquid trigger splash_displace()
 * (sand_liquid.c) internally as they settle into a flatter shape, which
 * has nothing to do with this rung and swamps it. A single row cannot
 * "fall onto occupied liquid" within itself, so that path never fires
 * here - confirmed by watching s.impulse_count stay at exactly 1 (the
 * mover alone) for the first several steps with transfer's own gate
 * hard-mutated off, the same live-mutation check this file uses
 * elsewhere.
 *
 * ONLY 4 STEPS - short on purpose, the same reason PLOW_STEPS is short
 * (that constant's own comment, a few hundred lines up): water's own
 * ordinary sideways equalisation is dead linear in step count (measured:
 * exactly matches a control scene with no mover in it at all, seed for
 * seed), so it keeps closing the gap on every step this runs - the margin
 * over that baseline that transfer actually adds is at its widest early
 * and shrinks the longer this scene keeps stepping. */
#define EJECTA_WATER_W 3
static long ejecta_water_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)EJECTA_SEEDS; seed++) {
        const int dx0 = 20, dx1 = dx0 + EJECTA_WATER_W;
        const int dy1 = EJECTA_H - 1, dy0 = dy1 - 1;
        sand_t g;
        memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
        sand_init(&g, cells, EJECTA_W, EJECTA_H, seed);
        sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
        for (int x = 0; x < EJECTA_W; x++) {
            sand_set(&g, x, EJECTA_H - 1, STONE);
        }
        for (int x = dx0; x < dx1; x++) {
            sand_set(&g, x, dy0, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
        sand_set(&g, dx0, dy0, STONE);
        enum { DIR_RIGHT = 2 };
        sand_impulse_dislodge(&g, dx0, dy0, DIR_RIGHT, 255,
                              SAND_IMPULSE_SPEED_RAMP);
        for (int i = 0; i < 4; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += ejecta_count_outside(&g, EJECTA_W, EJECTA_H, MAT_WATER,
                                      dx0, dx1, dy0, dy1);
    }
    return total;
}

/* MEASURED, 200 seeds: 454 water cells outside the layer's own original
 * footprint, against 387 for the SAME scene with transfer's own gate
 * hard-mutated to `false` (this section's own top comment has the reasoning
 * for why 387, not 0, is the right baseline here, and why both this and the
 * 454 figure moved from rung 3's 260/422 once drag and the transfer
 * direction changed). EJECTA_WATER_THRESHOLD sits comfortably above that
 * measured baseline and comfortably below the measured total - a regression
 * that silently disabled transfer for liquids would drop this scene back to
 * the 387 baseline and trip it. RAISED FROM 350, WHICH THIS RUNG'S OWN
 * NUMBERS BROKE - 350 sat above the OLD 260 baseline, but the new 387
 * baseline sits above 350 too, which would have made the threshold pass
 * even with transfer silently disabled; re-measuring rather than reusing
 * the old figure is what caught it. */
#define EJECTA_WATER_THRESHOLD 420
static void test_a_thrown_powder_grain_flings_water_out_of_the_pool_it_hits(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "ejecta grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "ejecta impulse queue must fit in what the framebuffer leaves");

    const long total = ejecta_water_total(cells, buf);

    free(buf);
    free(cells);

    char why[512];
    snprintf(why, sizeof why,
        "summed across 200 seeds, water flung outside the pool's own "
        "original footprint must clear %d - measured 454 with transfer, "
        "387 for the same scene with transfer's own gate hard-mutated off "
        "(water's own baseline sideways spread, unrelated to this rung); "
        "this threshold sits between the two, so a regression that quietly "
        "disabled transfer for liquids falls back to the baseline and "
        "trips it", EJECTA_WATER_THRESHOLD);
    TEST_ASSERT_GREATER_THAN_MESSAGE(EJECTA_WATER_THRESHOLD, total, why);
}

/* THE DETERMINISTIC HALF OF THE SAME CLAIM, and the reason the threshold
 * above is allowed to be a threshold. That test measures the BEHAVIOUR -
 * water leaving the pool - across 200 chaotic seeds, and its floor sits
 * between two measured numbers rather than at zero, because water spreads
 * on its own whatever this rung does. This one measures the MECHANISM in a
 * single step with one struck cell: the transfer entry has to exist, at the
 * cell the mover just vacated, carrying half the mover's post-drag speed
 * and a direction in the BACKWARD CONE (one of `dir+3`, `dir+4`, `dir+5` -
 * see the TRANSFER site's own comment, step_impulses(), sand.c, for why
 * that replaced the mover's own heading: queuing along the mover's own
 * direction drove struck material deeper into whatever it hit instead of
 * spraying it back out, which is why nothing visibly ejected on device).
 * Between them, a regression has nowhere to hide - if water physics on
 * main ever drifts the threshold's own numbers, this still says whether
 * transfer itself is alive.
 *
 * MEMBERSHIP, NOT EQUALITY, on direction - rng_below(&s->rng, 3) (the
 * transfer site) picks which of the three cone directions fires, so this
 * cannot pin one exact value the way DIR_RIGHT alone once could; asserting
 * membership in the 3-direction set is the honest version of the same
 * claim, not a weaker stand-in for it.
 *
 * Found by scanning rather than by index: the liquid passes run before the
 * flight pass and splash_displace() (sand_liquid.c) queues entries of its
 * own, so nothing guarantees which slot the transfer lands in. */
static void test_a_struck_water_cell_is_handed_impulse_in_a_backward_cone_from_the_mover(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    /* What the mover has left after one step: the plain ramp, plus drag for
     * the one water cell it displaced. The transfer is half of that. */
    const uint8_t mover_speed =
        (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP -
                  impulse_drag_of(CELL_MAKE(MAT_WATER, MASS_MAX)));
    const uint16_t vacated = (uint16_t)(ROW * W + SX);

    int found = -1;
    for (int i = 0; i < s.impulse_count; i++) {
        if (s.impulse_buf[i].index == vacated) {
            found = i;
            break;
        }
    }

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, found,
        "the water cell the chunk shouldered aside must itself be tracked "
        "as a flying entry afterwards, at the cell the chunk vacated - "
        "that is the whole of what momentum transfer means here");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(s.impulse_buf[found].cell),
        "and the entry queued there must be the WATER that was struck, not "
        "the chunk that struck it");

    const int cone[3] = { (DIR_RIGHT + 3) & 7, (DIR_RIGHT + 4) & 7,
                          (DIR_RIGHT + 5) & 7 };
    bool in_cone = false;
    for (int k = 0; k < 3; k++) {
        if (s.impulse_buf[found].dir == cone[k]) {
            in_cone = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(in_cone,
        "the struck cell's direction must be one of dir+3/dir+4/dir+5 - "
        "a backward cone, straight back and 45 degrees either side - not "
        "the mover's own heading: a struck cell squeezes back out through "
        "the surface it was hit through, it does not get shoved further "
        "along the exact path that just hit it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        (int)(((unsigned)mover_speed * SAND_IMPULSE_TRANSFER_KEEP) >> 8),
        s.impulse_buf[found].speed,
        "carrying exactly its share of the mover's own post-drag speed");
}

/* --- a KIND_POWDER mover's own distance, now that transfer is in scope
 * too - PLOW_W/H/SEEDS/STEPS, plow_build() and plow_mover_x() are rung 1's
 * own (a few hundred lines up); this reuses that exact shape and grid, not
 * a new one, following plow_total_distance()'s own idiom. That existing
 * helper hardcodes MAT_STONE as the mover, so a fresh pair - one to find a
 * DIFFERENT material's mover, one to total ITS distance - is what a
 * KIND_POWDER mover needs instead of a third copy of the whole scene. */
static int plow_mover_x_material(sand_t *g, uint8_t mat)
{
    for (int y = 0; y < PLOW_H - 1; y++) {
        for (int x = 0; x < PLOW_W; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == mat) {
                return x;
            }
        }
    }
    return -1;
}

static long plow_total_distance_powder(uint8_t *cells, impulse_t *buf,
                                       cell_t medium)
{
    long total = 0;
    for (uint32_t k = 1; k <= (uint32_t)PLOW_SEEDS; k++) {
        sand_t g;
        plow_build(&g, cells, buf, 4, k, MAT_SAND, medium);
        for (int i = 0; i < PLOW_STEPS; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += plow_mover_x_material(&g, MAT_SAND) - 1;
    }
    return total;
}

static void test_a_thrown_powder_grain_travels_less_far_through_dirt_than_through_air(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long air_total  = plow_total_distance_powder(cells, buf, 0);
    const long dirt_total = plow_total_distance_powder(cells, buf,
                                                       CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(air_total, dirt_total,
        "a thrown KIND_POWDER grain must still travel less far, summed "
        "across the same seeds, through a bank of dirt than through open "
        "air - rung 1 already covers this for the single-step drag "
        "formula (test_a_thrown_powder_grain_pays_drag_displacing_dirt), "
        "but nothing until now measured it behaviourally, over real "
        "distance, the way test_a_thrown_chunk_travels_less_far_through_"
        "dirt_than_through_air already does for a KIND_STATIC mover - and "
        "KIND_POWDER movers are now also in scope for transfer, not just "
        "drag");
}

/* --- the budget guard: impulse_buf is shared, and it is finite ----------
 *
 * SAND_CASCADE_MAX_PER_STEP caps how many deferred entries (cascade
 * relays AND transfers alike, since rung 3 - see step_impulses()'s own
 * comment on that array) can be QUEUED in any one step, but impulse_buf
 * itself (APP_IMPULSE_MAX in app_sand.c on the device) is a separate,
 * FIXED-SIZE budget shared with whatever explosion or splash is already
 * using it - sand_impulse() refuses silently once it is full. A long plow
 * through a wide, low-density bank (most cells clear SAND_IMPULSE_
 * TRANSFER_MIN_SPEED easily) is exactly the shape that could queue one
 * transfer per cell touched if the per-step cap were not there; this test
 * is what proves the cap actually holds the line rather than merely
 * assuming it does. */
/* SAME SHAPE AS plow_build() (a few hundred lines up) - dirt fills ONLY
 * row 0, everything below is OPEN except a single floor far at the
 * bottom, and the mover is thrown from (1, 0). NOT a full-width floor
 * directly under the mover's own row - an earlier version of this test
 * used exactly that, and the mover (KIND_STATIC) settled on literally the
 * SECOND step: fully supported with nowhere to fall, step_impulses()'s
 * own "SUPPORTED, NOT MERELY ROLLED" rule (sand.c) drops a KIND_STATIC
 * entry from tracking the instant one push-roll fails, and a floor
 * spanning the whole width guarantees that happens almost immediately at
 * this speed. plow_build()'s own open-below shape is what keeps the
 * mover - and the dirt feeding it from behind, which also falls once it
 * is unsupported, see that function's own comment - genuinely engaged for
 * many steps instead of freezing after one. Measured with this shape,
 * this exact scene (200-cell bank): activity (impulse_count > 0) for 27
 * steps, never above 3 entries concurrently even with the transfer cap
 * completely unbounded - BUDGET_IMPULSE_MAX is deliberately far below
 * what an ordinary run ever needs, so the assertion below is a real gate,
 * not a number this scene could never approach anyway. */
#define BUDGET_W 200
#define BUDGET_H 30
#define BUDGET_IMPULSE_MAX 8
#define BUDGET_STEPS 40

static void test_a_long_plow_through_a_wide_bank_never_exhausts_the_impulse_buffer(void)
{
    uint8_t *cells = malloc((size_t)BUDGET_W * BUDGET_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "budget-guard grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)BUDGET_IMPULSE_MAX * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "budget-guard impulse queue must fit in what the framebuffer "
        "leaves");

    sand_t g;
    memset(cells, 0, (size_t)BUDGET_W * BUDGET_H);
    sand_init(&g, cells, BUDGET_W, BUDGET_H, 9u);
    sand_enable_impulses(&g, buf, BUDGET_IMPULSE_MAX);

    for (int x = 0; x < BUDGET_W; x++) {
        sand_set(&g, x, BUDGET_H - 1, STONE);
    }
    for (int x = 2; x < BUDGET_W; x++) {
        sand_set(&g, x, 0, CELL_MAKE(MAT_DIRT, 0));
    }
    sand_set(&g, 1, 0, STONE);
    enum { DIR_RIGHT = 2 };
    sand_impulse_dislodge(&g, 1, 0, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    int max_seen = 0;
    for (int i = 0; i < BUDGET_STEPS; i++) {
        sand_step(&g, 0, 1000, 0);
        if (g.impulse_count > max_seen) {
            max_seen = g.impulse_count;
        }
        char why[192];
        snprintf(why, sizeof why,
            "step %d: impulse_count (%d) exceeded the buffer's own "
            "capacity (%d) - sand_impulse() should refuse silently before "
            "this can ever happen, so reaching it means the per-step cap "
            "did not hold", i, g.impulse_count, BUDGET_IMPULSE_MAX);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(BUDGET_IMPULSE_MAX,
            g.impulse_count, why);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, max_seen,
        "the plow must actually have queued something across its own "
        "run, or the buffer-capacity assertion above never exercised "
        "anything");

    /* Still stepping normally afterwards - a fresh sand_impulse() must
     * still succeed once the plow's own entries have had room to clear,
     * proving this was never a permanently wedged buffer. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.impulse_count,
        "the plow's own entries must have fully cleared by the end of "
        "this run - otherwise the fresh sand_impulse() below is not "
        "actually testing an empty buffer draining, it is testing a "
        "still-busy one");
    sand_set(&g, BUDGET_W - 2, BUDGET_H - 2, CELL_MAKE(MAT_SAND, 8));
    sand_impulse(&g, BUDGET_W - 2, BUDGET_H - 2, DIR_RIGHT, 200);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g.impulse_count,
        "a fresh sand_impulse() queued after the plow's own run must "
        "still succeed - the shared buffer must drain as entries settle, "
        "not fill up and stay full");

    free(buf);
    free(cells);
}

/* --- conservation: transfer relocates cells, it never creates or destroys
 * them --------------------------------------------------------------------
 *
 * Same shape as test_a_thrown_static_chunk_conserves_lava_mass_on_sink
 * (a few thousand lines up): every transferred entry is queued through
 * plain sand_impulse() (step_impulses()'s own append loop, sand.c), which
 * only ever moves a cell that is ALREADY on the board - nothing here
 * manufactures a fresh one. DIRT is checked by CELL COUNT, exactly - a
 * KIND_POWDER cell's own count never legitimately changes from ordinary
 * movement the way a liquid's can (mass_of()'s own comment). WATER is
 * checked by MASS, not cell count, for the same reason that test checks
 * lava by mass: a liquid's cell count is free to change as it spreads or
 * merges without a drop being lost. */
static void test_a_thrown_powder_grain_conserves_the_dirt_it_ejects(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "conservation grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "conservation impulse queue must fit in what the framebuffer "
        "leaves");

    const int dx0 = 10, dx1 = dx0 + EJECTA_DIRT_W;
    const int dy1 = EJECTA_H - 1, dy0 = dy1 - EJECTA_DIRT_H;
    sand_t g;
    memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
    sand_init(&g, cells, EJECTA_W, EJECTA_H, 5u);
    sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
    for (int x = 0; x < EJECTA_W; x++) {
        sand_set(&g, x, EJECTA_H - 1, STONE);
    }
    for (int y = dy0; y < dy1; y++) {
        for (int x = dx0; x < dx1; x++) {
            sand_set(&g, x, y, CELL_MAKE(MAT_DIRT, 0));
        }
    }
    const int dirt_count_before =
        ejecta_count_material(&g, EJECTA_W, EJECTA_H, MAT_DIRT);

    sand_set(&g, dx0, dy1 - 1, STONE);
    enum { DIR_RIGHT = 2 };
    sand_impulse_dislodge(&g, dx0, dy1 - 1, DIR_RIGHT, 255,
                          SAND_IMPULSE_SPEED_RAMP);
    for (int i = 0; i < 150; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    const int dirt_count_after =
        ejecta_count_material(&g, EJECTA_W, EJECTA_H, MAT_DIRT);

    free(buf);
    free(cells);

    /* -1 for the one cell the mover itself occupied when the bank was
     * built (it was DIRT there for a moment before being overwritten with
     * STONE), matching plow_build()'s own precedent of accounting for the
     * mover's own starting cell explicitly rather than folding it into an
     * inequality. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(dirt_count_before - 1, dirt_count_after,
        "total dirt cell count must be exactly conserved across the plow "
        "and every transfer it queues - transfer only ever relocates a "
        "cell already on the board, through plain sand_impulse(), so "
        "nothing here should create or destroy one");
}

static void test_a_thrown_powder_grain_conserves_the_water_mass_it_ejects(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "conservation grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "conservation impulse queue must fit in what the framebuffer "
        "leaves");

    const int dx0 = 20, dx1 = dx0 + EJECTA_WATER_W;
    const int dy1 = EJECTA_H - 1, dy0 = dy1 - 1;
    sand_t g;
    memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
    sand_init(&g, cells, EJECTA_W, EJECTA_H, 5u);
    sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
    for (int x = 0; x < EJECTA_W; x++) {
        sand_set(&g, x, EJECTA_H - 1, STONE);
    }
    for (int x = dx0; x < dx1; x++) {
        sand_set(&g, x, dy0, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    const long water_mass_before = mass_of(&g, EJECTA_W, EJECTA_H, MAT_WATER);

    sand_set(&g, dx0, dy0, STONE);
    enum { DIR_RIGHT = 2 };
    sand_impulse_dislodge(&g, dx0, dy0, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);
    for (int i = 0; i < 4; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    const long water_mass_after = mass_of(&g, EJECTA_W, EJECTA_H, MAT_WATER);

    free(buf);
    free(cells);

    /* MASS_MAX, not an inequality - one full cell's worth was overwritten
     * by the mover before ever entering the pool, the same accounting
     * test_a_thrown_powder_grain_conserves_the_dirt_it_ejects uses. */
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)(water_mass_before - MASS_MAX),
        (int)water_mass_after,
        "total water MASS must be exactly conserved across the plow and "
        "every transfer it queues, checked by mass rather than cell count "
        "- a liquid's own cell count is free to change as it spreads or "
        "merges without a drop being lost (mass_of()'s own comment), but "
        "the total amount must not move");
}

/* --- free fall and shaking ---------------------------------------------- */

static void test_nothing_moves_in_free_fall(void)
{
    fixture();
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    sand_step(&s, 0, 0, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0),
        "with no gravity there is no down, so nothing falls");
}

static void test_shaking_spreads_a_pile_sideways(void)
{
    fixture();

    /* A single tall column. Left alone it topples slowly; shaken hard it
     * should flatten, so its highest grain ends up lower. */
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, SAND_FIRST_SHADE);
    }

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1, 255);
    }

    int highest = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) != SAND_EMPTY && y < highest) {
                highest = y;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(2, highest,
        "shaking must flatten the pile, not leave the column standing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, sand_count(&s),
        "and shaking must still conserve grains");
}

/* --- on the real grid, on the real chip --------------------------------- */

/* Must match app_sand.c. Duplicated rather than shared because sand.h has no
 * business knowing the screen size - see the note at the top of sand.h.
 *
 * Outside the DEVICE_BUILD block below, along with the tiling that uses it,
 * so the host can check the SHAPE of the mixed-material scene even though
 * only the device can time it. */
#define REAL_W 184
#define REAL_H 224

/* How much of the mixed-material scene is left empty, so a gravity flip has
 * somewhere to launch into. */
#define EMPTY_SHARE_PERCENT 33

/* Which material lands on cell (x, y) in the all-pairs tiling.
 *
 * Bands were the first attempt and covered far less than they looked like
 * they did: stacking materials in horizontal strips puts only the
 * vertically-adjacent pairs in contact, and measured, just 20 of the 66
 * possible pairs ever met. Two thirds of the reactions this simulation can
 * perform never fired in the scene whose whole purpose is to fire all of
 * them.
 *
 * This tiles instead. Horizontal neighbours in row y differ by `stride`,
 * and successive rows step through every possible difference, so every pair
 * of materials ends up adjacent somewhere and the pattern wraps without a
 * seam.
 *
 * One copy, called by both the device test that times the scene and the
 * host test that checks its coverage. Written out twice they could drift,
 * and the host check would then be verifying a pattern nobody runs. */
static inline int all_pairs_material_at(int x, int y, int first, int n_mats)
{
    const int stride = (y % (n_mats - 1)) + 1;
    return first + ((x * stride + y) % n_mats);
}

/* Every pair of materials really is adjacent somewhere in that scene.
 *
 * The property the scene exists for, and until now it was a number somebody
 * measured by hand once and wrote in a comment - "66 of 66" - taken on
 * faith through two subsequent materials. It is derived from MAT_COUNT, so
 * it should survive adding one, but "should" is what a test is for: a
 * tiling that quietly lost coverage would leave the worst case measuring
 * less than it claims while still passing its own budget.
 *
 * Host-side, because coverage is a property of the pattern and needs no
 * clock. Only the timing has to happen on the chip.
 *
 * GUNPOWDER IS NOT, AND CANNOT BE, ONE OF THE PAIRS THIS COVERS -
 * all_pairs_material_at() enumerates material_id_t values, and gunpowder
 * is not one: it is a byte range inside MAT_EXTENDED's own nibble
 * (GUNPOWDER_BASE, material.h), so every slot this tiling assigns to
 * m == MAT_EXTENDED paints plain ice (CELL_MAKE(MAT_EXTENDED, 0)), never
 * gunpowder. Its own contact with fire, lava, conducted heat, water and
 * acid is exercised directly instead - see this suite's own gunpowder
 * section - rather than forcing an awkward second identity onto a
 * tiling keyed by id, which would also perturb
 * test_a_gravity_flip_on_every_material_at_once_stays_sane's calibrated
 * device budget for a scene this test doesn't touch. */
static void test_the_mixed_scene_puts_every_material_pair_in_contact(void)
{
    const int first  = MAT_EMPTY + 1;
    const int n_mats = MAT_COUNT - first;
    const int top    = (REAL_H * EMPTY_SHARE_PERCENT) / 100;
    const int want   = (n_mats * (n_mats - 1)) / 2;

    /* One bit per unordered pair. */
    static bool seen[MATERIAL_MAX][MATERIAL_MAX];
    memset(seen, 0, sizeof seen);

    int found = 0;
    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = all_pairs_material_at(x, y, first, n_mats);
            const int nb[2] = {
                x + 1 < REAL_W ? all_pairs_material_at(x + 1, y, first, n_mats) : m,
                y + 1 < REAL_H ? all_pairs_material_at(x, y + 1, first, n_mats) : m,
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

    char why[160];
    snprintf(why, sizeof why,
             "the mixed-material scene must put all %d pairs of %d "
             "materials in contact - it reached %d, so some reaction it "
             "claims to exercise never fires in it",
             want, n_mats, found);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, found, why);
}

/* --- three more scenes, built once and shared with a device benchmark --- */

/* A previous round of this project spent three device rounds optimising a
 * function a failing benchmark never actually called - the benchmark timed
 * a scene that did not exercise the code it claimed to. The rule that came
 * out of it: a benchmark must be proven to run the reactions it claims to
 * measure, and "proven" means a host test that builds the SAME scene
 * through the SAME builder and checks the reactions really fired, not a
 * comment asserting they do. The three scenes below follow that shape -
 * see all_pairs_material_at() above for where the pattern started. */

/* Four liquids of different density, painted upside down. Left alone in
 * their own settled order - lava at the bottom, oil on top, water and acid
 * between - the four of them stratify within a few steps and the scene
 * goes quiet: each layer finds its level and the interfaces that were
 * doing the reacting stop touching. Painted INVERTED instead - lava on
 * top, then acid, then water, then oil at the bottom - every layer has to
 * migrate through every other layer to reach where density wants it, so
 * the interfaces stay in contact and reacting for the whole measured
 * window instead of resolving into inert bands.
 *
 * One copy, called by both the device test that times it and the host test
 * below that checks the reactions it claims to keep alive actually are. */
static void build_four_liquid_scene(sand_t *s)
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

/* The property the scene above exists for: that inverting the density
 * order really does keep the reactions running instead of merely moving
 * where they happen. Host-side, same reasoning as
 * test_the_mixed_scene_puts_every_material_pair_in_contact - coverage is a
 * property of the scene and needs no clock, only the timing needs the
 * chip.
 *
 * Runs with the app's own per-material scatter, decay and mobility rather
 * than the defaults, because app_sand.c does too - see the device test
 * below for why that setting matters here specifically. */
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
    /* Dropped to 8 for one round while SAND_ACID_DILUTE_MASS_BIAS (sand.h)
     * let the acid band in this scene genuinely contest the water band it
     * sits against without water paying any cost of its own for losing -
     * every bite either grew a new water cell for free or grew a new acid
     * cell for free, so whichever side got the local upper hand snowballed.
     * Restored to 50 once the water/acid dilution ladder made BOTH
     * outcomes cost the winning side a cell (see that ladder's own comment,
     * sand_reactions.c) - water is no longer a runaway resource once
     * either side genuinely has to pay to win, and steam production is
     * back over 500 at the constants current when this was re-measured,
     * comfortably clearing the original floor again. */
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
 * the simulation - it is a heat source, it quenches to stone in water, it
 * boils water to steam, it turns sand to glass by heat, it ignites both
 * wood and oil, and it flares - so this puts all six of those in one scene
 * instead of spending one test per reaction.
 *
 * The empty column matters more than it looks. Without it, the roof water
 * perches on top of the columns and takes most of a minute to reach the
 * lava, so the quench and boil reactions - two of the six this scene
 * exists to exercise - never fire inside the measured window at all. With
 * it, water has somewhere to fall straight through, and reaches the lava
 * while the scene is still burning. */
static void build_lava_stress_scene(sand_t *s)
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

    /* This scene already has both ingredients a plant needs sitting in it -
     * wood, and sand that could take up water and become soil - and yet it
     * never grows one: the roof water reaches the lava through the chute
     * and flashes straight to steam before it ever gets to wet the sand,
     * so no dirt is ever made and the wood stays dry for the whole run.
     * That is an accident of how this scene happens to be tuned, not a
     * property anyone has checked - and the plant materials are under
     * active development, so pin it here instead of leaving it to keep
     * holding by luck. The device test beside this one gets its frame
     * budget pegged from a hardware capture of this same scene; if plant
     * growth ever starts happening inside that measured window, the
     * number being pegged would quietly stop describing what the test
     * claims to measure. This assertion is what makes that change
     * announce itself instead of passing silently. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended,
        "the lava stress scene should not be growing any plants - if it "
        "is, the device test's frame budget is no longer measuring the "
        "scene it claims to");
}

/* An edge-to-edge checkerboard of smoke and steam, with one spark of fire
 * in a bottom corner. The same "deliberately synthetic worst case, not
 * something the pour brush can produce" framing as the two full-screen
 * fire tests below already use for an edge-to-edge screen of fire: no
 * scene a user can actually paint packs the whole grid with gas, but the
 * reactions pass has to survive the case where one does.
 *
 * What this catches that neither of those two does: fire and plain gas
 * have no convection behaviour, but smoke and steam do - they warm what
 * they touch - and no benchmark in this suite has ever put either of them
 * on screen in quantity before. This is the scene where the reactions
 * pass's per-cell neighbour work for gases actually runs.
 *
 * Left at the DEFAULT scatter, decay and mobility - deliberately NOT the
 * per-material settings build_four_liquid_scene() uses above. At
 * per-material decay the smoke and steam fade away within the measured
 * window, and the scene stops being the steady worst case it exists to
 * be. */
static void build_smoke_and_steam_scene(sand_t *s)
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

/* A lattice of glass-walled compartments - 20 columns by 24 rows, 480 in
 * all - each one a ring of glass around a single payload, with a shatter
 * trigger sitting just outside the ring rather than inside it. Every other
 * thermal-shock test in this file places one pane at a chosen temperature
 * and drops one cold or hot thing next to it; this scene exists to ask
 * what the mechanism does at the scale the pour brush can actually
 * produce, with hundreds of panes cracking, draining and re-heating at
 * once instead of one.
 *
 * THE INVARIANT THAT MAKES THE SCENE HONEST: every ring is painted at
 * variant 2, 3 or 4 - strictly between SAND_SHOCK_COLD (1) and
 * SAND_SHOCK_HEAT (5) - so no compartment is born already qualifying for
 * a crack. An earlier draft of this scene used an asymmetric range that
 * reached down to 0 and 1, and it was a real dead end: those rings
 * shattered on step 1, through whichever shock direction their family was
 * NOT meant to be exercising, before the outside trigger had ramped
 * anything at all - the scene was testing its own setup rather than the
 * mechanism. Starting strictly inside the gap is also the stagger lever:
 * step_one_cold_cell() moves a pane one level per successful roll, so a
 * ring at 2 is one chill from the cold threshold and a ring at 4 is
 * three - and the same distances the other way round for the climb to
 * SAND_SHOCK_HEAT - so the 480 compartments do not all cross at once
 * even though they are all built from the same two triggers.
 *
 * WHY THE COMPARTMENTS ARE SEPARATED: each ring is 20 cells, far under
 * crack_run()'s CRACK_MAX of 256, and what keeps one shock from reaching
 * a neighbouring compartment's glass at all is the tile's own layout, not
 * the grid's leftover margin: lx 0 and ly 0-1 are left empty and the
 * trigger takes lx 1, lx 8 and ly 8, so the nearest glass in the next
 * tile is three cells away with a trigger and empty space in between.
 * (The four spare columns and eight spare rows - 20x9 is 180 of 184 and
 * 24x9 is 216 of 224 - are unused margin along the right and bottom
 * edges, and separate nothing.) See test_a_crack_does_not_jump_to_a_-
 * separate_pane, which is the same guarantee this scene leans on at 480x
 * the scale.
 *
 * WHY LAVA IS A PAYLOAD AND NEVER A TRIGGER: lava is a liquid, and an
 * outside trigger sits in a bare one-cell-wide U with nothing under it
 * from below the grid - a liquid there would simply drain away before it
 * ever got to test anything. The two outside triggers are instead the
 * materials that hold still on their own: burning wood (KIND_STATIC) and
 * ice (KIND_STATIC). Lava only ever appears as a payload, sitting inside
 * a box that can actually hold it. This is a deliberate departure from
 * the original sketch for this scene, which asked for lava as an outside
 * trigger too - it does not survive contact with how liquids move.
 *
 * WHAT THE FAMILY SPLIT DOES AND DOES NOT DO: the left ten columns
 * (family C) pair a burning-wood trigger with a cold payload - ice or
 * snow - so they are BUILT to favour the cold-onto-hot direction, and the
 * right ten columns (family H) pair an ice trigger with a hot payload -
 * wood or lava - to favour hot-onto-cold. MEASURED, the split is not
 * pure: family C's own cold payload chills its ring past SAND_SHOCK_COLD
 * from the inside, so hot-onto-cold fires there too, and family H's own
 * hot payload pushes its ring past SAND_SHOCK_HEAT from the inside, so
 * cold-onto-hot fires there as well. Both directions run in both halves
 * from step 1. The split earns its place as the payload/trigger MATRIX -
 * four combinations of {cold, hot} outside x {cold, hot} inside, laid out
 * so every compartment has an outside push and an inside push in the same
 * or opposite sense - not as proof that either half exercises only one
 * direction. What actually proves each direction fires is the pair of
 * counters in the host test below, which look at the mechanism's own
 * precondition directly rather than trusting the geometry to imply it.
 *
 * What this measures that nothing else in this file does: heat_ramp
 * climbing through hundreds of independent panes at once, in-glass
 * conduction along each ring, crack_run() firing under sustained load
 * instead of once, and the mixed aftermath of that all at once -
 * meltwater, steam, escaping fire and falling cullet sharing the same
 * screen.
 *
 * Runs at the app's own per-material scatter, decay and mobility, the
 * same choice build_lava_stress_scene() makes above and for the same
 * reason: app_sand.c does too. */
static void build_thermal_shock_scene(sand_t *s)
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

/* The two shock directions - cold arriving at hot glass in
 * step_one_cold_cell(), heat arriving at cold glass in try_heat_-
 * transform() - are different code paths that can break independently
 * and have (see test_heat_arriving_at_frosted_glass_cracks_it, which
 * exists for exactly that reason). This scene claims to exercise both at
 * once across the whole lattice, and the two counters below check that
 * claim directly rather than trusting the payload/trigger matrix to
 * imply it - see build_thermal_shock_scene()'s comment for why the
 * matrix alone is not that proof.
 *
 * d1_ready counts MAT_GLASS cells at variant >= SAND_SHOCK_HEAT with a
 * cardinal neighbour whose reaction_of() has chills != 0 - exactly
 * step_one_cold_cell()'s shock precondition, which takes no roll once it
 * holds. d2_ready is its mirror: MAT_GLASS cells at variant <=
 * SAND_SHOCK_COLD with a cardinal neighbour that cell_is_burning(), which
 * is try_heat_transform()'s precondition, also roll-free. Roll-free is
 * the whole reason to count preconditions rather than cracks: a standing
 * precondition is a fact about the board, not a probability, so a
 * non-zero count is real evidence that direction is live. It is not quite
 * a promise that those exact panes break next step - the movement passes
 * run first, and a drift or a melting block can leave the pane before the
 * reactions pass reaches it - which is why the assertions below grade
 * these on HOW MANY STEPS the precondition stands, not on a count.
 *
 * Both are counted after every one of the 10 measured steps, because the
 * claim is that each direction keeps firing across the window, not just
 * once at the start.
 *
 * WHY TEN STEPS, AND WHY THIRDS OF (step - 1) / 3: the window is graded
 * by charging each step's new cullet to one third of it - steps 1-3, 4-6,
 * 7-10 - and ten is the shortest window where the LAST third still earns
 * a real share of the total. Measured, ten steps split 54.1 / 28.9 /
 * 17.0 percent; twelve, fifteen and twenty all push the tail under 15 as
 * the early cracking dominates more and more of the run (11.8, 11.5 and
 * 13.6 percent), and eight steps split honestly into thirds leaves the
 * last one at 11.0. The divisor and the step count are one decision:
 * change the window without changing /3 and the buckets stop being
 * thirds at all, which is exactly how an earlier draft came to grade a
 * 2/2/4 split as if it were 3/3/4.
 *
 * The cullet tally needs a STICKY mask - a cell that was ever cullet,
 * tracked separately from what is cullet right now - and that is a real
 * finding rather than a stylistic choice. Cullet is MAT_SAND at a variant
 * SAND_CULLET_BASE or higher, and it is not inert: sand.heats_to is
 * MAT_GLASS, so a fallen shard sitting near a hot payload can re-fuse
 * into glass and later crack again. A naive per-step delta on a live
 * MAT_SAND count goes negative the moment that happens, undercounting
 * exactly the churn this scene exists to show. The sticky mask only ever
 * grows, so "new cullet this third" stays a meaningful, non-negative
 * quantity even while individual cells are cycling glass -> cullet ->
 * glass under the payload's heat.
 *
 * The mask is a BITSET, not a byte per cell. This is the only test in
 * the file that needs a second full-grid buffer alongside `big`, and the
 * device has only about 68 KB of heap left once the display framebuffer
 * is carved out of it - two 41,216-byte grids do not fit in that, one
 * byte per cell does. The first device run of this test with a byte mask
 * failed AND leaked 41,240 bytes for the rest of boot, because the null
 * check on the third malloc aborted the test before the frees at its end
 * ever ran. One bit per cell brings the mask down to 5,152 bytes, which
 * fits comfortably. */
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

        /* Grow the sticky mask, then charge the growth to this step's
         * third of the window - see the comment above for why the mask
         * has to be sticky rather than a live per-step count. The mask
         * only ever grows, so counting each bit's clear-to-set transition
         * right here, as it happens, is exactly equivalent to rescanning
         * the whole mask afterwards and diffing against the previous
         * total - a rescan could only ever find the same bits this loop
         * just set. */
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
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(800, steam,
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

    /* Model: the lava stress scene's own plant pin above. This scene
     * makes meltwater and has sand about (both plain and cullet), so wet
     * soil is reachable in principle; the plant materials are under
     * active development, and if growth ever starts happening inside
     * this measured window, the frame budget the device benchmark beside
     * this test is pegging from a hardware capture would quietly stop
     * describing the scene it claims to. This assertion is what makes
     * that change announce itself instead of passing silently.
     *
     * Also note this scene cannot reuse the lava stress scene's plain
     * cell_is_extended(c) form: this scene's own payload uses
     * MATX(MATX_ICE), which IS an extended cell, so the pin has to name
     * MATX_PLANT specifically or it would fail on the ice this scene
     * paints on purpose. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, matx_plant,
        "the thermal shock lattice should not be growing any plants - if "
        "it is, the device test's frame budget is no longer measuring "
        "the scene it claims to, and the ice payload means the usual "
        "cell_is_extended() plant pin cannot be reused here as-is");

    /* NOT an exact zero any more - see this file's own precedent on why a
     * pinned RNG-driven outcome is "measured, not derived... not a law"
     * (test_a_blast_inside_a_sealed_vessel_stays_inside_it's own comment
     * makes the same point for a dislodged wall's landing cell). Family
     * C's rings melting into lava under their own payload and trigger's
     * heat is real and expected eventually (unaffected by reaction_t.
     * vent_chance - lava never even appears in family C's own payload,
     * see build_thermal_shock_scene()'s comment) - only WHEN was ever
     * pinned here, and that timing rides the same shared RNG stream every
     * other reaction on the board draws from. Adding a second, per-step
     * roll to vent_chance (step_one_burning_cell(), sand_reactions.c) for
     * the many lava payloads on the right half advances that stream
     * faster on every step this scene has lava under a lid, which pulled
     * family C's own melt roll earlier - measured at step 9 now, not 16.
     * THE VENT MECHANISM DESCRIBED ABOVE IS GONE (bd esp32c6-0f2); this
     * paragraph is the recorded history of why the bound was widened,
     * not live behaviour. The bound stays for the reason the last
     * sentence gives, which never depended on venting.
     *
     * A small, single-digit residual by step 10 is exactly that timing
     * shift, not a new leak between the two families (lava is never
     * itself thrown by a vent - so this is always a LOCAL glass-to-lava
     * conversion,
     * never material crossing over from the right half). What this must
     * still catch is a real regression widening that leak far past a
     * timing nudge - the original, unbounded run measured 123 left-half
     * lava cells by step 40, three orders of magnitude past this bound. */
    TEST_ASSERT_LESS_THAN_MESSAGE(10, lava_left,
        "family C's rings (the left half) must not have melted into lava "
        "in bulk inside this window - a small residual is an expected "
        "RNG-timing shift (see this assertion's own comment), but this "
        "many means the window, or something else about this scene, "
        "genuinely regressed");

    /* step_one_warming_cell()'s call site is gated on three things at
     * once - r->warms, may_have_temperature and may_have_heat_holder -
     * see sand_reactions.c, the branch a previous tuning round added
     * that third flag for. The four assertions below pin all three, plus
     * the physical fact behind the last of them (something on the board
     * really can hold a temperature, not merely a flag saying so).
     * Asserting them together is what proves the warming path is
     * genuinely reachable in this scene rather than skipped by a gate
     * that happens to be shut. */
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

/* The boiler from test_the_boiler_end_to_end, scaled from one column to
 * the whole 184x224 grid and run as a SUSTAINED STEADY STATE rather than
 * a transient - the opposite of build_thermal_shock_scene() above,
 * deliberately, so the pair covers both shapes of thermal load this
 * simulation has to handle: a burst of damage that runs its course, and
 * a heat source left running that has to keep producing without either
 * exhausting its fuel or its water.
 *
 * The slab is 11 rows thick, the pour brush's real thickness - the same
 * figure test_the_boiler_end_to_end uses, and for the same reason.
 * conduct_heat() attenuates at roughly 0.86 per cell of depth it has to
 * cross, so slab thickness is the THROTTLE on how fast the basin can
 * boil: eleven rows is what keeps the rate sustainable across the whole
 * measured window instead of exhausting the basin partway through it.
 *
 * TWO BURNERS ON PURPOSE: lava never decays, so it is the steady heat
 * source; wood burns down (burn_decay 24) and is there so the OTHER heat
 * source path - an ember rather than a permanent liquid - is covered by
 * the same scene instead of needing a second one. Measured, all 356 wood
 * cells painted are still lit at the end of the window, and both halves
 * of the basin boil at close to the same rate - see the host test's
 * per-half assertions.
 *
 * WHY THE BOILING RATE IS SELF-SUSTAINING: steam made at the slab is
 * lighter than the water sitting above it, so try_bubble() (sand_gas.c)
 * swaps it upward one cell at a time and water falls back down onto the
 * slab to be boiled in its turn. The basin keeps refilling its own hot
 * face on its own; no extra geometry - chutes, gaps, anything - is
 * needed to make that happen, unlike build_lava_stress_scene() above,
 * which needs its chute for exactly this reason.
 *
 * WHY THE BURNER IS FULLY ENCLOSED (stone side walls the full depth of
 * the basin, a stone slab, the grid floor underneath): so that flare has
 * almost nowhere to put fresh fire, and the cell count therefore says
 * something about the boil rather than about how much empty space
 * happened to be lying around.
 *
 * "Almost" is the honest word, and it is why the host test below asserts
 * a FLOOR on the count rather than an equality. Measured: the count sits
 * exactly at its window-start value for the first twenty steps of the
 * measured window and then starts climbing, reaching 8293 from 8280 by
 * the end of it - the boil has by then opened enough gaps in the water
 * above the slab for flare to reach them. An equality would simply fail
 * here - and it is worth knowing that it held for the shorter settle an
 * earlier draft of this scene used only by a SINGLE step: total step 41
 * is where the count first moves, and that draft stopped at 40. That is
 * not a margin worth building an assertion on. */
static void build_boiler_scene(sand_t *s)
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

/* The boiler above really does keep boiling for the whole window rather
 * than front-loading its output and going quiet, checked the same way
 * the other scenes in this section are: build it through the same
 * function the device test uses, step it the same number of times, and
 * measure.
 *
 * 20 settle steps first - twice the "let it get going" allowance
 * test_four_liquids_reacting_at_once_fits_in_the_frame_budget gives its
 * own scene, because a basin takes longer to reach a steady boil than a
 * liquid stack takes to start mixing: at ten steps the board is still
 * filling with the first flush of steam (295 cells of it), at twenty it
 * is boiling at a rate that then holds for the whole window.
 *
 * Then 30 measured steps, sampled at 0, 7, 15, 22 and 30 steps into the
 * measured window - four intervals, so the per-quarter loss assertions
 * below can catch a basin that boils hard at first and then tails off,
 * which a single before/after comparison could not. Measured, the four
 * quarters lose 27, 34, 26 and 25 cells of water - a fraction of the
 * figures this test saw before reaction_t.boils existed, since water
 * now resists conducted-heat boiling instead of flashing to steam
 * unconditionally the moment heat reaches it (see material.c's own row
 * for water's real figure, raised once from its own first tuning pass
 * for resisting more than wanted). Level enough to call it steady, and
 * the assertions are held at 12 - roughly half the measured minimum -
 * so ordinary quarter-to-quarter variation does not read as a stall. */
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
    /* Condensation is a separate, orthogonal mechanic from the boiling
     * this scene measures, and it is not one-for-one the way boiling
     * water into steam is - a 2x2 patch of steam collapses into a SINGLE
     * water cell, a net loss of three cells each time it fires. Left at
     * its real (rare) figure, it would eventually violate the
     * sand_count_now floor below on a long enough run, for a reason that
     * has nothing to do with what this test exists to measure. Forced
     * off here; condensation gets its own dedicated test instead. */
    sand_set_condenses(&s, 0);
    /* The lava-burst chance (bd esp32c6-mqt) is the same kind of
     * orthogonal mechanic, for the same reason the now-removed vent
     * mechanism was (bd esp32c6-0f2): this scene's lava burner sits
     * fully enclosed - side walls, a stone slab above, the grid floor
     * below (build_boiler_scene(), above) - which puts a complete lid
     * over every burner cell regardless of which way is down. Left at
     * its real figure, a burst partway
     * through the measured window disrupts the slab that is supposed to
     * hold steady for the whole test, the same disruption vent_chance
     * used to risk here before it was removed. What this test exists to
     * measure is boiling, not bursting; forced off here for the same
     * reason condensation is, just above. */
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

/* Sand and dirt poured in equal amounts, then water dropped over both until
 * it settles - the first benchmark in this file to exercise the wet-earth
 * path at all. Sand slowly BECOMES dirt (material.c's MAT_SAND row:
 * `.soaks = 8, .soaks_to = MAT_DIRT`) while the dirt it becomes goes on
 * drinking, far faster (MAT_DIRT: `.soaks = 60`) and only slowly gives that
 * moisture back up (`.dries = 2`, a thirtieth of its own soak rate). A wet
 * dirt cell's variant IS the moisture level while it is wet (material.h's
 * CELL_MOISTURE()/SOIL_MOISTURE_MAX, 0 dry to 7 saturated; the same nibble
 * reads as a dry TONE once it is not, which is what pays for the shading
 * this scene never looks at) - so this scene's own composition keeps
 * changing while it runs: the sand/dirt split at the end is not the split
 * it was poured with.
 *
 * REACTION DISPATCH UNDER SUSTAINED LOAD is the thing this scene exists to
 * measure, not just liquid movement, and getting that to actually happen
 * turned out to be less obvious than it sounds. sand_step_reactions()
 * (sand_reactions.c) gates its whole pass behind six content flags and
 * clears each one the moment a pass finds nothing for it to do; a flag is
 * re-armed only by a cell WRITE (sand_priv.h's latch_content_flags(),
 * called from sand_set() and from a handful of reaction outcomes) - NEVER
 * by ordinary liquid movement in sand_liquid.c. A board of nothing but
 * water arms may_have_moisture once, at paint time, because water is
 * KIND_LIQUID - and the very first reactions pass clears it straight back
 * off, since nothing wet is touching anything that soaks. Nothing in
 * plain liquid movement ever re-arms it after that. Dirt is what breaks
 * the silence: a soil cell holding any MOISTURE keeps re-arming the flag
 * on every write that touches it (sand_priv.h: `r->dries != 0 &&
 * CELL_MOISTURE(cell) != 0`), so once this scene is wet, the reactions
 * pass keeps running every step for as long as any dirt anywhere is damp
 * - which, measured, is the whole window below. CELL_MOISTURE(), not the
 * raw variant this comment used to name: a DRY cell's variant is a tone,
 * and testing the whole nibble latched this for seven of every eight dry
 * cells for good, whether or not anything on the board was ever wet -
 * fixed alongside the re-encoding that gave dry soil those eight tones in
 * the first place (see material.h's own comment on soil's state split).
 *
 * EQUAL, AND MIXED DOWN TO ONE CELL. Sand and dirt are painted as a
 * single-cell checkerboard - material = (x + y) & 1 - rather than as two
 * stacked halves or even column-wide stripes, so the water above meets
 * both in exactly the same proportion at every column and every depth it
 * reaches, instead of one material happening to sit nearer the surface
 * and racing the other's soak rate by geometry rather than by the
 * materials' own numbers. Fully packed, no gaps, so the bed is exactly as
 * stable a floor as the other builders' solid blocks above - 12,420 cells
 * of each, verified by the host test below.
 *
 * WHY THE WATER IS PAINTED RESTING DIRECTLY ON THE EARTH, WITH NO GAP.
 * The obvious way to write "drop water over them" is to leave headroom
 * above the bed and let it fall - and an early draft of this scene did
 * exactly that, with a ten-row gap between the water and the earth's
 * surface. It measured nothing at all: see the may_have_moisture
 * reasoning above - the flag is armed once, at paint time, and with a
 * gap to fall through first it is cleared again by the very first
 * reactions pass, ten-odd steps before the water actually reaches the
 * earth, and nothing in ordinary liquid movement ever re-arms it after
 * that. Four hundred steps of that draft produced not one wet dirt cell.
 * Painting the water already flush against the earth's surface keeps
 * real contact present from step one, while the flag is still armed from
 * painting it, so the very first reactions pass finds moisture and keeps
 * the flag alive for the ones that follow.
 *
 * NOT A FULL-WIDTH SLAB, EITHER. A slab already spanning the whole 184
 * columns at a uniform depth is already at rest - flat on a flat floor,
 * with nothing for gravity or the liquid's own mass-diffusion to do -
 * which would leave nothing for "until the water settles" to describe.
 * Painted instead over the CENTER HALF of the width only (x in [46,
 * 138)), it has to spread sideways to reach the flanks, which is the
 * active settling this benchmark is named for: measured (three seeds),
 * the water first touches earth across the full 184-column width
 * somewhere between step 30 and step 31, having started touching only
 * the center 92 columns.
 *
 * ENOUGH TO PERCOLATE, NOT JUST WET A CRUST. Percolation depth - the
 * deepest row below the earth's surface holding any dirt moisture at all
 * - reaches row 13 by the time the water has finished spreading (step
 * 35, the settle allowance below) and keeps climbing through the whole
 * measured window, to row 17-22 by step 65 (measured, three seeds). The
 * earth bed is 135 rows deep, so this is a front still advancing into a
 * bed nowhere near saturated, not a shallow soak that stalls at the
 * surface.
 *
 * sand_set_soak() is OFF by default, unlike scatter, decay and mobility -
 * see its own comment in sand.h: "half the tests in the suite put sand in
 * water to check that sand SINKS", and a mechanic that arrived switched
 * on would have rewritten every one of them. None of the other builders
 * in this section call it, because none of them need to; this one does,
 * and is the only one that does. Left off, this whole scene would
 * silently measure nothing but liquid movement. Runs at
 * SAND_SOAK_PER_MATERIAL alongside the app's own scatter, decay and
 * mobility settings - app_sand.c calls all four. */
static void build_wet_earth_scene(sand_t *s)
{
    const int earth_top = (REAL_H * 2) / 5;    /* bottom three fifths,
                                                 * 135 rows */
    for (int y = earth_top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const material_id_t m = ((x + y) & 1) ? MAT_SAND : MAT_DIRT;
            sand_set(s, x, y, CELL_MAKE(m, 0));
        }
    }

    /* Water over the center half only, resting flush on the earth - see
     * the comment above for why neither a headroom gap nor a full-width
     * slab would measure the scene this claims to. */
    const int water_h = earth_top / 2;
    const int water_top = earth_top - water_h;
    const int cx0 = REAL_W / 4, cx1 = (REAL_W * 3) / 4;
    for (int y = water_top; y < earth_top; y++) {
        for (int x = cx0; x < cx1; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* One scan, reused for the window's start, its four checkpoints and its
 * end - the water mass still held (summed variant, not cell count: a cell
 * count only moves when a WHOLE unit is used up, see pay_quench_cost()'s
 * "written as CELL_EMPTY rather than a zero variant" reasoning in
 * sand_reactions.c, so it steps in noisy jumps; the mass sum falls by
 * exactly what soaking took, every single step, with no such noise - see
 * the host test below for the seed-to-seed numbers that made this the
 * quantity to grade on), how many cells are dirt, and how much moisture
 * they hold between them. */
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

/* The scene above really does keep percolating for the whole measured
 * window, checked with counters taken MID-FLIGHT rather than on the
 * freshly built scene - enumerating what a scene is BUILT from is not the
 * same as what it CONTAINS once running, the round-16 finding that let 300
 * metal cells hide inside an earlier benchmark unnoticed.
 *
 * 35 SETTLE STEPS, not the 20-30 the boiler and lava stress scenes use.
 * The number here is not a "let it get going" allowance in the usual
 * sense - it is chosen against the one concrete milestone
 * build_wet_earth_scene()'s comment names: full-width contact, which
 * measured (three seeds) lands at step 30 for one seed and step 31 for
 * the other two. 35 is that milestone plus a margin, not a round number
 * picked first and checked after - the host test below asserts the
 * milestone directly (touching every column) rather than trusting the
 * step count alone to have reached it, for the same reason the mixed
 * scene's own coverage test above does not trust geometry to imply
 * contact either.
 *
 * Unlike the boiler or the thermal shock lattice, this scene has no tail
 * to avoid measuring past - watched out to 1200 steps on the host (40x
 * this benchmark's own window), water mass keeps falling and dirt keeps
 * gaining at close to the same rate the whole way, because the earth bed
 * is 135 rows deep and nowhere near saturated by the time any budget this
 * suite can afford would stop. The only transient here is the SPREADING
 * one the settle allowance exists to clear - once the water has reached
 * every column, the scene does not go quiet again within any window this
 * file has time to run.
 *
 * FOUR CHECKPOINTS across the 30 measured steps - at +7, +15, +22 and +30,
 * the same spacing test_the_boiler_scene_keeps_boiling_across_the_window
 * uses for the same reason: a single before/after comparison cannot catch
 * a scene that is active at first and stalls partway through. Measured
 * (three seeds, water mass lost per quarter): 274-373 units, comfortably
 * clear of the 150 floor below; moisture gained per quarter: 190-271
 * against a floor of 100. Both floors sit at roughly half the worst-case
 * measured value, the same margin the rest of this file's coverage
 * assertions use.
 *
 * DIRT GAINED PER QUARTER moved when PART 2 of the roots-and-percolation
 * change split percolation out of `spread` into its own, deliberately
 * slower SOIL_PERCOLATE_CHANCE (sand_reactions.c) - sand converting to
 * dirt BELOW a wet cell is exactly the branch that constant now governs,
 * and lateral diffusion (still at the old, unchanged rate) only ever
 * carried part of this scene's total. Re-measured on this scene's own
 * fixed seed (53u - not the three the water-mass and moisture figures
 * above were originally taken across): 48, 51, 31, 54 per quarter. Still
 * nonzero every quarter - conversion has not stalled, it is simply
 * slower, which is the entire point of PART 2 - so the honest fix is a
 * lower floor, not a faster constant that would undo the change this test
 * exists to guard downstream of. 20, at the same roughly-half-of-worst-
 * case margin the other two floors use against the new worst quarter
 * (31), rather than left at the old 50 to keep this scene "passing"
 * through a regression that was never a regression. */
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

/* =========================================================================
 * BLAST SCENES - a settled dune and a detonation at its centre, following
 * the same builder / host-guard-test / device-log-lines shape as
 * build_lava_stress_scene(), build_thermal_shock_scene() and
 * build_boiler_scene() above.
 *
 * WHY THIS EXISTS. Every blast test above this line checks an internal
 * detail of the mechanism - a specific cell's material, an entry's queue
 * order, a count that must stay under some bound - and every one of them
 * passed for four straight rounds while a real detonation on a real
 * device only ever disturbed the top tenth of its own disc (see c0e01a1's
 * own commit message for the full account). None of that internal
 * correctness is proof that a blast LOOKS like a blast - an outcome a
 * player can actually watch happen. This is the first blast test in the
 * file that measures the outcome itself: does material end up outside
 * where it started, how far, and how much of it is gone rather than
 * moved.
 * ========================================================================= */

/* How many steps make one "has anything changed" batch, and how many
 * batches settle_fully() below will spend looking for an unchanged one
 * before giving up. 20 steps a batch keeps the memcmp cost - one pass
 * over the whole grid - proportionate to the stepping it is checking; 300
 * batches (6,000 steps) is a safety rail against an actual bug turning
 * this into an infinite loop, not a number any real dune has come close
 * to needing. */
#define DUNE_SETTLE_BATCH_STEPS 20
#define DUNE_SETTLE_MAX_BATCHES 300

/* A 64-bit FNV-1a fold of the whole grid - settle_fully() below uses two of
 * these, one before a batch of steps and one after, to answer "did
 * anything change" without keeping a second copy of the grid around to
 * memcmp against.
 *
 * That second copy is exactly what this replaced, and the replacement is
 * not a nicety: `scratch`, a caller-owned REAL_W*REAL_H byte buffer, sat
 * alongside `big` for the whole settling phase of every one of this
 * section's five tests - 41,216 bytes each, 82,432 together, MORE than
 * the roughly 66,632 bytes this device has free once the display
 * framebuffer is carved out of the heap, before a single other buffer
 * (the block map, the impulse buffer, a footprint mask) is even in the
 * picture. No amount of shrinking those other buffers can fix that - two
 * full-grid buffers simply do not fit in a heap smaller than either one
 * doubled - so unlike the footprint mask below (still a mask, just a
 * bitset instead of a byte array), the fix here is to not keep a second
 * full-grid buffer at all.
 *
 * A hash cannot prove two DIFFERENT grids are different the way a byte
 * compare can - a collision is possible in principle, and this trades
 * that certainty for one that is merely overwhelming: FNV-1a's avalanche
 * behaviour means a single flipped cell changes most of the 64 output
 * bits, not one, so two genuinely different 41,216-byte grids landing on
 * the same 64-bit fold by chance is far less likely than an actual defect
 * turning up somewhere else in this file's other 500-plus tests. This
 * project already accepts probabilistic reasoning at exactly this scale
 * elsewhere (rng_next() itself, or the "1 in 2^32" a hash this size
 * implies); nothing about a settling check demands stronger proof than a
 * live simulation's own RNG already carries. */
static uint64_t grid_checksum(const uint8_t *cells, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;    /* FNV-1a 64-bit offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= cells[i];
        h *= 0x100000001b3ULL;             /* FNV-1a 64-bit prime */
    }
    return h;
}

/* Steps `s` until one whole batch of DUNE_SETTLE_BATCH_STEPS produces
 * literally no change to the grid, which is what "settled" has to mean
 * for a scene a blast is about to be measured against. A fixed step count
 * can only ever be a guess at how long a pile this size takes to stop
 * moving - a guess that undershoots would silently start measuring a pile
 * that was still falling, confusing the blast's own throw with gravity
 * still finishing its own job.
 *
 * Returns whether it actually converged within the budget above - a
 * caller measuring a scene against this dune must assert on that rather
 * than trust it silently, since a dune that never finished settling is
 * not the scene the rest of the test thinks it is. */
static bool settle_fully(sand_t *s, size_t cells_len)
{
    for (int batch = 0; batch < DUNE_SETTLE_MAX_BATCHES; batch++) {
        const uint64_t before = grid_checksum(s->cells, cells_len);
        for (int i = 0; i < DUNE_SETTLE_BATCH_STEPS; i++) {
            sand_step(s, 0, 1000, 0);
        }
        if (grid_checksum(s->cells, cells_len) == before) {
            return true;
        }
    }
    return false;
}

/* The settled-footprint mask, ONE BIT PER CELL rather than a bool[] - the
 * identical treatment 565f72e already gave the thermal shock scene's
 * ever_cullet mask, and for the identical reason: this is a byte-per-cell
 * flag that only ever holds 0 or 1, on the same REAL_W*REAL_H grid, on the
 * same device budget. A bool[] here would cost 41,216 bytes; the bitset
 * costs 5,152. See EVER_CULLET_BYTES's own comment above for the fuller
 * accounting - this is the same fix, applied to this section's own mask
 * rather than reusing that one, since the two masks answer unrelated
 * questions (settled footprint here, sticky cullet there) and have no
 * reason to share storage or a lifetime. */
#define DUNE_FOOTPRINT_BYTES \
    (((size_t)REAL_W * (size_t)REAL_H + 7) / 8)

static inline bool footprint_get(const uint8_t *mask, size_t idx)
{
    return (mask[idx >> 3] >> (idx & 7)) & 1u;
}

static inline void footprint_set(uint8_t *mask, size_t idx)
{
    mask[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

/* HOW FAR PAST THE DUNE'S OWN EDGE, not how far from an arbitrary point
 * inside it - see this file's own dune-scene tests for why "distance from
 * the detonation centre" turned out to be the wrong question. Found by
 * searching outward from (x, y) in expanding square rings - the same
 * eight-directions-at-once shape ring_dir() in sand_priv.h moves grains
 * in - checking only each ring's own perimeter against `footprint` until
 * one of its bits is set. That is a Chebyshev (8-connected) distance
 * transform, the exact same value a multi-source breadth-first flood
 * fill seeded from every footprint cell would compute for this one cell -
 * two ways of answering the identical question, not two different
 * questions - so switching to it changes nothing about WHAT a grain that
 * merely slid down the dune's own slope reads as versus one genuinely
 * thrown clear (see this file's own top-of-section comment for why that
 * distinction is the entire point).
 *
 * WHY NOT THE FLOOD FILL THIS REPLACED: it answered the question for
 * every one of the grid's 41,216 cells whether or not anything downstream
 * ever asked, which needed `dist` and `queue`, REAL_W*REAL_H ints each -
 * 164,864 bytes apiece, 329,728 together. This scene only ever asks for
 * the handful of cells that turn out to be outside the settled footprint
 * after a blast - 105 of them in a measured run, against the 41,216 the
 * flood fill priced itself for regardless. A bounded RECTANGLE around the
 * footprint - the fix this section's dune scene otherwise follows for
 * `footprint` itself, just scoped down instead of reshaped - could not
 * replace it either: this scene's own settled dune measured 127 cells
 * wide (69% of the grid's own 184-cell width, a wide, short pile rather
 * than a tall narrow one), and even a ZERO-margin box exactly matching
 * that footprint's bounding rectangle costs 24,003 bytes at the smallest
 * correct per-cell types (a 1-byte Chebyshev distance, a 2-byte queue
 * slot - see this file's own commit message for both derivations) against
 * roughly 7,952 bytes left once `big`, `blocks`, `impulses` and the
 * footprint bitset above are accounted for. A per-query outward search
 * has no rectangle to fit into at all: its cost is proportional to how
 * FAR a query cell sits from the nearest footprint cell, not to the
 * footprint's own size, so this pile's actual queries - every one of them
 * resolving within two or three rings, measured - stay cheap regardless
 * of how wide the pile itself gets, and it needs no storage beyond this
 * function's own local variables.
 *
 * `cap` bounds the search so a genuinely pathological grid still
 * terminates - callers pass the largest Chebyshev distance any two cells
 * on this grid could possibly have, (max(REAL_W, REAL_H) - 1) = 223, so
 * the cap can never itself produce a wrong answer for a cell this grid
 * actually contains; it only bounds the search, the same role CRACK_MAX
 * plays for crack_run() in sand_reactions.c, not a correctness knob. */
static int nearest_footprint_distance(const uint8_t *footprint, int w, int h,
                                      int x, int y, int cap)
{
    if (footprint_get(footprint, (size_t)y * (size_t)w + (size_t)x)) {
        return 0;
    }

    for (int r = 1; r <= cap; r++) {
        const int x0 = x - r, x1 = x + r;
        const int y0 = y - r, y1 = y + r;

        for (int xx = x0; xx <= x1; xx++) {
            if (xx < 0 || xx >= w) {
                continue;
            }
            if (y0 >= 0 &&
                footprint_get(footprint, (size_t)y0 * (size_t)w + (size_t)xx)) {
                return r;
            }
            if (y1 < h &&
                footprint_get(footprint, (size_t)y1 * (size_t)w + (size_t)xx)) {
                return r;
            }
        }
        for (int yy = y0 + 1; yy <= y1 - 1; yy++) {
            if (yy < 0 || yy >= h) {
                continue;
            }
            if (x0 >= 0 &&
                footprint_get(footprint, (size_t)yy * (size_t)w + (size_t)x0)) {
                return r;
            }
            if (x1 < w &&
                footprint_get(footprint, (size_t)yy * (size_t)w + (size_t)x1)) {
                return r;
            }
        }
    }
    return cap + 1;   /* not found within cap - see this function's own comment */
}

/* The largest Chebyshev distance any two cells on this grid could ever
 * have - see nearest_footprint_distance()'s own comment for why this is
 * the cap it is called with, and why that makes the cap a search bound
 * rather than a correctness one. */
#define NEAREST_FOOTPRINT_CAP ((REAL_W > REAL_H ? REAL_W : REAL_H) - 1)

/* Mirrors app_sand.c's DETONATE_RADIUS_PX/APP_IMPULSE_MAX exactly, at the
 * same CELL_MIN scale REAL_W/REAL_H already represent (see their own
 * comment above) - so a sweep run against this scene, and the numbers it
 * reports, read on the same scale a real device detonation does, not
 * some arbitrary test-only radius.
 *
 * WAS 24 (48 px), DOUBLED TO 48 (96 px) - following a device request,
 * "it needs a much bigger radius in general" - and THAT DOUBLING BRIEFLY
 * BROKE THE FEATURE OUTRIGHT on real hardware, TWICE, for two different
 * reasons caught by two different device flashes. First: the impulse
 * buffer used to be sized FROM this radius
 * (`(355*r*r)/113 + 5*r + 3` entries), so doubling it demanded a ~43.8 KB
 * allocation nothing on this board could satisfy. Second, after that got
 * fixed by decoupling buffer size from radius (see below) and sizing the
 * fixed budget against a ~76 KB TOTAL free-heap boot-log figure instead:
 * a live serial capture at that "fixed" budget still failed, showing
 * `heap_caps_get_largest_free_block()` stuck at an identical 14,592
 * bytes across three different quality settings - proof the relevant
 * number was never total free heap at all, but the single largest
 * contiguous run, which can be far smaller than the sum of everything
 * technically free. Neither failure was visible to this test's fixed RNG
 * seed and unlimited host `malloc()` - nothing here ever fails to
 * allocate, which is exactly why this bug needed a device twice to be
 * believed. See SAND_IMPULSE_BUDGET_BYTES's own comment in app_sand.c
 * for the full arithmetic of both incidents.
 *
 * NEITHER FIX WAS A SMALLER RADIUS. Both were decoupling buffer size
 * from radius entirely: APP_IMPULSE_MAX (app_sand.c) is now a FIXED
 * entry count chosen once from the device's own heap budget - now
 * against the observed largest-contiguous-block number, not total free
 * heap - and sand_explode() itself (sand.c) now THINS its own seeding
 * density automatically whenever a disc's true cell count would exceed
 * whatever buffer it was actually given - evenly, across the whole disc,
 * rather than truncating its shape - see queue_outward_impulse()'s own
 * comment in sand.c. That decoupling is what let the radius become a
 * genuinely free choice again - which is exactly what it became next:
 * a real device confirmed 96 px allocating and detonating without a
 * crash, at a visibly thinned density, and the user chose to trade that
 * size back down for a SMALLER radius at FULL density instead, on the
 * actual measured numbers (see DETONATE_RADIUS_PX's own comment in
 * app_sand.c for the full account and the "why 25 cells, not a round
 * number" derivation). This constant follows DETONATE_RADIUS_PX's own
 * value rather than drifting from it, same as before - the point of the
 * fix was never that the radius COULDN'T shrink, only that it no longer
 * HAD to just to keep the buffer allocating. */
#define DUNE_BLAST_RADIUS 25

/* A FIXED ENTRY COUNT MIRRORING APP_IMPULSE_MAX EXACTLY, not a formula in
 * DUNE_BLAST_RADIUS - see APP_IMPULSE_MAX's own comment in app_sand.c for
 * why the two constants split apart: this scene's own impulse buffer no
 * longer needs to be "big enough for whatever DUNE_BLAST_RADIUS's disc
 * requires", because sand_explode() now degrades its own seeding density
 * to fit whatever buffer it is actually given. What this DOES still need
 * to mirror is the app's real device budget, not the app's radius - a
 * host test buffer sized any differently would measure a blast fighting
 * a different memory ceiling than the one the device actually has, which
 * defeats the entire point of this scene reading "on the same scale a
 * real device detonation does" (this file's own top comment, above). */
#define DUNE_IMPULSE_MAX  2048

/* A settled dune, poured rather than painted - the same way app_sand.c's
 * own starting heap is: sand_spawn() dropped from height and left to find
 * its own angle of repose under ordinary gravity, exactly what a player's
 * finger produces. A painted rectangle would not be a dune - it has no
 * slope for a blast to disturb, and its own square corners would slide
 * under plain gravity before an explosion ever got a turn, which would
 * muddy "the blast displaced this" with "gravity was already going to".
 *
 * Settling is deliberately NOT done here - see settle_fully() above and
 * this file's other build_*_scene() functions, none of which step at
 * all: a builder places material, and whatever steps a caller needs
 * (rest, in this file's usual case; convergence, in this scene's) is the
 * caller's own job, so the builder stays reusable exactly as it is by a
 * caller that wants a MID-fall dune instead of a settled one. */
static void build_sand_dune_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
}

/* THE OUTCOME THIS ROUND WAS MISSING - see this section's own top comment.
 * Three numbers, not a boolean, because a boolean cannot tell power from
 * reach from destruction apart, and conflating them is exactly how a
 * change that helps one and hurts another would go unnoticed:
 *
 *   grains outside footprint   did anything escape the dune AT ALL - the
 *                              user's own criterion, and the real
 *                              pass/fail test
 *   maximum throw distance     how FAR the furthest grain got, which a
 *                              plain yes/no on "outside" cannot
 *                              distinguish from "barely"
 *   material destroyed         how much was converted or lost rather
 *                              than thrown - the core's own fire cost,
 *                              not a mistake to chase out
 *
 * "Outside the footprint" is measured against the SETTLED footprint,
 * recorded once and only once, right after settle_fully() returns and
 * before sand_explode() is ever called - a cell counts as displaced only
 * if it holds MAT_SAND now and was NOT already occupied by the dune
 * before the blast touched anything. Checking material specifically
 * excludes the fire the core itself becomes (see SAND_EXPLODE_CORE_
 * DIVISOR's own comment in sand.h) from counting as an "escaped grain" -
 * fire landing outside the footprint is the fireball's own edge doing
 * exactly what it is supposed to, not sand flying off. */
static void test_the_sand_dune_scene_throws_grains_beyond_its_own_footprint(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big      = malloc(cells_len);
    uint8_t   *blocks   = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                 ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t   *footprint = malloc(DUNE_FOOTPRINT_BYTES);
    impulse_t *impulses  = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL &&
                          footprint != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(footprint); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map, a one-bit-per-cell "
                          "footprint mask, and an impulse buffer for the "
                          "dune scene, and at least one failed to allocate");
    }
    memset(footprint, 0, DUNE_FOOTPRINT_BYTES);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 51u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_sand_dune_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    /* The settled footprint, and its bounding box - "detonate at its
     * centre" means the centre of what actually settled, which is lower
     * and narrower than where sand_spawn() dropped it, not the drop
     * point itself. */
    int before = 0;
    int min_x = REAL_W, max_x = -1, min_y = REAL_H, max_y = -1;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const bool occupied = sand_at(&real, x, y) != SAND_EMPTY;
            if (occupied) {
                footprint_set(footprint, (size_t)y * REAL_W + x);
                before++;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    /* AT THE 25-CELL RADIUS THIS BLASTS AT (DUNE_BLAST_RADIUS - a
     * deliberate, user-chosen retune toward "small and dense" after a
     * real device confirmed a bigger, thinned blast working, not a value
     * forced down by a memory bug - see that constant's own comment for
     * the full account and the tradeoff it was chosen over). A settled
     * dune's own bounding-box centre sits only ~30 cells above the true
     * floor (a wide, short pile with height well under a 48-cell radius,
     * the value this scene used before), so unlike that larger radius
     * this smaller one does not reliably reach REAL_H - the grid's own
     * bottom edge - and does not need to: full-density seeding at this
     * radius is what the scene is measuring, not edge contact. Verified
     * this still measures something real rather than a degenerate scene:
     * "outside"/"destroyed" stayed small fractions of `before` (an
     * 800-seed sweep against the real, shipped sand_explode() - at full
     * density, zero thinning, since this radius's true disc fits inside
     * DUNE_IMPULSE_MAX entirely - averaged 2.63% and 1.94% at this
     * radius and budget, not a plurality of the dune, still less all of
     * it) - see this test's own assertions below, unchanged, for the
     * actual bar. */
    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    /* Past the deterministic flight-time bound (see SAND_IMPULSE_SPEED_
     * RAMP's own comment in sand.h), computed from the constants rather
     * than a bare number so this keeps measuring the same thing after
     * either one is retuned, plus margin for gravity to bring a landed
     * grain to rest and for a water/collapse scene's own refill to
     * finish. */
    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Distance to the NEAREST footprint cell, not to the detonation
     * centre - see nearest_footprint_distance()'s own comment for why: a
     * straight-line distance from one fixed interior point conflates a
     * grain genuinely thrown clear with one that merely slid down the
     * dune's own slope and stopped at its base, since both can end up
     * geometrically far from the centre for reasons that have nothing
     * to do with how hard the blast pushed. Computed per outside cell
     * rather than once for the whole grid - see that function's own
     * comment for why this scene's own footprint shape makes a
     * precomputed distance field, bounded or not, the wrong tool here. */
    int outside = 0;
    int max_throw = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (footprint_get(footprint, (size_t)y * REAL_W + x)) {
                continue;   /* inside the original dune - not an escape */
            }
            if (CELL_MATERIAL(sand_at(&real, x, y)) != MAT_SAND) {
                continue;   /* fire, not a grain - see this test's own comment */
            }
            outside++;
            const int d = nearest_footprint_distance(footprint, REAL_W, REAL_H,
                                                      x, y, NEAREST_FOOTPRINT_CAP);
            if (d > max_throw) {
                max_throw = d;
            }
        }
    }
    const int after = sand_count(&real);
    const int destroyed = before - after;

    free(big);
    free(blocks);
    free(footprint);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune must actually stop moving within the settle budget - a "
        "pile still falling is not a dune, it is a rectangle in the "
        "middle of becoming one");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, before,
        "the dune must have settled into SOMETHING - an empty footprint "
        "means sand_spawn() itself failed, not that the blast did");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, outside,
        "at least one grain must land outside the dune's own settled "
        "footprint - the user's own criterion, and the one no existing "
        "test checked: a blast that only ever disturbs its own footprint "
        "reads as a shuffle, not a throw");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, max_throw,
        "the furthest grain must land at least one step past the dune's "
        "own edge, by the corrected (nearest-footprint-cell) distance - "
        "this bar is deliberately low for now: measured at exactly 1 "
        "with today's constants, which is the same finding that motivates "
        "the retune and the displacement work queued right after this "
        "commit, and it should rise once either lands");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, destroyed,
        "destruction is bounded below by zero - sand_count() must never "
        "rise from a blast, whatever else changes about it");
    TEST_ASSERT_LESS_THAN_MESSAGE(before / 2, destroyed,
        "losing more than half the dune to the core's own fire is a sign "
        "the core divisor has drifted back toward eating the blast "
        "rather than flashing it - see SAND_EXPLODE_CORE_DIVISOR's own "
        "comment in sand.h");
}

/* =========================================================================
 * VARIANTS ON THE SAME DUNE - water pool, stone vessel, wood, layered
 * dune - each reusing settle_fully()/DUNE_BLAST_RADIUS/DUNE_IMPULSE_MAX
 * above rather than inventing its own settling or sizing rules.
 * ========================================================================= */

/* The base dune, plus a deep pool of water along the right third of the
 * grid - deep enough that a blast thrown into it still leaves plenty of
 * water to flow back in with, not just a thin sheet that boils away
 * entirely. Detonating INSIDE the pool (see the guard test below) is
 * what actually exercises "does the cavity collapse and refill", not
 * detonating in the dune and merely having water somewhere on the same
 * screen. */
static void build_dune_beside_water_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);

    /* Laid down at roughly the depth this volume settles to anyway,
     * spread across the basin, rather than stacked in the right-hand
     * third. The old shape had its left face open, so the pool spent 2,480
     * steps - 124 settle batches against 29-39 for every other scene in
     * this file, and ~98% of this test's runtime - travelling sideways to
     * reach the same equilibrium. Same water, same basin, same claims;
     * it simply starts where it was always going to end up. */
    const int pool_depth = 38;
    for (int y = REAL_H - pool_depth; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* A cavity in a liquid is not a cavity in sand: nothing here needed the
 * ring-order fix or the cap sizing at all, but it is the one place in
 * this file that checks the claim from Explosion-Plan.md's own "what to
 * look at" list - "detonate in water: the cavity should collapse and
 * slosh" - at more than a hand-wave. Detonating inside the pool, not the
 * dune, is deliberate: the dune already has its own scene above, and
 * mixing the two claims into one scene would leave neither checked
 * cleanly. */
/* How much WATER, and how much EMPTY, sits within `r` of (cx, cy). The
 * refill claim below needs both, and needs them counted by material: the
 * cavity a disturbance opens in a pool gets filled by falling sand or by
 * impulse-thrown debris whether or not the liquid can flow at all, so
 * "something is there now" is not evidence of anything. */
static int water_within(const sand_t *s, int cx, int cy, int r)
{
    int n = 0;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
                continue;
            }
            const int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            if (CELL_MATERIAL(sand_at(s, x, y)) == MAT_WATER) {
                n++;
            }
        }
    }
    return n;
}

static int empty_within(const sand_t *s, int cx, int cy, int r)
{
    int n = 0;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
                continue;
            }
            const int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            if (sand_at(s, x, y) == SAND_EMPTY) {
                n++;
            }
        }
    }
    return n;
}

static void test_the_water_pool_scene_refills_its_own_cavity(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big     = malloc(cells_len);
    uint8_t   *blocks  = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the water pool scene, and at least one "
                          "failed to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 61u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_beside_water_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    int water_before = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WATER) {
                water_before++;
            }
        }
    }

    /* Well inside the pool, away from its own edges - see this function's
     * own top comment for why detonating in the dune instead would not
     * exercise the claim this test exists for. */
    /* FOUND, not hardcoded. A fixed row only lands inside the pool for one
     * particular water level, so it silently becomes a precondition on
     * where the pool happened to settle - and then any unrelated change to
     * how water or sand comes to rest fails this test for a reason that has
     * nothing to do with what it tests. Descending from the surface keeps
     * the centre genuinely submerged whatever the level turns out to be. */
    const int cx = (REAL_W * 5) / 6;
    int surface_y = -1;
    for (int y = 0; y < REAL_H; y++) {
        if (CELL_MATERIAL(sand_at(&real, cx, y)) == MAT_WATER) {
            surface_y = y;
            break;
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(-1, surface_y,
        "the pool must have a water surface in the column this test "
        "detonates in, or there is no pool to test");
    const int cy = surface_y + 12 < REAL_H - 2 ? surface_y + 12 : REAL_H - 2;
    const int centre_material_before = CELL_MATERIAL(sand_at(&real, cx, cy));

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 40; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const bool centre_refilled = sand_at(&real, cx, cy) != SAND_EMPTY;

    int water_after = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WATER) {
                water_after++;
            }
        }
    }

    /* THE CLAIM THIS TEST IS NAMED FOR, asserted at last.
     *
     * Until 2026-09-01 the only refill check was "the blast's own centre
     * is not empty" - which cannot fail, for two compounding reasons. The
     * blast never empties that centre (queue_outward_impulse() skips it by
     * design, sand.c), and in a fully packed region an impulse SWAPS two
     * occupied cells, so occupancy is conserved and no hole is opened
     * anywhere. Measured: with move_liquid_grain() stubbed to return false,
     * so liquids cannot move AT ALL, this test still passed.
     *
     * So the cavity is carved directly rather than hoped for from the
     * blast, and the assertion asks for WATER back, not merely for
     * something. Measured over 113 carved cells: 89 refill normally, 32
     * with liquids immobile; half the carved count sits between them with
     * roughly 50% margin either way. Disabling cross-flow levelling instead
     * refills 113 and passes, correctly - water falling vertically is a
     * different mechanism, and test_a_pool_settles_at_the_angle_it_is_
     * tilted_to is the test that dies when cross-flow does.
     *
     * Carved AFTER water_after is counted, so the conservation assertion
     * above still measures the blast alone. */
    const int carve_r = 6;
    for (int y = cy - carve_r; y <= cy + carve_r; y++) {
        for (int x = cx - carve_r; x <= cx + carve_r; x++) {
            if ((unsigned)x >= (unsigned)REAL_W ||
                (unsigned)y >= (unsigned)REAL_H) {
                continue;
            }
            const int ddx = x - cx, ddy = y - cy;
            if (ddx * ddx + ddy * ddy <= carve_r * carve_r) {
                sand_set(&real, x, y, SAND_EMPTY);
            }
        }
    }
    const int carved_empty = empty_within(&real, cx, cy, carve_r);
    for (int refill_step = 0; refill_step < 100; refill_step++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int carved_water_after = water_within(&real, cx, cy, carve_r);

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune and the pool must both stop moving within the settle "
        "budget before anything is measured against them");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1000, water_before,
        "the pool must actually hold a good depth of water before the "
        "blast touches it, or 'still has water after' proves nothing");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_WATER, centre_material_before,
        "the chosen centre must actually be inside the pool, or this "
        "is not testing what it claims to");
    TEST_ASSERT_TRUE_MESSAGE(centre_refilled,
        "the blast's own centre must not be left an empty void once "
        "everything has settled - a liquid closes over a disturbance, "
        "it does not leave a permanent hole in itself");
    TEST_ASSERT_GREATER_THAN_MESSAGE(carved_empty / 2, carved_water_after,
        "a cavity carved into the pool must fill back up with WATER, not "
        "merely with something - this is the claim this test is named for, "
        "and for a long time nothing here checked it: the old assertion "
        "asked only that the blast's centre be non-empty, which held even "
        "with liquids unable to move at all");
    TEST_ASSERT_GREATER_THAN_MESSAGE(water_before / 2, water_after,
        "the pool must still hold most of its own water after settling - "
        "a blast in water should slosh and refill, not boil the whole "
        "pool away");
}

/* The base dune, walled inside a sealed stone vessel with real empty
 * space left OUTSIDE the vessel (not just the grid's own implicit
 * boundary, which is solid for free and would make "contained" trivially
 * true regardless of whether the vessel itself does anything). Detonating
 * inside must leave that outside margin exactly as empty as it started -
 * the inverse of the base scene's own claim, checked at the same real
 * scale rather than the tiny hand-built vessel the mechanism-level tests
 * above already cover.
 *
 * STILL THE "WEAK OR DISTANT" HALF of the two-part guarantee a wall's
 * density-scaled dislodge chance now leaves (see test_a_strong_close_
 * blast_can_breach_a_wall in the mechanism-level section above, and
 * queue_flying_grain()'s own comment in sand.c, for the other half) -
 * DUNE_BLAST_RADIUS against VESSEL_MARGIN's own distance is a genuinely
 * weak comparison at this real scale (a 25-cell blast against a wall
 * `VESSEL_MARGIN` cells away, VESSEL_MARGIN chosen well past that
 * radius), so the annulus here never actually reaches a wall cell to
 * roll against - this measures the common case, a built container
 * still working as a container against an ordinary detonation, not a
 * claim that no wall can ever be breached at any radius or distance. */
#define VESSEL_MARGIN 20
#define VESSEL_WALL   3
static void build_dune_in_a_vessel_scene(sand_t *s)
{
    for (int y = VESSEL_MARGIN; y < REAL_H - VESSEL_MARGIN; y++) {
        for (int x = VESSEL_MARGIN; x < REAL_W - VESSEL_MARGIN; x++) {
            const bool on_wall =
                x < VESSEL_MARGIN + VESSEL_WALL ||
                x >= REAL_W - VESSEL_MARGIN - VESSEL_WALL ||
                y < VESSEL_MARGIN + VESSEL_WALL ||
                y >= REAL_H - VESSEL_MARGIN - VESSEL_WALL;
            if (on_wall) {
                sand_set(s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }

    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
}

static void test_the_vessel_scene_lets_nothing_reach_outside_it(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big     = malloc(cells_len);
    uint8_t   *blocks  = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the vessel scene, and at least one failed "
                          "to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 71u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_in_a_vessel_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    /* The dune's own centre, from its SAND footprint specifically - the
     * walls are also "occupied" and would skew a plain min/max scan. */
    int min_x = REAL_W, max_x = -1, min_y = REAL_H, max_y = -1;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_SAND) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    int outside_occupied = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const bool outside_vessel =
                x < VESSEL_MARGIN || x >= REAL_W - VESSEL_MARGIN ||
                y < VESSEL_MARGIN || y >= REAL_H - VESSEL_MARGIN;
            if (outside_vessel && sand_at(&real, x, y) != SAND_EMPTY) {
                outside_occupied++;
            }
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune inside the vessel must stop moving within the settle "
        "budget before anything is measured against it");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, max_x,
        "the vessel must actually contain a settled dune to detonate, or "
        "this is not testing containment against anything");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, outside_occupied,
        "at a blast this weak relative to this vessel's own distance, "
        "nothing may occupy the margin outside its walls - this is the "
        "inverse of the base dune scene's own claim, for the common case "
        "a built container is meant to survive; see test_a_strong_close_"
        "blast_can_breach_a_wall for why 'never, at any radius' is no "
        "longer the claim this project makes");
}

/* The base dune, with a strip of wood forming the floor it settles onto -
 * guaranteeing contact between the settled dune and the wood regardless
 * of the exact shape settling leaves, unlike wood planted mid-air before
 * the falling sand has even reached it. Checks the plan's own claim -
 * "no material explodes... [but the trigger is] easier to judge after
 * seeing it than before" - by proving the one direction that already
 * works today: a blast's own fire reaching nearby fuel, exactly as
 * painted fire already would. */
static void build_dune_over_wood_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);

    /* CELL_MAKE(MAT_WOOD, 0), not MASS_MAX - wood's own variant is burn
     * life remaining (see cell_is_burning()'s own comment in material.h),
     * not a fill level the way a liquid's is. MASS_MAX there would have
     * planted this floor already on fire, which is what the thermal-
     * shock and lava-stress scenes above deliberately want as their own
     * trigger - this scene wants the opposite: unlit wood, waiting for
     * THIS test's blast to be the first thing that ever lights it. */
    for (int y = REAL_H - 12; y < REAL_H; y++) {
        for (int x = REAL_W / 2 - REAL_W / 5; x < REAL_W / 2 + REAL_W / 5; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WOOD, 0));
        }
    }
}

static void test_the_wood_floor_scene_catches_fire(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big     = malloc(cells_len);
    uint8_t   *blocks  = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the wood floor scene, and at least one "
                          "failed to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 83u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_over_wood_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    int wood_before = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WOOD) {
                wood_before++;
            }
        }
    }

    /* The CORE's own bottom edge placed right at the wood floor's top
     * surface, not the dune's geometric centre - fire has to actually
     * touch (or nearly touch) the wood to ignite it, and fire is LIGHTER
     * than sand (see SAND_EXPLODE_CORE_DIVISOR's own comment on
     * can_enter()'s displacement rule), so it rises up through the pile
     * rather than sinking down toward a floor beneath it. A centre placed
     * at the dune's own middle - tried first, and measured, not assumed -
     * left the core entirely inside sand, several cells short of the
     * wood, and ignited nothing at all: this is why "detonated somewhere
     * in the dune" is not the same claim as "detonated where its fire
     * can actually reach the fuel". */
    const int cx = REAL_W / 2;
    const int cy = (REAL_H - 12) - (DUNE_BLAST_RADIUS / SAND_EXPLODE_CORE_DIVISOR) - 1;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    int burning_wood = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&real, x, y);
            if (CELL_MATERIAL(c) == MAT_WOOD && cell_is_burning(c)) {
                burning_wood++;
            }
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune over its wood floor must stop moving within the "
        "settle budget before anything is measured against it");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, wood_before,
        "the wood floor must have survived settling - if sand displaced "
        "all of it before the blast even happens, this proves nothing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, burning_wood,
        "a blast detonated against a wood floor must leave at least "
        "some of it burning - the core's own fire reaching nearby fuel "
        "exactly as painted fire already would, not a special case a "
        "blast needs of its own");
}

/* The base dune, poured in three bands of decreasing radius with real
 * settling time between each - not one uniform pour - so pour_phase (see
 * its own comment on sand_t in sand.h) has genuinely moved on between
 * bands and each one settles with a visibly different shade, the same
 * way two pours a few seconds apart on the real app would. A wedding-
 * cake dune with real, distinguishable layers, not a paint job.
 *
 * 40 steps between pours, not a much longer rest - measured, not
 * guessed: at 150 steps the dune had that much longer for scatter to
 * random-walk its base sideways between every pour, and by the time all
 * three had landed the footprint had spread across 86% of the grid's own
 * width - so wide that DUNE_BLAST_RADIUS's own disc around the centre
 * never reached any genuinely empty ground to throw material into, and
 * the guard test below measured zero grains outside the footprint,
 * every time. 40 steps is enough for pour_phase to still land each band
 * on a visibly different shade (5 distinct shades in the settled dune,
 * against 150 steps' own 7 - plenty either way) while keeping the dune
 * itself narrow enough for its own blast radius to still reach past its
 * edge. */
static void build_layered_dune_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
    for (int i = 0; i < 40; i++) {
        sand_step(s, 0, 1000, 0);
    }
    sand_spawn(s, REAL_W / 2, REAL_H / 4, (REAL_W / 5) * 2 / 3, MAT_SAND);
    for (int i = 0; i < 40; i++) {
        sand_step(s, 0, 1000, 0);
    }
    sand_spawn(s, REAL_W / 2, REAL_H / 4, (REAL_W / 5) / 3, MAT_SAND);
}

/* Displaced layers, not just displaced sand - the base scene above
 * already proves grains escape the footprint at all; this proves the
 * blast reaches deep enough to mix bands that would otherwise never
 * meet, which is what "throw is visible as displaced layers" actually
 * means on the panel. Counted by distinct shade (CELL_VARIANT), not by
 * tracking any one band's own identity: three pours spaced by real
 * settling time land in different parts of MATERIAL_VARIANTS' shade
 * range (see random_cell()'s own use of pour_phase), so more than one
 * distinct shade appearing outside the original footprint is direct
 * evidence that more than one band contributed to what escaped, not
 * just the most recent, surface-most pour skimming off the top. */
static void test_the_layered_dune_scene_throws_more_than_one_band(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big      = malloc(cells_len);
    uint8_t   *blocks   = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                 ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t   *footprint = malloc(DUNE_FOOTPRINT_BYTES);
    impulse_t *impulses  = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL &&
                          footprint != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(footprint); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map, a one-bit-per-cell "
                          "footprint mask and an impulse buffer for the "
                          "layered dune scene, and at least one failed "
                          "to allocate");
    }
    memset(footprint, 0, DUNE_FOOTPRINT_BYTES);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 97u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_layered_dune_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    int min_x = REAL_W, max_x = -1, min_y = REAL_H, max_y = -1;
    bool seen_variant_before[SAND_SHADE_COUNT] = { false };
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&real, x, y);
            const bool occupied = c != SAND_EMPTY;
            if (occupied) {
                footprint_set(footprint, (size_t)y * REAL_W + x);
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (CELL_MATERIAL(c) == MAT_SAND) {
                    seen_variant_before[CELL_VARIANT(c)] = true;
                }
            }
        }
    }
    int distinct_bands = 0;
    for (int v = 0; v < SAND_SHADE_COUNT; v++) {
        if (seen_variant_before[v]) distinct_bands++;
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    bool seen_variant_outside[SAND_SHADE_COUNT] = { false };
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (footprint_get(footprint, (size_t)y * REAL_W + x)) {
                continue;
            }
            const cell_t c = sand_at(&real, x, y);
            if (CELL_MATERIAL(c) == MAT_SAND) {
                seen_variant_outside[CELL_VARIANT(c)] = true;
            }
        }
    }
    int distinct_bands_outside = 0;
    for (int v = 0; v < SAND_SHADE_COUNT; v++) {
        if (seen_variant_outside[v]) distinct_bands_outside++;
    }

    free(big);
    free(blocks);
    free(footprint);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the layered dune must stop moving within the settle budget "
        "before anything is measured against it");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct_bands,
        "three pours spaced by real settling time must have left more "
        "than one distinct shade in the settled dune - if they did not, "
        "the bands never separated and this scene is not testing what "
        "it claims to");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct_bands_outside,
        "more than one shade band must appear outside the original "
        "footprint - a single band escaping would just be the base "
        "scene's own claim again, not displaced LAYERS specifically");
}

/* --- water over lava: a continuous pour onto a sealed pool --------------
 *
 * REPLACES the vent-spam scene that used to occupy this section (bd
 * esp32c6-0f2 removed the vent machinery it measured; asked for
 * 2026-09-02, "we would just need to rebuild the vent scene it's simple,
 * water over lava, and re-peg the performance"). Its replacement -
 * covered lava converting to stone and bursting (bd esp32c6-mqt) - has no
 * mechanism left anywhere near as expensive as the vent scan this scene
 * used to hold open: one roll per covered lava cell per step, at odds of
 * roughly 1 in 256, with a 3-neighbour cover_mask() walk only after the
 * roll passes. A "burst spam" scene built the same way (many cells
 * forced-covered, chance pinned to maximum) would measure a real cost,
 * but not a REPRESENTATIVE one - production never pins the chance, and
 * the walk it is paying for is cheap. This scene instead measures the
 * ordinary, sustained thing a player actually does: pour water onto
 * lava. That single act chains through three separate reactions -
 * quench (direct water-lava contact converting to stone),
 * cool_off_chain() (that conversion's own cost paid outward into
 * neighbouring lava, sand_reactions.c), and the burst gate (once enough
 * of a stone crust has formed over what's left) - so a regression in any
 * of the three shows up here, not only in its own narrower correctness
 * test.
 *
 * DO NOT compare this row's numbers against the old vent-spam capture
 * that used to sit here. This is a different scene measuring a different
 * mechanism; the old figure describes a machinery that no longer exists,
 * not a slower or faster version of what replaced it. See test_the_
 * water_over_lava_scene_fits_in_the_frame_budget's own comment (below,
 * beside the other frame-budget tests) for the first-capture convention
 * this file already has for exactly this situation. */

/* Half the grid lava, half water, in direct contact along one full-width
 * seam - not vent-spam's many small sealed pockets, because nothing here
 * needs to stay sealed: quench and cool_off_chain() only need lava
 * touching water at all, and the burst gate only needs enough of a crust
 * to form, which a wide, deep pool supplies on its own as the interface
 * quenches. A single seam this wide puts as many lava cells in
 * simultaneous contact with water as the grid can hold, which is the
 * worst case for the quench pass; the crust it leaves behind covers the
 * pool beneath it just as completely, which is the worst case for the
 * burst gate. */
#define WATER_LAVA_LAVA_TOP (REAL_H / 2)

/* Same real device impulse budget the vent-spam scene this replaces used
 * (that scene's own comment, git history, has the full account) - the
 * app's own buffer is sized APP_IMPULSE_MAX (2048), and this scene should
 * be fighting the same memory ceiling a real device pour actually has,
 * not a looser one a differently-sized test buffer would hide. */
#define WATER_LAVA_IMPULSE_MAX 2048

/* sand_set_lava_cooloff()/sand_set_lava_burst() forced to their maximum,
 * the same reasoning the vent-spam scene this replaces gave for forcing
 * sand_set_vent_chance(255) (git history): production leaves both
 * deliberately rare (SAND_LAVA_COOLOFF_CHANCE, SAND_LAVA_BURST_CHANCE,
 * sand.h), and a benchmark that mostly rolls "no" would not be measuring
 * the mechanisms it claims to. Quench itself has no chance to force - a
 * burning liquid touching a quenching one always converts - so only
 * these two need it. */
static void build_water_over_lava_scene(sand_t *s)
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

/* This scene really does reach the three paths it claims to, checked the
 * same way this file's other scene tests are: build it through the same
 * function the device test uses, step it the same number of times, and
 * count - not "did the frame-budget test merely run without crashing".
 *
 * THREE INDEPENDENT SIGNALS, one per claimed path:
 *
 * - STONE PRESENT AT ALL proves quench fired - water touching lava
 *   converts it, and nothing else in this scene produces stone.
 *
 * - STONE COUNT BEYOND ONE SEAM'S WORTH proves cool_off_chain() carried
 *   the conversion beyond direct contact - a single interface exactly
 *   REAL_W cells wide is what quench alone could ever reach on its own
 *   in one pass, so a count past that many can only be the chain
 *   reaching cells that were never themselves touching water.
 *
 * - FIRE PRESENT proves the burst path fired - ordinary quench only ever
 *   produces stone (material.c's quench_to), so the only source of fire
 *   anywhere in this scene is sand_explode()'s own core fill on a burst
 *   (bd esp32c6-mqt's own comment, sand_reactions.c, pins that the
 *   centre cell ends up as fire, not the stone the burst itself just
 *   wrote). */
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
 * RNG seed - see docs/Sand/Architecture.md's "Verifying performance on
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

static void test_a_screen_of_water_fits_in_the_frame_budget(void)
{
    /* Measured separately from sand, because water takes an entirely different
     * path through the step - and the one part of it that is not local, the
     * search across the flow, runs per cell. Something has to watch that. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);

    /* Half a screen of water, dropped in as an uneven slab so it is genuinely
     * flowing rather than already settled - the expensive case. */
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "water flowing on %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

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

    /* UNPEGGED - THIS NUMBER IS A PLACEHOLDER AND MUST BE RE-PEGGED FROM A
     * REAL CAPTURE BEFORE THIS TEST MEANS ANYTHING.
     *
     * This suite cannot be run on a laptop (it is DEVICE_BUILD, and
     * esp_timer_get_time() has no host equivalent), and the session that
     * wrote it does not flash the board. Run
     *
     *     ./launcher/test/run_device_tests.sh
     *
     * read the "portrait->landscape turn on a settled ... pool" line out of
     * the capture, and replace the number below with measured * 0.9,
     * rounded, stating the measurement and its date here the way every other
     * budget in this file does - see docs/Sand/Perf-Round-Guide.md, "Peg or
     * re-peg budgets from what the capture actually measured", and "Never
     * raise a budget".
     *
     * 14000 is borrowed from test_a_screen_of_water_fits_in_the_frame_
     * budget's own screen-wide-collapse figure purely so this compiles and
     * runs; it is a guess about a scene nobody has measured yet, in either
     * direction, and it is not a budget. */
    TEST_ASSERT_LESS_THAN_MESSAGE(14000, (int)per_step,
        "turning the board a quarter turn with a settled pool on it must "
        "still fit in a frame or two - the pool re-levels across the whole "
        "grid width, so the cross-flow search is the thing to suspect. "
        "THIS BUDGET IS UNPEGGED: see the comment above it");
}

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

    /* The scene is DERIVED from materials[] and laid out by
     * all_pairs_material_at() so that every PAIR of materials touches -
     * not merely every material appearing somewhere. See that function
     * for why bands were not enough, and
     * test_the_mixed_scene_puts_every_material_pair_in_contact, which
     * checks the coverage on the host rather than leaving it a claim in
     * a comment.
     *
     * A share of the board is left empty (EMPTY_SHARE_PERCENT) so the
     * flip has somewhere to launch into - the same reasoning as the
     * other flip tests. */
    const int first = MAT_EMPTY + 1;
    const int n_mats = MAT_COUNT - first;
    const int top = (REAL_H * EMPTY_SHARE_PERCENT) / 100;

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, n_mats,
        "the pattern below needs at least two materials to interleave");

    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = all_pairs_material_at(x, y, first, n_mats);
            /* sand_spawn() with radius 0 rather than sand_set(): it goes
             * through random_cell(), so a liquid arrives full, a transient
             * arrives at full life and a powder gets a shade - the same
             * cells a real pour produces. */
            sand_spawn(&real, x, y, 0, (material_id_t)m);
        }
    }

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

    /* 54000 us: a REDUCTION TARGET, not headroom. Set 10% below a measured
     * 60091 us so this fails today and stops failing only when the code
     * gets faster. That is the same thing the mixed-scene budget above
     * does, and the reason that one went from 26.2% over to 7.0% under
     * without the number ever moving.
     *
     * THE 60091 IS STALE, AND KNOWINGLY SO. It was taken on 2026-08-25
     * against a scene of TWELVE materials covering 66 pairs. There are
     * fourteen now - glass and snow - so the same derived scene covers 91
     * pairs, and the reactions pass has gained a per-cell branch and a
     * second kind of participant (cells with a temperature) since. The
     * scene this number describes no longer exists.
     *
     * The number is deliberately NOT adjusted for that. A reduction target
     * moved to accommodate the code is no longer a target, and this file
     * has already been burned twice by figures that were reasoned about
     * rather than measured - 100000 picked with no hardware, then 300000
     * extrapolated from another scene's ratio, which came out four times
     * too pessimistic. The right correction is a fresh device capture, not
     * an estimate.
     *
     * On the extrapolation that produced 300000: fire measures ~318x host
     * and this scene ~63x, so it predicted 226-244 ms against an actual
     * 60 ms. Worth remembering before anyone extrapolates again - on this
     * chip the ratio is dominated by cache behaviour the host does not
     * model, and it is scene-specific.
     *
     * The staleness warning the paragraphs above carried is RESOLVED:
     * the fresh capture the 2026-08-26 re-base ran on (see
     * FULL_STEP_BUDGET_US's comment) measured the fourteen-material
     * scene at 74911 us, and the target followed the same uniform rule
     * as every other budget: measured * 0.9, rounded -> 67500.
     * Numerically up from the stale 54000, still a tenth below what the
     * current scene actually costs. */
    TEST_ASSERT_LESS_THAN_MESSAGE(67500, (int)per_step,
        "the mixed-material flip is held to 10% below what it measured, "
        "as a reduction target - this failing means the work has not been "
        "done yet, not that something broke");
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

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, count,
        "setup: cells must only ever convert material, never appear or "
        "vanish, across a screen of smoke and steam");
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
 * see the seventh attempt in docs/Sand/Performance-Tuning-Attempts.md, and
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
                                          int *gathered, int *partial_bands)
{
    for (int i = 0; i < settle_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1,
                                row_n);
        gfx_present();
    }

    gfx_reset_strip_send_counts();

    int64_t total_us = 0;
    for (int i = 0; i < measured_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1,
                                row_n);

        const int64_t start = esp_timer_get_time();
        gfx_present();
        total_us += esp_timer_get_time() - start;
    }

    gfx_get_strip_send_counts(full_bands, gathered, partial_bands);

    return total_us / measured_steps;
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
    sand_init(&real, big, REAL_W, REAL_H, 99u);
    sand_track_dirty_rows(&real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W,
                                          REAL_H);

    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(&real, x, y, SAND_FIRST_SHADE);
            }
        }
    }

    int full_bands = 0, gathered = 0, partial_bands = 0;
    const int measured_steps = 20;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W,
        REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1, 0, 5, measured_steps,
        &full_bands, &gathered, &partial_bands);

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
        measured_steps, &full_bands, &gathered, &partial_bands);

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
        measured_steps, &full_bands, &gathered, &partial_bands);

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

void run_sand_suite(void)
{
    RUN_TEST(test_acid_bubbles_do_not_favour_one_wall);
    RUN_TEST(test_acid_bubbles_still_fire_once_the_block_is_asleep);









    RUN_TEST(test_oil_trapped_under_water_floats_to_the_surface);
    RUN_TEST(test_sand_floats_on_oil);
    RUN_TEST(test_dirt_still_sinks_through_oil);
    RUN_TEST(test_sand_turns_to_glass_under_sustained_heat);
    RUN_TEST(test_glass_conducts_heat_like_stone);
    RUN_TEST(test_snow_floats_on_water);
    RUN_TEST(test_the_air_agrees_about_weight_speed_and_lifetime);
    RUN_TEST(test_steam_melts_ice_and_plain_gas_does_not);
    RUN_TEST(test_two_pours_apart_in_time_lay_down_different_shades);
    RUN_TEST(test_a_moving_grain_keeps_the_shade_it_was_poured_with);
    RUN_TEST(test_wet_sand_becomes_soil_wet_with_no_tone_of_its_own);
    RUN_TEST(test_a_root_darkens_as_more_root_grows_around_it);
    RUN_TEST(test_root_neighbours_are_counted_across_three_rows);
    RUN_TEST(test_the_right_extended_materials_are_grained);
    RUN_TEST(test_metal_hatched_body_lines_and_shine_differ);
    RUN_TEST(test_metal_shine_does_not_vary_between_cells);
    RUN_TEST(test_a_watered_plant_roots_into_the_soil_it_drinks_from);
    RUN_TEST(test_a_trunk_standing_on_its_own_root_is_anchored);
    RUN_TEST(test_a_root_column_does_not_spend_the_trees_lift);
    RUN_TEST(test_a_buried_root_does_not_cut_off_the_water_below_it);
    RUN_TEST(test_a_root_column_reaches_below_the_collar);
    RUN_TEST(test_a_canopy_waters_the_soil_through_its_own_roots);
    RUN_TEST(test_lava_burns_a_root_out_of_the_ground);
    RUN_TEST(test_fire_leaves_a_root_alone);
    RUN_TEST(test_an_orphaned_root_in_dry_ground_rots_away);
    RUN_TEST(test_a_root_column_under_a_living_tree_does_not_rot);
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
    RUN_TEST(test_a_gunpowder_grain_keeps_its_tone_as_it_falls);
    RUN_TEST(test_material_name_says_gunpowder_for_every_code);
    RUN_TEST(test_gunpowder_palette_tones_are_distinct_and_moisture_darkens);
    RUN_TEST(test_gunpowder_codes_decode_to_the_documented_moisture);
    RUN_TEST(test_gunpowder_falls_and_piles_like_a_powder);
    RUN_TEST(test_gunpowder_sinks_through_liquids_and_rests_on_and_under_sand);
    RUN_TEST(test_gunpowder_is_conserved_under_every_gravity);
    RUN_TEST(test_fire_beside_dry_gunpowder_lights_it);
    RUN_TEST(test_lava_beside_dry_gunpowder_lights_it_through_the_heat_path);
    RUN_TEST(test_heat_conducted_through_stone_lights_gunpowder);
    RUN_TEST(test_a_lit_gunpowder_trail_burns_along_itself);
    RUN_TEST(test_a_lit_two_by_two_of_gunpowder_detonates);
    RUN_TEST(test_a_detonating_two_by_two_leaves_no_lit_gunpowder_behind);
    RUN_TEST(test_fuse_blasts_are_capped_at_one_per_step);
    RUN_TEST(test_a_longer_fuse_cooldown_delays_the_next_blast);
    RUN_TEST(test_a_one_wide_lit_trail_never_detonates);
    RUN_TEST(test_a_buried_lit_gunpowder_cell_is_not_smothered);
    RUN_TEST(test_water_quenches_lit_gunpowder_to_soaked);
    RUN_TEST(test_gunpowder_without_impulses_burns_to_fire);
    RUN_TEST(test_soaked_gunpowder_never_lights_beside_lava);
    RUN_TEST(test_heat_dries_wet_gunpowder_one_level_with_steam);
    RUN_TEST(test_damp_gunpowder_ignites_less_readily_than_dry);
    RUN_TEST(test_water_wets_gunpowder_and_it_dries_out_slowly);
    RUN_TEST(test_gunpowder_moisture_never_multiplies_as_it_spreads);
    RUN_TEST(test_soaked_gunpowder_can_turn_into_oil_and_dry_never_does);
    RUN_TEST(test_acid_dissolves_gunpowder);
    RUN_TEST(test_a_wet_neighbour_does_not_put_out_a_lit_fuse);
    RUN_TEST(test_a_root_does_not_drink_from_or_eat_gunpowder);
    RUN_TEST(test_plants_do_not_sprout_in_gunpowder);
    RUN_TEST(test_a_lit_fuse_is_not_re_placed_by_heat);
    RUN_TEST(test_a_reaction_never_mints_a_static_from_gunpowder_or_the_reverse);
    RUN_TEST(test_wood_burning_state_is_byte_identical_under_lit_from);
    RUN_TEST(test_ice_cracks_hot_glass_and_stays_where_it_is_put);
    RUN_TEST(test_the_mixed_scene_puts_every_material_pair_in_contact);
    RUN_TEST(test_the_four_liquid_scene_keeps_reacting_after_settling);
    RUN_TEST(test_the_lava_stress_scene_reaches_every_reaction_it_claims);
    RUN_TEST(test_the_smoke_and_steam_scene_stays_a_gas_screen);
    RUN_TEST(test_the_thermal_shock_scene_shatters_in_both_directions);
    RUN_TEST(test_the_boiler_scene_keeps_boiling_across_the_window);
    RUN_TEST(test_the_wet_earth_scene_keeps_percolating_across_the_window);
    RUN_TEST(test_the_sand_dune_scene_throws_grains_beyond_its_own_footprint);
    RUN_TEST(test_the_water_pool_scene_refills_its_own_cavity);
    RUN_TEST(test_the_vessel_scene_lets_nothing_reach_outside_it);
    RUN_TEST(test_the_wood_floor_scene_catches_fire);
    RUN_TEST(test_the_layered_dune_scene_throws_more_than_one_band);
    RUN_TEST(test_the_water_over_lava_scene_reaches_the_quench_cooloff_and_burst_paths_it_claims);
    RUN_TEST(test_acid_spends_at_least_a_unit_of_itself_per_cell_dissolved);
    RUN_TEST(test_acid_fizzes_while_it_eats);
    RUN_TEST(test_the_fizz_rises_out_of_the_acid);
    RUN_TEST(test_acid_and_water_dilute_each_other);
    RUN_TEST(test_the_dilution_split_favours_neither_side);
    RUN_TEST(test_water_winning_the_dilution_boils_the_water_cell_to_steam);
    RUN_TEST(test_acid_winning_the_dilution_boils_the_acid_cell_to_gas);
    RUN_TEST(test_oil_mostly_boils_off_into_gas_not_acid);
    RUN_TEST(test_the_acid_that_ate_oil_can_die_in_a_single_bite);
    RUN_TEST(test_acid_evaporates_into_gas_when_forced);
    RUN_TEST(test_a_relentless_pour_of_acid_overwhelms_a_pool_of_water);
    RUN_TEST(test_a_relentless_pour_of_water_overwhelms_a_pool_of_acid);
    RUN_TEST(test_a_little_acid_cannot_eat_an_unlimited_amount);
    RUN_TEST(test_every_liquid_declares_a_mobility);
    RUN_TEST(test_water_does_not_drill_into_oil_when_tilted);
    RUN_TEST(test_oil_flows_more_slowly_than_water);
    RUN_TEST(test_lava_does_not_decay_away);
    RUN_TEST(test_water_freezes_lava_into_stone);
    RUN_TEST(test_lava_quenched_into_stone_mid_pass_arms_the_heat_holder_flag);
    RUN_TEST(test_a_water_pour_freezes_a_lava_pool_below_its_crust);
    RUN_TEST(test_a_lava_pool_in_a_dry_stone_bowl_does_not_freeze_itself);
    RUN_TEST(test_lava_that_melts_sand_into_glass_sometimes_freezes_itself);
    RUN_TEST(test_the_cool_off_chain_is_bounded);
    RUN_TEST(test_lava_does_not_put_fire_out);
    RUN_TEST(test_falling_lava_does_not_flare);
    RUN_TEST(test_steam_bubbles_up_through_standing_water);
    RUN_TEST(test_bubbling_conserves_the_water_it_displaces);
    RUN_TEST(test_plain_gas_bubbles_up_through_water_too);
    RUN_TEST(test_a_bubble_does_not_push_through_a_solid);
    RUN_TEST(test_quenching_makes_steam_but_burning_out_makes_smoke);
    RUN_TEST(test_stone_conducts_heat_into_water_beyond_it);
    RUN_TEST(test_stone_does_not_conduct_fire_into_empty_space);
    RUN_TEST(test_a_thick_wall_still_conducts);
    RUN_TEST(test_conduction_stops_at_the_reach_cap);
    RUN_TEST(test_a_thick_wall_conducts_more_slowly_than_a_thin_one);
    RUN_TEST(test_boiling_converts_the_cell_nearest_the_heat);
    RUN_TEST(test_sand_set_boils_zero_disables_conducted_heat_boiling);
    RUN_TEST(test_boiled_steam_starts_at_full_life);
    RUN_TEST(test_boiling_acid_produces_gas_not_steam);
    RUN_TEST(test_the_boiler_end_to_end);
    RUN_TEST(test_dry_dirt_beside_lava_smelts_into_metal_or_stone);
    RUN_TEST(test_saturated_dirt_smelts_roughly_eight_times_slower);
    RUN_TEST(test_watered_dirt_steaming_precedes_resolving_when_it_happens);
    RUN_TEST(test_wet_dirt_can_still_steam_before_spoiling_at_least_sometimes);
    RUN_TEST(test_wet_dirt_can_spoil_into_sand_instead_of_smelting);
    RUN_TEST(test_dry_dirt_smelting_reaches_both_metal_and_stone);
    RUN_TEST(test_a_held_flame_smelts_dirt_as_lava_does);
    RUN_TEST(test_heat_through_a_stone_wall_smelts_the_dirt_beyond_it);
    RUN_TEST(test_sand_still_becomes_glass_beside_the_new_dirt_branch);
    RUN_TEST(test_a_metal_run_conducts_further_than_a_stone_one);
    RUN_TEST(test_the_rod_terminates_at_conduct_reach_not_the_far_wall);
    RUN_TEST(test_acid_eats_metal_between_stone_and_sand);
    RUN_TEST(test_wood_and_steam_grain_count_is_conserved);
    RUN_TEST(test_a_2x2_block_of_steam_condenses_into_one_water_cell);
    RUN_TEST(test_condensation_needs_a_genuine_2x2_square);
    RUN_TEST(test_a_qualifying_gas_steam_pocket_collapses_into_one_cell);
    RUN_TEST(test_a_rained_acid_cell_keeps_dissolving_after_the_collapse);
    RUN_TEST(test_acid_rain_needs_at_least_two_of_each_species);
    RUN_TEST(test_acid_rain_resolves_to_both_acid_and_water);

    RUN_TEST(test_every_cell_change_marks_its_row_dirty);
    RUN_TEST(test_grains_are_never_created_or_destroyed);
    RUN_TEST(test_a_grain_keeps_its_shade_as_it_falls);

    RUN_TEST(test_grains_fall_upward_when_the_board_is_inverted);
    RUN_TEST(test_grains_fall_sideways_when_the_board_is_on_its_edge);
    RUN_TEST(test_a_heap_settles_against_whichever_wall_is_down);

    RUN_TEST(test_spawn_fills_a_disc);
    RUN_TEST(test_spawn_is_clipped_to_the_grid);
    RUN_TEST(test_spawning_onto_existing_grains_does_not_double_count);
    RUN_TEST(test_spawned_grains_use_the_full_range_of_shades);

    RUN_TEST(test_erase_removes_a_disc);
    RUN_TEST(test_erasing_empty_space_removes_nothing);
    RUN_TEST(test_erase_is_clipped_to_the_grid);
    RUN_TEST(test_erase_marks_the_rows_it_emptied);

    RUN_TEST(test_an_emitter_fills_its_own_cell_when_empty);
    RUN_TEST(test_an_emitter_does_not_overwrite_an_occupied_cell);
    RUN_TEST(test_an_emitter_wakes_a_sleeping_block);
    RUN_TEST(test_emitted_water_produces_a_continuing_stream);
    RUN_TEST(test_an_emitted_liquid_cell_is_full_not_the_placeholders_zero_mass);
    RUN_TEST(test_an_emitted_lava_cell_is_full_not_the_placeholders_zero_mass);
    RUN_TEST(test_an_emitted_transient_cell_has_full_life_not_the_placeholders_zero);
    RUN_TEST(test_an_emitted_powder_still_lands_in_a_valid_shade);
    RUN_TEST(test_an_emitter_and_sand_spawn_cell_agree_about_every_material);
    RUN_TEST(test_a_running_water_emitter_accumulates_mass_on_the_floor);
    RUN_TEST(test_adding_an_emitter_over_an_occupied_cell_still_registers);
    RUN_TEST(test_adding_at_an_existing_emitter_replaces_its_cell);
    RUN_TEST(test_the_emitter_cap_is_respected);
    RUN_TEST(test_out_of_bounds_emitters_are_rejected);
    RUN_TEST(test_remove_emitters_only_removes_those_in_radius);
    RUN_TEST(test_erase_stops_an_emitter_from_emitting);
    RUN_TEST(test_erase_count_excludes_emitters);
    RUN_TEST(test_sand_init_clears_emitters_from_a_previous_use);
    RUN_TEST(test_material_can_emit_matches_every_brush_by_kind);

    RUN_TEST(test_a_blast_inside_a_sealed_vessel_stays_inside_it);
    RUN_TEST(test_a_strong_close_blast_can_breach_a_wall);
    RUN_TEST(test_a_dislodged_wall_keeps_falling_even_if_its_first_push_roll_fails);
    RUN_TEST(test_sand_displace_alone_never_creates_fire_or_smoke);
    RUN_TEST(test_a_blast_conserves_grains);
    RUN_TEST(test_a_blast_at_the_edge_stays_in_bounds);
    RUN_TEST(test_a_dropped_entry_never_moves_someone_elses_cell);
    RUN_TEST(test_the_cap_degrades_gracefully);
    RUN_TEST(test_two_overlapping_blasts_share_the_buffer_evenly);
    RUN_TEST(test_a_blast_wakes_the_blocks_it_touches);
    RUN_TEST(test_a_flying_grain_keeps_its_outward_push_while_falling);
    RUN_TEST(test_an_energetic_static_chunk_over_a_powder_bank_now_stops_within_the_first_few_layers);
    RUN_TEST(test_a_spent_static_chunk_rests_on_a_powder_bank_instead_of_sinking_forever);
    RUN_TEST(test_a_static_chunk_thrown_far_still_stops_shallow_in_the_bed_it_hits);
    RUN_TEST(test_an_energetic_static_chunk_still_sinks_into_water_instead_of_resting_on_its_surface);
    RUN_TEST(test_a_spent_static_chunk_still_sinks_through_water_to_the_bottom);
    RUN_TEST(test_a_thrown_static_chunk_conserves_lava_mass_on_sink);
    RUN_TEST(test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_of_settling_and_both_eventually_land);
    RUN_TEST(test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder);
    RUN_TEST(test_a_thrown_chunk_travels_less_far_through_dirt_than_through_air);
    RUN_TEST(test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank);
    RUN_TEST(test_a_thrown_chunk_travels_less_far_through_dirt_than_through_water);
    RUN_TEST(test_a_thrown_chunk_loses_speed_proportional_to_the_density_it_displaces);
    RUN_TEST(test_a_thrown_powder_grain_pays_drag_displacing_dirt);
    RUN_TEST(test_a_thrown_liquid_grain_pays_no_drag_displacing_water);
    RUN_TEST(test_a_thrown_powder_grain_bounces_off_a_wall_instead_of_waiting);
    RUN_TEST(test_a_thrown_chunk_displacing_nothing_loses_only_the_plain_ramp);
    RUN_TEST(test_a_sub_divisor_speed_impulse_never_moves_more_than_one_cell_a_step);
    RUN_TEST(test_a_full_speed_static_chunk_moves_several_cells_in_one_push);
    RUN_TEST(test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table);
    RUN_TEST(test_a_thrown_chunk_reverses_direction_bouncing_off_a_flat_floor);
    RUN_TEST(test_a_thrown_chunk_deflects_off_a_flat_floor_instead_of_reversing);
    RUN_TEST(test_a_low_speed_entry_below_the_bounce_floor_still_just_waits);
    RUN_TEST(test_a_chunk_bounces_off_the_grid_edge_instead_of_waiting_there_forever);
    RUN_TEST(test_a_chunk_thrown_into_a_closed_box_comes_to_rest);
    RUN_TEST(test_a_chunk_dropped_on_flat_ground_still_settles_on_it);
    RUN_TEST(test_a_chunk_thrown_into_a_brush_drawn_wall_conserves_itself_and_settles);
    RUN_TEST(test_a_blast_in_a_packed_bed_opens_a_cavity_and_reaches_beyond_the_radius);
    RUN_TEST(test_a_blast_queues_impulses_on_every_side_of_the_centre);
    RUN_TEST(test_detonating_empty_space_still_flashes_the_core);
    RUN_TEST(test_without_a_buffer_explode_does_nothing);
    RUN_TEST(test_the_two_wall_explosion_scene_bounces_more_than_once_before_settling);
    RUN_TEST(test_a_thrown_powder_grain_flings_dirt_out_of_the_bank_it_hits);
    RUN_TEST(test_a_stone_chunk_thrown_into_a_sand_bed_launches_sand_airborne);
    RUN_TEST(test_a_powder_grain_thrown_far_still_ejects_from_the_bank_it_hits);
    RUN_TEST(test_a_thrown_powder_grain_flings_water_out_of_the_pool_it_hits);
    RUN_TEST(test_a_struck_water_cell_is_handed_impulse_in_a_backward_cone_from_the_mover);
    RUN_TEST(test_a_thrown_powder_grain_travels_less_far_through_dirt_than_through_air);
    RUN_TEST(test_a_long_plow_through_a_wide_bank_never_exhausts_the_impulse_buffer);
    RUN_TEST(test_a_thrown_powder_grain_conserves_the_dirt_it_ejects);
    RUN_TEST(test_a_thrown_powder_grain_conserves_the_water_mass_it_ejects);

    RUN_TEST(test_nothing_moves_in_free_fall);
    RUN_TEST(test_shaking_spreads_a_pile_sideways);

#ifdef DEVICE_BUILD
    RUN_TEST(test_the_sand_app_can_still_allocate_everything_it_needs);
    RUN_TEST(test_a_full_size_step_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_settled_sand_costs_almost_nothing);
    RUN_TEST(test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget);
    RUN_TEST(test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_water_fits_in_the_frame_budget);
    RUN_TEST(test_a_gravity_flip_on_every_material_at_once_stays_sane);
    RUN_TEST(test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_a_full_screen_of_fire_fits_in_the_frame_budget);
    RUN_TEST(test_four_liquids_reacting_at_once_fits_in_the_frame_budget);
    RUN_TEST(test_the_lava_stress_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget);
    RUN_TEST(test_the_thermal_shock_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_boiler_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_wet_earth_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_water_over_lava_scene_fits_in_the_frame_budget);

    RUN_TEST(test_present_cost_against_a_falling_sand_scene);
    RUN_TEST(test_present_cost_against_the_lava_stress_scene);
    RUN_TEST(test_present_cost_against_the_thermal_shock_scene);
#endif
}

SUITE_REGISTER(run_sand_suite);
