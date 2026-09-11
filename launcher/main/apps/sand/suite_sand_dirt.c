/*
 * Portable suite: the falling-sand automaton - dirt - soaking, drying, and
 * sand turning into soil.
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

/* ===================================================================
 * Dirt: soaking, drying, and sand turning into soil.
 * =================================================================== */

/* Wet sand slowly becomes soil, and the water is SPENT doing it.
 *
 * Consuming the liquid is what separates soaking from `thaws`, which snow
 * uses to melt in one - snow survives on what the liquid gives it for
 * free. A shoreline that turned to soil without the sea getting any
 * shallower would be making matter out of nothing. */
static void test_wet_sand_becomes_dirt_and_spends_the_water(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    const int water_before = liquid_mass_of(MAT_WATER);

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_DIRT) > 0,
        "sand left sitting under water must turn into dirt - slowly, but "
        "it must happen");
    TEST_ASSERT_TRUE_MESSAGE(liquid_mass_of(MAT_WATER) < water_before,
        "and the water must be spent doing it, or soil is being made out "
        "of nothing");
}

/* Dirt holds moisture in its variant, and gives it back up.
 *
 * Both halves matter: without soaking there is no wet soil for anything to
 * grow in, and without drying a single watering would make a patch fertile
 * forever, which turns watering from something you DO into something you
 * did once. */
static void test_dirt_takes_on_moisture_and_dries_out_again(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_DIRT, 0));
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    int wettest = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT && CELL_MOISTURE(c) > wettest) {
                wettest = CELL_VARIANT(c);
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(wettest > 0,
        "dirt under water must take moisture on - its variant is how wet "
        "it is");

    /* Take the water away and let it dry. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                sand_set(&s, x, y, 0);
            }
        }
    }
    int still_wet = 1;
    for (int i = 0; i < 4000 && still_wet; i++) {
        sand_step(&s, 0, 1000, 0);
        still_wet = 0;
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT && CELL_MOISTURE(c) != 0) {
                still_wet = 1;
            }
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(still_wet,
        "and with the water gone it must dry back out, or one watering "
        "makes a patch fertile for good");
}

/* Freshly drawn dirt is dry, and freshly poured dirt is banded the way a
 * freshly poured grain of sand is - see random_cell() (sand.c).
 *
 * A brushful is centred on ONE pour band with the same +/-1 jitter sand's
 * own shade uses, so it is NOT expected to use every one of
 * SOIL_DRY_TONES - it only has to be more than a single flat fill, which
 * is the flatness eight tones exist to fix. */
static void test_new_dirt_starts_dry_in_a_random_tone(void)
{
    fixture();
    sand_clear(&s);
    sand_spawn(&s, W / 2, H / 2, 2, MAT_DIRT);

    bool seen[SOIL_DRY_TONES];
    memset(seen, 0, sizeof seen);
    int distinct = 0;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) != MAT_DIRT) {
                continue;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_MOISTURE(c),
                "painted dirt must arrive bone dry - variant 0..7 is a dry "
                "tone and variant 8 and up is moisture (material.h), and "
                "soil that arrives already wet is fertile ground for "
                "free");
            const uint8_t tone = CELL_SOIL_TONE(c);
            TEST_ASSERT_TRUE_MESSAGE(tone < SOIL_DRY_TONES,
                "a freshly poured cell's tone must stay inside the dry "
                "range - anything at or past SOIL_DRY_TONES would alias a "
                "moisture level instead of a tone");
            if (!seen[tone]) {
                seen[tone] = true;
                distinct++;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(distinct > 1,
        "a brushful of dirt must show more than one tone - a single flat "
        "fill is exactly the flatness this many tones exist to avoid");
}

/* A SECOND pour, once the pour clock has moved on, must land on a
 * different band - that is what gives a bank built from several pours its
 * layers.
 *
 * `pour_phase` is jumped directly rather than run forward through 64 real
 * steps: scatter would carry grains sideways across the very columns this
 * test tells apart. */
static void test_consecutive_dirt_pours_land_on_different_bands(void)
{
    fixture();
    sand_clear(&s);

    sand_spawn(&s, 1, H / 2, 1, MAT_DIRT);
    int first_tone = -1;
    for (int y = 0; y < H && first_tone < 0; y++) {
        for (int x = 0; x < W / 2 && first_tone < 0; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_DIRT) {
                first_tone = CELL_SOIL_TONE(c);
            }
        }
    }

    /* One tick of POUR_BAND_SHIFT (sand.c, not exposed here - duplicated
     * by value with this comment tying the two together, the same
     * discipline MATERIAL_LIQUID_DEPTH_BAND's own comment (material.h)
     * asks of any two constants that have to agree). */
    s.pour_phase = 1u << 6;

    sand_spawn(&s, 6, H / 2, 1, MAT_DIRT);
    int second_tone = -1;
    for (int y = 0; y < H && second_tone < 0; y++) {
        for (int x = W / 2; x < W && second_tone < 0; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_DIRT) {
                second_tone = CELL_SOIL_TONE(c);
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(first_tone >= 0 && second_tone >= 0,
        "both pours must have landed something to compare");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(first_tone, second_tone,
        "a pour once the band has moved on must land on a different tone "
        "- two pours that always agreed would mean the band never moves "
        "at all");
}

/* A dry tone travels with the grain exactly the way a sand shade does -
 * see test_a_grain_keeps_its_shade_as_it_falls, whose pattern this
 * repeats for dirt. */
static void test_a_dry_dirt_grain_keeps_its_tone_as_it_falls(void)
{
    fixture();
    const cell_t grain = CELL_SOIL(MAT_DIRT, 5, 0);
    sand_set(&s, 3, 0, grain);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(grain, sand_at(&s, 3, 3),
        "a dry tone must travel with the grain, or a falling pile of dirt "
        "shimmers the way a falling pile of sand used to");
}

/* WETTING DISCARDS THE TONE, AND DRYING PICKS A FRESH ONE.
 *
 * A wet cell carries no tone of its own (material.h), so soil_dry_out()
 * reassigns one from scratch, biased by whatever is still wet nearby.
 * With stone on every side but the top there is nothing to bias from, so
 * a cell painted at tone 5 must come back at tone 0. */
static void test_soil_loses_its_tone_across_a_wetting_and_gets_a_fresh_one_drying(void)
{
    fixture();
    sand_clear(&s);

    const int x = W / 2, y = H / 2;
    sand_set(&s, x - 1, y, STONE);
    sand_set(&s, x + 1, y, STONE);
    sand_set(&s, x, y + 1, STONE);
    sand_set(&s, x, y, CELL_SOIL(MAT_DIRT, 5, SOIL_MOISTURE_MAX));

    int still_wet = 1;
    for (int i = 0; i < 4000 && still_wet; i++) {
        sand_step(&s, 0, 1000, 0);
        still_wet = CELL_MOISTURE(sand_at(&s, x, y)) != 0;
    }
    TEST_ASSERT_FALSE_MESSAGE(still_wet,
        "an isolated cell with nowhere to hand its moisture off to must "
        "still dry out on its own, through ambient decay alone");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_SOIL_TONE(sand_at(&s, x, y)),
        "with no neighbour left to bias from, a cell drying out must land "
        "on tone 0, not the tone - 5 - it was originally painted with");
}

/* THE DRYING-FRONT IMPRINT: a donor cell that empties itself by handing
 * its very last unit of moisture to a drier neighbour dries out biased by
 * THAT neighbour, not bone pale - see soil_dry_out() (sand_reactions.c).
 * It is what makes a pile that dried top-down legible as having dried
 * top-down.
 *
 * The four columns sit two apart so a diagonal percolation attempt from
 * one never reaches another, and there are four because which of
 * percolation or ambient decay fires first on any one column is a roll. */
static void test_soil_dries_biased_by_the_neighbour_it_just_watered(void)
{
    fixture();
    sand_clear(&s);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 0; x < W; x += 2) {
        sand_set(&s, x, H - 3, CELL_SOIL(MAT_DIRT, 5, 1));   /* about to run dry */
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 5, 0));   /* dry, room for water */
    }

    bool done[W];
    memset(done, 0, sizeof done);
    bool saw_any_dry = false, saw_biased_dry = false;

    for (int i = 0; i < 4000; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x += 2) {
            if (done[x]) {
                continue;
            }
            const cell_t donor = sand_at(&s, x, H - 3);
            if (CELL_MATERIAL(donor) != MAT_DIRT || CELL_MOISTURE(donor) != 0) {
                continue;
            }
            done[x] = true;
            saw_any_dry = true;
            const uint8_t sink_m = CELL_MOISTURE(sand_at(&s, x, H - 2));
            if (sink_m != 0 && CELL_SOIL_TONE(donor) == sink_m) {
                saw_biased_dry = true;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_any_dry,
        "at least one donor must actually dry out within the budget, or "
        "this proves nothing");
    TEST_ASSERT_TRUE_MESSAGE(saw_biased_dry,
        "at least one donor drying at the exact moment it hands its last "
        "unit of moisture to a drier neighbour must carry that "
        "neighbour's resulting moisture as its own dry tone - always "
        "landing on tone 0 regardless of what it just watered would mean "
        "the imprint was never wired in");
}

/* A WHOLE BANK, watered and then left to dry, must not come back flat.
 *
 * Every cell starts at the SAME tone deliberately: any spread at the end
 * is the drying itself talking, not the pour band it was laid down
 * with. */
#define DRY_BANK_W 16
#define DRY_BANK_H 14

static void test_a_watered_bank_does_not_dry_back_to_one_flat_tone(void)
{
    uint8_t *grid = malloc((size_t)DRY_BANK_W * DRY_BANK_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(grid,
        "the drying-bank grid must fit in what the framebuffer leaves");
    sand_t t;
    sand_init(&t, grid, DRY_BANK_W, DRY_BANK_H, 12345u);
    sand_clear(&t);
    sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < DRY_BANK_W; x++) {
        sand_set(&t, x, DRY_BANK_H - 1, STONE);
        for (int y = DRY_BANK_H - 6; y < DRY_BANK_H - 1; y++) {
            sand_set(&t, x, y, CELL_SOIL(MAT_DIRT, 0, 0));
        }
        for (int y = DRY_BANK_H - 9; y < DRY_BANK_H - 6; y++) {
            sand_set(&t, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    for (int i = 0; i < 3000; i++) {
        sand_step(&t, 0, 1000, 0);
    }
    for (int y = 0; y < DRY_BANK_H; y++) {
        for (int x = 0; x < DRY_BANK_W; x++) {
            if (CELL_MATERIAL(sand_at(&t, x, y)) == MAT_WATER) {
                sand_set(&t, x, y, 0);
            }
        }
    }

    bool wet = true;
    for (int i = 0; i < 40000 && wet; i++) {
        sand_step(&t, 0, 1000, 0);
        wet = false;
        for (int y = 0; y < DRY_BANK_H && !wet; y++) {
            for (int x = 0; x < DRY_BANK_W; x++) {
                const cell_t c = sand_at(&t, x, y);
                if (CELL_MATERIAL(c) == MAT_DIRT && CELL_MOISTURE(c) != 0) {
                    wet = true;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(wet,
        "the bank has to actually finish drying inside the budget, or "
        "everything below this is measuring a half-dry pile");

    int hist[SOIL_DRY_TONES];
    memset(hist, 0, sizeof hist);
    int total = 0;
    for (int y = 0; y < DRY_BANK_H; y++) {
        for (int x = 0; x < DRY_BANK_W; x++) {
            const cell_t c = sand_at(&t, x, y);
            if (CELL_MATERIAL(c) == MAT_DIRT) {
                hist[CELL_SOIL_TONE(c)]++;
                total++;
            }
        }
    }

    int distinct = 0, commonest = 0;
    for (int i = 0; i < SOIL_DRY_TONES; i++) {
        if (hist[i] != 0) {
            distinct++;
        }
        if (hist[i] > commonest) {
            commonest = hist[i];
        }
    }

    char why[192];
    snprintf(why, sizeof why,
             "%d cells, %d distinct tones, commonest holds %d "
             "(%d/%d/%d/%d/%d/%d/%d/%d)",
             total, distinct, commonest, hist[0], hist[1], hist[2],
             hist[3], hist[4], hist[5], hist[6], hist[7]);
    /* THREE tones and no tone holding half the bank: measured on this
     * scene it comes out 27/25/24/3/1/0/0/0 across 80 cells. Tone 6 or 7
     * needs soil that was ALREADY damp to hand to - a far bigger pile
     * than this - so asserting the whole range would be asserting a scene
     * this test does not build. The floor is where a regression lands:
     * lose the imprint and all eighty dry onto tone 0. */
    TEST_ASSERT_TRUE_MESSAGE(distinct >= 3 && commonest * 2 < total, why);

    free(grid);
}

/* ONE MONOTONE RAMP, not two independently-shifted tones that each had to
 * clear the other: soil's whole nibble is read by STATE - a dry tone
 * below SOIL_DRY_TONES, a moisture level from there up (material.h's own
 * comment) - so every variant must be strictly darker than the one
 * before it, the whole way from bone dry to saturated. */
static void test_soil_is_one_monotone_luminance_ramp(void)
{
    const gfx_color_t *pal = material_palette();
    const int top = SOIL_DRY_TONES - 1 + SOIL_MOISTURE_MAX;

    int prev_lum = 256; /* brighter than anything panel_luminance() can return */
    for (int v = 0; v <= top; v++) {
        const int lum = panel_luminance(pal[CELL_MAKE(MAT_DIRT, (uint8_t)v)]);
        char why[96];
        snprintf(why, sizeof why,
            "soil variant %d must be strictly darker than variant %d", v, v - 1);
        TEST_ASSERT_TRUE_MESSAGE(lum < prev_lum, why);
        prev_lum = lum;
    }

    /* Variant top+1 (15, unused) degrades to the saturated end rather
     * than rendering whatever an unrelated garbage colour a corrupt low
     * nibble would otherwise land on. */
    TEST_ASSERT_EQUAL_MESSAGE(pal[CELL_MAKE(MAT_DIRT, (uint8_t)top)],
        pal[CELL_MAKE(MAT_DIRT, (uint8_t)(top + 1))],
        "the unused top variant must degrade to the same colour as the "
        "saturated end, not an unrelated garbage colour");
}


/* Only WATER wets what it touches: wetness is not the same question as
 * fluidity, so the flag belongs to the liquid rather than to KIND_LIQUID,
 * which is wrong for three of the four liquids on this board.
 *
 * Oil is the liquid to test with. Acid dissolves sand and lava fuses it,
 * so with either of those "the sand is gone" proves nothing about
 * soaking; oil leaves it alone entirely. */
static void test_only_water_wets_what_it_touches(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 6));
        sand_set(&s, x, H - 3, CELL_SOIL(MAT_DIRT, 1, 0));
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_OIL, MASS_MAX));
    }

    for (int i = 0; i < 800; i++) {
        sand_step(&s, 0, 1000, 0);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) != MAT_DIRT) {
                    continue;
                }
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_MOISTURE(c),
                    "soil under OIL must stay bone dry - oil is a liquid "
                    "and is not wet, and the absorbing side cannot tell "
                    "the difference on its own");
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SAND) > 0,
        "and sand under oil must still be sand - it turned into a bank of "
        "saturated soil, which is the same bug seen from the other end");
}

/* Nothing soaks unless the simulation is told to let it.
 *
 * The override exists because soaking is a property of sand, and half the
 * suite puts sand in water to check that sand SINKS. Those tests are about
 * density and have no opinion about chemistry; they all broke the moment
 * soaking arrived switched on. */
static void test_soaking_is_off_unless_asked_for(void)
{
    fixture();
    sand_clear(&s);
    /* deliberately NOT calling sand_set_soak */

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_DIRT),
        "with soaking off, sand under water must stay sand - a mechanic "
        "that arrives switched on rewrites every scene that already "
        "existed");
}


/* A wetting front has to REACH: moving half the difference at each hop,
 * the ordinary way a diffusion settles, so soil several cells away from
 * anything the water touched must still end up wet - a front that hands
 * over only enough to leave the receiver below the pass-on threshold
 * dies at the first ring. */
static void test_a_wetting_front_spreads_past_the_cells_it_touched(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    /* One saturated grain of soil at one end, and no water anywhere: what
     * spreads has to be what that one cell is holding. */
    sand_set(&s, 0, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int reach = -1;
    for (int x = 0; x < W; x++) {
        if (CELL_MATERIAL(sand_at(&s, x, H - 2)) == MAT_DIRT) {
            reach = x;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(reach >= 3,
        "one saturated cell of soil must wet sand several cells away, not "
        "just the grain it touches - a front that hands over one level and "
        "leaves the receiver below the threshold to pass it on stops dead "
        "at the first ring");
}

/* And it must reach WITHOUT inventing water.
 *
 * The obvious way to make a front travel further is to give the receiver
 * more than the donor gives up, which spreads beautifully and quietly
 * creates moisture out of nothing - and moisture is what a plant spends,
 * so it would mean one watering could grow a forest. Half the difference
 * is exactly conservative; this is what says so. */
static void test_moisture_is_conserved_as_it_spreads(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    sand_set(&s, 0, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    const int placed = (int)SOIL_MOISTURE_MAX;

    /* Drying only ever removes moisture, so the total can fall but must
     * never rise above what was put in. */
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        int total = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) == MAT_DIRT) {
                    total += CELL_MOISTURE(c);
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(total <= placed,
            "spreading moisture must move it, not multiply it - a front "
            "that gains on every hop is a watering can that fills itself");
    }
}


/* And what it hands over is a SHARE, not a token: a converted grain must
 * arrive able to carry the front on itself, holding enough to wet a
 * neighbour in turn - one holding less would make every new grain a dead
 * end, creeping outward only as fast as the original cell could top it
 * up. */
static void test_soil_a_wetting_front_converts_is_handed_a_real_share(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    sand_set(&s, 0, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));

    /* Caught the step it happens, before drying has taken any of it. */
    int handed = -1;
    for (int i = 0; i < 400 && handed < 0; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 1, H - 2);
        if (CELL_MATERIAL(c) == MAT_DIRT) {
            handed = CELL_MOISTURE(c);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(handed >= 0,
        "the grain beside saturated soil must become soil at all");
    TEST_ASSERT_TRUE_MESSAGE(handed >= (int)SOIL_MOISTURE_MAX / 2,
        "and must be handed a real share of what wet it - a grain born "
        "holding 1 is below the level it needs to wet anything itself, "
        "which makes every cell the front converts a dead end");
}


/* A shattered pane comes back as CULLET, not as beach.
 *
 * Sand's variant is a shade, so recording a grain's glass origin costs
 * four of the sixteen shades it could have had, and buys wreckage that
 * mixes into an ordinary dune without becoming it.
 *
 * The second assert is the one that matters for how it LOOKS: a band that
 * is not varied inside itself is a slab of colour. */
static void test_a_shattered_pane_comes_back_as_cullet(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, 0));   /* fully frosted */
    }

    for (int i = 0; i < 60 && count_cells_of(MAT_SAND) == 0; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SAND) > 0,
        "the pane has to have actually shattered");

    int shades[MATERIAL_VARIANTS] = { 0 };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) != MAT_SAND) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) >= SAND_CULLET_BASE,
                "sand from a shattered pane must land in the cullet band - "
                "the top of the dune ramp is still the dune ramp, and pale "
                "tan reads as sand rather than as broken glass");
            shades[CELL_VARIANT(c)]++;
        }
    }

    int distinct = 0;
    for (int v = SAND_CULLET_BASE; v < MATERIAL_VARIANTS; v++) {
        distinct += shades[v] ? 1 : 0;
    }
    /* Three of the four, not merely "more than one". The band's top value
     * is exactly what the general placement helper hands a new cell, so a
     * crack that forgot to vary its shade still lands IN the band - a
     * mutation reverting the spread to place_reacted() passed a
     * two-distinct check on the strength of the one cell that starts the
     * crack. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(2, distinct,
        "and must vary across the band - one flat value over a whole pane "
        "is a slab of colour, which is what this replaced");
}

/* Cullet does not drink, and so never binds into soil: the shards are
 * glass, with none of the pore space that binds wet dune grains. The water
 * assert catches a fix made at the conversion alone - a shard that refuses
 * to become soil but still spends the water it stood in drains a lake for
 * nothing. */
static void test_cullet_neither_drinks_water_nor_turns_into_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, SAND_CULLET_BASE));
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    const int water_before = liquid_mass_of(MAT_WATER);

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_DIRT),
        "cullet left sitting under water must stay cullet - glass has no "
        "pore space to bind a grain with");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W, count_cells_of(MAT_SAND),
        "and none of it may go missing along the way");
    TEST_ASSERT_EQUAL_INT_MESSAGE(water_before, liquid_mass_of(MAT_WATER),
        "nor may the water be spent on it - refusing to become soil while "
        "still drinking would drain a lake for nothing");
}

/* Soaking converts a grain three ways - standing in water, a wet neighbour
 * handing over moisture, percolation from above - so a fix covering only
 * the first still turns a cullet bed under a watered bank into dirt, which
 * is what a player gets after breaking a window over soil. */
static void test_wet_soil_does_not_bind_cullet_from_above(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, SAND_CULLET_BASE));
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_DIRT, 0));
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(W, count_cells_of(MAT_DIRT),
        "the watered bank must not have grown down into the cullet bed");
    for (int x = 0; x < W; x++) {
        const cell_t c = sand_at(&s, x, H - 2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(c),
            "every shard under the bank has to still be sand");
        TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) >= SAND_CULLET_BASE,
            "and still in the cullet band - soaking must not have quietly "
            "restyled it as a dune shade either");
    }
}

/* And painted sand must never land in that band.
 *
 * A reserved band only means anything if it is reserved. Without this the
 * ordinary brush would scatter grains claiming to be broken glass through
 * every dune on the board, at a quarter of them. */
static void test_painted_sand_stays_out_of_the_cullet_band(void)
{
    fixture();
    sand_clear(&s);
    sand_spawn(&s, W / 2, H / 2, 3, MAT_SAND);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) != MAT_SAND) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) < SAND_CULLET_BASE,
                "a painted grain must be a dune shade - the cullet band is "
                "reserved, and a brush that reaches into it makes every "
                "pile look like it has broken glass mixed through it");
        }
    }
}

/* The two bands have to be different colours, and not merely different
 * ends of one ramp.
 *
 * This is the whole point and nothing else would catch it: cullet built
 * from sand's own colours would pass every test above while looking
 * exactly like pale sand, which is what it looked like before. */
static void test_cullet_does_not_look_like_sand(void)
{
    const gfx_color_t *pal = material_palette();

    for (int c = SAND_CULLET_BASE; c < MATERIAL_VARIANTS; c++) {
        for (int d = 0; d < SAND_CULLET_BASE; d++) {
            char why[96];
            snprintf(why, sizeof why, "cullet %d against dune shade %d", c, d);
            TEST_ASSERT_TRUE_MESSAGE(
                pal[CELL_MAKE(MAT_SAND, c)] != pal[CELL_MAKE(MAT_SAND, d)],
                why);
        }
    }
}


/* Water reaches the BOTTOM of a submerged pile.
 *
 * Diffusion alone cannot: half-the-difference settles into a gradient of
 * one level per cell and stops there, because half of a gap of one is
 * zero, so the depth reached is set by the size of the moisture range
 * rather than by how much water is standing on the pile. Gravity is what
 * carries it down - percolation needs no gradient, only room in the cell
 * it is going to. */
static void test_water_percolates_to_the_bottom_of_a_submerged_pile(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        for (int y = 2; y < H - 1; y++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_SAND, 6));
        }
    }

    /* Saturated, not merely damp - and that is the whole point of the
     * assert. This grid is shallower than the moisture range, so a pile
     * of it can be wet to the floor by diffusion alone; what diffusion
     * cannot do is SATURATE the bottom, because it settles at one level
     * per cell and the surface only ever holds the maximum. A full bottom
     * row is a water table, and only something that runs downhill without
     * needing a gradient builds one. */
    int wet_floor = 0;
    for (int i = 0; i < 1200 && !wet_floor; i++) {
        /* Held under: the pile has standing water on it throughout. */
        for (int x = 0; x < W; x++) {
            for (int y = 0; y < 2; y++) {
                if (CELL_IS_EMPTY(sand_at(&s, x, y))) {
                    sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
                }
            }
        }
        sand_step(&s, 0, 1000, 0);

        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT &&
                CELL_MOISTURE(c) == SOIL_MOISTURE_MAX) {
                wet_floor = 1;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(wet_floor,
        "the deepest row of a submerged pile must SATURATE - diffusion "
        "settles into a gradient of one level per cell and stops there, "
        "which leaves the bottom of a pile drier than the top for ever, "
        "however much water is standing on it");
}


/* Percolation goes down and SIDEWAYS-down, not straight down: fingers
 * that wander, split and join, rather than a flat sheet of damp
 * descending one row at a time, and the difference between water getting
 * past an obstacle and water stopping at one.
 *
 * The wet cell is walled in on both sides and directly beneath, so the
 * only way out is diagonal. */
static void test_water_percolates_diagonally_as_well_as_straight_down(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* One row above the floor, so the grains the water has to reach
     * are resting on it - a grain with empty space under it falls
     * out of the scene before any of this gets a turn. */
    const int cx = W / 2, cy = H - 3;

    sand_set(&s, cx, cy, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    sand_set(&s, cx - 1, cy, STONE);          /* no way out sideways */
    sand_set(&s, cx + 1, cy, STONE);
    sand_set(&s, cx, cy + 1, STONE);          /* nor straight down */
    sand_set(&s, cx - 1, cy + 1, CELL_MAKE(MAT_SAND, 6));   /* only these */
    sand_set(&s, cx + 1, cy + 1, CELL_MAKE(MAT_SAND, 6));

    int reached = 0;
    for (int i = 0; i < 600 && !reached; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int d = -1; d <= 1; d += 2) {
            if (CELL_MATERIAL(sand_at(&s, cx + d, cy + 1)) == MAT_DIRT) {
                reached = 1;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(reached,
        "water walled in on both sides and underneath must still get out "
        "diagonally - percolation that only goes straight down is a "
        "rising damp, not water soaking into sand");
}

void run_sand_dirt_suite(void)
{
    RUN_TEST(test_wet_sand_becomes_dirt_and_spends_the_water);
    RUN_TEST(test_dirt_takes_on_moisture_and_dries_out_again);
    RUN_TEST(test_new_dirt_starts_dry_in_a_random_tone);
    RUN_TEST(test_consecutive_dirt_pours_land_on_different_bands);
    RUN_TEST(test_a_dry_dirt_grain_keeps_its_tone_as_it_falls);
    RUN_TEST(test_soil_loses_its_tone_across_a_wetting_and_gets_a_fresh_one_drying);
    RUN_TEST(test_soil_dries_biased_by_the_neighbour_it_just_watered);
    RUN_TEST(test_a_watered_bank_does_not_dry_back_to_one_flat_tone);
    RUN_TEST(test_soil_is_one_monotone_luminance_ramp);
    RUN_TEST(test_only_water_wets_what_it_touches);
    RUN_TEST(test_soaking_is_off_unless_asked_for);
    RUN_TEST(test_a_wetting_front_spreads_past_the_cells_it_touched);
    RUN_TEST(test_moisture_is_conserved_as_it_spreads);
    RUN_TEST(test_soil_a_wetting_front_converts_is_handed_a_real_share);
    RUN_TEST(test_a_shattered_pane_comes_back_as_cullet);
    RUN_TEST(test_painted_sand_stays_out_of_the_cullet_band);
    RUN_TEST(test_cullet_does_not_look_like_sand);
    RUN_TEST(test_cullet_neither_drinks_water_nor_turns_into_soil);
    RUN_TEST(test_wet_soil_does_not_bind_cullet_from_above);
    RUN_TEST(test_water_percolates_to_the_bottom_of_a_submerged_pile);
    RUN_TEST(test_water_percolates_diagonally_as_well_as_straight_down);
}

SUITE_REGISTER(run_sand_dirt_suite);
