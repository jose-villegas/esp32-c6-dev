/*=============================================================================
 * Portable suite: the falling-sand automaton - material identity,
 * displacement, and levelling.
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

#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/* --- materials ------------------------------------------------------------ */

/* Everything above this point is about sand. These are about the fact that a
 * cell is now a material, and that materials behave differently from each
 * other - which is the whole basis of the sandbox.
 *
 * WATER/STONE/SAND/GAS/FIRE/WOOD/STEAM/SMOKE/EMBER and mass_of()/count_of()
 * live in suite_sand_common.{c,h} - reused throughout the split. */

static void test_a_cell_carries_both_material_and_variant(void)
{
    const cell_t c = CELL_MAKE(MAT_WATER, 5);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(c),
        "the high nibble is the material");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, CELL_VARIANT(c),
        "the low nibble is the variant");
    TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(c), "and it is not empty");

    /* The trap this guards: a cell whose variant happens to be zero is still
     * occupied, so emptiness has to be read off the material nibble alone. */
    TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(CELL_MAKE(MAT_SAND, 0)),
        "a variant of zero is not an empty cell");
    TEST_ASSERT_TRUE(CELL_IS_EMPTY(CELL_EMPTY));
}

static void test_stone_never_moves(void)
{
    fixture();
    sand_set(&s, 3, 0, STONE);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(STONE, sand_at(&s, 3, 0),
        "a static material stays exactly where it is put, unsupported or not "
        "- that is what makes it something to build with");
}

static void test_nothing_displaces_stone(void)
{
    fixture();
    sand_set(&s, 3, 5, STONE);
    sand_set(&s, 3, 4, SAND);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(STONE, sand_at(&s, 3, 5),
        "sand must not sink through a stone floor however heavy it is");
}

static void test_sand_sinks_through_water(void)
{
    fixture();
    /* A pool with a grain of sand sitting on top of it. */
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "sand is denser than water, so it must sink all the way through the "
        "pool rather than float on it");
}

static void test_water_does_not_sink_through_sand(void)
{
    fixture();
    /* The mirror of the last one, and the half that a naive swap gets wrong:
     * displacement has to be one-way, or the two materials trade places back
     * and forth for ever. */
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_set(&s, 3, 3, WATER);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, 3)),
        "water is lighter than sand, so it must sit on top rather than sink "
        "into it");
}

static void test_displacement_conserves_both_materials(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    for (int x = 1; x < 5; x++) {
        sand_set(&s, x, 2, SAND);
    }
    const long water = mass_of(&s, W, H, MAT_WATER);
    const int  sand  = count_of(MAT_SAND);

    for (int i = 0; i < 80; i++) {
        sand_step(&s, 200, 1000, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(water, mass_of(&s, W, H, MAT_WATER),
            "displacing a liquid moves its mass about and destroys none of "
            "it - measured as an amount, since cells merge and split");
        TEST_ASSERT_EQUAL_INT_MESSAGE(sand, count_of(MAT_SAND),
            "and the powder doing the displacing is still whole cells");
    }
}

static void test_water_finds_its_own_level(void)
{
    fixture();
    /* A column of water in the middle of the floor. Sand would stand there as
     * a heap at its angle of repose; water must not. */
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, WATER);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int tallest = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!CELL_IS_EMPTY(sand_at(&s, x, y))) {
                tallest = y;
                y = H;
                break;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(2, tallest,
        "a column of water must collapse and spread along the floor - having "
        "no angle of repose is the whole difference between a liquid and a "
        "powder");
}

static void test_a_powder_still_holds_a_heap(void)
{
    /* The other side of the same coin: making water spread must not have made
     * sand spread too. */
    fixture();
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, SAND);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int occupied_columns = 0;
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H; y++) {
            if (!CELL_IS_EMPTY(sand_at(&s, x, y))) {
                occupied_columns++;
                break;
            }
        }
    }

    TEST_ASSERT_LESS_THAN_MESSAGE(W, occupied_columns,
        "sand must still pile rather than level out - if it spread across the "
        "whole floor it has stopped being a powder");
}

static void test_water_can_be_held_by_a_stone_basin(void)
{
    /* What makes it a sandbox rather than a toy: build something, and it
     * holds. */
    fixture();
    sand_set(&s, 2, H - 1, STONE);
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 1, STONE);
    sand_set(&s, 5, H - 2, STONE);
    for (int x = 3; x < 5; x++) {
        sand_set(&s, x, 4, WATER);
    }
    const int water = count_of(MAT_WATER);

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int held = 0;
    for (int x = 3; x < 5; x++) {
        for (int y = H - 2; y < H; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                held++;
            }
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(water, held,
        "every drop must still be inside the basin - a wall that leaks is not "
        "a wall");
}

static void test_a_drop_resting_on_a_pool_comes_to_rest(void)
{
    /* The reported behaviour: settled water crept about like slime, a lump
     * sliding over the surface instead of becoming part of it.
     *
     * A drop with nothing on top of it has no weight pressing it anywhere, so
     * it must simply stop. Without the pressure test it slides sideways every
     * step - and since the sweep direction alternates, it slid left, right,
     * left, wandering the surface for ever. */
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    sand_set(&s, 3, 3, WATER);       /* one drop, on top, nothing above it */

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Checked one step at a time, not by comparing two distant snapshots.
     * The wandering is an OSCILLATION - the sweep direction alternates, so a
     * loose drop slides left, then right, and lands back where it started
     * every second step. Comparing step 40 against step 80 sees no difference
     * at all and passes a bug that is plainly visible on screen. */
    uint8_t settled[W * H];
    for (int i = 0; i < 5; i++) {
        memcpy(settled, cells, sizeof(settled));
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(settled, cells, sizeof(settled),
            "water with nothing resting on it must come to a complete stop - "
            "sliding about on a level surface is what made it look like slime");
    }
}

static void test_water_under_weight_still_spreads(void)
{
    /* The other half, and the one a careless fix breaks: water DOES have to
     * spread, or it stands in a column on a flat floor and stops behaving
     * like a liquid at all. What makes it spread is the weight above it. */
    fixture();
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, WATER);   /* a column standing on the floor */
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int on_the_floor = 0;
    for (int x = 0; x < W; x++) {
        if (CELL_MATERIAL(sand_at(&s, x, H - 1)) == MAT_WATER) {
            on_the_floor++;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, on_the_floor,
        "a column of water must collapse and run along the floor - the cells "
        "underneath have the whole column pressing on them");
}

static void test_water_poured_into_a_basin_reaches_both_ends(void)
{
    /* End to end: dropped in one corner, it must find the far side. */
    fixture();
    for (int y = H - 3; y < H; y++) {
        sand_set(&s, 0, y, STONE);
        sand_set(&s, W - 1, y, STONE);
    }
    for (int y = 0; y < 6; y++) {
        sand_set(&s, 1, y, WATER);
    }

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, W - 2, H - 1)),
        "water poured in at one end must travel to the other - if it cannot, "
        "the pressure rule has been made too strict to flow at all");
}

/* A basin needs room around it, so these get a grid of their own. */
#define POUR_W 18
#define POUR_H 14
static uint8_t pour_cells[POUR_W * POUR_H];
static sand_t  pour;

/* An open-topped stone basin, filled with water and settled level. */
static void build_full_basin(void)
{
    sand_init(&pour, pour_cells, POUR_W, POUR_H, 9u);

    for (int y = POUR_H - 6; y < POUR_H; y++) {
        sand_set(&pour, 5,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&pour, 12, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int x = 5; x < 13; x++) {
        sand_set(&pour, x, POUR_H - 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int y = POUR_H - 4; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            sand_set(&pour, x, y, CELL_MAKE(MAT_WATER, 8));
        }
    }
    for (int i = 0; i < 200; i++) {
        sand_step(&pour, 0, 1000, 0);
    }
}

static int material_in_basin(material_id_t m)
{
    int n = 0;
    for (int y = POUR_H - 6; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            if (CELL_MATERIAL(sand_at(&pour, x, y)) == m) {
                n++;
            }
        }
    }
    return n;
}

/* How much liquid is INSIDE the basin.
 *
 * Region-limited on purpose: whole-grid mass is conserved by definition, so
 * measuring that and calling it "the basin emptied" tests nothing at all. The
 * water is still on screen after it pours out - it is just somewhere else. */
static long mass_in_basin(void)
{
    long total = 0;
    for (int y = POUR_H - 6; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            const cell_t c = sand_at(&pour, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

static void test_a_tipped_basin_pours_its_water_out(void)
{
    /* The reported behaviour: tilting the board poured some of the water and
     * then stopped, leaving a lump sitting in the basin like sand.
     *
     * The cause was that a liquid spread along a SCREEN row, which is only
     * "along the surface" while gravity points straight down. Tilted, water
     * could no longer level in its own frame, so it heaped against the low
     * wall instead of running over the lip. */
    build_full_basin();
    const long held = mass_in_basin();
    TEST_ASSERT_GREATER_THAN_MESSAGE(100, held,
        "the basin must actually be full to begin with");

    /* Tipped hard to the right - far past any angle of repose. */
    for (int i = 0; i < 600; i++) {
        sand_step(&pour, 1000, 300, 0);
    }

    /* Not held/10. That threshold encoded the OLD bug rather than the
     * physics: under this gravity the basin's right-hand wall (stone at
     * x=12, y=8..13, floor stone at y=13) is a FLOOR, and the corner at
     * (11,12) is a genuine pocket - the only way out is to climb to y<=7
     * and go over the wall's top corner at (12,7). The level plane through
     * that spill corner is (x-12)*1000 + (y-7)*300 > 0 for "below the
     * plane", and within the region mass_in_basin() measures (x 6..11,
     * y 8..12) only (11,11) and (11,12) satisfy it - two cells, 30 units of
     * the 144 this basin started with. No correct simulation can empty this
     * basin below that 30, so held/10 = 14 was never a reachable target; it
     * was only ever met while cross-flow treated a vertical ray as level and
     * let water climb the wall for nothing. The fix leaves 51: those 30
     * physically-trapped units plus about 1.4 cells of residual film, which
     * is the levelling rule's own dead band - a transfer moves whole mass
     * units, so up to one unit of level difference can persist at each hop
     * of a chain, and the escape route out of this corner is a four-hop
     * chain. 75 sits above the fix's 51 with room, and still below anything
     * that would call a heaped-up basin a pour. */
    const long left = mass_in_basin();
    TEST_ASSERT_LESS_THAN_MESSAGE(75, (int)left,
        "held on its side, a basin of water must pour rather than heap - 75 "
        "is set above the ~30 units this tilt physically traps in the low "
        "corner, not at the old held/10 threshold, which no correct "
        "simulation of this scene could ever meet");

    /* The property the count above is only a proxy for: what water is left
     * must sit in the LOW CORNER, not spread across the basin floor the way
     * a powder heaps. Verified on the fix: the water that remains is only at
     * x=10 and x=11, so x<=9 - more than half the basin floor's width - must
     * be completely dry. This is the stronger statement of "water has no
     * angle of repose": not just that little is left, but that what is left
     * has the SHAPE a puddle in a pocket has, not the shape a pile has. */
    for (int y = POUR_H - 6; y < POUR_H - 1; y++) {
        for (int x = 6; x <= 9; x++) {
            const cell_t c = sand_at(&pour, x, y);
            char why[128];
            snprintf(why, sizeof why,
                     "water heaped at (%d,%d), away from the low corner - "
                     "that is a powder's shape, not a liquid's", x, y);
            TEST_ASSERT_FALSE_MESSAGE(
                !CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER, why);
        }
    }
}

static void test_a_tipped_basin_keeps_its_sand(void)
{
    /* The control, and the reason the last test means anything: sand tipped
     * the same way must NOT all run out. If both emptied, the test above would
     * be measuring gravity rather than the difference between a liquid and a
     * powder. */
    sand_init(&pour, pour_cells, POUR_W, POUR_H, 9u);
    for (int y = POUR_H - 6; y < POUR_H; y++) {
        sand_set(&pour, 5,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&pour, 12, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int x = 5; x < 13; x++) {
        sand_set(&pour, x, POUR_H - 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int y = POUR_H - 4; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            sand_set(&pour, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    for (int i = 0; i < 200; i++) {
        sand_step(&pour, 0, 1000, 0);
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&pour, 1000, 300, 0);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, material_in_basin(MAT_SAND),
        "sand has friction and an angle of repose, so a tipped basin must "
        "keep some of it");
}

/* Wide enough that a puddle has somewhere to go. On a grid the pour can fill,
 * water and sand both end up "full" and the comparison measures nothing.
 *
 * WIDE_W/WIDE_H/wide_cells/wide (this file's second shared fixture, after
 * s/fixture() itself, reused from here through the boiler/conduction/
 * metal-rod tests much further down) and OIL/LAVA/GLASS/SNOW live in
 * suite_sand_common.{c,h}. */

/* CONDUCT_REACH_TEST/CAP_W/CAP_H moved to suite_sand_common.h - reused far
 * past this section, by conduction/metal-rod tests elsewhere in the
 * split. */

/* How tall a heap the same pour leaves, in cells above the floor. */
static int poured_height(material_id_t m)
{
    /* Self-contained: malloc, use, free, all within one call - the two
     * callers below each get their own fresh grid rather than sharing
     * one across both pours. */
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "poured-height comparison grid must fit in what the framebuffer "
        "leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);

    /* Measured shortly after the pour, not once everything has long since
     * settled. The mound is a TRANSIENT - water arriving faster than it can
     * flow away - and given hundreds of idle steps even a crawl levels out,
     * so a late measurement passes happily while the screen looks wrong. */
    for (int i = 0; i < 60; i++) {
        if (i < 20) {
            sand_spawn(&wide, WIDE_W / 2, 1, 2, m);
        }
        sand_step(&wide, 0, 1000, 0);
    }

    int result = 0;
    for (int y = 0; y < WIDE_H && result == 0; y++) {
        for (int x = 0; x < WIDE_W; x++) {
            if (!CELL_IS_EMPTY(sand_at(&wide, x, y))) {
                result = WIDE_H - y;
                break;
            }
        }
    }

    /* Freed BEFORE returning: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(wide_cells);
    return result;
}

static void test_water_puddles_where_sand_heaps(void)
{
    /* The reported behaviour: water poured from a finger built a mound
     * instead of spreading out.
     *
     * Two causes, and the second is the interesting one. Water had no way to
     * level in a tilted frame, and - even level - it spread only ONE cell per
     * step, which is far slower than a finger delivers. The mound was the
     * pour outrunning the flow.
     *
     * Sand is the control. If both heaped, or neither did, this would be
     * measuring the pour rather than the difference between a liquid and a
     * powder. */
    const int water = poured_height(MAT_WATER);
    const int sand  = poured_height(MAT_SAND);

    TEST_ASSERT_GREATER_THAN_MESSAGE(4, sand,
        "sand must build a real heap, or there is nothing to compare against");
    /* Measures 4 against sand's 8 - half the height, and about as flat as the
     * volume allows. Stated as a ratio rather than an absolute so the test
     * survives tuning the pour, and loose enough that it is guarding against a
     * mound rather than pinning an exact shape. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(sand / 2, water,
        "water poured from one spot must end up far flatter than the same "
        "sand - a liquid has no angle of repose and should not build a mound");
}

static void test_a_large_body_of_water_levels(void)
{
    /* Small puddles levelled while large ones froze into a staircase, because
     * a cell could only see eight columns and the terrace edges of a wide pool
     * are tens of columns apart. Nothing could see anywhere lower, so nothing
     * moved - and thousands of further steps changed nothing at all.
     *
     * The volume here is deliberately much wider than a cell can see, which is
     * the whole point: it must level by looking further than one step's worth
     * of travel. */
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "large-body-of-water levelling grid must fit in what the "
        "framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);

    for (int i = 0; i < 90; i++) {
        sand_spawn(&wide, WIDE_W / 2, 1, 3, MAT_WATER);
        sand_step(&wide, 0, 1000, 0);
    }
    const long volume = mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER);
    for (int i = 0; i < 600; i++) {
        sand_step(&wide, 0, 1000, 0);
    }
    const long volume_after = mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER);

    /* Level means every column is the same depth, give or take the one row a
     * remainder has to sit somewhere. */
    /* Depth measured as AMOUNT per column, which is far finer than counting
     * cells: a column holding two full cells and a third of another is 35
     * units, not "two or three". Levelness can be asserted to a fraction of a
     * cell rather than rounded to whole ones. */
    int shallowest = 1 << 30;
    int deepest    = 0;
    for (int x = 0; x < WIDE_W; x++) {
        int depth = 0;
        for (int y = 0; y < WIDE_H; y++) {
            const cell_t c = sand_at(&wide, x, y);
            if (!CELL_IS_EMPTY(c)) {
                depth += CELL_VARIANT(c);
            }
        }
        if (depth < shallowest) {
            shallowest = depth;
        }
        if (depth > deepest) {
            deepest = depth;
        }
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of wide_cells (via `wide`) are done by this
     * point. */
    free(wide_cells);

    TEST_ASSERT_EQUAL_INT_MESSAGE(volume, volume_after,
        "and none of it may be lost on the way");
    TEST_ASSERT_GREATER_THAN_MESSAGE(2 * MASS_MAX, deepest,
        "there must actually be a pool here to level");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(MASS_MAX / 2, deepest - shallowest,
        "a settled pool must be level to within half a cell - a staircase "
        "means it stopped because it could not reach anywhere lower, not "
        "because it had finished");
}

static void test_a_settled_pool_does_not_flicker(void)
{
    /* The reported bug: water that looked finished settling kept visibly
     * flashing between shades and back - which is exactly what it looks
     * like when a large amount of mass swings from one cell to another and
     * back, since colour is read straight off fill level.
     *
     * Reproduced with gravity held slightly OFF axis, not straight down.
     * Exactly (0, 1000) never dithers at all - see
     * sand_gravity_direction_dithered() - so a test using it could never
     * catch a bug that only shows up once dithering is active, which real
     * handling almost always has: a hand is never perfectly level either.
     *
     * The cause was equalise_liquids()'s cross-flow axis being taken from
     * the DITHERED direction, which by design changes between two octants
     * almost every step once off axis - so the axis a "is this level"
     * search runs along changed out from under it constantly, and a pool
     * level along one axis can read as wildly unbalanced along the other. */
    const int gx = 60, gy = 1000;

    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "settled-pool flicker grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    for (int i = 0; i < 90; i++) {
        sand_spawn(&wide, WIDE_W / 2, 1, 3, MAT_WATER);
        sand_step(&wide, gx, gy, 0);
    }
    for (int i = 0; i < 600; i++) {
        sand_step(&wide, gx, gy, 0);
    }

    /* Once settled, no single step may move much mass at all. The bug moved
     * roughly half the deepest column's worth in one step, then its
     * opposite the step after; this bounds it far below that, loose enough
     * to tolerate an unevenly-divisible remainder still finding its exact
     * rest spot at the surface. */
    uint8_t prev[WIDE_W * WIDE_H];
    memcpy(prev, wide_cells, sizeof(prev));

    long worst_churn = 0;
    for (int i = 0; i < 200; i++) {
        sand_step(&wide, gx, gy, 0);

        long churn = 0;
        for (int c = 0; c < WIDE_W * WIDE_H; c++) {
            const int a = CELL_IS_EMPTY(prev[c])       ? 0 : CELL_VARIANT(prev[c]);
            const int b = CELL_IS_EMPTY(wide_cells[c]) ? 0 : CELL_VARIANT(wide_cells[c]);
            churn += (a > b) ? (a - b) : (b - a);
        }
        if (churn > worst_churn) {
            worst_churn = churn;
        }
        memcpy(prev, wide_cells, sizeof(prev));
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of wide_cells are done by this point. */
    free(wide_cells);

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(4 * MASS_MAX, worst_churn,
        "a settled pool must not swing large amounts of mass around once "
        "level - the reported symptom was water that looked settled "
        "visibly changing colour and resettling, over and over");
}

/* Settles a w x h pool of water under (gx, gy) for `steps` steps, then
 * reports how much its surface tilts: the difference between the total
 * water mass held in the leftmost w/8 columns and the rightmost w/8
 * columns, in units of 1/1024 of a cell of depth per cell of x.
 *
 * Grid and block array are malloc()'d and freed here rather than static, so
 * this can be called at several different sizes - see the big-grid tests
 * above (e.g. test_the_four_liquid_scene_keeps_reacting_after_settling) for
 * the same shape. */
static int settled_surface_slope_q10(int w, int h, int gx, int gy, int steps)
{
    uint8_t *cells  = malloc((size_t)w * (size_t)h);
    uint8_t *blocks = malloc((size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    /* Free whatever succeeded BEFORE asserting, not after - see 565f72e.
     * TEST_ASSERT_NOT_NULL(blocks) alone would longjmp straight past both
     * frees and leak `cells` for the rest of that boot if the second
     * allocation ever came back NULL, and this helper is called eight
     * times in one test, so it would leak eight times over. */
    if (cells == NULL || blocks == NULL) {
        free(cells);
        free(blocks);
        TEST_FAIL_MESSAGE("need a grid and a block map to settle a pool "
                           "and measure its slope, and at least one of "
                           "the two failed to allocate");
    }

    sand_t s;
    sand_init(&s, cells, w, h, 41u);
    sand_enable_sleeping(&s, blocks);

    /* The bottom h/3 rows, full width, at MASS_MAX, and then ONE more row
     * above that at mass 7. The part-full row matters: a volume that
     * happens to divide exactly into full rows has no partially-filled cell
     * anywhere, and a partial cell is the only thing cross-flow can move -
     * such a pool sits dead still at any tilt, on HEAD as well as the fix,
     * because there is nothing for either ray to transfer. A real pool
     * always has a ragged surface, and this is what gives the fixture one. */
    const int full_rows = h / 3;
    for (int y = h - full_rows; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int x = 0; x < w; x++) {
        sand_set(&s, x, h - full_rows - 1, CELL_MAKE(MAT_WATER, 7));
    }

    for (int i = 0; i < steps; i++) {
        sand_step(&s, gx, gy, 0);
    }

    const int q = w / 8;
    long lo = 0, hi = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < q; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                lo += CELL_VARIANT(c);
            }
        }
        for (int x = w - q; x < w; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                hi += CELL_VARIANT(c);
            }
        }
    }

    free(cells);
    free(blocks);

    /* An end-to-end difference of two eighths, deliberately, rather than a
     * regression fitted over every column: it is blind ON PURPOSE to the
     * surface's own +/-1 cell texture and to a few columns running dry at
     * the shallow end, both of which would swamp a per-column measure with
     * noise that has nothing to do with the angle. Multiplied out in
     * `long` - (hi-lo) runs to about 40000 on the biggest fixture here, and
     * *1024 does not overflow a long - then cast down to the int this
     * returns. */
    return (int)(((long)(hi - lo) * 1024) / ((long)q * (w - q) * MASS_MAX));
}

static void test_a_pool_settles_at_the_angle_it_is_tilted_to(void)
{
    /* The reported bug: a settled liquid surface only ever came out
     * perfectly flat or snapped to the 45-degree diagonal, never anywhere
     * in between - because equalise_liquids() levelled along a single ray
     * taken perpendicular to the NEAREST of the eight gravity directions,
     * so a settled surface could only ever be perpendicular to one of
     * those eight, and quantised to 0/45/90 degrees. It is worst at the
     * app's LOW quality setting, whose grid is 368/6 x 448/6 = 61x74 -
     * which is why 61x74 is one of the fixtures below by name, not a round
     * number picked for convenience.
     *
     * Two tilts are needed, not one: 20 and 26 degrees sit either side of
     * the 22.5-degree boundary between two of the eight octants, and the
     * old behaviour failed them in OPPOSITE directions - flat below the
     * boundary (the nearest direction was still the axis) and snapped to
     * the diagonal above it (the nearest direction became the diagonal).
     * A fix that only moved the boundary rather than removing it could pass
     * one of these and fail the other.
     *
     * Measured on the host, all four fixtures, 2500 steps, this q10 slope
     * against the true one:
     *   on the fix:  20deg 0.262-0.369 (true 0.364); 26deg 0.394-0.479 (true 0.488)
     *   before it:   20deg 0.012-0.032 (dead flat);  26deg 0.645-0.941 (snapped
     *                                                 to the diagonal)
     * so the band asserted below, 0.15 to 0.58 (154 to 594 in q10), is green
     * on the fix at every fixture with at least 0.10 of margin, and red
     * before it at every fixture in both directions. */
    static const struct { int w, h; } fixtures[] = {
        { 32, 20 }, { 40, 28 }, { 61, 74 }, { 92, 56 },
    };
    const int steps = 2500;

    for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        const int w = fixtures[i].w, h = fixtures[i].h;
        char why[256];

        /* 20 degrees: true slope 0.364, 373 in q10. */
        const int slope20 = settled_surface_slope_q10(w, h, 342, 939, steps);
        snprintf(why, sizeof why,
                 "%dx%d fixture at 20 degrees: measured slope %d (q10), "
                 "expected roughly 373 (true slope 0.364) - flat near 0 "
                 "means the surface snapped to the axis, the old bug below "
                 "the octant boundary", w, h, slope20);
        TEST_ASSERT_GREATER_THAN_MESSAGE(154, slope20, why);
        TEST_ASSERT_LESS_THAN_MESSAGE(594, slope20, why);

        /* 26 degrees: true slope 0.488, 500 in q10. */
        const int slope26 = settled_surface_slope_q10(w, h, 422, 906, steps);
        snprintf(why, sizeof why,
                 "%dx%d fixture at 26 degrees: measured slope %d (q10), "
                 "expected roughly 500 (true slope 0.488) - close to 1024 "
                 "means the surface snapped to the diagonal, the old bug "
                 "above the octant boundary", w, h, slope26);
        TEST_ASSERT_GREATER_THAN_MESSAGE(154, slope26, why);
        TEST_ASSERT_LESS_THAN_MESSAGE(594, slope26, why);
    }
}

#define SPLASH_W 3
#define SPLASH_H 10
static uint8_t splash_cells[SPLASH_W * SPLASH_H];

static void test_water_falling_onto_water_also_queues_a_small_displacement(void)
{
    /* A single dropped grain, falling through open space, lands on an
     * existing puddle's surface - move_liquid_grain()'s own straight-down
     * give_mass() call (sand_liquid.c) fires sand_displace() on that
     * landing, gated to a genuine fall (open space one step above) rather
     * than ordinary internal levelling. */
    enum { CX = 1, POOL_TOP = 6, SURFACE_MASS = MASS_MAX / 2 };
    /* Sized well past exact_disc_count(SAND_SPLASH_RADIUS_WATER) (sand.c),
     * NOT SPLASH_W * SPLASH_H - the disc this call seeds is a property of
     * the RADIUS, not of this tiny 3-wide grid, and a buffer only as big
     * as the grid's own cell count starved queue_outward_impulse()'s own
     * thinning: `keep = min(disc_count, room)` came out buffer-limited,
     * and thinning spread those few kept slots evenly across the WHOLE
     * untrimmed disc - most of which falls off a grid this narrow - so
     * every kept slot could land out of bounds and nothing ever queued,
     * even though the trigger itself (chance, radius) fired correctly.
     * 4096, not 1024 any more - RADIUS_WATER doubling to 20 put
     * exact_disc_count() at 1257, past the old 1024, which would have
     * silently reintroduced exactly this starvation. 4096 comfortably
     * clears today's radius with headroom for tuning it further. */
    /* HEAP, not the stack: impulse_t is 6 bytes, so 4096 of them is 24 KB
     * against this device's 3,584-byte main task stack
     * (CONFIG_ESP_MAIN_TASK_STACK_SIZE). On the host, with megabytes of
     * stack, the array was invisible; on the board it panicked with a
     * Stack protection fault and reboot-looped the whole self-test before
     * it could reach any frame-budget test. That is the same bug, in this
     * same file, that the sixth tuning attempt fixed once already - see
     * docs/sand/Performance-Tuning-Attempts.md - and the same fix: every
     * other fixture here mallocs, and so must this one. */
    impulse_t *drop_impulse_buf = malloc(4096 * sizeof *drop_impulse_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(drop_impulse_buf,
        "the splash impulse queue must fit in what the framebuffer leaves");
    sand_init(&fx.splash_sim, splash_cells, SPLASH_W, SPLASH_H, 1u);
    sand_enable_impulses(&fx.splash_sim, drop_impulse_buf, 4096);

    for (int y = POOL_TOP + 1; y < SPLASH_H; y++) {
        for (int x = 0; x < SPLASH_W; x++) {
            sand_set(&fx.splash_sim, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    /* The surface row has ROOM - a fully-packed target has nothing for the
     * straight-down transfer this trigger reads to give it. */
    sand_set(&fx.splash_sim, CX, POOL_TOP, CELL_MAKE(MAT_WATER, SURFACE_MASS));
    sand_set(&fx.splash_sim, CX, 0, CELL_MAKE(MAT_WATER, MASS_MAX));

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fx.splash_sim.impulse_count,
        "setup: nothing should be queued before the drop has even fallen");

    /* Checked EVERY step, not just after all 15 - the queued impulse is a
     * single directed grain (sand_impulse(), not a sand_displace() spray),
     * and step_impulses() can resolve a single grain's flight within just
     * a step or two of the landing that queued it, well before the loop
     * ends. */
    bool queued = false;
    for (int i = 0; i < 15 && !queued; i++) {
        sand_step(&fx.splash_sim, 0, 1000, 0);
        queued = fx.splash_sim.impulse_count > 0;
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs, and this file has already had one such
     * leak starve every later malloc()-based test on this no-PSRAM heap. */
    free(drop_impulse_buf);

    TEST_ASSERT_TRUE_MESSAGE(queued,
        "a drop that fell through open space and landed on an existing "
        "puddle's surface must queue a small directed impulse");
}

#define CRATER_W 11
#define CRATER_H 12
static uint8_t crater_cells[CRATER_W * CRATER_H];

static void test_a_water_splash_actually_opens_a_gap(void)
{
    /* "impulse_count > 0" (an earlier version of this test) only proves
     * something got QUEUED - it says nothing about whether the queued
     * swap ever produced a visible change. can_impulse_enter()
     * (step_impulses(), sand.c) lets a flying grain swap into ANY
     * non-static occupant, not only an empty one, so a splash that only
     * ever aims at more water "succeeds" mechanically while changing
     * nothing on screen - reported on device as "still just merging, not
     * a repel". This test pins the actual, visible claim instead: a
     * genuine, MULTI-CELL crater opens around the point of impact, not
     * one lonely cell - see splash_displace()'s own comment (sand_liquid.c)
     * for why one cell was all an earlier version of the mechanic itself
     * could ever produce (every push shared one origin and only one could
     * ever win), which this scene is built wide enough to actually catch.
     *
     * A narrow (3-wide) pool was tried first and could not exercise this
     * at all: the redesigned mechanic pushes a NEIGHBOUR further outward,
     * which needs two clear cells past that neighbour, and a 3-wide grid
     * has nowhere with that much room next to a contact point. This pool
     * sits 3 columns wide (POOL_L..POOL_R) in an 11-wide grid so BOTH
     * flanks, and both lower diagonals, have real clearance beyond them -
     * four independent directions a crater could plausibly open in, from
     * four different source cells that cannot collide with each other the
     * way a single shared origin did. */
    enum { CX = 6, POOL_TOP = 6, SURFACE_MASS = MASS_MAX / 2,
           POOL_L = 5, POOL_R = 7 };
    /* 4096, not CRATER_W * CRATER_H - see drop_impulse_buf's own comment
     * in the test above for why a buffer only as big as the grid's own
     * cell count starves queue_outward_impulse()'s thinning and can queue
     * nothing at all, even on a correctly-firing trigger, and for why 1024
     * itself stopped being enough once RADIUS_WATER doubled to 20. */
    /* HEAP, not the stack: impulse_t is 6 bytes, so 4096 of them is 24 KB
     * against this device's 3,584-byte main task stack
     * (CONFIG_ESP_MAIN_TASK_STACK_SIZE). On the host, with megabytes of
     * stack, the array was invisible; on the board it panicked with a
     * Stack protection fault and reboot-looped the whole self-test before
     * it could reach any frame-budget test. That is the same bug, in this
     * same file, that the sixth tuning attempt fixed once already - see
     * docs/sand/Performance-Tuning-Attempts.md - and the same fix: every
     * other fixture here mallocs, and so must this one. */
    impulse_t *buf = malloc(4096 * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "the crater impulse queue must fit in what the framebuffer leaves");
    sand_init(&fx.crater_sim, crater_cells, CRATER_W, CRATER_H, 1u);
    sand_enable_impulses(&fx.crater_sim, buf, 4096);

    for (int y = POOL_TOP; y < CRATER_H; y++) {
        for (int x = POOL_L; x <= POOL_R; x++) {
            if (x == CX && y == POOL_TOP) {
                continue;   /* the surface cell gets SURFACE_MASS below */
            }
            sand_set(&fx.crater_sim, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    /* The surface row has ROOM - a fully-packed target has nothing for
     * the straight-down transfer this trigger reads to give it. */
    sand_set(&fx.crater_sim, CX, POOL_TOP, CELL_MAKE(MAT_WATER, SURFACE_MASS));
    sand_set(&fx.crater_sim, CX, 0, CELL_MAKE(MAT_WATER, MASS_MAX));

    const int nbr_x[4] = { POOL_L, POOL_R, POOL_L, POOL_R };
    const int nbr_y[4] = { POOL_TOP, POOL_TOP, POOL_TOP + 1, POOL_TOP + 1 };

    for (int i = 0; i < 4; i++) {
        char why[128];
        snprintf(why, sizeof why,
                 "setup: neighbour %d (%d, %d) must start as water for "
                 "the splash to have anything to push", i, nbr_x[i], nbr_y[i]);
        TEST_ASSERT_FALSE_MESSAGE(
            CELL_IS_EMPTY(sand_at(&fx.crater_sim, nbr_x[i], nbr_y[i])), why);
    }

    int cleared = 0;
    for (int i = 0; i < 15; i++) {
        sand_step(&fx.crater_sim, 0, 1000, 0);
    }
    for (int i = 0; i < 4; i++) {
        if (CELL_IS_EMPTY(sand_at(&fx.crater_sim, nbr_x[i], nbr_y[i]))) {
            cleared++;
        }
    }

    /* Freed before the assertion, for the same reason drop_impulse_buf is. */
    free(buf);

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, cleared,
        "a water splash landing with real room on multiple sides must "
        "clear more than one cell around the point of impact - a single "
        "cleared cell means the crater is still only ever one grain wide, "
        "whatever the radius or chance settings claim to allow");
}

#define CASCADE_TEST_W 1
#define CASCADE_TEST_H 16
static uint8_t cascade_test_cells[CASCADE_TEST_W * CASCADE_TEST_H];

static void test_a_cascading_impulse_moves_more_than_one_cell(void)
{
    /* An ordinary impulse only ever moves the ONE grain it was queued
     * for. See step_impulses()'s own CASCADE comment (sand.c) for the
     * fix: a successful WATER or ACID move relays its push into whatever
     * of the SAME material sits one step BEHIND where it started (so
     * that cell can advance into the gap this one just left), queued for
     * the NEXT step's pass rather than this one's - the speed halves
     * each hop (SAND_CASCADE_SPEED_DIVISOR), so the wave loses energy and
     * dies out on its own, but a chain of connected liquid should still
     * move as a group for a few hops, not as one grain stepping aside
     * while the rest of the chain stays exactly put.
     *
     * A VERTICAL column, pushed UP (against gravity), not a horizontal
     * row pushed sideways - tried first, and confounded by something
     * unrelated to the cascade entirely: cross-flow (equalise_liquids(),
     * sand_liquid.c) runs every step PERPENDICULAR to gravity, and with
     * gravity straight down that perpendicular axis is horizontal - the
     * exact axis the impulse was also pushing along. The row filled
     * itself completely within three steps through perfectly ordinary
     * levelling, with or without any impulse, so "is any cell in the row
     * empty" could never isolate the cascade's own contribution. Gravity
     * can never move water UP on its own, so a column pushed upward has
     * no such confound: any water found above where the column originally
     * ended must have arrived via the impulse system, and specifically
     * MORE THAN ONE cell up there at once (checked at a single instant,
     * after the loop below) is only possible if more than one entry was
     * in flight together - a lone, non-cascading impulse can only ever
     * occupy one cell at a time, however far it travels alone over
     * however many steps.
     *
     * EXACTLY ONE CELL WIDE, not merely narrow - a 3-wide version of
     * this same scene was tried first and had a THIRD confound: the
     * "down the slope" diagonal slides in move_liquid_grain()
     * (sand_liquid.c) let the column leak sideways into the empty
     * flanking columns even though it was already fully settled
     * vertically, corrupting the column's own mass distribution over
     * time for reasons that had nothing to do with any impulse. A grid
     * exactly as wide as the column leaves no adjacent column to leak
     * into at all - both sides read as solid via sand_at()'s own
     * off-grid convention, the same guarantee a real wall would give. */
    enum { COL = 0, TOP = 8, COL_LEN = 8, DIR_UP = 4 };
    impulse_t buf[64];
    sand_init(&fx.cascade_test_sim, cascade_test_cells, CASCADE_TEST_W,
             CASCADE_TEST_H, 1u);
    sand_enable_impulses(&fx.cascade_test_sim, buf, 64);

    for (int y = TOP; y < TOP + COL_LEN; y++) {
        sand_set(&fx.cascade_test_sim, COL, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    sand_impulse(&fx.cascade_test_sim, COL, TOP, DIR_UP, 255);

    /* NOT "is any cell below TOP empty", any more - that check's own
     * premise ("nothing else in this scene ever touches those cells") was
     * simply wrong, caught by SAND_SPLASH_SPEED_DECAY_SHIFT's arrival
     * (sand.h): the mover's own vacancy at TOP is exactly the open cell
     * ordinary gravity needs to pull the column's next grain DOWN into,
     * every step, before step_impulses() even runs - so the front of the
     * chain spends most of its life oscillating between TOP and TOP+1
     * (impulse pushes up, gravity pulls back down) rather than cleanly
     * escaping upward, and every swap behind that oscillation trades
     * water for water rather than ever leaving a cell empty long enough
     * for this loop to catch it. Confirmed by direct trace, not merely
     * reasoned about: SAND_CASCADE_MIN_SPEED's own comment (sand.h)
     * covers the gate half of that discovery.
     *
     * SIMULTANEOUS FLIGHT, NOT A GAP, is what actually proves "more than
     * one cell moved" without depending on gravity ever losing that
     * race: a single, non-cascading impulse can only ever be ONE entry in
     * s->impulse_buf at a time - see sand_impulse()'s own comment. Seeing
     * impulse_count climb above 1 during this run is only possible if a
     * successful move actually triggered CASCADE's relay (step_impulses(),
     * sand.c) and queued a second, independent entry for the SAME
     * material one step behind the first - exactly the claim this test
     * exists to pin down, observed directly rather than inferred from a
     * side effect gravity can erase. */
    bool cascade_confirmed = false;
    for (int i = 0; i < 10 && !cascade_confirmed; i++) {
        sand_step(&fx.cascade_test_sim, 0, 1000, 0);
        if (fx.cascade_test_sim.impulse_count > 1) {
            cascade_confirmed = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(cascade_confirmed,
        "a strong impulse into a connected water column must relay "
        "through more than one cell of it - a single, non-cascading "
        "impulse can only ever be one entry in flight at a time, so "
        "impulse_count climbing above 1 proves the cascade queued a "
        "second, independent entry rather than the lone grain simply "
        "moving alone");
}

/* --- pouring water must not stir the dirt bed underneath it --------------- */

#define STIR_W 14
#define STIR_H 30
static uint8_t stir_cells[STIR_W * STIR_H];

/* THE PINPOINTING TEST THE MAINTAINER ASKED FOR. Reported: pouring water
 * over dirt makes the submerged dirt "move a lot". Measured (see can_
 * impulse_enter()'s own comment, sand.c, for the full numbers) rather than
 * argued: the obvious suspect, splash_displace()'s directed ring kick
 * (sand_liquid.c), barely ever touched a dirt cell - 60 times out of
 * roughly 7,500 firings on this same shape of scene. The actual cause was
 * can_impulse_enter() itself, which used to let a flying water grain swap
 * into ANY non-static occupant on the way to landing, dirt included -
 * every splash grain the pour kicked up tunnelled straight through
 * whatever dirt sat in its path. Built as close to the measured scene as
 * this file's own helpers allow: a settled dirt bed, a settled pool of
 * water resting on it, and a continuous stream poured on top of that pool
 * for hundreds of steps, so plenty of drops land hard on an already-full
 * surface and splash.
 *
 * TRACKS POSITION, NOT BYTES - "assert on dirt cells moving, not on
 * impulse internals" is the brief this test was written against. A dirt
 * cell's own MOISTURE nibble legitimately changes near standing water
 * (wicking - unrelated to this bug, and not something this test should
 * ever fail on), so what is pinned here is whether MAT_DIRT itself ever
 * leaves one of its starting cells - the thing the report actually
 * complained about - not whether any byte of it changed in place. */
static void test_pouring_water_over_a_dirt_bed_never_moves_a_dirt_cell(void)
{
    enum {
        FLOOR      = STIR_H - 1,
        DIRT_TOP   = FLOOR - 12,     /* a 12-row dirt bed */
        POOL_TOP   = DIRT_TOP - 5,   /* 5 settled rows of water on it */
        STREAM_X   = STIR_W / 2,
        POUR_STEPS = 600,
    };

    /* HEAP, not the stack - see drop_impulse_buf's own comment above
     * (test_water_falling_onto_water_also_queues_a_small_displacement)
     * for the stack-protection panic a buffer this size caused on-device
     * the one other time this file put one on the stack instead. */
    impulse_t *buf = malloc(4096 * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "the pour impulse queue must fit in what the framebuffer leaves");
    sand_init(&fx.stir_sim, stir_cells, STIR_W, STIR_H, 0xC0FFEEu);
    sand_enable_impulses(&fx.stir_sim, buf, 4096);

    for (int y = 0; y < STIR_H; y++) {
        sand_set(&fx.stir_sim, 0, y, STONE);
        sand_set(&fx.stir_sim, STIR_W - 1, y, STONE);
    }
    for (int x = 0; x < STIR_W; x++) {
        sand_set(&fx.stir_sim, x, FLOOR, STONE);
    }
    for (int y = DIRT_TOP; y < FLOOR; y++) {
        for (int x = 1; x < STIR_W - 1; x++) {
            sand_set(&fx.stir_sim, x, y, CELL_MAKE(MAT_DIRT, 0));
        }
    }
    for (int y = POOL_TOP; y < DIRT_TOP; y++) {
        for (int x = 1; x < STIR_W - 1; x++) {
            sand_set(&fx.stir_sim, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    bool was_dirt[STIR_W * STIR_H];
    for (int i = 0; i < STIR_W * STIR_H; i++) {
        was_dirt[i] = CELL_MATERIAL(fx.stir_sim.cells[i]) == MAT_DIRT;
    }

    for (int i = 0; i < POUR_STEPS; i++) {
        sand_spawn(&fx.stir_sim, STREAM_X, 1, 2, MAT_WATER);   /* 5-wide stream */
        sand_step(&fx.stir_sim, 0, 1000, 0);
    }

    free(buf);

    int left_its_cell = 0, arrived_elsewhere = 0;
    for (int i = 0; i < STIR_W * STIR_H; i++) {
        const bool is_dirt_now = CELL_MATERIAL(fx.stir_sim.cells[i]) == MAT_DIRT;
        if (was_dirt[i] && !is_dirt_now) {
            left_its_cell++;
        }
        if (!was_dirt[i] && is_dirt_now) {
            arrived_elsewhere++;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, left_its_cell,
        "pouring water onto the pool over a settled dirt bed must never "
        "move a submerged dirt cell out of its own position - this same "
        "scene moved hundreds before can_impulse_enter() learned to refuse "
        "a liquid mover's swap into a non-liquid target");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, arrived_elsewhere,
        "and MAT_DIRT must not show up outside the bed's own footprint "
        "either - the two counts are the same swap seen from its two ends");
}

/* THE DIRECT VERSION OF THE SAME CLAIM, isolated from the pinpointing
 * scene's own moving parts (splash chance, radius decay, a continuous
 * pour) down to one water grain and one dirt grain: queue a single
 * impulse aimed straight at a dirt cell and watch it fail to arrive.
 *
 * SIDEWAYS, not straight down - traced two confounds out of this test
 * before landing here, neither one this file's fault. A single dirt cell
 * with open, empty ground either side let ordinary gravity's own diagonal
 * slide walk the water around it before the impulse ever ran; filling the
 * whole row with dirt fixed that but let sand_step_liquids()'s cross-flow
 * spread the one water cell sideways across the open row above it instead
 * (see splash_displace()'s own comment, sand_liquid.c, for the identical
 * "open ground either side" shape of that older, related bug). Neither
 * confound is why a THIRD attempt, straight down with a full dirt row AND
 * a floor pinning the water in place, still passed on UNFIXED code: dirt
 * is DENSER than water (62 against 30), so the instant the impulse's own
 * bugged swap puts water below dirt, ORDINARY gravity - can_enter(),
 * sand_priv.h, ALLOWS a denser powder to sink through a lighter liquid -
 * immediately sinks the dirt back down through the water on the very next
 * step, before this test's own loop ever gets to look. The two mechanisms
 * fight to the same visible resting place, byte-for-byte, and a test that
 * only checks the FINAL position cannot tell "the impulse rule already
 * refused this" from "ordinary gravity kept undoing what the impulse rule
 * allowed" - confirmed by a full per-step board dump, not assumed.
 * SIDEWAYS has no such shadow: ordinary powder movement only ever
 * considers straight-down and the two gravity-relative diagonals as
 * candidates (step_one_grain(), sand.c) - a purely horizontal neighbour is
 * never one of them, density or no density - so a floor under both cells
 * (blocking the vertical candidates outright) leaves the impulse this test
 * queues as the ONLY mechanism that could ever move either cell at all. */
static void test_a_flying_water_grain_does_not_swap_into_dirt_in_its_path(void)
{
    fixture();
    impulse_t buf[8];
    sand_enable_impulses(&s, buf, 8);

    enum { ROW = 4, WX = 3, DX = 4, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, WX, ROW, WATER);
    sand_set(&s, DX, ROW, CELL_MAKE(MAT_DIRT, 0));

    sand_impulse(&s, WX, ROW, DIR_RIGHT, 255);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_DIRT,
        CELL_MATERIAL(sand_at(&s, DX, ROW)),
        "a flying water grain queued straight at a dirt cell beside it "
        "must not swap into that cell - can_impulse_enter() (sand.c) now "
        "refuses a KIND_LIQUID mover's swap into anything but another "
        "KIND_LIQUID target, pinned here directly rather than through a "
        "whole pour scene");
}

/* THE OTHER HALF OF THE SAME RULE: the new gate is about foreign, NON-
 * liquid occupants specifically, not a blanket ban on a flying liquid
 * displacing anything at all - a flying grain of water must still be able
 * to shoulder its way into another liquid, exactly as it always could, or
 * splashing into oil, acid or lava (see splash_displace()'s own comment,
 * sand_liquid.c) would have quietly stopped working alongside the fix.
 * PINNED HORIZONTALLY, with a floor beneath both cells, for the same
 * gravity-confound reason the test above goes straight down - here the
 * push direction ITSELF is sideways, so a floor is what keeps ordinary
 * gravity from pulling either cell out of the row before the impulse gets
 * to try its own move.
 *
 * A WALL ONE CELL PAST OX, new alongside SAND_IMPULSE_CELLS_PER_STEP_
 * DIVISOR (sand.h) - a full-speed push now covers several cells in one
 * step (that constant's own comment), so without something to stop it
 * there water swaps into the oil cell and immediately keeps going through
 * the open air beyond, and this test's own poll (once per whole step, not
 * per cell) never catches it AT rest on OX. The wall caps the push at
 * exactly the one cell this test cares about - whether the swap into
 * another liquid happens at all - without touching how far an unobstructed
 * throw travels anywhere else. */
static void test_a_flying_water_grain_still_displaces_another_liquid(void)
{
    fixture();
    impulse_t buf[8];
    sand_enable_impulses(&s, buf, 8);

    enum { ROW = 4, WX = 3, OX = 4, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, WX, ROW, WATER);
    sand_set(&s, OX, ROW, OIL);
    sand_set(&s, OX + 1, ROW, STONE);

    sand_impulse(&s, WX, ROW, DIR_RIGHT, 255);

    bool water_reached_target = false;
    for (int i = 0; i < H && !water_reached_target; i++) {
        sand_step(&s, 0, 1000, 0);
        water_reached_target =
            CELL_MATERIAL(sand_at(&s, OX, ROW)) == MAT_WATER;
    }

    TEST_ASSERT_TRUE_MESSAGE(water_reached_target,
        "a flying water grain must still be able to swap into another "
        "liquid's cell - the can_impulse_enter() fix (sand.c) is a rule "
        "about foreign, NON-liquid occupants, not a blanket ban on "
        "displacing whatever the mover finds in its path");
}

#define LIQ_CASCADE_W 1
#define LIQ_CASCADE_H 16
static uint8_t liq_cascade_cells[LIQ_CASCADE_W * LIQ_CASCADE_H];

/* SAME SCENE SHAPE AND SAME impulse_count > 1 SIGNAL AS
 * test_a_cascading_impulse_moves_more_than_one_cell ABOVE - see that
 * test's own comment for why a vertical column pushed UP (against
 * gravity, in a grid exactly as wide as the column) is what isolates a
 * genuine multi-hop CASCADE relay from ordinary gravity or cross-flow.
 * Run again here specifically against the new can_impulse_enter()
 * liquid-vs-non-liquid gate: water relaying into water is the SAME kind
 * on both sides of every hop in this chain, so the gate - which only
 * narrows a KIND_LIQUID mover's swap into a target that is NOT liquid -
 * must never fire once in this whole run. */
static void test_a_water_into_water_cascade_is_untouched_by_the_liquid_fix(void)
{
    enum { COL = 0, TOP = 8, COL_LEN = 8, DIR_UP = 4 };
    impulse_t buf[64];
    sand_init(&fx.liq_cascade_sim, liq_cascade_cells, LIQ_CASCADE_W,
             LIQ_CASCADE_H, 1u);
    sand_enable_impulses(&fx.liq_cascade_sim, buf, 64);

    for (int y = TOP; y < TOP + COL_LEN; y++) {
        sand_set(&fx.liq_cascade_sim, COL, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    sand_impulse(&fx.liq_cascade_sim, COL, TOP, DIR_UP, 255);

    bool cascade_confirmed = false;
    for (int i = 0; i < 10 && !cascade_confirmed; i++) {
        sand_step(&fx.liq_cascade_sim, 0, 1000, 0);
        if (fx.liq_cascade_sim.impulse_count > 1) {
            cascade_confirmed = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(cascade_confirmed,
        "a same-material (water-into-water) cascade must still relay more "
        "than one entry after can_impulse_enter()'s new liquid-vs-non-"
        "liquid gate - the gate only narrows what happens when the target "
        "is NOT liquid, so an all-water chain must come through completely "
        "untouched");
}

void run_sand_materials_suite(void)
{
    RUN_TEST(test_a_cell_carries_both_material_and_variant);
    RUN_TEST(test_stone_never_moves);
    RUN_TEST(test_nothing_displaces_stone);
    RUN_TEST(test_sand_sinks_through_water);
    RUN_TEST(test_water_does_not_sink_through_sand);
    RUN_TEST(test_displacement_conserves_both_materials);
    RUN_TEST(test_water_finds_its_own_level);
    RUN_TEST(test_a_powder_still_holds_a_heap);
    RUN_TEST(test_water_can_be_held_by_a_stone_basin);
    RUN_TEST(test_a_drop_resting_on_a_pool_comes_to_rest);
    RUN_TEST(test_water_under_weight_still_spreads);
    RUN_TEST(test_water_poured_into_a_basin_reaches_both_ends);
    RUN_TEST(test_a_tipped_basin_pours_its_water_out);
    RUN_TEST(test_a_tipped_basin_keeps_its_sand);
    RUN_TEST(test_water_puddles_where_sand_heaps);
    RUN_TEST(test_a_large_body_of_water_levels);
    RUN_TEST(test_a_settled_pool_does_not_flicker);
    RUN_TEST(test_a_pool_settles_at_the_angle_it_is_tilted_to);
    RUN_TEST(test_water_falling_onto_water_also_queues_a_small_displacement);
    RUN_TEST(test_a_water_splash_actually_opens_a_gap);
    RUN_TEST(test_a_cascading_impulse_moves_more_than_one_cell);
    RUN_TEST(test_pouring_water_over_a_dirt_bed_never_moves_a_dirt_cell);
    RUN_TEST(test_a_flying_water_grain_does_not_swap_into_dirt_in_its_path);
    RUN_TEST(test_a_flying_water_grain_still_displaces_another_liquid);
    RUN_TEST(test_a_water_into_water_cascade_is_untouched_by_the_liquid_fix);
}

SUITE_REGISTER(run_sand_materials_suite);
