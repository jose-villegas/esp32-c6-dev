/*=============================================================================
 * Portable suite: the falling-sand automaton - glass's heat ramp, cooling,
 * frost, and thermal shock.
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

/* ===================================================================
 * Glass as a material with a temperature: the heat ramp, cooling, and
 * thermal shock against snow.
 * =================================================================== */

/* The hottest glass cell anywhere, or 0 if there is no glass at all. */
static int hottest_glass(void)
{
    int hot = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_GLASS && CELL_VARIANT(c) > hot) {
                hot = CELL_VARIANT(c);
            }
        }
    }
    return hot;
}

/* A pane with lava held against its underside, for `steps` steps. */
static void hold_lava_under_a_pane(int steps)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, GLASS);
    }
    for (int i = 0; i < steps; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
    }
}

/* One more step of the scene hold_lava_under_a_pane() built, lava topped
 * back up. Separate so a caller can soak to a condition instead of to a
 * step count. */
static void hold_lava_under_a_pane_again(void)
{
    for (int x = 1; x < W - 1; x++) {
        if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
            sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }
    sand_step(&s, 0, 1000, 0);
}

/* Heat ACCUMULATES in the pane rather than transforming it on contact.
 *
 * This is the difference between `heat_ramp` and the `heat_chance` sand
 * uses, and it is the whole reason glass melting can mean "long exposure"
 * at all: a per-step roll has no memory, so under it a brief fierce flame
 * and a long slow one are the same event with different luck. Banking the
 * exposure in the cell is what lets duration be a real requirement.
 *
 * Asserted as "still glass, but changed" rather than on a specific level,
 * because the level is a race between the ramp and cooling and pinning it
 * would make this a test of the RNG. */
static void test_glass_banks_heat_rather_than_melting_on_contact(void)
{
    hold_lava_under_a_pane(60);

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_GLASS) > 0,
        "a brief touch of lava must not melt glass outright - if it does, "
        "the ramp is not being consulted and heat is transforming on "
        "contact the way it does for sand");
    TEST_ASSERT_TRUE_MESSAGE(hottest_glass() > 0,
        "and it must have GAINED heat while being touched, or nothing is "
        "accumulating and the pane is simply immune");
}

/* Held long enough, the same fire wins and the pane runs. */
static void test_a_fire_held_long_enough_melts_glass_to_lava(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, GLASS);
    }

    int melted = 0;
    for (int i = 0; i < 4000 && !melted; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
        melted = count_cells_of(MAT_GLASS) == 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(melted,
        "lava held against a pane must eventually melt it. Cooling faster "
        "than the ramp climbs would make this never happen for any fire of "
        "any size, which is an easy thing to do by accident with two "
        "constants that pull opposite ways");
}

/* Take the fire away and the heat drains back out.
 *
 * This is the half of the mechanism that makes the ramp mean DURATION
 * rather than lifetime total. Without it a pane remembers every flame it
 * ever met, so a candle lit for one step a day melts it just as surely as
 * a furnace - the exposure simply accumulates forever. */
static void test_glass_forgets_a_fire_that_went_out(void)
{
    /* Soaked in short bursts up to a CONDITION rather than for a fixed
     * count: the ramp is fast enough now that a constant long enough to
     * heat the pane on a slow build melts it outright on this one, and a
     * fixture that destroys its own subject reports on nothing. */
    hold_lava_under_a_pane(1);
    for (int i = 0; i < 400 && hottest_glass() <= SAND_AMBIENT_HEAT + 2; i++) {
        hold_lava_under_a_pane_again();
    }
    const int peak = hottest_glass();
    TEST_ASSERT_TRUE_MESSAGE(peak > 0,
        "fixture check: the pane has to be hot before cooling it means "
        "anything");

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_LAVA) {
                sand_set(&s, x, y, 0);
            }
        }
    }
    for (int i = 0; i < 3000 && hottest_glass() > SAND_AMBIENT_HEAT; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest_glass(),
        "with the fire gone the pane must drain all the way back to room "
        "temperature - which is SAND_AMBIENT_HEAT, not 0, since 0 now means "
        "frosted");
}

/* And it drains on a board where nothing is burning and nothing dissolves.
 *
 * Separate from the test above because it fails differently: the reactions
 * pass returns early unless something wants it, and hanging heat off the
 * fire flag would leave a pane frozen at whatever level the fire left it -
 * cooling forever pending a fire that is, by definition, already out. */
static void test_glass_cools_on_a_board_with_no_fire_at_all(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1, hottest_glass(),
        "fixture check: the pane starts at the top of its ramp");

    for (int i = 0; i < 3000 && hottest_glass() > SAND_AMBIENT_HEAT; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest_glass(),
        "hot glass must cool with no fire and no acid anywhere on the "
        "board - temperature is its own reason to run the reactions pass");
}

/* Glass made by heat arrives COLD.
 *
 * place_reacted() gives a new cell a full variant, which is right for a
 * liquid (a full one) and for a transient (a fresh one) and would be
 * catastrophic here: sand fusing under a flame would produce a pane
 * already at the top of its melt ramp, and the next step would run it to
 * lava. Sand under a steady fire would reach lava in two ticks and the
 * duration this whole mechanism exists to express would be unreachable. */
static void test_freshly_fused_glass_starts_cold(void)
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
        "fixture check: fire over sand has to make some glass");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest_glass(),
        "glass fused out of sand must start at room temperature - starting "
        "full would melt it to lava on the following step, and starting at "
        "0 would hand the player a pane that begins life frosted");
}

/* Snow on a glowing pane cracks it, and it goes back to being sand.
 *
 * Thermal shock needs a gradient, and a gradient needs something the
 * player can SEE is cold. An earlier draft used water for the cold side,
 * which works as a rule and fails as a design: nothing in this simulation
 * says water is cold, so a pane cracking beside it reads as "glass breaks
 * near water" rather than as a temperature difference. */
static void test_snow_shatters_a_glowing_pane_into_sand(void)
{
    fixture();
    const int panes = W - 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
        sand_set(&s, x, H - 3, SNOW);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* ALL of them, and it takes one step. This asked for "most" while
     * shock had to win a cooling roll first: the roll that decided whether
     * to crack was the same roll that cooled the pane, so a drift often
     * talked a pane down below the threshold instead of breaking it, and a
     * bank eating itself from its own meltwater could run a cell short.
     * Contact alone is enough now. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "snow touching panes above the shock threshold must shatter every "
        "one of them");
    TEST_ASSERT_EQUAL_INT_MESSAGE(panes, count_cells_of(MAT_SAND),
        "and shattered glass must come back as SAND - that is what closes "
        "the loop heat opened, so the player can un-make the material");
}

/* The threshold is sharp, and it is the ONLY thing that decides.
 *
 * One level below it a pane is untouchable and one level above it breaks
 * on contact, which is a strange rule to have unless the player can see
 * which side of the line a pane is on - which is what the colour test
 * below is for. The two belong together: this one fixes the behaviour to
 * SAND_SHOCK_HEAT, that one fixes the appearance to the same number. */
static void test_the_shock_threshold_is_exact(void)
{
    const int panes = W - 2;

    for (int heat = SAND_SHOCK_HEAT - 1; heat <= SAND_SHOCK_HEAT; heat++) {
        fixture();
        sand_clear(&s);
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int x = 1; x < W - 1; x++) {
            sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, (uint8_t)heat));
            sand_set(&s, x, H - 3, SNOW);
        }

        for (int i = 0; i < 60; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        if (heat < SAND_SHOCK_HEAT) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(panes, count_cells_of(MAT_GLASS),
                "a pane one level BELOW the shock threshold must survive "
                "snow - it only cools, which is what makes the threshold "
                "mean something");
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
                "and a pane exactly AT the threshold must break");
        }
    }
}

/* And the biggest colour change in glass's ramp is at that same level.
 *
 * Glass is the one material whose variant the player has to be able to
 * read, because it is the only one where the variant changes what the
 * material DOES rather than how it looks. A smooth ramp hid that: a pane
 * at 5 and a pane at 6 behave completely differently and looked nearly
 * identical, so pouring snow on a basin that was not quite hot enough
 * produced no reaction and no explanation for it.
 *
 * Asserted as "the largest step in the ramp", not "these two colours
 * differ", because any two entries of a gradient differ. The claim worth
 * defending is that this break is the one you notice. */
static void test_glass_looks_different_at_the_shock_threshold(void)
{
    const gfx_color_t *pal = material_palette();

    int gap[MATERIAL_VARIANTS] = { 0 };
    for (int v = 1; v < MATERIAL_VARIANTS; v++) {
        const gfx_color_t a = pal[MAT_GLASS * MATERIAL_VARIANTS + v - 1];
        const gfx_color_t b = pal[MAT_GLASS * MATERIAL_VARIANTS + v];
        /* Stored byte-swapped for the panel - see GFX_RGB in gfx_color.h. */
        const uint16_t ua = (uint16_t)((a >> 8) | (a << 8));
        const uint16_t ub = (uint16_t)((b >> 8) | (b << 8));
        const int dr = ((ua >> 11) & 0x1F) - ((ub >> 11) & 0x1F);
        const int dg = ((ua >>  5) & 0x3F) - ((ub >>  5) & 0x3F);
        const int db = ( ua        & 0x1F) - ( ub        & 0x1F);
        gap[v] = (dr < 0 ? -dr : dr) * 2 +
                 (dg < 0 ? -dg : dg) +
                 (db < 0 ? -db : db) * 2;
    }

    int widest = 1;
    for (int v = 2; v < MATERIAL_VARIANTS; v++) {
        if (gap[v] > gap[widest]) {
            widest = v;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_SHOCK_HEAT, widest,
        "the biggest colour change along glass's ramp has to land exactly "
        "where its behaviour changes - a pane that snow will shatter must "
        "not look like one that snow will merely cool");
}

/* A resting pane beside snow gets COLDER, and shows it.
 *
 * The report that produced this: "I still don't see any colour change of
 * glass when near snow." There was none to see, and it was not a rendering
 * problem. Ambient used to be 0, the bottom of the variant range, so a
 * pane at rest had nothing to lose - chilling it changed no number, so it
 * changed no colour, and snow beside glass was indistinguishable from snow
 * beside nothing.
 *
 * Two things had to change for this to be observable, and both are
 * asserted here: room temperature had to move off the floor so cold has
 * somewhere to go, and chilling had to be driven from the SNOW, because a
 * pane at rest never gets a turn of its own and so never looked at what
 * was sitting on it. */
static void test_snow_frosts_a_resting_pane(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, GLASS);      /* at rest, not heated */
        sand_set(&s, x, H - 3, SNOW);
    }

    int coldest = MATERIAL_VARIANTS;
    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 1; x < W - 1; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_GLASS && CELL_VARIANT(c) < coldest) {
                coldest = CELL_VARIANT(c);
            }
        }
    }

    TEST_ASSERT_LESS_THAN_MESSAGE(SAND_AMBIENT_HEAT, coldest,
        "snow resting on a pane at room temperature must pull it BELOW "
        "room temperature - if ambient is the bottom of the scale there is "
        "nothing to see, and the player gets no sign that snow and glass "
        "interact at all");

    /* And the palette has to disagree about the two, or the number moving
     * is still invisible. */
    const gfx_color_t *pal = material_palette();
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        pal[MAT_GLASS * MATERIAL_VARIANTS + SAND_AMBIENT_HEAT],
        pal[MAT_GLASS * MATERIAL_VARIANTS + coldest],
        "and a frosted pane must not be drawn in the same colour as a "
        "resting one");
}

/* Cold spreads THROUGH the glass, past the cells the snow is touching.
 *
 * Without this the effect was real and nearly invisible: only the single
 * cell under a flake ever changed, and it barely changed, because snow
 * melts after a chill or two and `cools` drags the cell straight back
 * towards ambient. One cell one level off ambient is not something anyone
 * spots on a 184x224 board.
 *
 * Spreading it makes a patch of frost that creeps outward from where the
 * snow landed, which is both what frost looks like and what makes the
 * state readable. It is the same `conducts` that carries a fire's heat
 * through a wall, applied within the material - scaled down hard, because
 * at its own value a pane goes isothermal in a step or two and a wall that
 * is all one temperature cannot be hot inside and cold outside. */
static void test_frost_spreads_beyond_the_snow_touching_it(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GLASS);
    }
    /* Snow on the middle two cells only. */
    const int lo = W / 2 - 1, hi = W / 2;
    for (int x = lo; x <= hi; x++) {
        sand_set(&s, x, H - 3, SNOW);
    }

    int reached = 0;
    for (int i = 0; i < 600 && !reached; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            /* Strictly outside the snow's own footprint and its two
             * immediate sides, so this cannot pass on direct contact. */
            if (x >= lo - 1 && x <= hi + 1) {
                continue;
            }
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_GLASS &&
                CELL_VARIANT(c) < SAND_AMBIENT_HEAT) {
                reached = 1;
                break;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(reached,
        "cold must travel along the pane to cells no snow ever touched - a "
        "single chilled cell one level off ambient is a state change nobody "
        "can see");
}

/* Snow keeps on ordinary cold glass.
 *
 * It did not, briefly, and the report was exact: snow turned to water on
 * contact with glass when it never used to. Chilling had just been moved
 * so that it reaches panes at rest, and chilling costs the cold material
 * its own `heats_to` - so snow started paying the price tuned for standing
 * beside a FIRE in exchange for pushing a resting pane one level cooler.
 *
 * The cost now tracks what was actually absorbed: taking heat out of
 * something above room temperature melts snow, pushing cold into something
 * at or below it does not. Otherwise a snowbank cannot be kept anywhere
 * near the one building material it is meant to be used against. */
static void test_snow_keeps_on_ordinary_cold_glass(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, SNOW);
    }
    const int flakes = count_cells_of(MAT_SNOW);

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(flakes, count_cells_of(MAT_SNOW),
        "snow resting on glass at room temperature must not melt - it pays "
        "for heat it takes, and there was none to take");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_WATER),
        "and it must leave no water behind, which is how the melting showed "
        "up on the board");
}

/* Shock works HOT ONTO COLD as well.
 *
 * Thermal shock is a large temperature CHANGE, not a high temperature, and
 * for a while only half of it existed: cold arriving at hot glass broke it,
 * heat arriving at frosted glass did not. Nobody could have explained that
 * asymmetry to a player, and the obvious experiment - chill a vessel, then
 * pour something hot into it - quietly did nothing.
 *
 * Kept as its own test rather than folded into the cold-onto-hot one
 * because the two run through completely different code: this direction
 * lives in try_heat_transform(), driven by the heat source, and the other
 * in step_one_cold_cell(), driven by the cold cell. They can break
 * independently and have. */
static void test_heat_arriving_at_frosted_glass_cracks_it(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, 0));   /* fully frosted */
    }
    const int panes = W;
    const int sand_before = count_cells_of(MAT_SAND);

    int cracked = 0;
    for (int i = 0; i < 60 && !cracked; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
        cracked = count_cells_of(MAT_SAND) > sand_before;
    }

    TEST_ASSERT_TRUE_MESSAGE(cracked,
        "lava arriving at a frosted pane must crack it, not warm it "
        "through - shock is about the size of the change, and it has to "
        "work in both directions or it is not that");
    TEST_ASSERT_LESS_THAN_MESSAGE(panes, count_cells_of(MAT_GLASS),
        "and the pane must actually be gone, not merely warmed");
}

/* A pane at room temperature is not cracked by heat arriving.
 *
 * The guard on the test above, and the reason SAND_SHOCK_COLD is not
 * simply "below ambient": ordinary glass meeting fire has to warm up
 * through the ramp the way it always did, or every pane in the game breaks
 * the first time anyone lights something next to it. */
static void test_heat_arriving_at_resting_glass_only_warms_it(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GLASS);            /* at rest */
    }
    const int sand_before = count_cells_of(MAT_SAND);

    for (int i = 0; i < 12; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(sand_before, count_cells_of(MAT_SAND),
        "glass at room temperature must warm up when fire reaches it, not "
        "shatter - only glass that was already COLD is shocked by heat");
}

/* Frost fades. It is a state, not a scar.
 *
 * Ambient is a resting point that gets approached from BOTH sides, which
 * is what makes putting cold below it work at all - otherwise the first
 * snowfall would leave every pane it touched permanently pale. */
static void test_a_frosted_pane_warms_back_to_room_temperature(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, 0));   /* fully frosted */
    }

    int coldest = 0;
    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
        coldest = MATERIAL_VARIANTS;
        for (int x = 1; x < W - 1; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_GLASS && CELL_VARIANT(c) < coldest) {
                coldest = CELL_VARIANT(c);
            }
        }
        if (coldest >= SAND_AMBIENT_HEAT) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, coldest,
        "a frosted pane left alone must warm back to room temperature - "
        "the same `cools` drift that brings a hot one down, running the "
        "other way");
}

/* Lava on one side of a wall, snow on the other: it cracks.
 *
 * The scenario the mechanism exists for, end to end and with nothing
 * pre-set - hold lava against a glass wall until it glows, then bank snow
 * on the far face. Every other shock test places a pane at a chosen
 * temperature, which tests the rule but assumes the pane can get hot at
 * all; here the heat has to arrive from a real source, through the
 * material, and reach the same cell the snow is touching.
 *
 * A vertical wall, deliberately. Laid flat with lava on top there is
 * nowhere for the snow to be except on top of the lava, where it flashes
 * off without ever meeting the hot cell - which is exactly what happens on
 * the board if you pour snow into an open basin rather than banking it
 * against the outside.
 *
 * The ORDER is the other half and the second block asserts it. Snow
 * present from the start fights the ramp instead of exploiting it:
 * chilling is faster than heating, so the wall never arrives at the
 * threshold and just sits there, warm on one face and frosted on the
 * other. Heat first, chill second. */
static void test_lava_one_side_snow_the_other_cracks_the_wall(void)
{
    const int wall = W / 2;

    /* --- heat it first ---------------------------------------------- */
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 1; y < H - 1; y++) {
        sand_set(&s, wall, y, GLASS);
    }
    for (int y = H - 3; y < H - 1; y++) {
        for (int x = 1; x < wall; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    /* Soak until THE CELL THE SNOW WILL TOUCH is hot, not until any glass
     * anywhere is. Waiting on hottest_glass() passes as soon as some cell
     * up the wall gets there, which need not be the one being chilled a
     * moment later - a fixture that tests the wrong cell reports on the
     * wrong thing. */
    const int face = H - 2;
    int i;
    for (i = 0; i < 4000; i++) {
        const cell_t c = sand_at(&s, wall, face);
        if (CELL_MATERIAL(c) == MAT_GLASS &&
            CELL_VARIANT(c) >= SAND_SHOCK_HEAT) {
            break;
        }
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GLASS,
        CELL_MATERIAL(sand_at(&s, wall, face)),
        "fixture check: the wall cell being tested must survive the soak - "
        "if lava melted it there is nothing left to shatter");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_SHOCK_HEAT,
        CELL_VARIANT(sand_at(&s, wall, face)),
        "fixture check: lava held against a glass wall has to drive THAT "
        "cell past the shock threshold on its own, or the rest proves "
        "nothing about heat arriving from a real source");

    const int sand_before = count_cells_of(MAT_SAND);

    /* Topped up every step, the way a player pours rather than places.
     * Snow has scatter 90 - it drifts as it settles - so a single flake
     * set against the wall can wander off the one cell being tested
     * before the reactions pass ever looks at it, which makes a
     * single-placement version of this test a coin flip on the movement
     * RNG rather than a test of shock. */
    int cracked = 0;
    for (int k = 0; k < 60 && !cracked; k++) {
        for (int y = face - 1; y <= face; y++) {
            if (CELL_IS_EMPTY(sand_at(&s, wall + 1, y))) {
                sand_set(&s, wall + 1, y, SNOW);
            }
        }
        sand_step(&s, 0, 1000, 0);
        cracked = count_cells_of(MAT_SAND) > sand_before;
    }

    TEST_ASSERT_TRUE_MESSAGE(cracked,
        "snow banked against a wall that lava has heated from the far side "
        "must crack it - the gradient works whichever side the heat came "
        "from, which is the whole point of it being a gradient");

    /* --- and without snow it never cracks at all --------------------- */
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 1; y < H - 1; y++) {
        sand_set(&s, wall, y, GLASS);
    }
    for (int y = H - 3; y < H - 1; y++) {
        for (int x = 1; x < wall; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    const int dry_sand_before = count_cells_of(MAT_SAND);
    for (int k = 0; k < 900; k++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(dry_sand_before, count_cells_of(MAT_SAND),
        "the identical wall with no snow must never shatter, however long "
        "the lava works on it - lava MELTS glass and only cold SHATTERS it, "
        "and a test that cannot tell those apart would pass on a board "
        "where snow did nothing at all");
}



/* One shock takes the whole pane, not one cell of it.
 *
 * Shattering used to convert a single cell, so breaking a pane needed as
 * many separate successful shocks as it had cells - and each one needs
 * something cold touching glass that is still hot, at the moment it
 * touches. Getting that to happen once is the interesting part; needing it
 * sixty times in the same place is attrition, and on the board it read as
 * thermal shock barely working.
 *
 * It is also what glass does. A pane does not crumble cell by cell as each
 * part independently decides to - a crack starts somewhere and travels. */
static void test_one_shock_cracks_the_whole_pane(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* A pane the full width, hot, with a single flake at one END. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
    }
    sand_set(&s, 0, H - 3, SNOW);

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "a crack started at one end of a pane must run the length of it - "
        "the far end going too is the whole point, and it is why one "
        "successful shock is enough to matter");
}

/* But it does not jump a gap.
 *
 * The crack follows the material, so two panes that are not touching are
 * two panes. Without this the previous test passes just as well against
 * "any shock shatters all glass on the board", which would make glass
 * unusable anywhere near anything cold. */
static void test_a_crack_does_not_jump_to_a_separate_pane(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* Two panes with a clear gap between them. */
    const int gap = W / 2;
    int far_side = 0;
    for (int x = 0; x < W; x++) {
        if (x == gap || x == gap + 1) {
            continue;
        }
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
        if (x > gap) {
            far_side++;
        }
    }
    sand_set(&s, 0, H - 3, SNOW);

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(far_side, count_cells_of(MAT_GLASS),
        "the pane on the other side of the gap must be untouched - a crack "
        "runs through the material it is in, not through the air beside it");
}

/* The same snow on a cold pane does nothing at all.
 *
 * Which is what makes the rule about a GRADIENT rather than about snow
 * being corrosive to glass. Without this the previous test passes just as
 * happily against "snow destroys glass", a far worse rule that would make
 * the only acid-proof container in the game vulnerable to weather. */
static void test_cold_glass_is_unharmed_by_snow(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    const int panes = W - 2;
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, GLASS);
        sand_set(&s, x, H - 3, SNOW);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(panes, count_cells_of(MAT_GLASS),
        "snow on COLD glass must leave every pane intact - shock is about "
        "the temperature difference, not about snow being bad for glass");
}

/* Chilling costs the snow. It melts doing it.
 *
 * Snow that drained a glowing pane for free would be an unlimited heat
 * sink made of a material that arrives in a drift, so any pane anywhere
 * near weather would be unusable. Paying for the exchange is what keeps a
 * glass vessel over a fire a thing you can actually build. */
static void test_snow_melts_where_it_chills(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, SAND_SHOCK_HEAT - 1));
        sand_set(&s, x, H - 3, SNOW);
    }
    const int flakes = count_cells_of(MAT_SNOW);

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SNOW) < flakes,
        "snow that cools a hot pane must be spent doing it, or it is a "
        "free and permanent heat sink");
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_WATER) > 0,
        "and what it turns into is water, not nothing");
}

/* A re-initialised simulation remembers nothing about the old board.
 *
 * The may_have_* flags are an optimisation - they let whole passes be
 * skipped - so a stale one is not a wrong answer, it is a pass running
 * when it need not. A stale FALSE is the dangerous direction, and that is
 * what a missing reset produces on a fresh board.
 *
 * This is here because the omission hid a second bug rather than causing
 * one directly. may_have_temperature was not reset, the suite reuses one
 * static sand_t, so the flag arrived already true from whichever test ran
 * before - and the two tests written to prove the brush latched it could
 * not fail, because the thing they checked was true either way. Both bugs
 * were mine, in the same change, and the second made the first
 * untestable. */
static void test_reinitialising_forgets_the_old_board(void)
{
    fixture();
    sand_clear(&s);
    sand_set(&s, 1, 1, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_set(&s, 2, 1, GAS);
    sand_set(&s, 3, 1, FIRE);
    sand_set(&s, 4, 1, CELL_MAKE(MAT_ACID, MASS_MAX));
    sand_set(&s, 5, 1, SNOW);

    TEST_ASSERT_TRUE_MESSAGE(s.may_have_liquid && s.may_have_gas &&
                             s.may_have_burning && s.may_have_dissolver &&
                             s.may_have_temperature,
        "fixture check: this board has one of everything, so every flag "
        "should be set before we throw it away");

    fixture();

    TEST_ASSERT_FALSE_MESSAGE(s.may_have_liquid,      "liquid flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_gas,         "gas flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_burning,     "burning flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_dissolver,   "dissolver flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_temperature, "temperature flag leaked");
}

void run_sand_glass_thermal_suite(void)
{
    RUN_TEST(test_glass_banks_heat_rather_than_melting_on_contact);
    RUN_TEST(test_a_fire_held_long_enough_melts_glass_to_lava);
    RUN_TEST(test_glass_forgets_a_fire_that_went_out);
    RUN_TEST(test_glass_cools_on_a_board_with_no_fire_at_all);
    RUN_TEST(test_freshly_fused_glass_starts_cold);
    RUN_TEST(test_snow_shatters_a_glowing_pane_into_sand);
    RUN_TEST(test_the_shock_threshold_is_exact);
    RUN_TEST(test_glass_looks_different_at_the_shock_threshold);
    RUN_TEST(test_snow_frosts_a_resting_pane);
    RUN_TEST(test_frost_spreads_beyond_the_snow_touching_it);
    RUN_TEST(test_snow_keeps_on_ordinary_cold_glass);
    RUN_TEST(test_heat_arriving_at_frosted_glass_cracks_it);
    RUN_TEST(test_heat_arriving_at_resting_glass_only_warms_it);
    RUN_TEST(test_a_frosted_pane_warms_back_to_room_temperature);
    RUN_TEST(test_lava_one_side_snow_the_other_cracks_the_wall);
    RUN_TEST(test_one_shock_cracks_the_whole_pane);
    RUN_TEST(test_a_crack_does_not_jump_to_a_separate_pane);
    RUN_TEST(test_cold_glass_is_unharmed_by_snow);
    RUN_TEST(test_snow_melts_where_it_chills);
    RUN_TEST(test_reinitialising_forgets_the_old_board);
}

SUITE_REGISTER(run_sand_glass_thermal_suite);
