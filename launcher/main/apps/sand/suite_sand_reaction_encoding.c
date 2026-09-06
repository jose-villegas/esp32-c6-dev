/*=============================================================================
 * Portable suite: the falling-sand automaton - reaction encoding - no
 * reaction may mint an ambiguous byte.
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

/* --- reaction dispatch: the table must not skip past the ladder ------- */

/* Written independently of reaction_first_stage() (sand_priv.h), on
 * purpose - if a future edit changes the stage order in one place and
 * not the other, this copy is what notices. Same field order the
 * ladder in step_one_reacting_row() (sand_reactions.c) walks. */
static int
reaction_ladder_reference_stage(const reaction_t *r, bool is_acid_rain_material)
{
    if (r->burns != 0) {
        return RSTAGE_BURN_ALWAYS;
    }
    if (r->burn_decay != 0) {
        return RSTAGE_BURN_CHECK;
    }
    if (r->dissolves != 0) {
        return RSTAGE_DISSOLVE;
    }
    if (is_acid_rain_material) {
        return RSTAGE_ACID_RAIN;
    }
    if (r->condenses != 0) {
        return RSTAGE_CONDENSE;
    }
    if (r->heat_ramp != 0) {
        return RSTAGE_HEAT_RAMP;
    }
    if (r->chills != 0) {
        return RSTAGE_CHILL;
    }
    if (r->warms != 0) {
        return RSTAGE_WARM;
    }
    if (r->soaks != 0 || r->dries != 0) {
        return RSTAGE_SOAK_DRY;
    }
    if (r->falls != 0) {
        return RSTAGE_FALL;
    }
    if (r->withers != 0) {
        return RSTAGE_WITHER;
    }
    if (r->drinks != 0) {
        return RSTAGE_DRINK;
    }
    if (r->roots != 0) {
        return RSTAGE_ROOT;
    }
    if (r->grows != 0) {
        return RSTAGE_GROW;
    }
    if (r->sprouts != 0) {
        return RSTAGE_SPROUT;
    }
    if (r->buds != 0) {
        return RSTAGE_BUD;
    }
    return RSTAGE_END;
}

/* Dispatching EARLIER than the reference is safe - dead field checks get
 * walked for nothing. Dispatching LATER silently drops behaviour, which
 * is what this asserts against, for every row in both tables. */
static void test_reaction_first_stage_never_dispatches_later_than_the_ladder(void)
{
    for (int m = 0; m < MAT_COUNT; m++) {
        const reaction_t *r = &reactions[m];
        const bool is_acid_rain = (m == MAT_GAS || m == MAT_STEAM);
        const int actual = reaction_first_stage(r, is_acid_rain);
        const int reference = reaction_ladder_reference_stage(r, is_acid_rain);
        char why[96];
        snprintf(why, sizeof why, "reactions[%s]",
            material_by_id((material_id_t)m)->name);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(reference, actual, why);
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        const reaction_t *r = &extended_reactions[k];
        const int actual = reaction_first_stage(r, false);
        const int reference = reaction_ladder_reference_stage(r, false);
        char why[64];
        snprintf(why, sizeof why, "extended_reactions[%d]", k);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(reference, actual, why);
    }
}

/* Acid-rain is gated on material IDENTITY, not a field -
 * reaction_first_stage() only sees it via the hand-threaded
 * is_acid_rain_material flag. Dropping the flag here stands in for a
 * future identity gate that forgets to thread it. */
static void test_dropping_the_acid_rain_identity_flag_dispatches_late(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(RSTAGE_ACID_RAIN,
        reaction_first_stage(&reactions[MAT_GAS], true),
        "gas, correctly flagged, must land on the acid-rain stage");
    TEST_ASSERT_EQUAL_MESSAGE(RSTAGE_ACID_RAIN,
        reaction_first_stage(&reactions[MAT_STEAM], true),
        "steam, correctly flagged, must land on the acid-rain stage");

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(RSTAGE_ACID_RAIN,
        reaction_first_stage(&reactions[MAT_GAS], false),
        "gas with no identity flag has no field to fall back on and "
        "must land LATE - the flag, not a field, is what makes this "
        "stage reachable at all");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(RSTAGE_ACID_RAIN,
        reaction_first_stage(&reactions[MAT_STEAM], false),
        "steam with no identity flag falls back to its own condenses "
        "field and still lands LATER than acid-rain - the same silent "
        "skip a future identity gate must not repeat");
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

void run_sand_reaction_encoding_suite(void)
{
    RUN_TEST(test_a_reaction_never_mints_a_static_from_gunpowder_or_the_reverse);
    RUN_TEST(test_wood_burning_state_is_byte_identical_under_lit_from);
    RUN_TEST(test_reaction_first_stage_never_dispatches_later_than_the_ladder);
    RUN_TEST(test_dropping_the_acid_rain_identity_flag_dispatches_late);
    RUN_TEST(test_ice_cracks_hot_glass_and_stays_where_it_is_put);
    RUN_TEST(test_snow_floats_on_water);
    RUN_TEST(test_glass_conducts_heat_like_stone);
    RUN_TEST(test_sand_turns_to_glass_under_sustained_heat);
    RUN_TEST(test_acid_spends_at_least_a_unit_of_itself_per_cell_dissolved);
    RUN_TEST(test_acid_fizzes_while_it_eats);
    RUN_TEST(test_the_fizz_rises_out_of_the_acid);
    RUN_TEST(test_acid_and_water_dilute_each_other);
    RUN_TEST(test_a_relentless_pour_of_acid_overwhelms_a_pool_of_water);
    RUN_TEST(test_a_relentless_pour_of_water_overwhelms_a_pool_of_acid);
    RUN_TEST(test_the_dilution_split_favours_neither_side);
    RUN_TEST(test_water_winning_the_dilution_boils_the_water_cell_to_steam);
    RUN_TEST(test_acid_winning_the_dilution_boils_the_acid_cell_to_gas);
    RUN_TEST(test_oil_mostly_boils_off_into_gas_not_acid);
    RUN_TEST(test_the_acid_that_ate_oil_can_die_in_a_single_bite);
    RUN_TEST(test_acid_evaporates_into_gas_when_forced);
    RUN_TEST(test_a_little_acid_cannot_eat_an_unlimited_amount);
    RUN_TEST(test_every_liquid_declares_a_mobility);
    RUN_TEST(test_water_does_not_drill_into_oil_when_tilted);
    RUN_TEST(test_oil_flows_more_slowly_than_water);
    RUN_TEST(test_oil_trapped_under_water_floats_to_the_surface);
    RUN_TEST(test_sand_floats_on_oil);
    RUN_TEST(test_dirt_still_sinks_through_oil);
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
}

SUITE_REGISTER(run_sand_reaction_encoding_suite);
