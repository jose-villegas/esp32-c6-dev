/*
 * Portable suite: the falling-sand automaton - the extended range - sixteen
 * materials behind the last slot.
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

/* ===================================================================
 * The extended range: sixteen materials behind the last slot.
 * =================================================================== */

/* Heat through a wall LIGHTS oil. It does not boil it away.
 *
 * conduct_heat() turned anything on the far side into steam if it was a
 * liquid and did not itself burn. Water is a liquid and does not burn, so
 * that was right when water was the only liquid that could be there - and
 * it has been wrong for every liquid added since. Lava was caught first,
 * because lava boiling itself is spectacular. Oil is quieter: it just
 * disappears.
 *
 * Measured before the fix: 180 units of oil in a stone pan over a fire
 * went to zero in sixty steps, leaving fourteen cells of steam. Steam is
 * the tell - oil has no business producing any at all, which is what makes
 * it a sharper assertion than the oil count. */
static void test_heat_through_a_pan_lights_oil_rather_than_boiling_it(void)
{
    fixture();
    sand_clear(&s);
    sand_set_flammability(&s, SAND_FLAMMABILITY_PER_MATERIAL);
    sand_set_conduction(&s, SAND_CONDUCTION_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 3, STONE);            /* the pan */
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_OIL, MASS_MAX));
    }

    bool lit = false;
    for (int i = 0; i < 300; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);

        TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_STEAM),
            "oil must never make steam - it is fuel, not a kettle, and "
            "steam here means heat is evaporating it instead of lighting "
            "it");
        if (count_cells_of(MAT_OIL) < W - 2) {
            lit = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(lit,
        "and the heat must actually reach it - oil in a pan over a held "
        "fire has to catch, or this passes on a board where nothing "
        "happened at all");
}

/* A powder lands ON another powder, and still sinks through a liquid.
 *
 * A grain can push its way down through water or through smoke and cannot
 * push its way through packed grains however heavy it is. Density decides
 * fluids and stops deciding anything between solids.
 *
 * Both halves in one test, because "powders stack" passes just as well on
 * a board where nothing displaces anything at all - which would leave sand
 * sitting on top of water. */
static void test_a_powder_lands_on_a_powder_but_sinks_in_a_liquid(void)
{
    static const struct { uint8_t bed, dropped; bool sinks; } cases[] = {
        { MAT_SNOW,  MAT_DIRT,  false },
        { MAT_SNOW,  MAT_SAND,  false },
        { MAT_SAND,  MAT_DIRT,  false },
        { MAT_WATER, MAT_SAND,  true  },
        { MAT_WATER, MAT_DIRT,  true  },
    };

    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const uint8_t bed = cases[k].bed, dropped = cases[k].dropped;
        fixture();
        sand_clear(&s);
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = H - 4; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y,
                         material_by_id((material_id_t)bed)->kind == KIND_LIQUID
                             ? CELL_MAKE(bed, MASS_MAX) : CELL_MAKE(bed, 4));
            }
        }
        for (int x = 2; x < W - 2; x++) {
            /* BONE DRY (variant 0): variant IS moisture for a soil material
             * (CELL_MOISTURE()), so a wet DIRT grain here could soak/
             * percolate into the bed below by itself - unrelated to the
             * displacement rule this test checks, and indistinguishable
             * from real sinking in this test's own measurement (lowest cell
             * of dropped's material). Starting bone dry removes that
             * soaking side channel entirely on both SAND and DIRT, so this
             * test is only ever sensitive to displacement. */
            sand_set(&s, x, 1, CELL_MAKE(dropped, 0));
        }

        for (int i = 0; i < 300; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        int lowest = -1, highest_bed = H;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const uint8_t m = CELL_MATERIAL(sand_at(&s, x, y));
                if (m == dropped && y > lowest) {
                    lowest = y;
                }
                if (m == bed && y < highest_bed) {
                    highest_bed = y;
                }
            }
        }

        char why[160];
        snprintf(why, sizeof why,
                 "%s dropped on %s should %s", material_by_id((material_id_t)dropped)->name,
                 material_by_id((material_id_t)bed)->name,
                 cases[k].sinks ? "sink through it - density decides fluids"
                                : "land on top of it - grains do not pass "
                                  "through grains");
        if (cases[k].sinks) {
            TEST_ASSERT_TRUE_MESSAGE(lowest > highest_bed, why);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(lowest <= highest_bed, why);
        }
    }
}

/* Hot gas warms what it touches - convection.
 *
 * It exists for what it makes VISIBLE rather than for what it achieves:
 * warmer air does not help shatter glass - it costs snow, the scarce
 * resource, its life. What it does do is make heat reach where
 * conduction cannot: on a stone flue with a fire at the bottom, the top
 * sits at ambient without it and one to two levels above with it. */
static void test_hot_gas_warms_what_it_touches(void)
{
    fixture();
    sand_clear(&s);
    /* This scene refills a whole row of steam every step for 300 steps -
     * plenty of chances for a 2x2 patch to condense away before it ever
     * reaches the stone below it, which has nothing to do with what this
     * test exists to measure (see reaction_t.condenses). */
    sand_set_condenses(&s, 0);

    /* A stone slab with nothing but steam against it - no fire, no
     * conduction path, nothing else that could account for the heat. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, STONE);
    }
    for (int i = 0; i < 300; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, STEAM);
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    int hottest = 0;
    for (int x = 0; x < W; x++) {
        const cell_t c = sand_at(&s, x, H - 2);
        if (CELL_MATERIAL(c) == MAT_STONE && CELL_VARIANT(c) > hottest) {
            hottest = CELL_VARIANT(c);
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest,
        "steam resting against stone must warm it - there is no fire here "
        "and nothing to conduct through, so the gas is the only thing that "
        "could have");
}

/* But it is not a fire: it lights nothing.
 *
 * The cheap way to get this heating was to give smoke `burns`, which would
 * have made a chimney full of smoke set light to a wooden roof. Convection
 * is a separate field for that reason, and the difference is worth a test
 * rather than a comment. */
static void test_hot_gas_does_not_set_fire_to_anything(void)
{
    fixture();
    sand_clear(&s);
    sand_set_flammability(&s, SAND_FLAMMABILITY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, WOOD);
    }
    for (int i = 0; i < 400; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, SMOKE);
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    for (int x = 0; x < W; x++) {
        TEST_ASSERT_FALSE_MESSAGE(cell_is_burning(sand_at(&s, x, H - 2)),
            "smoke must not light wood - it warms things that hold a "
            "temperature and does nothing else, which is the whole reason "
            "it is not simply `burns`");
    }
}

void run_sand_extended_range_suite(void)
{
    RUN_TEST(test_heat_through_a_pan_lights_oil_rather_than_boiling_it);
    RUN_TEST(test_a_powder_lands_on_a_powder_but_sinks_in_a_liquid);
    RUN_TEST(test_hot_gas_warms_what_it_touches);
    RUN_TEST(test_hot_gas_does_not_set_fire_to_anything);
}

SUITE_REGISTER(run_sand_extended_range_suite);
