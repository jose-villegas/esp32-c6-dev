/*=============================================================================
 * Portable suite: the falling-sand automaton - seeds, foliage, and growing.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
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

#include "material_palette.h"
#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/* --- seeds ---------------------------------------------------------------- */

/* A seed painted in mid-air falls.
 *
 * It cannot fall the ordinary way. `kind` lives in materials[], and every
 * extended material shares one row of it, so making the plant a
 * KIND_POWDER would make ICE one too - and would break the plant itself,
 * since a grown stem is the same material as the seed and a column of
 * powder six tall would slump the moment it existed. So it falls in the
 * cold pass instead, into empty space and nowhere else. */
static void test_a_seed_falls_until_it_lands(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, W / 2, 0, MATX(MATX_PLANT));

    /* Long enough to fall the height of the board at a leaf's pace, and
     * no longer. */
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, W / 2, H - 2),
        "a seed dropped from the top must come to rest on the floor - it "
        "is poured like a grain, and a grain that hangs where the brush "
        "left it is not one");
}

/* Two of them falling side by side must not hold each other up.
 *
 * The rule that keeps a branch attached to its tree is "touching more of
 * yourself", and on its own it is wrong in exactly this way: a pair of
 * seeds falling together each counted the other and the pair stopped dead
 * in mid-air, hanging off nothing at all. An anchor has to be something
 * that is itself standing on something. */
static void test_two_falling_seeds_do_not_hold_each_other_up(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, W / 2, 1, MATX(MATX_PLANT));
    sand_set(&s, W / 2 + 1, 1, MATX(MATX_PLANT));

    /* Long enough to fall the height of the board at a leaf's pace, and
     * no longer. */
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Not "both on the floor". Once one of them is down, the other may
     * legitimately come to rest on its shoulder - a seed perched on a
     * grounded seed is a heap, which is what a handful of them poured out
     * should look like. What must not happen is the pair stopping where
     * the brush left them. */
    for (int x = 0; x < W; x++) {
        for (int y = 0; y <= 1; y++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MATX(MATX_PLANT), sand_at(&s, x, y),
                "neither seed may still be at the height it was painted - "
                "two of them touching are two unsupported things, not one "
                "supported thing");
        }
    }
    int landed = 0;
    for (int x = 0; x < W; x++) {
        if (sand_at(&s, x, H - 2) == MATX(MATX_PLANT)) {
            landed = 1;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(landed,
        "and at least one of them has to reach the floor");
}


/* A whole BRUSHFUL of seeds falls, not just one or two.
 *
 * The brush paints a disc, and a disc is the shape that breaks a careless
 * attachment rule. Two seeds side by side were already covered - each is
 * an anchor for the other only if it is standing on something, and
 * neither is. Two seeds STACKED were not, and they are worse: the upper
 * one qualifies as an anchor because it has something under it, and the
 * something is the very cell asking whether it may fall. The pair holds
 * itself up, and so does everything painted around it.
 *
 * Reported as the plant only falling when water was poured over it, which
 * is the reactions pass being woken for another reason and finding the
 * pile exactly where the brush left it. */
static void test_a_brushful_of_seeds_does_not_hang_in_the_air(void)
{
    fixture();
    sand_clear(&s);

    sand_spawn_cell(&s, W / 2, 2, 2, MATX(MATX_PLANT));
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_EXTENDED) > 4,
        "the brush has to have painted a disc, not a single cell - one "
        "seed on its own cannot show this at all");

    /* Watched for, not sampled at some fixed step: what the test means is
     * that it got to the bottom, so that is what it waits for. */
    int landed = 0;
    for (int i = 0; i < 300 && !landed; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, H - 1) == MATX(MATX_PLANT)) {
                landed = 1;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(landed,
        "a painted disc of seeds must reach the floor - a cell ABOVE "
        "another cannot be holding it up, and counting it as an anchor "
        "makes the whole pile support itself in mid-air");

    /* No second assert about the top of the disc being empty by then. It
     * is five cells tall on an eight-cell board, so when the lowest one
     * touches the floor the highest is still near where it started - and
     * "it reached the floor at all" is already the whole claim. */
}


/* A seed in a narrow shaft falls down it, rather than sticking to a wall.
 *
 * Only what is GRAVITY-WARD holds a body up. Counting a neighbour beside
 * it as support is an easy thing to write - it is a neighbour, it is
 * solid - and it wedges anything against any vertical surface, which on
 * this board means every seed poured next to a stone wall stops at the
 * height it was poured. */
static void test_a_seed_in_a_shaft_does_not_stick_to_the_walls(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int y = 0; y < H; y++) {
        sand_set(&s, cx - 1, y, STONE);
        sand_set(&s, cx + 1, y, STONE);
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, cx, 0, MATX(MATX_PLANT));

    /* Long enough to fall the height of the board at a leaf's pace, and
     * no longer. */
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT), sand_at(&s, cx, H - 2),
        "a seed between two walls must fall to the bottom of the shaft - "
        "what is beside a thing does not hold it up, and treating it as "
        "support wedges everything against every wall on the board");
}


/* A settled faller keeps the pass armed.
 *
 * may_have_faller gates the whole reactions pass, and it was being set by
 * a cell MOVING rather than by one existing. So a board holding one
 * settled plant cleared the flag on the first step - and then nothing
 * could set it again, because latching happens when a cell is created and
 * a plant that is already there is not created twice.
 *
 * Everything downstream of that is quietly dead: dissolve the ground out
 * from under a plant with acid and it hangs in the air, since nothing
 * re-arms the flag that would let it fall. The cold pass documents the
 * same shape of bug for snow sitting on dry ground, and for the same
 * reason: a cell with nothing to do NOW is not a cell with nothing to do
 * EVER.
 *
 * Asserting on the flag rather than on a scene, deliberately - the flag
 * is the actual invariant. */
static void test_a_settled_plant_keeps_the_reaction_pass_armed(void)
{
    fixture();
    sand_clear(&s);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* Resting on the floor, so it never moves - it just sits there being
     * a plant. */
    sand_set(&s, W / 2, H - 2, MATX(MATX_PLANT));
    sand_set(&s, W / 2 + 1, H - 2, CELL_MAKE(MAT_WOOD, 0));

    for (int i = 0; i < 100; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, W / 2, H - 2), "the plant has to still be there");
    TEST_ASSERT_TRUE_MESSAGE(s.may_have_faller,
        "a plant on the board must keep the faller flag armed even when it "
        "has not moved for a hundred steps - the flag says what is PRESENT, "
        "and once it clears nothing can arm it again");
}

/* And what a tree grows must NOT fall.
 *
 * The other half of the same rule, and the reason it cannot simply be
 * "fall when there is nothing underneath": a branch grows out sideways
 * over thin air, and without attachment every limb would snap off on the
 * step it appeared. */
static void test_a_growing_tree_does_not_shed_what_it_grows(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int grown = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        /* Nothing may ever be sitting on empty space unattached. */
        for (int y = 0; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) != MATX(MATX_PLANT)) {
                    continue;
                }
                grown++;
                /* All EIGHT, because a branch grows out at an angle -
                 * that is what makes it a branch - so the cell it grew
                 * from is diagonally below it and orthogonally it may be
                 * touching nothing at all. Checking four was checking the
                 * shape a tree does not have. */
                int touching = 0;
                for (int d = 0; d < 8; d++) {
                    static const int ox[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
                    static const int oy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
                    const int nx = x + ox[d];
                    const int ny = y + oy[d];
                    if ((unsigned)nx >= (unsigned)W ||
                        (unsigned)ny >= (unsigned)H) {
                        continue;
                    }
                    if (!CELL_IS_EMPTY(sand_at(&s, nx, ny))) {
                        touching = 1;
                    }
                }
                TEST_ASSERT_TRUE_MESSAGE(touching,
                    "no part of a tree may be floating free - a limb that "
                    "detaches from what grew it is a bug in the falling "
                    "rule, not weather");
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(grown > 0, "the tree has to have grown at all");
}


/* A BURIED seed comes up through the soil.
 *
 * Which is how you plant one - drop a seed, cover it over, water it - and
 * it was the one arrangement guaranteed to do nothing at all. Growth put
 * its new cell in empty space, and a buried seed has none: the cell it
 * wanted was occupied, and occupied was the end of the matter.
 *
 * A shoot shoves instead. The run of loose material above it shifts up one
 * and the shoot takes the space, which is why the soil count is checked as
 * carefully as the emergence - a shoot that ATE its way out would pass the
 * first assert perfectly and quietly hollow out every bank on the board. */
static void test_a_buried_seed_comes_up_through_the_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        for (int y = H - 5; y < H - 1; y++) {
            sand_set(&s, x, y, CELL_SOIL(MAT_DIRT, x & 1,
                                         y >= H - 3 ? SOIL_MOISTURE_MAX : 0));
        }
    }
    /* Two rows of soil on top of it. */
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));
    const int soil = count_cells_of(MAT_DIRT);

    int up = 0;
    for (int i = 0; i < 1500 && !up; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            for (int y = 0; y <= H - 6; y++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT) ||
                    CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WOOD) {
                    up = 1;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(up,
        "a seed under two rows of watered soil must reach daylight - "
        "burying a seed and watering it is how you plant one, and it was "
        "the one way to guarantee nothing happened");
    TEST_ASSERT_EQUAL_INT_MESSAGE(soil, count_cells_of(MAT_DIRT),
        "and it must SHOVE the soil aside, not eat it - a shoot that "
        "consumed its cover would tunnel every bank on the board hollow "
        "while passing every other test here");
}

/* But it will not push through stone.
 *
 * The other half of the same rule, and what keeps shoving from being a
 * licence to go anywhere: a shoot displaces loose things - powders,
 * liquids, gases - and stops dead at anything STATIC. Without that, a seed
 * under a flagstone lifts it. */
static void test_a_seed_under_stone_stays_put(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        sand_set(&s, x, H - 4, STONE);          /* a lid */
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    /* Kept watered - not load-bearing for the shove rule this test checks,
     * but keeps the scene closer to a real watered planting. */
    for (int i = 0; i < 1500; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "a seed with a stone lid over it must stay one seed - a shoot "
        "shoves what is loose and stops at what is not, or it lifts "
        "flagstones");
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(STONE, sand_at(&s, x, H - 4),
            "and the lid must still be where it was put");
    }
}


/* A limb attached to a TRUNK stays on it.
 *
 * A branch grows out at an angle, so the cell it came from is diagonally
 * below it; orthogonally it is often touching nothing. Checking four
 * neighbours for an anchor therefore snapped off every limb whose trunk
 * did not happen to continue past it - and on a tilt, where the wood stays
 * put while gravity swings round underneath, whole crowns detached at once
 * and dropped a cell a step. Reported as the plant appearing to teleport,
 * and as the falling looking harsh: it was not the speed, it was how much
 * of the tree was falling.
 *
 * Hardening is what makes this bite, which is why the trunk here is wood:
 * a limb has to recognise what its own material turns into as something
 * to hold on to. */
static void test_a_limb_hangs_on_to_a_wooden_trunk(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    /* Diagonally off the top of it, touching nothing else. */
    sand_set(&s, cx + 1, H - 5, MATX(MATX_PLANT));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, cx + 1, H - 5),
        "a limb growing diagonally off a trunk must stay where it grew - "
        "it is touching the tree, just not squarely, and a tree that sheds "
        "every branch it grows is not one");
}


/* A tree is not a stick.
 *
 * Growth used to go straight up from the tip, every time, which grew a
 * one-cell column and nothing else - reported as growing "mostly one
 * side". Reaching the tip is still the common case; what makes it a tree
 * is that it sometimes leans, sometimes starts a limb further down, and
 * sometimes thickens the trunk instead. The last of those is what turns a
 * sapling into wood, because hardening counts a straight run along gravity
 * and a second column beside the first is a second run of its own. */
static void test_a_tree_grows_wider_than_one_column(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int columns = 0;
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H - 2; y++) {
            const cell_t c = sand_at(&s, x, y);
            if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
                columns++;
                break;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, columns,
        "a tree must occupy more than the one column it was sown in - "
        "growing only from the tip and only straight up is a stick");
}


/* Water sitting on a plant goes into the ground.
 *
 * Every extended material shares one physics row, and that row is
 * `KIND_STATIC` at stone's density - so water cannot fall through foliage
 * and nothing about foliage can soak it up. Fill a bowl of leaves and the
 * water stays there for ever, which is exactly what it looked like.
 *
 * A plant conducts it instead: a unit of the liquid for a level of
 * moisture in the soil its roots reach. It cannot use the ordinary
 * `soaks` path to do it, because that raises the cell's own variant to
 * hold what it took, and a plant's variant is WHICH EXTENDED MATERIAL IT
 * IS - one soak would turn it into the next entry in the table.
 *
 * The scene walls the water in so the plant is the only way out. Water
 * that simply drained round the side would pass an assert about the water
 * going away while proving nothing at all. */
static void test_a_plant_drains_standing_water_into_the_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));   /* bone dry */
    }
    const int cx = W / 2;
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));               /* the plug */
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));

    /* Sampled as it goes, not at the end. Soil dries, so by the time the
     * water is gone the moisture it turned into has gone too - asserting
     * on the final state tests the drying rate, not the drinking. */
    int wet = 0;
    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
        int now = 0;
        for (int x = 0; x < W; x++) {
            now += CELL_MOISTURE(sand_at(&s, x, H - 2));
        }
        if (now > wet) {
            wet = now;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_WATER),
        "water walled in above a plant must drain through it - foliage is "
        "solid and undrainable otherwise, so a bowl of leaves holds a pond "
        "for ever");
    TEST_ASSERT_TRUE_MESSAGE(wet > 0,
        "and it must come out at the ROOTS - water that merely vanished "
        "would pass the assert above and quietly delete itself");
}

/* A plant with nowhere to put it does not drink.
 *
 * The guard on the test above, and the thing that stops this being a
 * disposal chute for water: what a plant does with a drink is hand it to
 * the soil. Rooted on bare stone there is no soil, so the water stays
 * where it is. */
static void test_a_plant_rooted_on_stone_does_not_drink(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, STONE);          /* no soil anywhere */
    }
    const int cx = W / 2;
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));

    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_WATER),
        "a plant standing on bare stone has nowhere to put a drink, so "
        "the water must stay - drinking is conduction into the ground, "
        "not a way of making water disappear");

    /* Nor does it drink OIL, however good the ground beneath it is. The
     * same rule as soaking and the same reason: only the liquid knows
     * whether it is wet, and a plant asking for "any adjacent liquid"
     * would siphon a slick into the soil as moisture. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_OIL, MASS_MAX));

    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_OIL),
        "oil walled in above a plant must stay there - a plant drinks "
        "water, and a rule that takes any liquid pipes a slick into the "
        "ground as moisture");
}



/* Hardening leaves a CROWN behind.
 *
 * Hardening is the only moment that holds a whole run at once, and after it
 * the tree has no green left low down - everything that grew is timber, and
 * growth is the only thing that makes cells. So a canopy has to be hung
 * here or nowhere; anything trying to leaf a tree through ordinary growth
 * is working on a part of it that no longer exists.
 *
 * Reported as branches spawning "and many times just be wood". */
static void test_a_hardened_trunk_is_left_with_foliage(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int leafed = 0;
    for (int i = 0; i < 3000 && !leafed; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);

        for (int y = 0; y < H && !leafed; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_LEAF)) {
                    leafed = 1;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(leafed,
        "a tree that hardens into wood must be left with foliage on it - "
        "nothing else can put any there, because hardening consumes every "
        "green cell it walks and wood does not grow");
}

/* And a hardened run is WIDER at its foot than at its top.
 *
 * "Plant should also widen as it grows, so it is interesting that wood is
 * just a stick." It was: thickening happened during growth, gated on how
 * far the growing cell was from the ground, and after the first hardening
 * every green cell sits on a wood column - so its lift is at least that
 * column's height and the allowance is zero for the rest of the tree's
 * life. Thickening was structurally dead the moment a tree first became a
 * tree.
 *
 * The taper is the half that is easy to lose. A trunk of uniform width is
 * a pillar, passes any "is it thick" assert, and looks nothing like a
 * tree - so this measures both ends and compares them, rather than
 * measuring one and hoping. */
static void test_a_hardened_trunk_is_thicker_at_the_foot(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int foot = 0, top = 0;
    for (int i = 0; i < 3000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);

        /* Widest wood row anywhere, against the widest in the top half. */
        for (int y = 0; y < H - 2; y++) {
            int wide = 0;
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WOOD) {
                    wide++;
                }
            }
            if (wide > foot) {
                foot = wide;
            }
            if (y < (H - 2) / 2 && wide > top) {
                top = wide;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, foot,
        "a hardened trunk must be more than one cell wide somewhere - "
        "thickening during growth cannot reach a tree that has already "
        "hardened, so it has to happen as the wood is laid");
    TEST_ASSERT_GREATER_THAN_MESSAGE(top, foot,
        "and it must be WIDER at the foot than up in the branches - a "
        "trunk of uniform width is a pillar, and passes every assert that "
        "only asks whether it is thick");
}

/* A limb TRAVELS. It does not go out one cell and then climb.
 *
 * Growth used to reckon every direction from gravity, which meant a branch
 * could never get anywhere: one cell out makes a run of ONE, and a run
 * under three trips the gate that forces the straight-up arm - on every
 * attempt, for ever. So limbs went out a single cell and then grew
 * vertically alongside the trunk, which is the same thin-thread shape that
 * made basal suckers look like floating debris.
 *
 * A run's direction is not stored anywhere; it is read back off the grid,
 * from where the run has been over its last few cells. `holds_line` is the
 * chance of using it, and zero restores the old behaviour exactly - which
 * is what this is really pinned against.
 *
 * On a grid of its own, because the shared fixture is eight by eight and a
 * diagonal limb runs out of ceiling in three cells - far too soon to tell
 * travelling from the sideways cell an occasional LEAN produces anyway.
 * The limb is pre-built with a heading already established, because what
 * is being tested is what a run does once it HAS a direction. */
#define LIMB_W 30
#define LIMB_H 26

static void test_a_limb_travels_outward_instead_of_climbing(void)
{
    /* HEAP, not static file scope - one malloc reused across all eight
     * seeds via memset + a fresh sand_init() each time, freed once after
     * the loop - see drop_impulse_buf's own comment above for why this
     * file's static test fixtures cannot share the framebuffer's memory
     * budget. */
    uint8_t *limb_cells = malloc((size_t)LIMB_W * LIMB_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(limb_cells,
        "limb-travel grid must fit in what the framebuffer leaves");

    /* Summed over eight seeds, and that is not laziness about a flaky
     * test - it is what the measurement actually supports. On any single
     * seed the two builds overlap: with the heading, one limb reached 7
     * and another 13; without it, the range is 6 to 7. A threshold picked
     * to split one seed would be fitted to that seed and would say
     * nothing. Totalled, the gap is unmistakable - 70 against 52 - and
     * the assert sits in the middle of it. */
    int total = 0;
    for (unsigned seed = 1; seed <= 8; seed++) {
        sand_t t;
        memset(limb_cells, 0, (size_t)LIMB_W * LIMB_H);
        sand_init(&t, limb_cells, LIMB_W, LIMB_H, seed * 7919u);
        sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);
        sand_set_decay(&t, SAND_DECAY_PER_MATERIAL);

        const int bx = 3;
        for (int x = 0; x < LIMB_W; x++) {
            sand_set(&t, x, LIMB_H - 1, STONE);
            sand_set(&t, x, LIMB_H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        for (int y = LIMB_H - 8; y < LIMB_H - 2; y++) {
            sand_set(&t, bx, y, CELL_MAKE(MAT_WOOD, 0));
        }
        sand_set(&t, bx + 1, LIMB_H - 9,  MATX(MATX_PLANT));
        sand_set(&t, bx + 2, LIMB_H - 10, MATX(MATX_PLANT));
        sand_set(&t, bx + 3, LIMB_H - 11, MATX(MATX_PLANT));

        int reach = bx + 3;
        for (int i = 0; i < 4000; i++) {
            for (int x = 0; x < LIMB_W; x++) {
                sand_set(&t, x, LIMB_H - 2,
                         CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
            }
            sand_step(&t, 0, 1000, 0);

            for (int y = 0; y < LIMB_H - 2; y++) {
                for (int x = 0; x < LIMB_W; x++) {
                    const cell_t c = sand_at(&t, x, y);
                    if ((c == MATX(MATX_PLANT) ||
                         CELL_MATERIAL(c) == MAT_WOOD) && x > reach) {
                        reach = x;
                    }
                }
            }
        }
        total += reach;
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(limb_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(60, total,
        "limbs with a heading must carry on in that direction - reckoning "
        "every growth from gravity instead sends one straight up the side "
        "of its own trunk, and no tree ever puts out a bough");
}


/* A crowned trunk puts out new growth; a bare one does not.
 *
 * This is where a tree's growth comes from now. It used to come from a
 * green tip that hardening deliberately spared - which meant every tree
 * carried green permanently, and growth scaled with how much of it there
 * was, because every green cell rolled every step.
 *
 * The "already in leaf" half is not decoration, it is the bound. A canopy
 * touches a dozen cells of wood; if bare wood could bud, the rate would
 * scale with the trunk and the forest would run away exactly as it did
 * when growth scaled with green. Crowned wood at the head of its trunk is
 * a handful of cells per tree however fat it gets. */
static void test_a_crowned_trunk_buds_and_a_bare_one_does_not(void)
{
    const int cx = W / 2;

    /* Crowned: wood, a leaf on it, wet ground under it. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&s, cx + 1, H - 5, MATX(MATX_LEAF));

    int budded = 0;
    for (int i = 0; i < 4000 && !budded; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !budded; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT)) {
                    budded = 1;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(budded,
        "a trunk in leaf and in reach of water must put out new growth - "
        "hardening leaves no tip behind, so this is the only way a tree "
        "gets any taller");

    /* Bare: the same trunk, same water, no leaf on it. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }

    for (int i = 0; i < 4000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(MATX(MATX_PLANT),
                    sand_at(&s, x, y),
                    "bare wood must NOT bud - a canopy touches a dozen "
                    "cells of trunk, and if every one of them could bud, "
                    "the rate would scale with the tree all over again");
            }
        }
    }

    /* And crowned, but on ground too thin to pay for a limb. A bud costs
     * BUD_COST levels of moisture, not one - because a bud is the only
     * thing here that COMPOUNDS, so what has to bound it is the scarce
     * thing rather than a probability. Priced at one level, buds simply
     * drank the pour and the forest ran away. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 1));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&s, cx + 1, H - 5, MATX(MATX_LEAF));

    for (int i = 0; i < 4000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 1));
        }
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(MATX(MATX_PLANT),
                    sand_at(&s, x, y),
                    "a crowned trunk on barely damp ground must NOT bud - "
                    "a limb has to be paid for, or the only thing bounding "
                    "the one mechanism that compounds is a dice roll");
            }
        }
    }
}

/* --- foliage -------------------------------------------------------------- */

/* A leaf on a tree never multiplies, and never moves.
 *
 * This is the entire reason foliage is its own material rather than more
 * plant. Every cell of a PLANT is a grower, and find_water() walks down
 * through wood so foliage can always drink - which means a canopy made of
 * plant would feed the growth loop with every leaf it put out, and that
 * loop has run away once already.
 * Charging moisture per leaf makes it expensive; having no `grows` field
 * makes it impossible.
 *
 * Two scenes, because one cannot show both halves without confusing them.
 * A trunk standing in wet soil BUDS leaves of its own (see wood's
 * `sprouts`), so a scene with both a trunk and watered ground cannot tell
 * "the leaf spread" from "the tree put out another one" - which is exactly
 * how this test first failed when budding changed from plant to foliage. */
static void test_a_leaf_neither_spreads_nor_falls(void)
{
    /* One: on watered soil with NO wood anywhere. Nothing else in the
     * scene can produce foliage - so any second leaf would have to have
     * come from the first. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_LEAF));

    for (int i = 0; i < 2000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "a leaf on watered ground must stay exactly one leaf - foliage "
        "that can grow is a canopy that feeds the growth loop, which is "
        "the whole reason it is not made of plant");

    /* Two: hanging off the side of a trunk, over nothing, on DRY ground -
     * dry so the trunk cannot bud, which would put leaves in the scene
     * that this half is not about. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&s, cx + 1, H - 4, MATX(MATX_LEAF));

    for (int i = 0; i < 2000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_LEAF),
        sand_at(&s, cx + 1, H - 4),
        "and it must not have moved - it has no `falls`, so a limb of "
        "foliage hangs off its trunk over thin air and stays there");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "nor multiplied, on dry ground where nothing can bud");
}

/* And water gets through a canopy.
 *
 * The field most likely to be left off, because it sounds like the
 * opposite of everything else about this material. Every extended material
 * shares one physics row - KIND_STATIC at stone's density - so water can
 * neither fall through foliage nor soak into it, and a bowl of leaves
 * holds a pond indefinitely. That was a real bug on the plant already, and
 * a leaf is the surface rain actually lands on. */
static void test_a_leaf_drains_standing_water_into_the_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));   /* bone dry */
    }
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_LEAF));            /* the only way out */
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));

    int wet = 0;
    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
        int now = 0;
        for (int x = 0; x < W; x++) {
            now += CELL_MOISTURE(sand_at(&s, x, H - 2));
        }
        if (now > wet) {
            wet = now;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_WATER),
        "water walled in above a leaf must drain through it - foliage is "
        "solid and undrainable otherwise, and a canopy would hold a pond");
    TEST_ASSERT_TRUE_MESSAGE(wet > 0,
        "and it must come out in the soil, not simply vanish");
}


/* --- growing ------------------------------------------------------------- */

/* A plant on wet soil climbs.
 *
 * It has no variant to grow WITH - the low nibble is which extended
 * material it is - so it grows by occupying cells, walking to the tip of
 * its own column and putting the new one beyond that. This is the test
 * that the walk happens at all: without it a plant could only ever be one
 * cell tall, because the moment it grew, the new cell would be out of
 * reach of the ground and nothing further could happen. */
static void test_a_plant_on_wet_soil_grows_upward(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int tall = 0;
    for (int y = 0; y < H; y++) {
        const cell_t c = sand_at(&s, W / 2, y);
        if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
            tall++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(tall >= 3,
        "a plant standing on watered soil must climb away from the ground "
        "- growth is spatial for a material with no variant to spend");
}

/* And on dry soil it does not.
 *
 * Water is the whole limit on how far a tree gets, so soil with nothing in
 * it has to stop one. Without this the plant is not a plant, it is a
 * self-replicating material that fills the screen. */
static void test_a_plant_on_dry_soil_stays_where_it_is(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    /* Wet soil on the far side of a stone divider, so the board as a
     * whole HAS moisture on it - otherwise may_have_moisture is clear, the
     * growth branch is never reached, and the test would pass on the
     * strength of a pass gate rather than on the plant looking at the
     * ground it is actually standing on. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, x == W / 2 + 1 ? STONE
                             : x > W / 2 + 1
                                 ? CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX)
                                 : CELL_SOIL(MAT_DIRT, 0, 0));
    }
    sand_set(&s, 1, H - 3, MATX(MATX_PLANT));

    /* The most it is ever seen to be, not what is left at the end - what
     * this is about is that it never GREW past its starting seed. */
    int tall = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        int now = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT)) {
                    now++;
                }
            }
        }
        if (now > tall) {
            tall = now;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tall,
        "a plant on bone-dry soil must stay a seedling - moisture is the "
        "only thing limiting how tall a tree gets");
}

/* A tall enough plant becomes a trunk - and the trunk is not on fire.
 *
 * Wood's variant is its BURN PROGRESS, and the general placement helper
 * hands a new cell MATERIAL_VARIANTS - 1, which is right for a fill level
 * or a life counter and means "well alight" for wood. That is deliberate
 * where the reaction is fire making an ember of a log; it is catastrophic
 * here. Every tree that reached this height burned to nothing over the
 * next couple of hundred steps, on a board with no flame anywhere on it,
 * so the assert on the variant matters as much as the one on the
 * material. */
static void test_a_tall_plant_hardens_into_wood_that_is_not_alight(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int wood = 0;
    for (int y = 0; y < H; y++) {
        const cell_t c = sand_at(&s, W / 2, y);
        if (CELL_MATERIAL(c) != MAT_WOOD) {
            continue;
        }
        wood++;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_VARIANT(c),
            "a trunk is wood that GREW, not wood that caught - wood's "
            "variant is burn progress, and a log placed at the top of it "
            "burns away on a board with no fire on it");
    }
    TEST_ASSERT_TRUE_MESSAGE(wood > 0,
        "a plant that grows a full run tall must harden into wood");
}


/* The grain hash must not stripe.
 *
 * Stone and wood take their speckle from this and nothing else, so if its
 * low bits do not vary they are not speckled - and for a long time they
 * were not. The low three bits came out very nearly constant along a row,
 * which drew both materials as flat horizontal bands, one shade each.
 * Nothing caught it because the hash lived in app_sand.c, which does not
 * compile on a host; moving it next to the tables that use it is half the
 * fix and this is the other half.
 *
 * Both halves of the assert matter. Even spread alone is satisfied by a
 * hash that stripes, as long as it stripes in equal proportions - it is
 * the ADJACENCY that says the variation is per cell rather than per row. */
static void test_the_grain_hash_does_not_stripe(void)
{
    enum { N = 64, BUCKETS = 8 };

    int same_row = 0, same_col = 0, seen[BUCKETS] = { 0 };
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const unsigned h = material_grain_hash(x, y) & (BUCKETS - 1);
            seen[h]++;
            if (x > 0 &&
                h == (material_grain_hash(x - 1, y) & (BUCKETS - 1))) {
                same_row++;
            }
            if (y > 0 &&
                h == (material_grain_hash(x, y - 1) & (BUCKETS - 1))) {
                same_col++;
            }
        }
    }

    /* One in eight neighbours will match by chance. Twice that is still
     * comfortably clear of the near-100% a striping hash produces. */
    const int pairs = N * (N - 1);
    const int limit = pairs / BUCKETS * 2;
    TEST_ASSERT_LESS_THAN_MESSAGE(limit, same_row,
        "side-by-side cells must mostly differ - a hash whose low bits are "
        "constant along a row paints stone and wood in flat stripes");
    TEST_ASSERT_LESS_THAN_MESSAGE(limit, same_col,
        "and so must cells one above another, for the same reason with the "
        "board turned ninety degrees");

    for (int i = 0; i < BUCKETS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[i] > N * N / BUCKETS / 2,
            "every shade must get used, and roughly evenly - a grain that "
            "reaches two of its eight values is two-tone noise");
    }
}


/* The dithered direction is recorded, and it is not the nearest one.
 *
 * Two different questions about gravity, and the simulation has always
 * needed both. "Which way is down, near enough" has to be steady, or a
 * resting pool judged against a constantly-changing axis reads as
 * unbalanced when it is not. "Which way is down THIS step" has to wobble
 * between the two eighths a tilt falls between, in proportion, or
 * everything flows at one of eight fixed angles instead of at the angle
 * the board is really at.
 *
 * The sweep and the liquid pass have used the dithered one all along.
 * Growth was reading the steady one, which is what made a stem a rigid
 * straight line - reported as the vertical growth being "too strict or
 * rigid whereas we have smoothing in the way we map tilt/gravity", and
 * exactly right. This is the plumbing that fixes it.
 *
 * Both asserts are needed: a build that recorded the nearest direction in
 * both fields passes the second on its own. */
static void test_a_tilt_between_two_directions_is_dithered_not_snapped(void)
{
    fixture();
    sand_clear(&s);

    /* Well off any of the eight axes, so the two it falls between should
     * both come up. */
    int steps_seen = 0, load_seen = 0;
    int step_dx[8] = { 0 }, load_dx[8] = { 0 };

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 400, 1000, 0);

        int found = 0;
        for (int k = 0; k < steps_seen; k++) {
            if (step_dx[k] == s.last_step_dx) {
                found = 1;
            }
        }
        if (!found && steps_seen < 8) {
            step_dx[steps_seen++] = s.last_step_dx;
        }

        found = 0;
        for (int k = 0; k < load_seen; k++) {
            if (load_dx[k] == s.last_load_dx) {
                found = 1;
            }
        }
        if (!found && load_seen < 8) {
            load_dx[load_seen++] = s.last_load_dx;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, steps_seen,
        "a tilt between two of the eight directions must spend steps on "
        "both - one value means everything on the board flows at a snapped "
        "angle rather than at the angle the board is at");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, load_seen,
        "while the NEAREST direction stays put, which is what anything "
        "measuring a resting state has to be judged against");
}


/* A stem that WANDERS is still one stem.
 *
 * Growth points along the dithered gravity direction, which spends steps
 * on each of the two eighths a tilt falls between - so a trunk climbs with
 * a kink in it rather than in a dead straight line. Every walk over a
 * plant has to tolerate that: the walk to the tip, the walk back to a
 * branch site, and the run that decides whether it has grown tall enough
 * to be wood.
 *
 * A staircase is the cheapest way to say so. Six cells, each one up or up
 * and across from the last, standing on wet soil: a walk that insists on a
 * straight line sees a run of two and this never becomes a trunk. */
static void test_a_stem_that_wanders_still_hardens(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    /* up, up, up-left, up, up-left, up */
    static const int stem[6][2] = {
        { 4, 5 }, { 4, 4 }, { 3, 3 }, { 3, 2 }, { 2, 1 }, { 2, 0 },
    };
    for (int i = 0; i < 6; i++) {
        sand_set(&s, stem[i][0], stem[i][1], MATX(MATX_PLANT));
    }

    /* Kept watered throughout. Soil dries, growth costs moisture, and
     * hardening only happens on a growth - so a single saturation turns
     * this into a race against the drying rate rather than a test of the
     * walk. */
    int hardened = 0;
    for (int i = 0; i < 4000 && !hardened; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
        for (int k = 0; k < 6; k++) {
            if (CELL_MATERIAL(sand_at(&s, stem[k][0], stem[k][1])) ==
                MAT_WOOD) {
                hardened = 1;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(hardened,
        "a stem six cells long must harden into wood even though it is not "
        "straight - a walk that only steps in one direction measures a run "
        "of two and no tree on a tilted board would ever become a trunk");
}


/* A bare trunk in wet ground buds again.
 *
 * The loop this closes: growth hardens a plant into wood, and hardening
 * consumes the very cells that could grow. So a tree that reached its full
 * height was finished for good, and one that lost its foliage - to fire,
 * to acid, to a landslide - stayed a bare post for ever. The scene here is
 * the worst case on purpose: wood only, no plant anywhere on the board, so
 * nothing but the trunk itself can be responsible for what appears.
 *
 * The dry half of the test is the half that matters. Budding out of
 * nothing would mean a wooden wall sprouted a hedge, and it is the
 * moisture that has to be doing the work. */
static void test_a_bare_trunk_in_wet_ground_buds_again(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, W / 2, y, CELL_MAKE(MAT_WOOD, 0));
    }

    /* Specifically FOLIAGE. It used to bud a plant, and a plant at the
     * foot of a trunk is a sucker - a grower, which climbed the outside
     * of the trunk as a wandering one-cell thread that never got thick
     * enough to harden and stop. Asserting on MAT_EXTENDED alone would
     * pass on either, which is what it did while the bug was there. */
    int budded = 0;
    for (int i = 0; i < 1500 && !budded; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !budded; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_LEAF)) {
                    budded = 1;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(budded,
        "wood standing in watered soil must put out new growth - without "
        "it a tree is a thing that happens once, and anything that takes "
        "its foliage leaves a post that can never recover");

    /* And on dry ground, nothing. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, W / 2, y, CELL_MAKE(MAT_WOOD, 0));
    }
    for (int i = 0; i < 1500; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_EXTENDED),
        "and on dry ground it must not - budding out of nothing is a "
        "wooden wall growing a hedge, and water is what pays for growth");

    /* A trunk in barely damp ground stays a trunk.
     *
     * Deliberately NOT claimed as a test that budding spends the water it
     * uses. It is not: budding puts its cell in an empty space beside the
     * trunk, and on a grid this size those run out long before the
     * moisture does, so a build that budded for free passes this
     * unchanged - checked, by mutation. The moisture cost is real and
     * bounds the total on a full board, and nothing here can see it.
     *
     * What this does pin is that one level of moisture is not a licence
     * to keep budding, which is the failure that would be visible. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    sand_set(&s, W / 2, H - 2, CELL_SOIL(MAT_DIRT, 1, 1));   /* one level */
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, W / 2, y, CELL_MAKE(MAT_WOOD, 0));
    }
    for (int i = 0; i < 1500; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(2, count_cells_of(MAT_EXTENDED),
        "a trunk in barely damp ground must put out a bud or two, not a "
        "thicket - budding is the one part of growth with no lift to "
        "limit it, since it happens at the foot of the trunk");
}

void run_sand_seeds_suite(void)
{
    RUN_TEST(test_a_seed_falls_until_it_lands);
    RUN_TEST(test_two_falling_seeds_do_not_hold_each_other_up);
    RUN_TEST(test_a_brushful_of_seeds_does_not_hang_in_the_air);
    RUN_TEST(test_a_seed_in_a_shaft_does_not_stick_to_the_walls);
    RUN_TEST(test_a_settled_plant_keeps_the_reaction_pass_armed);
    RUN_TEST(test_a_growing_tree_does_not_shed_what_it_grows);
    RUN_TEST(test_a_buried_seed_comes_up_through_the_soil);
    RUN_TEST(test_a_seed_under_stone_stays_put);
    RUN_TEST(test_a_limb_hangs_on_to_a_wooden_trunk);
    RUN_TEST(test_a_tree_grows_wider_than_one_column);
    RUN_TEST(test_a_plant_drains_standing_water_into_the_soil);
    RUN_TEST(test_a_plant_rooted_on_stone_does_not_drink);
    RUN_TEST(test_a_hardened_trunk_is_left_with_foliage);
    RUN_TEST(test_a_hardened_trunk_is_thicker_at_the_foot);
    RUN_TEST(test_a_limb_travels_outward_instead_of_climbing);
    RUN_TEST(test_a_crowned_trunk_buds_and_a_bare_one_does_not);
    RUN_TEST(test_a_leaf_neither_spreads_nor_falls);
    RUN_TEST(test_a_leaf_drains_standing_water_into_the_soil);
    RUN_TEST(test_a_plant_on_wet_soil_grows_upward);
    RUN_TEST(test_a_plant_on_dry_soil_stays_where_it_is);
    RUN_TEST(test_a_tall_plant_hardens_into_wood_that_is_not_alight);
    RUN_TEST(test_the_grain_hash_does_not_stripe);
    RUN_TEST(test_a_tilt_between_two_directions_is_dithered_not_snapped);
    RUN_TEST(test_a_stem_that_wanders_still_hardens);
    RUN_TEST(test_a_bare_trunk_in_wet_ground_buds_again);
}

SUITE_REGISTER(run_sand_seeds_suite);
