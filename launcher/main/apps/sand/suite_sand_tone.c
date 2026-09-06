/*=============================================================================
 * Portable suite: the falling-sand automaton - material appearance -
 * painting, stone/glass speckle, and cullet's colour cycle.
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

/* The brush and the setter must agree about what a cell implies.
 *
 * They did not, and the way they failed is the point. sand_set() and
 * try_spawn_one() each carried their own copy of the may_have_* latch
 * list, both with a comment noting the other one. A fifth flag was added
 * to one of them. Snow PLACED by sand_set() melted in water; snow PAINTED
 * with the brush never woke the reactions pass at all, so on a board with
 * no fire and no acid it sat in a pond forever and chilled nothing.
 *
 * Every test at the time used sand_set(), so every test passed and the
 * only way to find it was to draw snow into water by hand. This one walks
 * every material through both doors and compares what each one latched,
 * so a sixth flag cannot repeat it. */
static void test_the_brush_and_the_setter_agree_about_every_material(void)
{
    for (int m = 1; m < MAT_COUNT; m++) {
        const int cx = W / 2, cy = H / 2;

        /* The brush. */
        fixture();
        sand_clear(&s);
        sand_spawn(&s, cx, cy, 1, (material_id_t)m);
        const cell_t painted = sand_at(&s, cx, cy);
        const bool b_liquid = s.may_have_liquid;
        const bool b_gas    = s.may_have_gas;
        const bool b_burn   = s.may_have_burning;
        const bool b_diss   = s.may_have_dissolver;
        const bool b_temp   = s.may_have_temperature;

        TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(painted),
            "fixture check: the brush has to actually paint the centre "
            "cell, or this compares two empty boards");

        /* The setter, given the very cell the brush produced. */
        fixture();
        sand_clear(&s);
        sand_set(&s, cx, cy, painted);

        char why[96];
        snprintf(why, sizeof why,
                 "brush and setter disagree about %s", material_by_id((material_id_t)m)->name);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_liquid,      b_liquid, why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_gas,         b_gas,    why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_burning,     b_burn,   why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_dissolver,   b_diss,   why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_temperature, b_temp,   why);
    }

    /* GUNPOWDER cannot join the loop above - it has no material_id_t of
     * its own, only a byte range inside MAT_EXTENDED's nibble
     * (GUNPOWDER_BASE, material.h) - so it needs its own brush-vs-setter
     * comparison here, through sand_spawn_cell() rather than
     * sand_spawn(). */
    {
        const int cx = W / 2, cy = H / 2;

        fixture();
        sand_clear(&s);
        sand_spawn_cell(&s, cx, cy, 1, GUNPOWDER_CELL(0));
        const cell_t painted = sand_at(&s, cx, cy);
        const bool b_liquid = s.may_have_liquid;
        const bool b_gas    = s.may_have_gas;
        const bool b_burn   = s.may_have_burning;
        const bool b_diss   = s.may_have_dissolver;
        const bool b_temp   = s.may_have_temperature;

        TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(painted),
            "fixture check: the brush has to actually paint gunpowder "
            "into the centre cell too");

        fixture();
        sand_clear(&s);
        sand_set(&s, cx, cy, painted);

        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_liquid,      b_liquid, "gunpowder: liquid flag");
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_gas,         b_gas,    "gunpowder: gas flag");
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_burning,     b_burn,   "gunpowder: burning flag");
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_dissolver,   b_diss,   "gunpowder: dissolver flag");
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_temperature, b_temp,   "gunpowder: temperature flag");
    }
}

/* And the same thing end to end, through the door the player uses.
 *
 * The test above compares flags; this one says what the flags were for.
 * Snow painted into a pool on a board where nothing is burning has to
 * melt, and it is worth stating separately because the flag is only
 * machinery - the observable claim is that a drift does not sit in water
 * forever. */
static void test_snow_painted_into_water_melts(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    sand_spawn(&s, W / 2, 1, 2, MAT_SNOW);
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SNOW) > 0,
        "fixture check: the brush has to put some snow on the board");

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_SNOW),
        "snow PAINTED into water must melt, on a board with no fire and "
        "no acid anywhere - nothing else is running the reactions pass, "
        "so this is the whole of what wakes it");
}

/* Snow melts in liquid, not only near fire.
 *
 * Water is not a heat source in this simulation, so before `thaws` a
 * drift would ride on a pond forever - which looked wrong in the one
 * place snow is most likely to land. Nothing here has a temperature
 * except glass, so "liquid" stands in for "warm and touching you on
 * every side"; the alternative was giving water a temperature, which
 * means giving everything one, which means a second byte per cell that
 * this grid does not have. */
static void test_snow_melts_in_any_liquid(void)
{
    static const uint8_t liquids[] = { MAT_WATER, MAT_OIL, MAT_ACID };

    for (unsigned k = 0; k < sizeof liquids / sizeof liquids[0]; k++) {
        fixture();
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = H - 4; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, CELL_MAKE(liquids[k], MASS_MAX));
            }
        }
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, 0, SNOW);
        }

        for (int i = 0; i < 600; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_SNOW),
            "snow left sitting in a liquid must melt - any liquid, not "
            "water alone, since nothing here is at a temperature and a "
            "drift floating forever on oil needs more explaining than one "
            "that melts");
    }
}

/* And what it melts INTO is water, whatever melted it.
 *
 * Snow is frozen water and turns back into water; it does not become
 * more of whatever it touched. That would be an exploit rather than a
 * flourish - acid is spent as it dissolves, so snow that melted into
 * acid would be a bucket that refills itself, and snow melting into oil
 * would be a fuel printer.
 *
 * Checked by whether water ever APPEARED across the window, not whether
 * it is still there at the very end. The acid direction in particular
 * is a busy scene by 600 steps: melted water can dilute into the acid
 * pool it landed on (see step_one_dissolver_cell()'s own comment,
 * sand_reactions.c), and once that pool's own steam can actually rise
 * and condense back into water the way it always should have (a real
 * bug this file's own place_cell()-bypass fix corrected - the vapour
 * used to sit frozen in place, unable to go anywhere), the scene has
 * more genuine give-and-take in it than it used to, not less. A single
 * end-of-window snapshot can land on a step where every last drop of
 * melted water has momentarily gone back into acid, which is a real
 * possible state of a healthy simulation, not evidence that melting
 * ever produced the wrong material - that only ever needs one sighting
 * to disprove, and this window is long enough to reliably catch it if
 * it were happening. */
static void test_melting_snow_makes_water_not_more_of_the_liquid(void)
{
    static const uint8_t liquids[] = { MAT_OIL, MAT_ACID };

    for (unsigned k = 0; k < sizeof liquids / sizeof liquids[0]; k++) {
        fixture();
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = H - 3; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, CELL_MAKE(liquids[k], MASS_MAX));
            }
        }
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, 0, SNOW);
        }

        bool water_seen = false;
        for (int i = 0; i < 600 && !water_seen; i++) {
            sand_step(&s, 0, 1000, 0);
            water_seen = count_cells_of(MAT_WATER) > 0;
        }

        TEST_ASSERT_TRUE_MESSAGE(water_seen,
            "snow melted by oil or acid has to leave WATER behind at some "
            "point - it is frozen water, and turning into whatever "
            "dissolved it would make a snowbank a factory for that "
            "liquid");
    }
}

/* On dry ground it does not melt at all.
 *
 * Which is what stops the rule above from being "snow evaporates". A
 * drift has to keep on a bare floor, or it cannot be stockpiled and
 * carried to the pane it is meant to crack. */
static void test_snow_keeps_on_dry_ground(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, 0, SNOW);
    }
    const int fell = W - 2;

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(fell, count_cells_of(MAT_SNOW),
        "snow on bare stone must not melt - it melts in liquid and near "
        "heat, and a floor is neither");
}

/* liquid_mass_of() lives in suite_sand_common.{c,h} - reused far past this
 * section. */

/* Stone carries a temperature, the same as glass. */
static void test_stone_heats_up_next_to_lava(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }

    int hottest = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_STONE && CELL_VARIANT(c) > hottest) {
                hottest = CELL_VARIANT(c);
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest,
        "stone under lava must warm up - it reads the same temperature "
        "scale glass does, so a player who has learned one wall can read "
        "the other");
}

/* Seals a single WATER cell at (4, 3) into a one-cell pocket, walled on
 * every side except the face touching the hot stone this test cares
 * about - stone at (3, 3), left open on purpose. Without this, water is
 * KIND_LIQUID and falls: a bare CELL_MAKE(MAT_WATER, ...) placed beside
 * the target drains away under gravity within the first step or two, and
 * the "wet" test only ever sees the target cell for that first step - the
 * rest of a long cooldown then runs at the plain DRY rate regardless of
 * SAND_WET_COOLING_FACTOR, which is indistinguishable from the feature
 * being absent (caught by this test itself: it failed exactly this way
 * on the first pass, both runs converging to the same step count, before
 * the pocket was added). */
static void seal_water_beside(int wx, int wy)
{
    sand_set(&s, wx, wy, WATER);
    sand_set(&s, wx + 1, wy, STONE);       /* blocks rightward cross-flow */
    sand_set(&s, wx,     wy + 1, STONE);   /* blocks the straight-down fall */
    sand_set(&s, wx + 1, wy + 1, STONE);   /* blocks the down-right diagonal */
    sand_set(&s, wx - 1, wy + 1, STONE);   /* blocks the down-left diagonal -
                                             * directly under the target cell
                                             * this pocket sits beside, which
                                             * is otherwise the one open
                                             * gravity-ward move move_liquid_
                                             * grain() would take, draining
                                             * the pocket a little every step
                                             * until nothing was left to be
                                             * wet with */
}

/* Pour water on a hot wall and its banked heat drains back to room
 * temperature far faster than ambient `cools` alone manages -
 * SAND_WET_COOLING_FACTOR (sand.h), applied in step_one_tempered_cell()'s
 * neighbour walk (sand_reactions.c). Proven by DERIVING the step budget
 * from the wet cell's own run rather than guessing a constant: however
 * SAND_WET_COOLING_FACTOR or stone's own `cools` are ever retuned, a dry
 * twin given the exact same number of steps the wet cell needed to reach
 * ambient must still be short of it - if it were not, the multiplier
 * would not actually be doing anything. */
static void test_water_cools_hot_stone_back_to_room_temperature(void)
{
    fixture();
    sand_set(&s, 3, 3, CELL_MAKE(MAT_STONE, MATERIAL_VARIANTS - 1));
    seal_water_beside(4, 3);

    int wet_steps = 0;
    while (wet_steps < 2000 &&
           CELL_VARIANT(sand_at(&s, 3, 3)) != SAND_AMBIENT_HEAT) {
        sand_step(&s, 0, 1000, 0);
        wet_steps++;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT,
        CELL_VARIANT(sand_at(&s, 3, 3)),
        "stone started at the top of its ramp with water beside it the "
        "whole time - it must reach room temperature well inside this "
        "budget, or SAND_WET_COOLING_FACTOR is not doing anything");

    fixture();
    sand_set(&s, 3, 3, CELL_MAKE(MAT_STONE, MATERIAL_VARIANTS - 1));
    for (int i = 0; i < wet_steps; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(SAND_AMBIENT_HEAT,
        CELL_VARIANT(sand_at(&s, 3, 3)),
        "a DRY control cell, given the exact same number of steps that "
        "just cooled its wet twin all the way to ambient, must still be "
        "warmer than room temperature - proving the water did the "
        "cooling, not merely the passage of time");
}

/* Water is a coolant, not a chiller - it can knock banked heat back down
 * to SAND_AMBIENT_HEAT, but never past it. Going below ambient is
 * snow/ice's job (`chills`) alone; letting water do it too would let a
 * splash thermally shock glass, which step_one_tempered_cell()'s own
 * comment on the ABOVE-ambient gate explains is exactly what this design
 * avoids. Run far longer than test_water_cools_hot_stone_back_to_room_
 * temperature's own budget needs, on purpose - this is a floor, and it
 * has to hold forever, not just until the cell first reaches ambient. */
static void test_water_never_chills_stone_below_room_temperature(void)
{
    fixture();
    sand_set(&s, 3, 3, CELL_MAKE(MAT_STONE, MATERIAL_VARIANTS - 1));
    seal_water_beside(4, 3);

    int lowest = MATERIAL_VARIANTS;
    for (int i = 0; i < 2000; i++) {
        sand_step(&s, 0, 1000, 0);
        const int v = CELL_VARIANT(sand_at(&s, 3, 3));
        if (v < lowest) {
            lowest = v;
        }
    }

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, lowest,
        "a quenching liquid must never push a heat-ramping cell's own "
        "variant below SAND_AMBIENT_HEAT - that would let water thermally "
        "shock glass the way only snow is supposed to be able to");
}

/* But it never melts, however hot it gets, and that is the point of it.
 *
 * Glass names MAT_LAVA in `heats_to` and stone names nothing. If both
 * melted there would be no vessel that holds lava indefinitely and the
 * choice between the two materials would collapse into "glass, but it
 * dies". As it stands stone survives any heat and acid eats it, glass is
 * immune to acid and heat melts it, and both crack when chilled hot. */
static void test_stone_never_melts_however_hot(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reactions[MAT_STONE].heats_to,
        "stone must name nothing in heats_to - surviving heat is the "
        "reason to build out of it");

    fixture();
    sand_clear(&s);
    const int walls = W;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, CELL_MAKE(MAT_STONE, MATERIAL_VARIANTS - 1));
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }

    for (int i = 0; i < 800; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(walls, count_cells_of(MAT_STONE),
        "stone held at the top of its ramp under lava must still be stone "
        "after 800 steps");
}

/* A hot stone wall does NOT crack when it is chilled. Glass, right beside
 * it in the same scene, does.
 *
 * Stone reads the temperature and does nothing else with it. Rock quenched
 * from hot spalls and cracks; it does not turn into sand, and there is no
 * honest byproduct to name for it - so `shatters_to` is left off, and the
 * absence is the decision.
 *
 * Both halves in one test on one board, because the claim is a CONTRAST.
 * "Stone survives" passes just as well on a board where nothing shocks at
 * all, which would hide the feature breaking rather than show it. */
static void test_snow_cracks_glass_but_not_stone(void)
{
    fixture();
    sand_clear(&s);
    const int mid = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        /* left half stone, right half glass - both at the top of the ramp */
        sand_set(&s, x, H - 2,
                 CELL_MAKE(x < mid ? MAT_STONE : MAT_GLASS,
                           MATERIAL_VARIANTS - 1));
        sand_set(&s, x, H - 3, SNOW);
    }
    const int stone_before = count_cells_of(MAT_STONE);
    const int glass_before = count_cells_of(MAT_GLASS);

    for (int i = 0; i < 80; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reactions[MAT_STONE].shatters_to,
        "stone must name nothing in shatters_to - rock does not thermally "
        "shock into anything this simulation has a material for");
    TEST_ASSERT_EQUAL_INT_MESSAGE(stone_before, count_cells_of(MAT_STONE),
        "and a glowing stone wall packed with snow must survive it");
    TEST_ASSERT_LESS_THAN_MESSAGE(glass_before, count_cells_of(MAT_GLASS),
        "while glass on the same board, at the same temperature, under the "
        "same snow, must crack - otherwise this passes on a board where "
        "shock is simply broken");
}



/* Rough perceptual distance between two panel colours - both are RGB565
 * with the bytes swapped for the panel (GFX_RGB in gfx_color.h), so they
 * have to be unswapped before the channels mean anything. Green counts
 * once and red and blue twice, which is only a rule of thumb; nothing here
 * needs better than "clearly further apart". */
static int colour_gap(gfx_color_t x, gfx_color_t y)
{
    const uint16_t a = (uint16_t)((x >> 8) | (x << 8));
    const uint16_t b = (uint16_t)((y >> 8) | (y << 8));
    const int dr = ((a >> 11) & 0x1F) - ((b >> 11) & 0x1F);
    const int dg = ((a >>  5) & 0x3F) - ((b >>  5) & 0x3F);
    const int db = ( a        & 0x1F) - ( b        & 0x1F);
    return (dr < 0 ? -dr : dr) * 2 + (dg < 0 ? -dg : dg) +
           (db < 0 ? -db : db) * 2;
}

/* An outline moves less with temperature than the body it encloses.
 *
 * A wall going from grey to glowing changed its whole silhouette, so the
 * shape stopped being readable at exactly the moment it mattered. Cells
 * touching empty space are drawn much nearer their own resting colour, so
 * the outline stays put and the heat is shown by the inside.
 *
 * Asserted as a comparison rather than against fixed colours: the claim is
 * that the edge moves LESS than the body, which stays true if the ramps
 * are ever retuned. */
static void test_an_edge_shows_less_temperature_than_the_body(void)
{
    static const uint8_t tempered[] = { MAT_GLASS, MAT_STONE };

    for (unsigned k = 0; k < sizeof tempered / sizeof tempered[0]; k++) {
        const uint8_t m = tempered[k];
        gfx_color_t body[3], ed[3], hot[3], hot_ed[3];

        material_colours(CELL_MAKE(m, SAND_AMBIENT_HEAT), 0u, 0u, 255u, body);
        material_colours(CELL_MAKE(m, SAND_AMBIENT_HEAT), 0u,
                         MATERIAL_EDGE_LEFT, 255u, ed);
        material_colours(CELL_MAKE(m, MATERIAL_VARIANTS - 1), 0u, 0u, 255u,
                         hot);
        material_colours(CELL_MAKE(m, MATERIAL_VARIANTS - 1), 0u,
                         MATERIAL_EDGE_LEFT, 255u, hot_ed);

        const gfx_color_t rest_body = body[0], rest_edge = ed[0];
        const gfx_color_t hot_body = hot[0], hot_edge = hot_ed[0];

        char why[224];
        snprintf(why, sizeof why,
                 "%s: an edge must travel less than the body between rest "
                 "and full heat, or the outline changes with the "
                 "temperature and the shape stops reading",
                 material_by_id((material_id_t)m)->name);

        TEST_ASSERT_TRUE_MESSAGE(colour_gap(rest_edge, hot_edge) <
                                 colour_gap(rest_body, hot_body), why);
        TEST_ASSERT_TRUE_MESSAGE(colour_gap(rest_edge, hot_edge) > 0,
            "but it must still travel some - an outline frozen at ambient "
            "would hide heat entirely at the boundary");
    }
}

/* Every material is painted the way it is meant to be, and no other.
 *
 * material_colours() is consulted for every cell the renderer paints, so a
 * material picking up a pattern by accident is a visible change nobody
 * asked for - on a path the host tests otherwise never reach, because
 * app_sand.c does not compile here.
 *
 * PURELY VISUAL, all of it: nothing the simulation reads comes from this,
 * which is exactly why it needs its own guard. A wrong colour breaks no
 * behaviour and no other test would notice. */
static void test_each_material_is_painted_the_way_it_should_be(void)
{
    const gfx_color_t *pal = material_palette();
    /* Deepest depth - every KIND_LIQUID material below is asserted to
     * paint EXACTLY its own body colour at this point, now that every
     * liquid's interior (water included) uses the same plain shade-index
     * shift into its own ramp - see material_colours()'s own comment on
     * the liquid interior branch. */

    for (int m = 1; m < MAT_COUNT; m++) {
        for (int v = 0; v < MATERIAL_VARIANTS; v++) {
            const cell_t c = CELL_MAKE(m, v);
            gfx_color_t col[3] = { 0, 0, 0 };
            /* hash 1, not 0: hash 0 at the rest phase (0, this file's
             * default) is the one combination that glints cullet
             * (material.c's MAT_SAND case, CULLET_GLINT_ONE_IN's own
             * comment) - see this file's own CULLET GLINT tests for that
             * roll on its own terms, checked deliberately rather than by
             * accident here. */
            const material_pattern_t pat =
                material_colours(c, 1u, 0u, 255u, col);

            char why[128];
            snprintf(why, sizeof why, "%s variant %d", material_by_id((material_id_t)m)->name, v);

            if (m == MAT_GLASS) {
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_HATCHED, pat, why);
                TEST_ASSERT_TRUE_MESSAGE(col[0] != col[1] && col[1] != col[2],
                    "glass is hatched, so its body, its lines and their "
                    "crossings must all differ - equal ones paint a flat "
                    "pane and the shine vanishes");
            } else if (m == MAT_STONE) {
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_SPECKLED, pat, why);
            } else if (m == MAT_WOOD) {
                /* Speckled only while UNLIT. A burning log is a glow, and
                 * a glow that varies cell to cell reads as dirty rather
                 * than as fire. */
                TEST_ASSERT_EQUAL_MESSAGE(
                    v == 0 ? MATERIAL_SPECKLED : MATERIAL_FLAT, pat, why);
            } else if (material_by_id((material_id_t)m)->kind == KIND_LIQUID) {
                /* mask 0 here (this loop never passes anything else), so
                 * this is the INTERIOR case - see material_colours()'s own
                 * comment on why that paints the full body colour rather
                 * than the fill-indexed one, whatever variant this cell
                 * happens to carry. The rim half of the same split gets
                 * its own tests (test_a_liquid_body_paints_flat_inside and
                 * friends, near the palette tests below) precisely because
                 * this loop cannot exercise it without a mask to vary. */
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_FLAT, pat, why);
                TEST_ASSERT_EQUAL_MESSAGE(pal[CELL_MAKE(m, MASS_MAX)],
                                          col[0], why);
                TEST_ASSERT_EQUAL_MESSAGE(col[0], col[2], why);
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_FLAT, pat, why);
                TEST_ASSERT_EQUAL_MESSAGE(pal[c], col[0], why);
                TEST_ASSERT_EQUAL_MESSAGE(col[0], col[2], why);
            }
        }
    }
}

/* Glass has a grain too, and it is quieter than stone's.
 *
 * Stone is rock and wants visible speckle; glass is smooth and wants only
 * enough variation that a wall of it stops reading as one flat fill. Both
 * halves are asserted because both can fail alone - no variation is the
 * flat fill this exists to undo, and variation as loud as stone's would
 * make a pane look like gravel.
 *
 * Compared against stone rather than against a fixed number, so it stays
 * meaningful if either ramp is retuned. */
static void test_glass_grain_is_quieter_than_stone(void)
{
    int glass_spread = 0, stone_spread = 0;

    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        gfx_color_t g0[3], g1[3], s0[3], s1[3];
        material_colours(CELL_MAKE(MAT_GLASS, v), 0u, 0u, 255u, g0);
        material_colours(CELL_MAKE(MAT_GLASS, v), 3u, 0u, 255u, g1);
        material_colours(CELL_MAKE(MAT_STONE, v), 0u, 0u, 255u, s0);
        material_colours(CELL_MAKE(MAT_STONE, v), 7u, 0u, 255u, s1);

        glass_spread += colour_gap(g0[0], g1[0]);
        stone_spread += colour_gap(s0[0], s1[0]);
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, glass_spread,
        "glass must vary from cell to cell at all - without it a pane is "
        "one flat fill, which is what the grain exists to undo");
    TEST_ASSERT_TRUE_MESSAGE(glass_spread < stone_spread,
        "but it must vary LESS than stone does - glass is smooth and rock "
        "is not, and a pane speckled as hard as a wall reads as gravel");
}

/* The lines and the shine do NOT vary from cell to cell.
 *
 * They are light landing on the surface, not the surface itself. Letting
 * them wobble per cell makes a highlight look chewed instead of
 * reflective, so only the pane underneath carries the grain. */
static void test_the_shine_does_not_vary_between_cells(void)
{
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        gfx_color_t a[3], b[3];
        material_colours(CELL_MAKE(MAT_GLASS, v), 0u, 0u, 255u, a);
        material_colours(CELL_MAKE(MAT_GLASS, v), 2u, 0u, 255u, b);

        char why[128];
        snprintf(why, sizeof why,
                 "glass at temperature %d: the %%s must be identical in "
                 "every cell", v);
        TEST_ASSERT_EQUAL_MESSAGE(a[1], b[1], why);
        TEST_ASSERT_EQUAL_MESSAGE(a[2], b[2], why);
    }
}

/* Stone's speckle comes from the cell's POSITION, not from its variant.
 *
 * Stone used to carry a random shade and a wall looked like rock because
 * of it. Spending the variant on temperature took that away; deriving the
 * shade from a per-cell hash instead puts it back without the variant
 * having to mean two things at once.
 *
 * Both halves are asserted because both can fail on their own: a speckle
 * that does not vary is a flat slab again, and one that varies with
 * anything unstable would crawl and shimmer from frame to frame. */
static void test_stone_speckles_by_position_at_every_temperature(void)
{
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        const cell_t c = CELL_MAKE(MAT_STONE, v);
        gfx_color_t seen[8];
        int distinct = 0;

        for (unsigned h = 0; h < 8u; h++) {
            gfx_color_t col[3] = { 0, 0, 0 };
            material_colours(c, h, 0u, 255u, col);
            const gfx_color_t a = col[0];
            TEST_ASSERT_EQUAL_MESSAGE(col[0], col[1],
                "a speckled cell is one flat colour - the variation is "
                "BETWEEN cells, not inside one");
            int fresh = 1;
            for (int k = 0; k < distinct; k++) {
                if (seen[k] == a) {
                    fresh = 0;
                }
            }
            if (fresh) {
                seen[distinct++] = a;
            }
        }

        char why[160];
        snprintf(why, sizeof why,
                 "stone at temperature %d must offer more than one shade - "
                 "one shade is the flat slab this exists to undo", v);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, distinct, why);
    }

    /* Stable: the same cell asked twice gets the same answer. */
    gfx_color_t one[3], two[3];
    const cell_t c = CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT);
    material_colours(c, 12345u, 0u, 255u, one);
    material_colours(c, 12345u, 0u, 255u, two);
    TEST_ASSERT_EQUAL_MESSAGE(one[0], two[0],
        "the same cell must speckle the same way every time it is asked, "
        "or a stone wall shimmers");
}


/* panel_luminance() now lives in suite_sand_common.{c,h} - both this file's
 * cullet/tone tests and the soil-tone test it was originally written for
 * need the same unpacking math. */

/*=============================================================================
 * CULLET'S COLOUR CYCLE - each of the four reserved shades (SAND_CULLET_BASE
 * .. MATERIAL_VARIANTS - 1) is a STARTING POINT on a shared, slowly-advancing
 * 16-step colour cycle rather than a fixed colour of its own - see
 * material_set_cullet_phase() and material_colours()'s own MAT_SAND case,
 * both material.c, and the rewritten comment on SAND_CULLET_BASE in
 * material.h.
 *
 * material_set_cullet_phase() is file-static state in material.c, exactly
 * like foam_phase - every test below sets whatever phase it needs and resets
 * it to 0 before returning, so none of them can depend on run order, and a
 * test run after this file finishes sees the same phase-0 rest look it would
 * have seen if none of these had run at all.
 *===========================================================================*/

/* At rest (phase 0), the four cullet shades are four distinct tints, not one
 * colour repeated - the same claim test_each_material_is_painted_the_way_it_
 * should_be already makes about every OTHER material's variants, made
 * explicit here because cullet is the one place a shade's colour depends on
 * more than the cell byte alone. */
static void test_cullet_shades_are_four_distinct_tints(void)
{
    material_set_cullet_phase(0u);

    /* hash 1, not 0 - hash 0 at phase 0 is the one combination
     * CULLET_GLINT_ONE_IN's own roll (material.c's MAT_SAND case) turns
     * into a glint, and this test wants the plain pale cycle, not the
     * pure-white exception to it (hash 1 never glints at any phase: with
     * the roll's odd multiplier, 1 + 183 * phase is never 0 mod 192). See
     * the CULLET GLINT tests further down for that roll on its own terms. */
    gfx_color_t col[SAND_CULLET_SHADES][3];
    for (int i = 0; i < SAND_CULLET_SHADES; i++) {
        const material_pattern_t pat = material_colours(
            CELL_MAKE(MAT_SAND, (uint8_t)(SAND_CULLET_BASE + i)), 1u, 0u,
            255u, col[i]);
        TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_FLAT, pat,
            "cullet is a shade, not a pattern - it must stay flat");
    }

    for (int i = 0; i < SAND_CULLET_SHADES; i++) {
        for (int j = i + 1; j < SAND_CULLET_SHADES; j++) {
            char why[80];
            snprintf(why, sizeof why,
                "cullet shade %d must differ from shade %d at phase 0", i, j);
            TEST_ASSERT_TRUE_MESSAGE(col[i][0] != col[j][0], why);
        }
    }

    material_set_cullet_phase(0u);
}

/* The whole point of the feature: a cullet cell's PAINTED colour moves on
 * its own clock even though the cell byte underneath never changes. Every
 * one of the sixteen steps has to actually change the colour - a cycle with
 * a stuck or repeated step would shimmer through fewer tints than it claims
 * to - and the sixteenth step has to land exactly back on the first, or the
 * cycle is not a cycle. */
static void test_cullet_changes_colour_as_the_phase_advances(void)
{
    const cell_t c = CELL_MAKE(MAT_SAND, SAND_CULLET_BASE);

    /* hash 1, not 0 - see test_cullet_shades_are_four_distinct_tints just
     * above for why 0 is the wrong hash to probe the plain cycle with, and
     * that this stays clear of the glint roll across every phase this loop
     * visits (0..CULLET_CYCLE_LEN) is checked directly in the CULLET GLINT
     * tests further down. */
    material_set_cullet_phase(0u);
    gfx_color_t at_phase_0[3];
    material_colours(c, 1u, 0u, 255u, at_phase_0);

    gfx_color_t prev[3];
    memcpy(prev, at_phase_0, sizeof prev);

    for (unsigned phase = 1; phase <= CULLET_CYCLE_LEN; phase++) {
        gfx_color_t col[3];
        material_set_cullet_phase(phase);
        material_colours(c, 1u, 0u, 255u, col);

        char why[64];
        snprintf(why, sizeof why,
            "phase %u must paint a different colour than phase %u", phase,
            phase - 1);
        TEST_ASSERT_TRUE_MESSAGE(col[0] != prev[0], why);
        memcpy(prev, col, sizeof prev);
    }

    TEST_ASSERT_EQUAL_MESSAGE(at_phase_0[0], prev[0],
        "phase CULLET_CYCLE_LEN must wrap back to exactly phase 0's colour, "
        "or the loop is not actually 16 steps long");

    material_set_cullet_phase(0u);
}

/* An ordinary dune shade must never so much as glance at the phase - it is
 * not cullet, and the whole reason the cycle is safe to add is that it
 * touches nothing outside the reserved band. */
static void test_dune_sand_ignores_the_cullet_phase(void)
{
    gfx_color_t at_rest[SAND_DUNE_SHADES][3];
    material_set_cullet_phase(0u);
    for (int v = 0; v < SAND_DUNE_SHADES; v++) {
        material_colours(CELL_MAKE(MAT_SAND, (uint8_t)v), 0u, 0u, 255u,
                         at_rest[v]);
    }

    static const unsigned phases_to_try[] = { 5u, CULLET_CYCLE_LEN };
    for (unsigned pi = 0; pi < sizeof phases_to_try / sizeof phases_to_try[0];
         pi++) {
        material_set_cullet_phase(phases_to_try[pi]);
        for (int v = 0; v < SAND_DUNE_SHADES; v++) {
            gfx_color_t col[3];
            material_colours(CELL_MAKE(MAT_SAND, (uint8_t)v), 0u, 0u, 255u,
                             col);
            char why[80];
            snprintf(why, sizeof why,
                "dune shade %d must ignore cullet phase %u", v,
                phases_to_try[pi]);
            TEST_ASSERT_EQUAL_MESSAGE(at_rest[v][0], col[0], why);
        }
    }

    material_set_cullet_phase(0u);
}

/* A grain that was a window must ALWAYS be tellable from beach sand, at
 * every point in the cycle - not merely at rest. A cycle that ever drifted
 * onto a dune colour would make a heap of broken glass momentarily
 * indistinguishable from the sand it is sitting in. */
static void test_cullet_never_dresses_as_beach(void)
{
    gfx_color_t dune[SAND_DUNE_SHADES][3];
    material_set_cullet_phase(0u);
    for (int v = 0; v < SAND_DUNE_SHADES; v++) {
        material_colours(CELL_MAKE(MAT_SAND, (uint8_t)v), 0u, 0u, 255u,
                         dune[v]);
    }

    /* hash 1, not 0, through the whole phase range this loop covers
     * (0..CULLET_CYCLE_LEN-1) - see test_cullet_shades_are_four_distinct_
     * tints above for why, and note this checks the PALE colour path only:
     * a glint (the rare pure-white exception, material.c's MAT_SAND case)
     * is deliberately outside the pale band, and asserting against it here
     * would be asserting a constraint the feature was never given. */
    for (unsigned phase = 0; phase < CULLET_CYCLE_LEN; phase++) {
        material_set_cullet_phase(phase);
        for (int i = 0; i < SAND_CULLET_SHADES; i++) {
            gfx_color_t col[3];
            material_colours(CELL_MAKE(MAT_SAND, (uint8_t)(SAND_CULLET_BASE + i)),
                             1u, 0u, 255u, col);
            for (int v = 0; v < SAND_DUNE_SHADES; v++) {
                char why[112];
                snprintf(why, sizeof why,
                    "cullet shade %d at phase %u must not match dune shade %d",
                    i, phase, v);
                TEST_ASSERT_TRUE_MESSAGE(col[0] != dune[v][0], why);
            }
        }
    }

    material_set_cullet_phase(0u);
}

/* Pale is the whole design constraint on the cycle's four anchors (see their
 * own comment in material.c) - a retune that let the cycle wander toward
 * anything saturated or dark would still pass every test above (a
 * saturated colour is still a distinct, non-dune colour) while no longer
 * reading as ground glass. Floored against the darkest DUNE shade's own
 * luminance, with real headroom, rather than a fixed number: what matters
 * is that cullet stays clearly paler than sand ever gets, not any one
 * absolute brightness. */
static void test_cullet_stays_pale_at_every_phase(void)
{
    const gfx_color_t *pal = material_palette();
    const int darkest_dune_lum = panel_luminance(pal[CELL_MAKE(MAT_SAND, 0)]);
    const int pale_floor = darkest_dune_lum + 40;

    /* hash 1, not 0, for the same reason as the other cullet-cycle tests
     * above - see test_cullet_shades_are_four_distinct_tints. */
    for (unsigned phase = 0; phase < CULLET_CYCLE_LEN; phase++) {
        material_set_cullet_phase(phase);
        for (int i = 0; i < SAND_CULLET_SHADES; i++) {
            gfx_color_t col[3];
            material_colours(CELL_MAKE(MAT_SAND, (uint8_t)(SAND_CULLET_BASE + i)),
                             1u, 0u, 255u, col);
            const int lum = panel_luminance(col[0]);
            char why[96];
            snprintf(why, sizeof why,
                "cullet shade %d at phase %u must stay pale (%d <= floor %d)",
                i, phase, lum, pale_floor);
            TEST_ASSERT_TRUE_MESSAGE(lum > pale_floor, why);
        }
    }

    material_set_cullet_phase(0u);
}

/*=============================================================================
 * CULLET'S GLINT - the pale cycle above read as too white on the device, so
 * material_colours()'s MAT_SAND case now flashes a grain PURE WHITE instead
 * of its pale cycle colour, rarely (CULLET_GLINT_ONE_IN), for a different
 * few grains every phase step - a facet catching the light. See
 * CULLET_GLINT's and CULLET_GLINT_ONE_IN's own comments in material.c.
 *===========================================================================*/

/* A glint is the brightest thing the panel can show, full white - not a
 * brighter tint of the grain's own colour, which was tried first and read
 * worse on the device. This searches hashes from 0 up, black-box, for the
 * first one that actually glints at shade 12 phase 0, rather than assuming
 * the roll formula's shape. */
static void test_a_cullet_glint_is_pure_white(void)
{
    material_set_cullet_phase(0u);

    gfx_color_t pale[3];
    material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), 1u, 0u, 255u, pale);

    gfx_color_t glinting[3];
    unsigned hash = 0u;
    for (; hash < 4096u; hash++) {
        material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), hash, 0u, 255u,
                         glinting);
        if (glinting[0] != pale[0]) {
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(hash < 4096u,
        "no hash in 0..4095 made cullet shade 12 glint at phase 0 - is the "
        "glint roll broken, or CULLET_GLINT_ONE_IN retuned far past 4096?");

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(GFX_RGB(0xFFFFFF), glinting[0],
        "a glinting grain must be pure white, the panel's highest radiance");

    material_set_cullet_phase(0u);
}

/* RARE, as asked - not blinking. The design is one grain in
 * CULLET_GLINT_ONE_IN (192); this checks a band around that (1/384..1/96)
 * so a deliberate retune of the constant does not have to also edit this
 * test, while a roll that stopped being rare (or stopped glinting at all)
 * still fails it. */
static void test_cullet_glints_are_rare(void)
{
    material_set_cullet_phase(0u);

    gfx_color_t pale[3];
    material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), 1u, 0u, 255u, pale);

    const unsigned n = 4096u;
    unsigned glints = 0u;
    for (unsigned hash = 0u; hash < n; hash++) {
        gfx_color_t col[3];
        material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), hash, 0u, 255u,
                         col);
        if (col[0] != pale[0]) {
            glints++;
        }
    }

    char why[112];
    snprintf(why, sizeof why,
        "%u of %u cells glinted - expected roughly 1/192, want it between "
        "1/384 and 1/96", glints, n);
    TEST_ASSERT_TRUE_MESSAGE(glints >= n / 384u && glints <= n / 96u, why);

    material_set_cullet_phase(0u);
}

/* Glistening, not blinking, means the SET of grains that glint has to move:
 * the same heap of cullet should show different sparkle points from one
 * phase step to the next, not the same handful of cells lit up forever.
 * Checked as a symmetric difference over hashes 0..1023 between two
 * adjacent phases - at least one hash has to glint at exactly one of the
 * two. */
static void test_cullet_glints_move_with_the_phase(void)
{
    const unsigned n = 1024u;

    material_set_cullet_phase(0u);
    gfx_color_t pale0[3];
    material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), 1u, 0u, 255u, pale0);

    material_set_cullet_phase(1u);
    gfx_color_t pale1[3];
    material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), 1u, 0u, 255u, pale1);

    bool moved = false;
    for (unsigned hash = 0u; hash < n && !moved; hash++) {
        gfx_color_t col0[3], col1[3];
        material_set_cullet_phase(0u);
        material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), hash, 0u, 255u,
                         col0);
        material_set_cullet_phase(1u);
        material_colours(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), hash, 0u, 255u,
                         col1);

        const bool glinted0 = col0[0] != pale0[0];
        const bool glinted1 = col1[0] != pale1[0];
        if (glinted0 != glinted1) {
            moved = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(moved,
        "the glinting set at phase 0 must differ from phase 1 over hashes "
        "0..1023 - a glint that never moved would be a blink, not a "
        "glisten");

    material_set_cullet_phase(0u);
}

/* The roll lives inside the `v >= SAND_CULLET_BASE` branch of MAT_SAND's
 * case, which ordinary sand's `break` skips entirely before the roll is
 * even computed - but that is an implementation fact, not something this
 * suite should take on faith. This scans real hashes across ordinary sand
 * to confirm it holds, the same way test_dune_sand_ignores_the_cullet_phase
 * confirms the phase alone touches nothing outside the cullet band. */
static void test_dune_sand_never_glints(void)
{
    const gfx_color_t *pal = material_palette();

    static const unsigned phases_to_try[] = { 0u, 3u };
    for (unsigned pi = 0; pi < sizeof phases_to_try / sizeof phases_to_try[0];
         pi++) {
        material_set_cullet_phase(phases_to_try[pi]);
        for (int v = 0; v < SAND_DUNE_SHADES; v++) {
            const gfx_color_t expect = pal[CELL_MAKE(MAT_SAND, (uint8_t)v)];
            for (unsigned hash = 0u; hash < 256u; hash++) {
                gfx_color_t col[3];
                material_colours(CELL_MAKE(MAT_SAND, (uint8_t)v), hash, 0u,
                                 255u, col);
                char why[128];
                snprintf(why, sizeof why,
                    "dune shade %d, hash %u, phase %u must stay the plain "
                    "palette colour - the glint roll must never reach "
                    "ordinary sand", v, hash, phases_to_try[pi]);
                TEST_ASSERT_EQUAL_MESSAGE(expect, col[0], why);
            }
        }
    }

    material_set_cullet_phase(0u);
}

void run_sand_tone_suite(void)
{
    RUN_TEST(test_the_brush_and_the_setter_agree_about_every_material);
    RUN_TEST(test_snow_painted_into_water_melts);
    RUN_TEST(test_snow_melts_in_any_liquid);
    RUN_TEST(test_melting_snow_makes_water_not_more_of_the_liquid);
    RUN_TEST(test_snow_keeps_on_dry_ground);
    RUN_TEST(test_stone_heats_up_next_to_lava);
    RUN_TEST(test_water_cools_hot_stone_back_to_room_temperature);
    RUN_TEST(test_water_never_chills_stone_below_room_temperature);
    RUN_TEST(test_stone_never_melts_however_hot);
    RUN_TEST(test_snow_cracks_glass_but_not_stone);
    RUN_TEST(test_an_edge_shows_less_temperature_than_the_body);
    RUN_TEST(test_each_material_is_painted_the_way_it_should_be);
    RUN_TEST(test_glass_grain_is_quieter_than_stone);
    RUN_TEST(test_the_shine_does_not_vary_between_cells);
    RUN_TEST(test_stone_speckles_by_position_at_every_temperature);
    RUN_TEST(test_cullet_shades_are_four_distinct_tints);
    RUN_TEST(test_cullet_changes_colour_as_the_phase_advances);
    RUN_TEST(test_dune_sand_ignores_the_cullet_phase);
    RUN_TEST(test_cullet_never_dresses_as_beach);
    RUN_TEST(test_cullet_stays_pale_at_every_phase);
    RUN_TEST(test_a_cullet_glint_is_pure_white);
    RUN_TEST(test_cullet_glints_are_rare);
    RUN_TEST(test_cullet_glints_move_with_the_phase);
    RUN_TEST(test_dune_sand_never_glints);
}

SUITE_REGISTER(run_sand_tone_suite);
