/*=============================================================================
 * Portable suite: the falling-sand automaton - shared benchmark scenes -
 * four-liquid, lava-stress, smoke-and-steam, thermal-shock, boiler, and wet-
 * earth.
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
#include "suite_sand_scenes.h"

/* --- on the real grid, on the real chip --------------------------------- */

/* REAL_W/REAL_H moved to suite_sand_common.h - reused throughout the
 * scenes/perf/blast portion of the split, well beyond this section. */

/* EMPTY_SHARE_PERCENT moved to suite_sand_scenes.h - the frame-budget
 * equivalent of the test below, in suite_sand_perf.c, needs it too. */

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
int all_pairs_material_at(int x, int y, int first, int n_mats)
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
void build_wet_earth_scene(sand_t *s)
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
}

SUITE_REGISTER(run_sand_scenes_suite);
