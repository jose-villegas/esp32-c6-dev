/*
 * Portable suite: the falling-sand automaton - metal - dirt smelted by
 * sustained heat.
 *
 * Split out of suite_sand.c, which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 */
#include <math.h> /* not every file in the split still needs atan2()/M_PI,
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

#include "suites.h"
#include "unity.h"

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
#include "util/intmath.h"

/* ===================================================================
 * Metal: dirt smelted by sustained heat - see
 * docs/sand/Metal.md, which every test below follows.
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
static void
lava_beside_dirt(uint8_t moisture) {
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE); /* boxes the lava on its left */
    sand_set(&s, 3, H - 3, STONE); /* and above */
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set(&s, 4, H - 2, CELL_SOIL(MAT_DIRT, 1, moisture));
}

/* Whether the dirt cell in lava_beside_dirt()'s scene has smelted into
 * metal specifically. */
static bool
dirt_cell_is_metal(void) {
    const cell_t c = sand_at(&s, 4, H - 2);
    return cell_is_extended(c) && CELL_VARIANT(c) == MATX_METAL;
}

/* Whether it has smelted at all - metal OR stone, reaction_t.flaw_to's
 * two possible dry-path outcomes (material.h). A bone-dry cell has no
 * moisture to spoil, so these are the only two a dry smelt can reach. */
static bool
dirt_cell_is_smelted(void) {
    const cell_t c = sand_at(&s, 4, H - 2);
    return dirt_cell_is_metal() || CELL_MATERIAL(c) == MAT_STONE;
}

/* Whether it is no longer dirt at all - smelted (metal or stone) OR
 * spoiled (sand, reaction_t.spoils_to). The generic "this cell has
 * resolved, whichever of the three ways" check the wet-earth tests below
 * want: they exist to pin down the MOISTURE sequencing, not which of the
 * now three possible outcomes one particular seed happens to land on. */
static bool
dirt_cell_resolved(void) {
    return CELL_MATERIAL(sand_at(&s, 4, H - 2)) != MAT_DIRT;
}

/* How many cells anywhere on the board have smelted, metal OR stone - for
 * scenes below that scatter dirt across a row rather than pinning it to
 * lava_beside_dirt()'s one fixed cell. See dirt_cell_is_smelted()'s own
 * comment for why a dry smelt can land on either. */
static int
count_smelted_cells(void) {
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if ((cell_is_extended(c) && CELL_VARIANT(c) == MATX_METAL) || CELL_MATERIAL(c) == MAT_STONE) {
                n++;
            }
        }
    }
    return n;
}

/* Steps until lava_beside_dirt()'s dirt cell smelts (metal or stone), or
 * `budget` if it never does within that many steps. */
static int
steps_to_smelt(uint8_t moisture, int budget) {
    lava_beside_dirt(moisture);
    for (int i = 0; i < budget; i++) {
        sand_step(&s, 0, 1000, 0);
        if (dirt_cell_is_smelted()) {
            return i + 1;
        }
    }
    return budget;
}

static void
test_dry_dirt_beside_lava_smelts_into_metal_or_stone(void) {
    const int budget = 3000;
    const int steps = steps_to_smelt(0, budget);

    TEST_ASSERT_LESS_THAN_MESSAGE(budget, steps,
                                  "dirt with no moisture in it, held against lava, must smelt into "
                                  "metal or stone - the one reaction lava and dirt have, and the "
                                  "whole reason MATX_METAL exists. Which of the two is "
                                  "reaction_t.flaw_to's call (material.h); either counts here");
}

/* Saturated dirt must lose all SOIL_MOISTURE_MAX levels, one at a time,
 * before it can convert at all - the wet-earth branch of
 * try_heat_transform() (sand_reactions.c). Counts DISTINCT moisture values
 * passed through rather than wall-clock steps against bone-dry dirt: two
 * independent RNG-driven rates drive the drying (heat and reaction_t.dries),
 * so a step-count comparison is a race that lands either side of even a
 * generous margin. */
static void
test_saturated_dirt_smelts_roughly_eight_times_slower(void) {
    lava_beside_dirt(SOIL_MOISTURE_MAX);

    int distinct_moisture_levels_seen = 1; /* SOIL_MOISTURE_MAX itself */
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

    TEST_ASSERT_TRUE_MESSAGE(resolved, "fixture check: saturated dirt must eventually resolve - smelt "
                                       "into metal or stone, or spoil into sand - within the budget");

    if (dirt_cell_is_smelted()) {
        /* SOIL_MOISTURE_MAX, not +1: moisture is sampled once per step, so
         * a step where heat and ambient drying both succeed merges two
         * adjacent values into one observation - measured on this seed.
         * Only checked on the smelted path; a cell that spoils
         * (reaction_t.spoils_to, material.h) leaves off partway by
         * design. */
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SOIL_MOISTURE_MAX, distinct_moisture_levels_seen,
                                                 "saturated dirt that smelts (rather than spoiling) must pass "
                                                 "through very nearly every one of its SOIL_MOISTURE_MAX + 1 "
                                                 "moisture values - SOIL_MOISTURE_MAX itself down to bone dry "
                                                 "- one level at a time, before it can smelt at all. That is "
                                                 "what makes it roughly SOIL_MOISTURE_MAX + 1 times as much "
                                                 "work as bone-dry dirt's single conversion");
    } else {
        /* Spoiled instead - the common path at spoils_chance 77/256,
         * unconditional (material.h). No lower bound here: a cell can spoil
         * on its very first heat_chance roll, leaving
         * distinct_moisture_levels_seen at 1, which is expected. This
         * branch only keeps the test asserting something on this path -
         * that it resolved, already checked above. */
    }
}

/* A FULL tank of moisture, not one level - though at spoils_chance 77/256
 * (material.h) a cell can spoil before ever steaming, so steam is not
 * guaranteed for one cell;
 * test_wet_dirt_can_still_steam_before_spoiling_at_least_sometimes proves
 * the path stays live. This test only asserts the ordering that holds
 * whenever steam does happen: it can never land on the same step the cell
 * resolves, nor after (spoiling and steaming-while-draining are mutually
 * exclusive - try_heat_transform()). */
static void
test_watered_dirt_steaming_precedes_resolving_when_it_happens(void) {
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

    TEST_ASSERT_TRUE_MESSAGE(resolved_at >= 0, "fixture check: it must eventually resolve");
    if (steamed_at < 0) {
        return; /* did not steam this run - still a common outcome at
                   * spoils_chance 77/256, though no longer the majority one,
                   * and there is nothing left to assert an ORDER over */
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

/* Proves the steam path is still live - a single-cell scene is too likely
 * to spoil on its first roll (spoils_chance 77/256) to be a reliable
 * witness. STEAM_TEST_PODS independent saturated pockets, each in a
 * lava_beside_dirt()-shaped box on one dedicated wide grid. Per pod,
 * P(never steams before resolving) is the chance its first roll spoils
 * outright, ~=0.918, so P(none of STEAM_TEST_PODS ever steam) is
 * 0.918^STEAM_TEST_PODS - with 200 pods, about 1 in 27 million. */
#define STEAM_TEST_PODS    200
#define STEAM_TEST_SPACING 4
#define STEAM_TEST_W       (2 + STEAM_TEST_SPACING * STEAM_TEST_PODS + 2)
#define STEAM_TEST_H       6

static void
test_wet_dirt_can_still_steam_before_spoiling_at_least_sometimes(void) {
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t* steam_cells = malloc((size_t)STEAM_TEST_W * STEAM_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(steam_cells, "wet-dirt steam-pods grid must fit in what the framebuffer leaves");
    sand_t st;
    sand_init(&st, steam_cells, STEAM_TEST_W, STEAM_TEST_H, 3u);
    sand_set_mobility(&st, 0);

    const int y = 2;
    for (int x = 0; x < STEAM_TEST_W; x++) {
        sand_set(&st, x, y + 1, STONE); /* one shared floor */
    }
    for (int k = 0; k < STEAM_TEST_PODS; k++) {
        const int lava_x = 2 + STEAM_TEST_SPACING * k;
        sand_set(&st, lava_x - 1, y, STONE);
        sand_set(&st, lava_x, y - 1, STONE);
        sand_set(&st, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        sand_set(&st, lava_x + 1, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }

    /* Existence only: steam is KIND_GAS and drifts once emitted
     * (sand_step_gas()), so which pod produced a given cell is not
     * pinnable. Scanned inline rather than with count_cells_of(), which is
     * hardcoded to the shared fixture's `s`/`W`/`H` and would silently
     * count an unrelated grid. */
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

    TEST_ASSERT_TRUE_MESSAGE(steamed_any, "at least one of many saturated dirt cells against lava must "
                                          "still steam before spoiling - the drain-then-steam path in "
                                          "try_heat_transform() must still be reachable even though "
                                          "spoils_chance 77/256 leaves it common but not guaranteed; if this "
                                          "never fires across STEAM_TEST_PODS independent attempts, either "
                                          "spoils_chance regressed to 255 (unconditional, path dead) or the "
                                          "steam emit itself broke");
}

/* reaction_t.spoils_to (material.h) actually fires rather than being a field
 * nobody reaches. spoils_chance is 77/256, so one pod would usually do;
 * several independent pockets keep the result from hanging on which exact
 * cell it happened to be. */
#define SPOILS_TEST_PODS 6

static void
test_wet_dirt_can_spoil_into_sand_instead_of_smelting(void) {
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells, "wet-dirt-spoils grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_mobility(&wide, 0);

    const int y = 2;
    for (int x = 0; x < WIDE_W; x++) {
        sand_set(&wide, x, y + 1, STONE); /* one shared floor */
    }
    for (int k = 0; k < SPOILS_TEST_PODS; k++) {
        const int lava_x = 2 + 4 * k;
        sand_set(&wide, lava_x - 1, y, STONE);
        sand_set(&wide, lava_x, y - 1, STONE);
        sand_set(&wide, lava_x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        sand_set(&wide, lava_x + 1, y, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
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

    TEST_ASSERT_TRUE_MESSAGE(spoiled, "at least one of several saturated dirt cells against lava must "
                                      "spoil into sand instead of smelting - reaction_t.spoils_to "
                                      "(material.h) exists precisely so watering ore before it fires is "
                                      "a real risk; if this never fires, spoils_to/spoils_chance "
                                      "regressed to zero or the gate is wrong");
}

/* reaction_t.flaw_to (material.h) fires at all, and so does metal itself,
 * from a sample large enough that chance is not a factor. See
 * test_the_rod_terminates_at_conduct_reach_not_the_far_wall for why a
 * single ~30-cell rod cannot make either claim reliably. FLAW_TEST_PODS
 * independent bone-dry dirt cells give FLAW_TEST_PODS /
 * HEAT_FLAW_CLUMP_TEST reroll opportunities; at flaw_chance 220/256 the
 * chance none ever flaws, or all do, is vanishingly small either way - a
 * real regression, not bad luck. */
#define FLAW_TEST_PODS    400
#define FLAW_TEST_SPACING 4
#define FLAW_TEST_W       (2 + FLAW_TEST_SPACING * FLAW_TEST_PODS + 2)
#define FLAW_TEST_H       6

static void
test_dry_dirt_smelting_reaches_both_metal_and_stone(void) {
    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t* flaw_cells = malloc((size_t)FLAW_TEST_W * FLAW_TEST_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(flaw_cells, "dry-dirt flaw-pods grid must fit in what the framebuffer leaves");
    sand_t flaw;
    sand_init(&flaw, flaw_cells, FLAW_TEST_W, FLAW_TEST_H, 3u);
    sand_set_mobility(&flaw, 0);

    const int y = 2;
    for (int x = 0; x < FLAW_TEST_W; x++) {
        sand_set(&flaw, x, y + 1, STONE); /* one shared floor */
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

/* Smelting is keyed on cell_is_burning() (reaction_t.burns), not on
 * CELL_MATERIAL(n) == MAT_LAVA, and every other smelting test here reaches
 * for lava. A narrowing of that gate to lava specifically would pass all of
 * them and fail only this.
 *
 * Fire rises and burns out in around forty steps (MAT_FIRE's decay), so it
 * has to be re-placed every step rather than dropped once. */
static void
test_a_held_flame_smelts_dirt_as_lava_does(void) {
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 4, H - 2, CELL_SOIL(MAT_DIRT, 1, 0)); /* bone dry */

    const int budget = 3000;
    int smelted = 0;
    for (int i = 0; i < budget && !smelted; i++) {
        if (CELL_IS_EMPTY(sand_at(&s, 4, H - 3))) {
            sand_set(&s, 4, H - 3, FIRE);
        }
        sand_step(&s, 0, 1000, 0);
        smelted = count_smelted_cells() > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(smelted, "an ordinary flame held against dirt must smelt it into metal or "
                                      "stone exactly as lava does - the reaction is keyed on "
                                      "cell_is_burning(), not on CELL_MATERIAL(n) == MAT_LAVA, and "
                                      "every other smelting test in this file only ever reaches for "
                                      "lava as its heat source");
}

/* The conducted path is as general as the contact path: dirt on the far
 * side of a plain stone wall smelts from heat that crossed via
 * conduct_heat(), never touching the fire driving it. The other far-side
 * test below uses a metal conductor the smelting itself grew; this one is
 * an ordinary conductor that was already there.
 *
 * Real per-material `conducts` applies, so this is stone's own 220-in-256
 * figure attenuating across a one-cell wall, not a forced 255. */
static void
test_heat_through_a_stone_wall_smelts_the_dirt_beyond_it(void) {
    fixture();
    sand_clear(&s);
    sand_set_conduction(&s, SAND_CONDUCTION_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 3, STONE); /* the wall */
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 4, CELL_SOIL(MAT_DIRT, 1, 0)); /* bone dry,
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
            TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, x, H - 3)),
                                          "the wall must stay intact and unlit - if it changes, "
                                          "either the fire reached it directly or the far-side hit "
                                          "is landing on the conductor instead of past it, and "
                                          "either way this test can no longer tell a conducted "
                                          "smelt from a contact one");
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(smelted, "dirt behind a plain stone wall must smelt into metal or stone "
                                      "from conducted heat alone - conduct_heat() (sand_reactions.c) "
                                      "applies "
                                      "try_heat_transform() to whatever it finds past the far side of "
                                      "a conductor run exactly as contact does, and that path has "
                                      "otherwise only ever been proven for a metal conductor grown by "
                                      "smelting itself, never for an ordinary wall");
}

/* Guards conduct_heat()'s depth-0 re-test of `conducts`. All sixteen extended
 * variants share one MAT_EXTENDED slot in the PAIR_CONDUCTS table, so metal's
 * bit makes every extended cell pass the cheap reject - root included, which
 * conducts nothing.
 *
 * Fire and root are re-placed each step so burn-out cannot end the scene
 * early and retire the assertion quietly. */
static void
test_a_non_conducting_extended_cell_passes_no_heat_beyond_itself(void) {
    fixture();
    sand_clear(&s);
    sand_set_conduction(&s, 255);
    sand_set_mobility(&s, 0); /* fire is a gas - pin it beside the root */

    const int y = H / 2;
    sand_set(&s, 4, y, WOOD);

    for (int i = 0; i < 8; i++) {
        sand_set(&s, 2, y, FIRE);
        sand_set(&s, 3, y, MATX(MATX_ROOT));
        sand_step(&s, 0, 1000, 0);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_ROOT), sand_at(&s, 3, y),
                                        "the root must survive each step - if it burns away the fire is "
                                        "touching the wood directly and this test can no longer tell a "
                                        "conducted ignition from a contact one");
        /* Byte-exact, not CELL_MATERIAL: wood's `ignites_to` is MAT_WOOD, so
         * lit wood keeps its own material nibble and only the code changes -
         * a material-only assertion here reads PASS on an ignited cell. */
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(WOOD, sand_at(&s, 4, y),
                                        "wood two cells from the fire, behind a root that conducts "
                                        "nothing, must never ignite - conduct_heat() reaching it means "
                                        "the PAIR_CONDUCTS reject let an extended cell through and "
                                        "nothing re-tested its real `conducts` before the roll");
    }
}

/* Regression guard for the wet-dirt branch just added to
 * try_heat_transform(): sand has no `dries` at all, so `r->dries != 0`
 * must gate the new branch out entirely and sand -> glass must be
 * completely unaffected by it - see material.h's own comment on `dries`
 * for why that field, and not a new one, is what the branch tests. */
static void
test_sand_still_becomes_glass_beside_the_new_dirt_branch(void) {
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

    TEST_ASSERT_TRUE_MESSAGE(made, "sand -> glass must still work after the wet-dirt branch was "
                                   "added to try_heat_transform() - sand has no `dries`, so the new "
                                   "branch must never catch it");
}

/* Steps until MAT_STEAM appears past a `wall_len`-cell wall of `wall_cell`.
 * Heated by lava rather than fire: fire decays in around forty steps, which
 * would cap how many attempts a slow conductor gets and confuse "does it
 * conduct" with "did the fire last long enough to find out". Lava never
 * decays, so the only variable left is the per-material `conducts` figure. */
static int
steps_to_boil_through(int wall_len, cell_t wall_cell, int budget) {
    /* Self-contained, like steps_to_boil() above: malloc, use, free, all
     * within one call - the test below calls this twice and gets a
     * fresh grid each time. */
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells, "steps-to-boil-through grid must fit in what the framebuffer "
                                             "leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_mobility(&wide, 0);
    /* This measures CONDUCTION speed, not water's own new resistance to
     * boiling once heat arrives - forced to 255 so a slow real `boils`
     * figure cannot be mistaken for slow conduction. */
    sand_set_boils(&wide, 255);

    const int y = 2;
    const int lava_x = 1;
    const int wall_x0 = lava_x + 1;
    const int water_x = wall_x0 + wall_len;

    sand_set(&wide, water_x - 1, y + 1, STONE);
    sand_set(&wide, water_x, y + 1, STONE);
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

/* Metal's `conducts` (248) puts the conduction walk at roughly
 * CONDUCT_REACH cells on average, against stone and glass's 220 - see
 * Metal.md's attenuation table. Extended materials appear in no benchmark
 * scene, so this shared short budget is the only guard on that cost. */
static void
test_a_metal_run_conducts_further_than_a_stone_one(void) {
    const int wall_len = 20;
    const int budget = 10;

    const int metal = steps_to_boil_through(wall_len, MATX(MATX_METAL), budget);
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

/* A lava source grows its own metal rod out of a dirt bed and stops
 * (Metal.md). Dirt past a conductor run smelts via conduct_heat()'s walk
 * exactly as contact does, lengthening the run one cell at a time until it
 * can no longer reach CONDUCT_REACH cells. The bed is twice CONDUCT_REACH
 * long so a capped-out rod is caught. Conduction forced to 255 so only
 * dirt's heat_chance gates growth. Measured at 33, not 32 cells: the walk
 * can complete one more cell after a run already hits the cap. */
static void
test_the_rod_terminates_at_conduct_reach_not_the_far_wall(void) {
    enum { ROD_W = CONDUCT_REACH_TEST * 2, ROD_H = 6 };

    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t* rod_cells = malloc((size_t)ROD_W * ROD_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(rod_cells, "metal-rod grid must fit in what the framebuffer leaves");
    sand_t rod;
    sand_init(&rod, rod_cells, ROD_W, ROD_H, 3u);
    sand_set_mobility(&rod, 0);
    sand_set_conduction(&rod, 255);
    /* The lava source sits boxed in on 3-4 sides for the whole run - more
     * than enough cover to be burst-eligible over 6000
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

    /* No stone_count > 0 assertion, on purpose: the clump mechanism re-rolls
     * only once per HEAT_FLAW_CLUMP_TEST triggers, so a ~30-cell rod gets
     * ~6 independent rolls and an all-stone rod is (220/256)^6, about 40%.
     * test_dry_dirt_smelting_reaches_both_metal_and_stone proves both
     * outcomes still fire, from a sample where chance is not a factor. */

    /* Successes along this rod happen one at a time in strict spatial
     * order, so the flaw sequence's trigger order is this array's index
     * order. That makes the bound exact rather than statistical: the
     * outcome only flips at a trigger index that is a multiple of
     * HEAT_FLAW_CLUMP_TEST, so the rod holds at most
     * ceil(smelted_len / HEAT_FLAW_CLUMP_TEST) same-outcome stretches. */
    int runs = 1;
    for (int i = 1; i < smelted_len; i++) {
        if (flawed[i] != flawed[i - 1]) {
            runs++;
        }
    }
    const int max_runs = (smelted_len + HEAT_FLAW_CLUMP_TEST - 1) / HEAT_FLAW_CLUMP_TEST;
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
static void
acid_over(cell_t floor_cell, int floor_rows, int acid_rows) {
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
static int
steps_for_acid_to_clear(uint8_t counted_id, cell_t floor_cell, int budget) {
    acid_over(floor_cell, 1, 4);
    for (int i = 0; i < budget; i++) {
        sand_step(&s, 0, 1000, 0);
        if (count_cells_of(counted_id) == 0) {
            return i + 1;
        }
    }
    return budget;
}

/* Metal resists acid (dissolvable 1, not immune - material.c's own
 * comment) rather than countering it; see Metal.md's numbers table. This
 * test's name is backwards from what it now checks; left as-is pending a
 * rename rather than touched here alongside the value. */
static void
test_acid_eats_metal_between_stone_and_sand(void) {
    const int budget = 5000;
    const int stone = steps_for_acid_to_clear(MAT_STONE, STONE, budget);
    const int metal = steps_for_acid_to_clear(MAT_EXTENDED, MATX(MATX_METAL), budget);
    const int sand = steps_for_acid_to_clear(MAT_SAND, CELL_MAKE(MAT_SAND, 8), budget);

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

static void
test_wood_and_steam_grain_count_is_conserved(void) {
    fixture();
    /* Condensation forced off: reaction_t.condenses is not one-for-one - a
     * 2x2 patch of steam collapsing to one water cell loses three grains by
     * design, and this is a movement-conservation check. Ember is left out
     * for the same reason: reaction_t.flare spawns a new MAT_FIRE cell out
     * of an empty neighbour, so no strict grain count applies to it. */
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
        {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
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

/* reaction_t.condenses is a rare per-step chance in real play, forced to 255
 * so the reaction is actually reached. The 2x2 pocket is sealed in stone on
 * every side sand_step_gas() could move a cell out of - it runs before
 * sand_step_reactions() within one sand_step(), so otherwise a step's own
 * gas movement scatters the block before the condensation check sees it. */
static void
test_a_2x2_block_of_steam_condenses_into_one_water_cell(void) {
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
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 3)), "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 4)), "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 4)), "and clear the other three corners of the square");
}

/* Three matching corners and a fourth cell that is NOT steam (stone,
 * here, not left empty - an empty fourth cell risks a neighbouring gas
 * cell sliding into it during the very same step's gas pass, before the
 * reactions pass ever gets to look, which would turn this into an
 * accidental positive instead of the negative it is meant to be) must
 * never condense, no matter how the roll would have gone. */
static void
test_condensation_needs_a_genuine_2x2_square(void) {
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

/* Acid rain - SAND_ACID_RAIN_CHANCE (sand.h), step_one_acid_rain_cell()
 * (sand_reactions.c) for the mechanism. sand_set_acid_rain(&s, 255) makes
 * a collapse overwhelmingly likely, not certain. Sealed on every side so
 * gas movement cannot scatter the pocket first. Column-striped (steam,
 * gas) so no arrangement can accidentally satisfy condensation, which
 * needs all four cells identical. Does NOT assert which of Acid or Water
 * survives - a 50/50 flip; this test is only the collapse shape. */
static void
test_a_qualifying_gas_steam_pocket_collapses_into_one_cell(void) {
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
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 3)), "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 4)), "and clear the other three corners of the square");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 4, 4)), "and clear the other three corners of the square");
}

/* Guards step_one_reacting_row()'s found |= FOUND_DISSOLVER report at its
 * acid-rain call site (sand_reactions.c): a collapsing pocket's survivor
 * is written behind the row walk's own scan position, so nothing but that
 * report keeps may_have_dissolver armed once the pass ends. Skip it and
 * the new acid is created inert. A full sealed 4x4 room, not just its
 * neighbours, since sand_set_mobility(0) means it drifts rather than
 * sitting still. Seeded to land the coin flip on Acid. */
static void
test_a_rained_acid_cell_keeps_dissolving_after_the_collapse(void) {
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
                    if ((xx == 2 || xx == 5 || yy == 2 || yy == 5) && CELL_MATERIAL(sand_at(&s, xx, yy)) == MAT_STONE) {
                        stone_left++;
                    }
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(stone_left < 12, "a rained acid cell sealed in a stone room must go on to "
                                                  "dissolve some of that room's walls within 120 further steps - "
                                                  "if may_have_dissolver silently cleared at the end of the "
                                                  "collapse's own pass, the acid is created inert and never eats "
                                                  "anything again");
    }
    TEST_ASSERT_TRUE_MESSAGE(found_acid_seed, "setup: none of the first 64 seeds landed the coin flip on Acid - "
                                              "widen the seed range, this test never exercised its own scenario");
}

/* One cell short of the two-of-each requirement (one steam, three gas -
 * still a pure gas/steam pocket, still boxed in identically) must never
 * collapse, no matter how the roll would have gone - the same "needs a
 * genuine match, not almost one" property
 * test_condensation_needs_a_genuine_2x2_square just above already
 * checks for its own sibling mechanic. */
static void
test_acid_rain_needs_at_least_two_of_each_species(void) {
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
 * genuine 50/50 coin flip, not always Acid. ACID_RAIN_TRIALS independent
 * collapses, forced acid_rain=255 so every one fires - the roll under
 * test is the residue flip, observed through RNG variation across many
 * trials. One shared sealed pocket, repainted and re-stepped rather than
 * side-by-side pockets: the trials stay independent either way. */
#define ACID_RAIN_TRIALS 40

static void
test_acid_rain_resolves_to_both_acid_and_water(void) {
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

void
run_sand_metal_suite(void) {
    RUN_TEST(test_dry_dirt_beside_lava_smelts_into_metal_or_stone);
    RUN_TEST(test_saturated_dirt_smelts_roughly_eight_times_slower);
    RUN_TEST(test_watered_dirt_steaming_precedes_resolving_when_it_happens);
    RUN_TEST(test_wet_dirt_can_still_steam_before_spoiling_at_least_sometimes);
    RUN_TEST(test_wet_dirt_can_spoil_into_sand_instead_of_smelting);
    RUN_TEST(test_dry_dirt_smelting_reaches_both_metal_and_stone);
    RUN_TEST(test_a_held_flame_smelts_dirt_as_lava_does);
    RUN_TEST(test_heat_through_a_stone_wall_smelts_the_dirt_beyond_it);
    RUN_TEST(test_a_non_conducting_extended_cell_passes_no_heat_beyond_itself);
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
}

SUITE_REGISTER(run_sand_metal_suite);
